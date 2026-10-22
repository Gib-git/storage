#include "storage/core/duplicates.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "storage/core/index.hpp"
#include "storage/core/journal.hpp"

namespace storage {

const char* stageName(DupStage s) {
    switch (s) {
        case DupStage::Collecting: return "Collecting files";
        case DupStage::PartialHash: return "Comparing file starts and ends";
        case DupStage::FullHash: return "Hashing full contents";
        case DupStage::Verifying: return "Verifying byte by byte";
        case DupStage::Done: return "Done";
    }
    return "";
}

struct DuplicateFinder::Impl {
    Impl(FileSystem& f, DupOptions o, Index* c) : fs(f), opt(std::move(o)), cache(c) {}

    FileSystem& fs;
    DupOptions opt;
    Index* cache;

    std::atomic<bool> cancelFlag{false};
    std::atomic<int> stage{static_cast<int>(DupStage::Collecting)};
    std::atomic<uint64_t> filesDone{0}, filesTotal{0}, bytesDone{0}, bytesTotal{0}, errors{0};

    struct Candidate {
        DupFile file;
        std::string partial, full;
        bool failed = false;
    };

    unsigned threadCount() const {
        if (opt.threads) return opt.threads;
        return std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    }

    // Run fn(i) for i in [0, n) on a small worker pool.
    template <typename Fn>
    void parallelFor(size_t n, Fn fn) {
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t i; (i = next.fetch_add(1)) < n;) {
                if (cancelFlag.load(std::memory_order_relaxed)) return;
                fn(i);
            }
        };
        const unsigned t = static_cast<unsigned>(std::min<size_t>(threadCount(), std::max<size_t>(n, 1)));
        std::vector<std::thread> pool;
        for (unsigned k = 1; k < t; ++k) pool.emplace_back(work);
        work();
        for (auto& th : pool) th.join();
    }

    bool hiddenName(const std::string& name) const { return opt.skipHidden && !name.empty() && name[0] == '.'; }
    bool excludedName(const std::string& name) const {
        return std::find(opt.excludeNames.begin(), opt.excludeNames.end(), name) != opt.excludeNames.end();
    }

    std::vector<Candidate> collect(const Node& root) {
        std::vector<Candidate> out;
        std::unordered_set<uint64_t> seenInodes;  // hash of (dev, ino) for multi-link files
        std::vector<const Node*> stack{&root};
        const char sep = fs.separator();
        while (!stack.empty()) {
            const Node* n = stack.back();
            stack.pop_back();
            if (n->isDir()) {
                for (const auto& c : n->children) {
                    if (c->isDir() && (excludedName(c->name) || hiddenName(c->name))) continue;
                    stack.push_back(c.get());
                }
                continue;
            }
            if (n->kind != NodeKind::File || n->size == 0 || n->size < opt.minSize) continue;
            if (n->flags & NodeFlag::HardlinkRepeat) continue;
            if (hiddenName(n->name)) continue;
            if (n->nlink > 1 && n->ino && !seenInodes.insert(n->ino * 1000003ULL ^ n->dev).second) continue;
            Candidate c;
            c.file = {n->path(sep), n->size, n->mtime, n->dev, n->ino};
            out.push_back(std::move(c));
        }
        return out;
    }

    // Keep only candidates that share `key` with at least one other candidate.
    template <typename KeyFn>
    static std::vector<std::vector<Candidate*>> groupBy(std::vector<Candidate*>& items, KeyFn key) {
        std::unordered_map<std::string, std::vector<Candidate*>> m;
        for (Candidate* c : items)
            if (!c->failed) m[key(*c)].push_back(c);
        std::vector<std::vector<Candidate*>> out;
        for (auto& [k, v] : m)
            if (v.size() > 1) out.push_back(std::move(v));
        return out;
    }

    void loadCache(std::vector<Candidate*>& items) {
        if (!cache || items.empty()) return;
        std::vector<HashRecord> recs;
        recs.reserve(items.size());
        for (Candidate* c : items) recs.push_back({c->file.path, c->file.size, c->file.mtime, {}, {}});
        try {
            cache->lookupHashes(fs.volumeName(), recs);
        } catch (const std::exception&) {
            return;  // cache is an optimisation only
        }
        for (size_t i = 0; i < items.size(); ++i) {
            items[i]->partial = recs[i].partial;
            items[i]->full = recs[i].full;
        }
    }

    void saveCache(const std::vector<Candidate*>& items) {
        if (!cache) return;
        std::vector<HashRecord> recs;
        for (Candidate* c : items)
            if (!c->failed && (!c->partial.empty() || !c->full.empty()))
                recs.push_back({c->file.path, c->file.size, c->file.mtime, c->partial, c->full});
        try {
            cache->storeHashes(fs.volumeName(), recs);
        } catch (const std::exception&) {
        }
    }

    void setStage(DupStage s, uint64_t files, uint64_t bytes) {
        stage = static_cast<int>(s);
        filesDone = 0;
        bytesDone = 0;
        filesTotal = files;
        bytesTotal = bytes;
    }

    std::vector<DupGroup> run(const Node& root) {
        setStage(DupStage::Collecting, 0, 0);
        std::vector<Candidate> all = collect(root);

        // 1. Same size.
        std::unordered_map<uint64_t, std::vector<Candidate*>> bySize;
        for (auto& c : all) bySize[c.file.size].push_back(&c);
        std::vector<Candidate*> sized;
        for (auto& [size, v] : bySize)
            if (v.size() > 1) sized.insert(sized.end(), v.begin(), v.end());
        loadCache(sized);

        // 2. Same first and last 64 KiB (files that small are hashed whole here).
        std::vector<Candidate*> needPartial;
        for (Candidate* c : sized)
            if (c->partial.empty()) needPartial.push_back(c);
        setStage(DupStage::PartialHash, needPartial.size(), 0);
        parallelFor(needPartial.size(), [&](size_t i) {
            Candidate* c = needPartial[i];
            if (fs.hashPartial(c->file.path, c->file.size, c->partial)) {
                c->failed = true;
                errors.fetch_add(1);
            } else if (c->file.size <= 2 * FileSystem::kPartialChunk) {
                c->full = c->partial;  // the partial hash covered the whole file
            }
            filesDone.fetch_add(1);
        });
        if (cancelFlag) return {};

        auto partialGroups = groupBy(sized, [](const Candidate& c) {
            return std::to_string(c.file.size) + ":" + c.partial;
        });

        // 3. Same full content hash.
        std::vector<Candidate*> needFull;
        uint64_t fullBytes = 0;
        for (auto& g : partialGroups)
            for (Candidate* c : g)
                if (c->full.empty()) {
                    needFull.push_back(c);
                    fullBytes += c->file.size;
                }
        // Hash large files first so the progress bar is honest and threads stay busy.
        std::sort(needFull.begin(), needFull.end(),
                  [](const Candidate* a, const Candidate* b) { return a->file.size > b->file.size; });
        setStage(DupStage::FullHash, needFull.size(), fullBytes);
        parallelFor(needFull.size(), [&](size_t i) {
            Candidate* c = needFull[i];
            if (fs.hashFull(c->file.path, c->full, &cancelFlag, &bytesDone)) {
                if (!cancelFlag) {
                    c->failed = true;
                    errors.fetch_add(1);
                }
            }
            filesDone.fetch_add(1);
        });
        saveCache(sized);
        if (cancelFlag) return {};

        std::vector<Candidate*> hashed;
        for (auto& g : partialGroups) hashed.insert(hashed.end(), g.begin(), g.end());
        auto fullGroups = groupBy(hashed, [](const Candidate& c) { return std::to_string(c.file.size) + ":" + c.full; });

        // 4. Optional byte-by-byte confirmation against the first file of each group.
        if (opt.verifyBytes) {
            uint64_t files = 0, bytes = 0;
            for (auto& g : fullGroups) {
                files += g.size() - 1;
                bytes += (g.size() - 1) * g.front()->file.size;
            }
            setStage(DupStage::Verifying, files, bytes);
            std::vector<std::pair<Candidate*, Candidate*>> pairs;
            for (auto& g : fullGroups)
                for (size_t i = 1; i < g.size(); ++i) pairs.emplace_back(g.front(), g[i]);
            parallelFor(pairs.size(), [&](size_t i) {
                bool equal = false;
                if (fs.sameContent(pairs[i].first->file.path, pairs[i].second->file.path, equal, &cancelFlag) || !equal) {
                    if (!cancelFlag) pairs[i].second->failed = true;  // changed since hashing, or unreadable
                }
                filesDone.fetch_add(1);
                bytesDone.fetch_add(pairs[i].first->file.size);
            });
            if (cancelFlag) return {};
        }

        std::vector<DupGroup> groups;
        for (auto& g : fullGroups) {
            DupGroup dg;
            dg.size = g.front()->file.size;
            dg.hash = g.front()->full;
            for (Candidate* c : g)
                if (!c->failed) dg.files.push_back(c->file);
            if (dg.files.size() < 2) continue;
            std::sort(dg.files.begin(), dg.files.end(), [](const DupFile& a, const DupFile& b) { return a.path < b.path; });
            groups.push_back(std::move(dg));
        }
        std::sort(groups.begin(), groups.end(), [](const DupGroup& a, const DupGroup& b) {
            if (a.reclaimable() != b.reclaimable()) return a.reclaimable() > b.reclaimable();
            return a.files.front().path < b.files.front().path;
        });
        setStage(DupStage::Done, 0, 0);
        return groups;
    }
};

DuplicateFinder::DuplicateFinder(FileSystem& fs, DupOptions options, Index* cache)
    : d(std::make_unique<Impl>(fs, std::move(options), cache)) {}
DuplicateFinder::~DuplicateFinder() = default;

std::vector<DupGroup> DuplicateFinder::run(const Node& root) { return d->run(root); }
void DuplicateFinder::cancel() { d->cancelFlag = true; }
bool DuplicateFinder::cancelled() const { return d->cancelFlag; }

DupProgress DuplicateFinder::progress() const {
    DupProgress p;
    p.stage = static_cast<DupStage>(d->stage.load());
    p.filesDone = d->filesDone;
    p.filesTotal = d->filesTotal;
    p.bytesDone = d->bytesDone;
    p.bytesTotal = d->bytesTotal;
    p.errors = d->errors;
    return p;
}

}  // namespace storage

namespace storage {

const char* keepRuleName(KeepRule r) {
    switch (r) {
        case KeepRule::Newest: return "newest";
        case KeepRule::Oldest: return "oldest";
        case KeepRule::ShortestPath: return "shortest";
        case KeepRule::LongestPath: return "longest";
        case KeepRule::Alphabetical: return "alpha";
        case KeepRule::InFolder: return "folder";
    }
    return "oldest";
}

KeepRule parseKeepRule(const std::string& s) {
    for (KeepRule r : {KeepRule::Newest, KeepRule::Oldest, KeepRule::ShortestPath, KeepRule::LongestPath,
                       KeepRule::Alphabetical, KeepRule::InFolder})
        if (s == keepRuleName(r)) return r;
    throw std::invalid_argument("unknown keep rule '" + s + "' (newest, oldest, shortest, longest, alpha, folder)");
}

namespace {
bool under(const std::string& path, const std::string& folder) {
    if (folder.empty() || path.size() <= folder.size() || path.compare(0, folder.size(), folder) != 0) return false;
    const char next = path[folder.size()];
    return next == '/' || next == '\\' || folder.back() == '/' || folder.back() == '\\';
}
}  // namespace

size_t chooseKeeper(const DupGroup& g, KeepRule rule, const std::vector<std::string>& preferred) {
    if (g.files.empty()) return 0;
    auto best = [&](auto better) {
        size_t k = 0;
        for (size_t i = 1; i < g.files.size(); ++i)
            if (better(g.files[i], g.files[k])) k = i;
        return k;
    };
    auto shorter = [](const DupFile& a, const DupFile& b) {
        return a.path.size() != b.path.size() ? a.path.size() < b.path.size() : a.path < b.path;
    };
    switch (rule) {
        case KeepRule::Newest:
            return best([](const DupFile& a, const DupFile& b) { return a.mtime > b.mtime || (a.mtime == b.mtime && a.path < b.path); });
        case KeepRule::Oldest:
            return best([](const DupFile& a, const DupFile& b) { return a.mtime < b.mtime || (a.mtime == b.mtime && a.path < b.path); });
        case KeepRule::ShortestPath: return best(shorter);
        case KeepRule::LongestPath:
            return best([](const DupFile& a, const DupFile& b) {
                return a.path.size() != b.path.size() ? a.path.size() > b.path.size() : a.path < b.path;
            });
        case KeepRule::Alphabetical:
            return best([](const DupFile& a, const DupFile& b) { return a.path < b.path; });
        case KeepRule::InFolder:
            for (const auto& folder : preferred)
                for (size_t i = 0; i < g.files.size(); ++i)
                    if (under(g.files[i].path, folder)) return i;
            return best(shorter);
    }
    return 0;
}

std::vector<JournalOp> planDedupe(const std::vector<DupGroup>& groups, const std::vector<std::vector<bool>>& remove,
                                  const std::string& action) {
    if (remove.size() != groups.size()) throw std::invalid_argument("selection does not match groups");
    std::vector<JournalOp> ops;
    for (size_t g = 0; g < groups.size(); ++g) {
        const auto& files = groups[g].files;
        if (remove[g].size() != files.size()) throw std::invalid_argument("selection does not match group files");
        size_t keeper = files.size();
        for (size_t i = 0; i < files.size(); ++i)
            if (!remove[g][i]) {
                keeper = i;
                break;
            }
        bool any = false;
        for (bool r : remove[g]) any |= r;
        if (!any) continue;
        if (keeper == files.size())
            throw std::invalid_argument("refusing to remove every copy of " + files.front().path);
        for (size_t i = 0; i < files.size(); ++i) {
            if (!remove[g][i]) continue;
            JournalOp op;
            op.action = action;
            op.src = files[i].path;
            op.ref = files[keeper].path;
            op.size = files[i].size;
            op.mtime = files[i].mtime;
            op.hash = groups[g].hash;
            ops.push_back(std::move(op));
        }
    }
    return ops;
}

}  // namespace storage
