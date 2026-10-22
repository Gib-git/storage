// storagectl — headless front end to the Storage core. Every command supports
// --Json so scripts, SSH sessions and AI agents can drive it.
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <io.h>
#define STORAGE_ISATTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define STORAGE_ISATTY(f) isatty(fileno(f))
#endif

#include "storage/core/ai.hpp"
#include "storage/core/api.hpp"
#include "storage/core/cleaner.hpp"
#include "storage/core/duplicates.hpp"
#include "storage/core/executor.hpp"
#include "storage/core/format.hpp"
#include "storage/core/index.hpp"
#include "storage/core/journal.hpp"
#include "storage/core/json.hpp"
#include "storage/core/organize.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"
#include "storage/core/ssh_fs.hpp"
#include "storage/core/version.hpp"

#include "args.hpp"

using Json = storage::json::Value;
using namespace storage;

namespace {

std::atomic<Scanner*> gActiveScanner{nullptr};

void onSignal(int) {
    if (Scanner* s = gActiveScanner.load()) s->cancel();
}

struct Globals {
    std::string db;
    std::string nas;  // saved NAS name; empty = this computer
    bool json = false;
};

NasConfig findNas(const std::string& nameOrVolume) {
    for (const auto& c : loadNasConfigs())
        if (c.name == nameOrVolume || c.volume() == nameOrVolume) return c;
    throw cli::UsageError("no saved NAS named '" + nameOrVolume + "' (add one with `storagectl nas add`)");
}

// One connection per NAS for the life of the process.
FileSystem& connectNas(const NasConfig& cfg, bool quiet) {
    static std::map<std::string, std::unique_ptr<SshFileSystem>> open;
    auto& slot = open[cfg.volume()];
    if (!slot) {
        if (!quiet && STORAGE_ISATTY(stderr)) std::fprintf(stderr, "Connecting to %s…\n", cfg.name.c_str());
        auto fs = std::make_unique<SshFileSystem>(cfg);
        std::string message;
        if (auto ec = fs->connect(message))
            throw std::runtime_error("cannot connect to " + cfg.name + ": " + (message.empty() ? ec.message() : message));
        slot = std::move(fs);
    }
    return *slot;
}

FileSystem& filesystem(const Globals& g) {
    static LocalFileSystem local;
    return g.nas.empty() ? static_cast<FileSystem&>(local) : connectNas(findNas(g.nas), g.json);
}

FileSystem& filesystemForVolume(const std::string& volume, bool quiet) {
    static LocalFileSystem local;
    return volume == "local" || volume.empty() ? static_cast<FileSystem&>(local) : connectNas(findNas(volume), quiet);
}

// Local paths are made absolute; NAS paths are taken relative to the NAS's default folder.
std::string resolvePath(const Globals& g, const std::string& path) {
    if (g.nas.empty()) return normalizeLocalPath(path);
    if (!path.empty() && path[0] == '/') return path.size() > 1 && path.back() == '/' ? path.substr(0, path.size() - 1) : path;
    const NasConfig cfg = findNas(g.nas);
    std::string base = cfg.root.empty() ? "/" : cfg.root;
    if (path.empty() || path == ".") return base;
    return base.back() == '/' ? base + path : base + "/" + path;
}

std::filesystem::path dbPath(const Globals& g) {
    return g.db.empty() ? defaultIndexPath() : fromUtf8(g.db);
}

// The journal lives next to the index, so `--db` also isolates operation history.
std::filesystem::path journalPath(const Globals& g) {
    return g.db.empty() ? Journal::defaultPath() : fromUtf8(g.db).parent_path() / "journal.db";
}

std::unique_ptr<ScanTree> runScan(FileSystem& fs, const std::string& root, unsigned threads, bool crossMounts, bool quiet) {
    ScanOptions opts = ScanOptions::defaultsFor(root);
    opts.threads = threads;
    if (crossMounts) opts.stayOnDevice = false;

    Scanner scanner(fs, opts);
    gActiveScanner = &scanner;
    std::signal(SIGINT, onSignal);

    std::unique_ptr<ScanTree> tree;
    std::thread worker([&] { tree = scanner.run(); });

    const bool showProgress = !quiet && STORAGE_ISATTY(stderr);
    const auto start = std::chrono::steady_clock::now();
    while (!tree) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (showProgress && !tree) {
            const auto p = scanner.progress();
            std::fprintf(stderr, "\r\033[2KScanning… %s files, %s  %s", withThousands(p.files).c_str(),
                         humanSize(p.bytes).c_str(), p.current.substr(0, 80).c_str());
            std::fflush(stderr);
        }
    }
    worker.join();
    gActiveScanner = nullptr;
    std::signal(SIGINT, SIG_DFL);

    if (showProgress) {
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "\r\033[2K%s in %.1fs\n", tree->complete ? "Scanned" : "Cancelled", secs);
    }
    return tree;
}

Json nodeJson(const Node& n, int depth, size_t top) {
    Json j = Json::object({
        {"name", n.name},
        {"size", n.size},
        {"files", n.files},
        {"dirs", n.dirs},
        {"mtime", n.mtime},
        {"kind", n.isDir() ? "dir" : n.kind == NodeKind::Symlink ? "symlink" : "file"},
    });
    if (n.flags & NodeFlag::Unreadable) j["unreadable"] = true;
    if (n.flags & NodeFlag::Excluded) j["excluded"] = true;
    if (n.isDir() && depth > 0) {
        Json kids = Json::array();
        for (size_t i = 0; i < n.children.size() && i < top; ++i) kids.push_back(nodeJson(*n.children[i], depth - 1, top));
        j["children"] = std::move(kids);
    }
    return j;
}

void printTree(const Node& n, uint64_t total, int depth, size_t top, const std::string& lead,
               const std::string& indent, bool isRoot) {
    const double pct = total ? 100.0 * static_cast<double>(n.size) / static_cast<double>(total) : 0.0;
    std::string label = isRoot ? n.name : n.name + (n.isDir() ? "/" : "");
    if (n.flags & NodeFlag::Excluded) label += "  (not scanned)";
    if (n.flags & NodeFlag::Unreadable) label += "  (permission denied)";
    std::printf("%10s %6.1f%%  %s%s\n", humanSize(n.size).c_str(), pct, lead.c_str(), label.c_str());
    if (!n.isDir() || depth <= 0) return;

    const size_t shown = std::min(top, n.children.size());
    const bool hasMore = shown < n.children.size();
    for (size_t i = 0; i < shown; ++i) {
        const bool last = i + 1 == shown && !hasMore;
        printTree(*n.children[i], total, depth - 1, top, indent + (last ? "└── " : "├── "),
                  indent + (last ? "    " : "│   "), false);
    }
    if (hasMore) {
        uint64_t rest = 0;
        for (size_t i = shown; i < n.children.size(); ++i) rest += n.children[i]->size;
        std::printf("%10s %7s  %s└── … %zu more\n", humanSize(rest).c_str(), "", indent.c_str(),
                    n.children.size() - shown);
    }
}

Json scanInfoJson(const ScanInfo& s) {
    return Json::object({{"id", s.id},
                         {"root", s.root},
                         {"volume", s.volume},
                         {"started", s.started},
                         {"finished", s.finished},
                         {"files", s.files},
                         {"dirs", s.dirs},
                         {"bytes", s.bytes},
                         {"errors", s.errors},
                         {"complete", s.complete}});
}

int cmdScan(const cli::Parsed& p, const Globals& g) {
    const std::string root = resolvePath(g, p.positional[0]);
    auto tree = runScan(filesystem(g), root, static_cast<unsigned>(p.number("threads", 0)), p.has("cross-mounts"), g.json);
    if (!p.has("no-save")) Index(dbPath(g)).save(*tree);
    const Node& r = *tree->root;
    if (g.json) {
        std::cout << Json::object({{"scan_id", tree->id},
                                   {"root", r.name},
                                   {"bytes", r.size},
                                   {"files", r.files},
                                   {"dirs", r.dirs},
                                   {"errors", r.errors},
                                   {"complete", tree->complete}})
                         .dump(2)
                  << "\n";
    } else {
        std::printf("%s  %s files  %s folders  %s\n", humanSize(r.size).c_str(), withThousands(r.files).c_str(),
                    withThousands(r.dirs).c_str(), r.name.c_str());
        if (r.errors)
            std::printf("%s folders could not be read (permission denied)\n", withThousands(r.errors).c_str());
    }
    return tree->complete ? 0 : 130;
}

int cmdDu(const cli::Parsed& p, const Globals& g) {
    const std::string target = resolvePath(g, p.positional[0]);
    const int depth = static_cast<int>(p.number("depth", 1));
    const size_t top = static_cast<size_t>(std::max(1LL, p.number("top", 20)));

    std::unique_ptr<ScanTree> tree;
    const Node* node = nullptr;
    if (p.has("indexed")) {
        Index index(dbPath(g));
        for (const auto& s : index.scans()) {
            if (s.volume != filesystem(g).volumeName() || target.rfind(s.root, 0) != 0) continue;  // must contain target
            tree = index.load(s.id);
            if (tree && (node = tree->find(target))) break;
            tree.reset();
        }
        if (!node) throw std::runtime_error("no indexed scan contains " + target + " (run `storagectl scan` first)");
    } else {
        tree = runScan(filesystem(g), target, static_cast<unsigned>(p.number("threads", 0)), p.has("cross-mounts"), g.json);
        node = tree->root.get();
    }
    if (g.json) std::cout << nodeJson(*node, depth, top).dump(2) << "\n";
    else printTree(*node, node->size, depth, top, "", "", true);
    return 0;
}

int cmdScans(const cli::Parsed&, const Globals& g) {
    Index index(dbPath(g));
    const auto list = index.scans();
    if (g.json) {
        Json arr = Json::array();
        for (const auto& s : list) arr.push_back(scanInfoJson(s));
        std::cout << arr.dump(2) << "\n";
        return 0;
    }
    if (list.empty()) {
        std::printf("No scans yet. Try: storagectl scan ~\n");
        return 0;
    }
    std::printf("%5s  %10s  %12s  %-16s  %s\n", "ID", "SIZE", "FILES", "SCANNED", "ROOT");
    for (const auto& s : list)
        std::printf("%5lld  %10s  %12s  %-16s  %s%s\n", static_cast<long long>(s.id), humanSize(s.bytes).c_str(),
                    withThousands(s.files).c_str(), formatTime(s.finished).c_str(), s.root.c_str(),
                    s.complete ? "" : "  (partial)");
    return 0;
}


uint64_t parseSize(const std::string& s) {
    if (s.empty()) throw cli::UsageError("empty size");
    size_t used = 0;
    double v = 0;
    try {
        v = std::stod(s, &used);
    } catch (const std::exception&) {
        throw cli::UsageError("bad size '" + s + "' (examples: 500K, 10M, 2G)");
    }
    std::string unit = s.substr(used);
    for (auto& c : unit) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!unit.empty() && unit.back() == 'B') unit.pop_back();
    double mult = 1;
    if (unit == "K") mult = 1024.0;
    else if (unit == "M") mult = 1024.0 * 1024;
    else if (unit == "G") mult = 1024.0 * 1024 * 1024;
    else if (unit == "T") mult = 1024.0 * 1024 * 1024 * 1024;
    else if (!unit.empty()) throw cli::UsageError("bad size unit in '" + s + "'");
    return static_cast<uint64_t>(v * mult);
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ',');)
        if (!item.empty()) out.push_back(item);
    return out;
}

bool confirm(const std::string& question) {
    if (!STORAGE_ISATTY(stdin)) return false;
    std::fprintf(stderr, "%s [y/N] ", question.c_str());
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y');
}

// Finish anything a crash left half-done before starting new work.
void recoverJournal(Journal& journal, bool quiet) {
    for (const auto& volume : journal.volumesWithRunningBatches()) {
        std::vector<int64_t> ids;
        try {
            ids = Executor::recover(filesystemForVolume(volume, true), journal);
        } catch (const std::exception& e) {
            // An unreachable NAS keeps its batches as they are until it can be checked.
            if (!quiet) std::fprintf(stderr, "note: interrupted work on %s not checked yet: %s\n", volume.c_str(), e.what());
            continue;
        }
        for (int64_t id : ids)
            if (!quiet)
                std::fprintf(stderr, "note: batch %lld was interrupted; resume it with `storagectl resume %lld`\n",
                             static_cast<long long>(id), static_cast<long long>(id));
    }
}

Json batchJson(const JournalBatch& b) {
    return Json::object({{"id", b.id},         {"kind", b.kind},       {"title", b.title},     {"state", b.state},
                         {"created", b.created}, {"finished", b.finished}, {"total", b.total},   {"done", b.done},
                         {"failed", b.failed}, {"skipped", b.skipped}, {"undone", b.undone}, {"bytes", b.bytes}});
}

void printResult(const char* verb, const ExecResult& r) {
    std::printf("%s %d operations, skipped %d, failed %d — %s%s\n", verb, r.done, r.skipped, r.failed, humanSize(r.bytes).c_str(),
                r.cancelled ? " (stopped early)" : "");
}

// Run a batch with a progress line; Ctrl-C stops after the current file.
std::atomic<Executor*> gActiveExecutor{nullptr};
void onExecSignal(int) {
    if (Executor* e = gActiveExecutor.load()) e->cancel();
}

template <typename Fn>
ExecResult runWithProgress(Executor& ex, bool quiet, Fn fn) {
    gActiveExecutor = &ex;
    std::signal(SIGINT, onExecSignal);
    ExecResult result;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        result = fn();
        finished = true;
    });
    const bool show = !quiet && STORAGE_ISATTY(stderr);
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (show && !finished) {
            const auto p = ex.progress();
            std::fprintf(stderr, "\r\033[2K%d/%d  %s", p.done, p.total, p.current.substr(0, 90).c_str());
            std::fflush(stderr);
        }
    }
    worker.join();
    if (show) std::fprintf(stderr, "\r\033[2K");
    gActiveExecutor = nullptr;
    std::signal(SIGINT, SIG_DFL);
    return result;
}

int cmdDupes(const cli::Parsed& p, const Globals& g) {
    const std::string target = resolvePath(g, p.positional[0]);
    DupOptions opt;
    opt.minSize = std::max<uint64_t>(1, parseSize(p.value("min-size", "1")));
    opt.verifyBytes = p.has("paranoid");
    opt.skipHidden = p.has("skip-hidden");
    opt.excludeNames = splitList(p.value("exclude", ".git,.svn,.hg,node_modules"));
    opt.threads = static_cast<unsigned>(p.number("threads", 0));
    const KeepRule rule = parseKeepRule(p.value("keep", "oldest"));
    std::vector<std::string> prefer;
    for (const auto& f : splitList(p.value("prefer"))) prefer.push_back(resolvePath(g, f));
    const std::string action = p.value("action", Action::Trash);
    if (action != Action::Trash && action != Action::Delete && action != Action::Hardlink && action != Action::Clone)
        throw cli::UsageError("--action must be trash, delete, hardlink or clone");

    std::unique_ptr<ScanTree> tree = runScan(filesystem(g), target, 0, false, g.json);
    FileSystem& fs = filesystem(g);
    Index index(dbPath(g));
    DuplicateFinder finder(fs, opt, &index);

    std::vector<DupGroup> groups;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        groups = finder.run(*tree->root);
        finished = true;
    });
    const bool show = !g.json && STORAGE_ISATTY(stderr);
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (show && !finished) {
            const auto pr = finder.progress();
            std::fprintf(stderr, "\r\033[2K%s… %s/%s files  %s/%s", stageName(pr.stage),
                         withThousands(pr.filesDone).c_str(), withThousands(pr.filesTotal).c_str(),
                         humanSize(pr.bytesDone).c_str(), humanSize(pr.bytesTotal).c_str());
            std::fflush(stderr);
        }
    }
    worker.join();
    if (show) std::fprintf(stderr, "\r\033[2K");

    // Selection: keep one per group by rule, remove the rest.
    std::vector<std::vector<bool>> remove(groups.size());
    uint64_t reclaim = 0;
    size_t removeCount = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
        const size_t keep = chooseKeeper(groups[i], rule, prefer);
        remove[i].assign(groups[i].files.size(), true);
        remove[i][keep] = false;
        reclaim += groups[i].reclaimable();
        removeCount += groups[i].files.size() - 1;
    }

    const bool apply = p.has("apply");
    if (g.json && !apply) {
        Json arr = Json::array();
        for (size_t i = 0; i < groups.size(); ++i) {
            Json files = Json::array();
            for (size_t k = 0; k < groups[i].files.size(); ++k) {
                const auto& f = groups[i].files[k];
                files.push_back(Json::object({{"path", f.path}, {"mtime", f.mtime}, {"keep", !remove[i][k]}}));
            }
            arr.push_back(Json::object({{"size", groups[i].size},
                                        {"hash", groups[i].hash},
                                        {"reclaimable", groups[i].reclaimable()},
                                        {"files", std::move(files)}}));
        }
        std::cout << Json::object({{"groups", std::move(arr)},
                                   {"reclaimable", reclaim},
                                   {"keep_rule", keepRuleName(rule)},
                                   {"action", action},
                                   {"applied", false}})
                         .dump(2)
                  << "\n";
        return 0;
    }

    if (!g.json) {
        const size_t limit = static_cast<size_t>(std::max(0LL, p.number("top", 20)));
        for (size_t i = 0; i < groups.size() && i < limit; ++i) {
            const auto& gr = groups[i];
            std::printf("\n%s × %zu  (%s reclaimable)\n", humanSize(gr.size).c_str(), gr.files.size(),
                        humanSize(gr.reclaimable()).c_str());
            for (size_t k = 0; k < gr.files.size(); ++k)
                std::printf("  %s %s\n", remove[i][k] ? "✗" : "✓", gr.files[k].path.c_str());
        }
        if (groups.size() > limit) std::printf("\n… and %zu more groups\n", groups.size() - limit);
        std::printf("\n%zu duplicate groups · %zu extra copies · %s reclaimable (keeping %s)\n", groups.size(),
                    removeCount, humanSize(reclaim).c_str(), keepRuleName(rule));
    }
    if (!apply || groups.empty()) {
        if (!g.json && !groups.empty())
            std::printf("Dry run. Add --apply to %s the ✗ files.\n", action == Action::Trash ? "move to Trash" : action.c_str());
        return 0;
    }

    if (!p.has("yes") &&
        !confirm(action == Action::Delete ? "Permanently delete " + std::to_string(removeCount) + " files?"
                                          : "Apply '" + action + "' to " + std::to_string(removeCount) + " files?")) {
        std::fprintf(stderr, "Nothing changed. (Use --yes to skip this question.)\n");
        return 1;
    }

    Journal journal(journalPath(g));
    recoverJournal(journal, g.json);
    auto ops = planDedupe(groups, remove, action);
    const int64_t batch = journal.createBatch("dedupe", "Duplicates in " + target + " (" + action + ")", fs.volumeName(), ops);
    ExecOptions eo;
    eo.verifyContent = !p.has("no-verify");
    Executor ex(fs, journal, eo);
    const ExecResult r = runWithProgress(ex, g.json, [&] { return ex.run(batch); });
    if (g.json) {
        Json out = batchJson(journal.batch(batch));
        out["applied"] = true;
        std::cout << out.dump(2) << "\n";
    } else {
        printResult("Done", r);
        std::printf("Batch %lld — undo with `storagectl undo %lld`\n", static_cast<long long>(batch),
                    static_cast<long long>(batch));
    }
    return r.failed ? 1 : 0;
}


RuleSet loadRules(const cli::Parsed& p) {
    if (p.values.count("rules")) {
        std::ifstream f(fromUtf8(p.value("rules")), std::ios::binary);
        if (!f) throw std::runtime_error("cannot read " + p.value("rules"));
        std::stringstream ss;
        ss << f.rdbuf();
        return RuleSet::fromJson(Json::parse(ss.str()));
    }
    const std::string name = p.value("preset", "By file type");
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string wanted = lower(name);
    for (const auto& preset : builtinPresets()) {
        const std::string n = lower(preset.name);
        if (n == wanted) return preset;
    }
    for (const auto& preset : builtinPresets())  // unique prefix / word match: "tidy", "photos"
        if (lower(preset.name).find(wanted) != std::string::npos) return preset;
    throw cli::UsageError("unknown preset '" + name + "' (see `storagectl presets`)");
}

int cmdPresets(const cli::Parsed& p, const Globals& g) {
    const auto presets = builtinPresets();
    if (p.values.count("show")) {
        cli::Parsed q;
        q.values["preset"] = p.value("show");
        std::cout << loadRules(q).toJson().dump(2) << "\n";
        return 0;
    }
    if (g.json) {
        Json arr = Json::array();
        for (const auto& r : presets) arr.push_back(r.toJson());
        std::cout << arr.dump(2) << "\n";
        return 0;
    }
    for (const auto& r : presets) std::printf("%-26s %s\n", r.name.c_str(), r.description.c_str());
    std::printf("\nCustomize: storagectl presets --show \"By file type\" > my.json, edit, then --rules my.json\n");
    return 0;
}

int cmdOrganize(const cli::Parsed& p, const Globals& g) {
    const std::string target = resolvePath(g, p.positional[0]);
    RuleSet rules;
    std::unique_ptr<ScanTree> aiTree;
    if (p.has("ai")) {
        OllamaSettings ai;
        ai.host = p.value("ollama", ai.host);
        ai.model = p.value("model", ai.model);
        aiTree = runScan(filesystem(g), target, 0, false, g.json);
        if (!g.json && STORAGE_ISATTY(stderr)) std::fprintf(stderr, "Asking %s for organize rules…\n", ai.model.c_str());
        rules = suggestRules(ai, *aiTree->root, p.value("goal"));
        if (!g.json) std::printf("Suggested rules (save with --json, edit, reuse with --rules):\n%s\n\n", rules.toJson().dump(2).c_str());
    } else {
        rules = loadRules(p);
    }
    const std::string dest = p.values.count("dest") ? resolvePath(g, p.value("dest")) : std::string();

    std::unique_ptr<ScanTree> tree = aiTree ? std::move(aiTree) : runScan(filesystem(g), target, 0, false, g.json);
    FileSystem& fs = filesystem(g);
    const OrganizePlan plan = Organizer(fs, rules).plan(*tree->root, dest);
    const bool apply = p.has("apply");
    auto relOf = [](const std::string& path, const std::string& base) {
        return path.size() > base.size() && path.compare(0, base.size(), base) == 0 ? path.substr(base.size() + 1) : path;
    };

    if (g.json && !apply) {
        Json moves = Json::array();
        for (const auto& m : plan.moves)
            moves.push_back(Json::object({{"from", m.src}, {"to", m.dst}, {"rule", m.rule}, {"size", m.size}, {"renamed", m.renamed}}));
        Json removed = Json::array();
        for (const auto& d : plan.removeDirs) removed.push_back(d);
        std::cout << Json::object({{"preset", rules.name},
                                   {"scope", plan.scope},
                                   {"destination", plan.destination},
                                   {"moves", std::move(moves)},
                                   {"remove_empty_folders", std::move(removed)},
                                   {"already_in_place", plan.alreadyInPlace},
                                   {"unmatched", plan.unmatched},
                                   {"conflicts_skipped", plan.conflictsSkipped},
                                   {"protected_skipped", plan.protectedSkipped},
                                   {"bytes", plan.bytes},
                                   {"applied", false}})
                         .dump(2)
                  << "\n";
        return 0;
    }

    if (!g.json) {
        const size_t limit = static_cast<size_t>(std::max(0LL, p.number("top", 30)));
        std::printf("Rules: %s\n\n", rules.name.c_str());
        for (size_t i = 0; i < plan.moves.size() && i < limit; ++i) {
            const auto& m = plan.moves[i];
            std::printf("  %s\n    → %s%s\n", relOf(m.src, plan.scope).c_str(), relOf(m.dst, plan.destination).c_str(),
                        m.renamed ? "  (renamed: name taken)" : "");
        }
        if (plan.moves.size() > limit) std::printf("  … and %zu more\n", plan.moves.size() - limit);
        std::printf("\n%zu files to move (%s) · %zu folders to create · %zu emptied folders to remove\n",
                    plan.moves.size(), humanSize(plan.bytes).c_str(), plan.createDirs.size(), plan.removeDirs.size());
        std::printf("%zu already in place · %zu not matched by any rule · %zu skipped (name taken) · %zu inside "
                    "apps/libraries/.git left alone\n",
                    plan.alreadyInPlace, plan.unmatched, plan.conflictsSkipped, plan.protectedSkipped);
    }
    if (!apply || plan.moves.empty()) {
        if (!g.json && !plan.moves.empty()) std::printf("Dry run. Add --apply to organize.\n");
        return 0;
    }
    if (!p.has("yes") && !confirm("Move " + std::to_string(plan.moves.size()) + " files?")) {
        std::fprintf(stderr, "Nothing changed. (Use --yes to skip this question.)\n");
        return 1;
    }
    Journal journal(journalPath(g));
    recoverJournal(journal, g.json);
    const int64_t batch = journal.createBatch("organize", "Organize " + target + " (" + rules.name + ")", fs.volumeName(), plan.toOps());
    Executor ex(fs, journal);
    const ExecResult r = runWithProgress(ex, g.json, [&] { return ex.run(batch); });
    if (g.json) {
        Json out = batchJson(journal.batch(batch));
        out["applied"] = true;
        std::cout << out.dump(2) << "\n";
    } else {
        printResult("Done", r);
        std::printf("Batch %lld — undo with `storagectl undo %lld`\n", static_cast<long long>(batch),
                    static_cast<long long>(batch));
    }
    return r.failed ? 1 : 0;
}


int cmdClean(const cli::Parsed& p, const Globals& g) {
    if (!g.nas.empty()) throw cli::UsageError("`clean` works on this computer; for a NAS use `insights` and `dupes`");
    std::vector<CleanTarget> targets;
    const auto only = splitList(p.value("only"));
    for (const auto& t : cleanCatalogue()) {
        const bool chosen = only.empty() ? (p.has("all") || t.defaultOn) : std::find(only.begin(), only.end(), t.id) != only.end();
        if (chosen) targets.push_back(t);
    }
    if (targets.empty()) throw cli::UsageError("no matching cleanup targets (see `storagectl clean --all`)");

    LocalFileSystem fs;
    if (!g.json && STORAGE_ISATTY(stderr)) std::fprintf(stderr, "Measuring %zu locations…\n", targets.size());
    const auto items = analyzeCleanTargets(fs, targets);
    const bool permanent = !p.has("trash");
    uint64_t total = 0;
    for (const auto& i : items) total += i.bytes;

    if (g.json && !p.has("apply")) {
        Json arr = Json::array();
        for (const auto& i : items)
            arr.push_back(Json::object({{"id", i.target.id},
                                        {"group", i.target.group},
                                        {"name", i.target.name},
                                        {"safety", safetyName(i.target.safety)},
                                        {"bytes", i.bytes},
                                        {"files", i.files},
                                        {"entries", static_cast<uint64_t>(i.entries.size())},
                                        {"default", i.target.defaultOn}}));
        Json cmds = Json::array();
        for (const auto& c : cleanCommands())
            cmds.push_back(Json::object({{"name", c.name}, {"command", c.command}, {"description", c.description}}));
        std::cout << Json::object({{"targets", std::move(arr)}, {"bytes", total}, {"commands", std::move(cmds)}, {"applied", false}})
                         .dump(2)
                  << "\n";
        return 0;
    }
    if (!g.json) {
        std::printf("%-22s %-10s %-8s %10s  %s\n", "ID", "GROUP", "SAFETY", "SIZE", "WHAT");
        for (const auto& i : items)
            std::printf("%-22s %-10s %-8s %10s  %s%s\n", i.target.id.c_str(), i.target.group.c_str(),
                        safetyName(i.target.safety), humanSize(i.bytes).c_str(), i.target.name.c_str(),
                        i.target.hint.empty() ? "" : ("  — " + i.target.hint).c_str());
        std::printf("\nTotal: %s. Safe caches are deleted permanently (use --trash to move them to the Trash instead);\n"
                    "everything else goes to the Trash.\n",
                    humanSize(total).c_str());
        std::printf("\nYou can also run these yourself:\n");
        for (const auto& c : cleanCommands()) std::printf("  %-40s  # %s\n", c.command.c_str(), c.description.c_str());
    }
    if (!p.has("apply") || total == 0) {
        if (!g.json && total) std::printf("\nDry run. Add --apply to clean the listed locations.\n");
        return 0;
    }
    if (!p.has("yes") && !confirm("Clean up " + humanSize(total) + "?")) return 1;
    Journal journal(journalPath(g));
    recoverJournal(journal, g.json);
    const int64_t batch = journal.createBatch("clean", "Clean up " + humanSize(total), "local", planClean(items, permanent));
    Executor ex(fs, journal);
    const ExecResult r = runWithProgress(ex, g.json, [&] { return ex.run(batch); });
    if (g.json) std::cout << batchJson(journal.batch(batch)).dump(2) << "\n";
    else printResult("Cleaned", r);
    return r.failed ? 1 : 0;
}

int cmdInsights(const cli::Parsed& p, const Globals& g) {
    const std::string target = resolvePath(g, p.positional[0]);
    const int days = static_cast<int>(p.number("older", 365));
    const uint64_t minSize = parseSize(p.value("min-size", "100M"));
    auto tree = runScan(filesystem(g), target, 0, false, g.json);
    const char sep = filesystem(g).separator();
    const auto large = findLargeOldFiles(*tree->root, sep, minSize, days, 50);
    const auto junk = findProjectJunk(*tree->root, sep, std::min(days, 180));
    const auto empty = findEmptyFolders(*tree->root, sep);
    auto list = [](const std::vector<Finding>& v) {
        Json a = Json::array();
        for (const auto& f : v)
            a.push_back(Json::object({{"path", f.path}, {"size", f.size}, {"mtime", f.mtime}, {"reason", f.reason}}));
        return a;
    };
    if (g.json) {
        std::cout << Json::object({{"large_old_files", list(large)}, {"project_junk", list(junk)}, {"empty_folders", list(empty)}})
                         .dump(2)
                  << "\n";
        return 0;
    }
    auto section = [](const char* title, const std::vector<Finding>& v, bool sized) {
        uint64_t total = 0;
        for (const auto& f : v) total += f.size;
        std::printf("\n%s (%zu%s)\n", title, v.size(), sized ? (", " + humanSize(total)).c_str() : "");
        for (size_t i = 0; i < v.size() && i < 15; ++i)
            std::printf("  %10s  %s  %s\n", sized ? humanSize(v[i].size).c_str() : "", formatTime(v[i].mtime).substr(0, 10).c_str(),
                        v[i].path.c_str());
        if (v.size() > 15) std::printf("  … and %zu more\n", v.size() - 15);
    };
    section("Large files not modified in a while", large, true);
    section("Rebuildable project folders in idle projects", junk, true);
    section("Empty folders", empty, false);
    return 0;
}


int cmdNas(const cli::Parsed& p, const Globals& g) {
    const std::string action = p.positional[0];
    const std::string name = p.positional.size() > 1 ? p.positional[1] : std::string();
    auto list = loadNasConfigs();
    if (action == "list") {
        if (g.json) {
            Json arr = Json::array();
            for (const auto& c : list) arr.push_back(c.toJson());
            std::cout << arr.dump(2) << "\n";
        } else if (list.empty()) {
            std::printf("No NAS saved. Add one: storagectl nas add mynas --host 192.168.1.20 --user admin --root /volume1\n");
        } else {
            for (const auto& c : list) std::printf("%-16s %-32s root %s\n", c.name.c_str(), c.volume().c_str(), c.root.c_str());
        }
        return 0;
    }
    if (name.empty()) throw cli::UsageError("`nas " + action + "` needs a name");
    if (action == "add") {
        NasConfig c;
        c.name = name;
        c.host = p.value("host", name);
        c.user = p.value("user");
        c.port = static_cast<int>(p.number("port", 0));
        c.identityFile = p.value("key");
        c.root = p.value("root", "/");
        list.erase(std::remove_if(list.begin(), list.end(), [&](const NasConfig& x) { return x.name == name; }), list.end());
        list.push_back(c);
        saveNasConfigs(list);
        std::printf("Saved %s (%s). Test it with: storagectl nas test %s\n", name.c_str(), c.volume().c_str(), name.c_str());
        return 0;
    }
    if (action == "remove") {
        const size_t before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const NasConfig& x) { return x.name == name; }), list.end());
        if (list.size() == before) throw cli::UsageError("no saved NAS named '" + name + "'");
        saveNasConfigs(list);
        std::printf("Removed %s\n", name.c_str());
        return 0;
    }
    if (action == "test") {
        const NasConfig cfg = findNas(name);
        SshFileSystem fs(cfg, 1);
        std::string message;
        if (auto ec = fs.connect(message)) {
            std::fprintf(stderr, "Could not connect to %s: %s\n", cfg.name.c_str(), message.empty() ? ec.message().c_str() : message.c_str());
            std::fprintf(stderr, "\nStorage uses your ssh keys (no passwords). Set one up with:\n  ssh-keygen -t ed25519   (if you have no key)\n  ssh-copy-id %s%s\n",
                         cfg.user.empty() ? "" : (cfg.user + "@").c_str(), cfg.host.c_str());
            return 1;
        }
        const auto& c = fs.caps();
        DirEntry root;
        const bool rootOk = !fs.stat(cfg.root, root) && root.kind == NodeKind::Directory;
        if (g.json) {
            std::cout << Json::object({{"connected", true}, {"system", c.system}, {"gnu_find", c.gnuFind}, {"stat", c.stat},
                                       {"sha", c.sha}, {"reflink", c.reflink}, {"root_ok", rootOk}})
                             .dump(2)
                      << "\n";
        } else {
            std::printf("Connected to %s (%s)\n  listing: %s\n  hashing: %s\n  clones:  %s\n  root %s: %s\n", cfg.name.c_str(),
                        c.system.c_str(), c.gnuFind ? "GNU find (fast)" : ("stat (" + c.stat + ")").c_str(),
                        c.sha.empty() ? "unavailable — duplicate search will not work" : c.sha.c_str(),
                        c.reflink ? "supported" : "not supported", cfg.root.c_str(), rootOk ? "ok" : "NOT FOUND");
        }
        return rootOk ? 0 : 1;
    }
    throw cli::UsageError("unknown nas action '" + action + "' (add, list, remove, test)");
}


int cmdServe(const cli::Parsed& p, const Globals& g) {
    Service service(dbPath(g), journalPath(g));
    const std::string token = p.values.count("token") ? p.value("token") : apiToken();
    const std::string host = p.value("host", "127.0.0.1");
    http::Server server([&](const http::Request& r) { return handleRest(service, r, token); });
    if (auto ec = server.listen(host, static_cast<int>(p.number("port", 8765))))
        throw std::runtime_error("cannot listen on " + host + ": " + ec.message());
    std::printf("Storage API on http://%s:%d/v1  (token: %s)\n", host.c_str(), server.port(), token.c_str());
    std::printf("Try: curl -s -H 'Authorization: Bearer %s' http://%s:%d/v1/tools\n", token.c_str(), host.c_str(), server.port());
    if (host != "127.0.0.1" && host != "localhost")
        std::printf("Warning: listening beyond this computer. Anyone with the token can change files.\n");
    std::fflush(stdout);
    server.serve();
    return 0;
}

int cmdMcp(const cli::Parsed&, const Globals& g) {
    Service service(dbPath(g), journalPath(g));
    runMcpServer(service, std::cin, std::cout);
    return 0;
}

int cmdHistory(const cli::Parsed& p, const Globals& g) {
    Journal journal(journalPath(g));
    recoverJournal(journal, g.json);
    const auto list = journal.batches(static_cast<int>(p.number("limit", 30)));
    if (g.json) {
        Json arr = Json::array();
        for (const auto& b : list) arr.push_back(batchJson(b));
        std::cout << arr.dump(2) << "\n";
        return 0;
    }
    if (list.empty()) {
        std::printf("No operations yet.\n");
        return 0;
    }
    std::printf("%5s  %-16s  %-11s  %6s  %6s  %10s  %s\n", "ID", "WHEN", "STATE", "DONE", "OTHER", "SIZE", "WHAT");
    for (const auto& b : list)
        std::printf("%5lld  %-16s  %-11s  %6d  %6d  %10s  %s\n", static_cast<long long>(b.id), formatTime(b.created).c_str(),
                    b.state.c_str(), b.done, b.failed + b.skipped, humanSize(b.bytes).c_str(), b.title.c_str());
    if (p.has("verbose") || list.size() == 1) {
        for (const auto& op : journal.ops(list.front().id))
            std::printf("  %-8s %-8s %s%s%s%s\n", op.state.c_str(), op.action.c_str(), op.src.c_str(),
                        op.action == Action::Move ? (" → " + op.dst).c_str() : "",
                        op.detail.empty() ? "" : "  — ", op.detail.c_str());
    }
    return 0;
}

int cmdUndoResume(const cli::Parsed& p, const Globals& g, bool undo) {
    long long id = 0;
    try {
        id = std::stoll(p.positional[0]);
    } catch (const std::exception&) {
        throw cli::UsageError("batch id must be a number (see `storagectl history`)");
    }
    Journal journal(journalPath(g));
    recoverJournal(journal, true);
    const JournalBatch b = journal.batch(id);
    if (!undo && b.state == BatchState::Done) {
        std::printf("Batch %lld already finished.\n", id);
        return 0;
    }
    if (!p.has("yes") && !confirm(std::string(undo ? "Undo" : "Resume") + " \"" + b.title + "\"?")) return 1;
    FileSystem& fs = filesystemForVolume(b.volume, g.json);
    Executor ex(fs, journal);
    const ExecResult r = runWithProgress(ex, g.json, [&] { return undo ? ex.undo(id) : ex.run(id); });
    if (g.json) std::cout << batchJson(journal.batch(id)).dump(2) << "\n";
    else printResult(undo ? "Reverted" : "Done", r);
    return r.failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    cli::Parser parser("storagectl", "storagectl — index, deduplicate, organize and clean disks");
    parser.global()
        .option("db", 0, "PATH", "Index database (default: per-user app data directory)")
        .option("nas", 0, "NAME", "Work on a saved NAS over SSH instead of this computer")
        .flag("json", 0, "Machine-readable JSON output")
        .flag("help", 'h', "Show help")
        .flag("version", 0, "Show version");

    parser.add("scan", "Scan a folder or drive and save it to the index")
        .positional("path")
        .option("threads", 'j', "N", "Worker threads (default: CPU count)")
        .flag("cross-mounts", 0, "Descend into other mounted filesystems")
        .flag("no-save", 0, "Do not write the result to the index");

    parser.add("du", "Show the largest folders and files under a path")
        .positional("path")
        .option("depth", 'd', "N", "Levels to show (default 1)")
        .option("top", 'n', "N", "Entries per level (default 20)")
        .option("threads", 'j', "N", "Worker threads (default: CPU count)")
        .flag("indexed", 'i', "Use the saved index instead of rescanning")
        .flag("cross-mounts", 0, "Descend into other mounted filesystems");

    parser.add("scans", "List scans saved in the index");

    parser.add("dupes", "Find duplicate files (dry run unless --apply)")
        .positional("path")
        .option("min-size", 0, "SIZE", "Ignore files smaller than this, e.g. 100K, 5M (default 1 byte)")
        .option("keep", 'k', "RULE", "Which copy to keep: oldest (default), newest, shortest, longest, alpha, folder")
        .option("prefer", 0, "DIRS", "With --keep folder: comma-separated folders whose copies are kept")
        .option("action", 'a', "ACTION", "trash (default), hardlink, clone or delete")
        .option("exclude", 0, "NAMES", "Folder names to skip (default .git,.svn,.hg,node_modules)")
        .option("top", 'n', "N", "Groups to print (default 20)")
        .option("threads", 'j', "N", "Hashing threads (default up to 4; use 1 for spinning disks)")
        .flag("skip-hidden", 0, "Ignore dot-files and dot-folders")
        .flag("paranoid", 0, "Confirm duplicates byte by byte after hashing")
        .flag("apply", 0, "Actually perform the action (journaled, undoable)")
        .flag("yes", 'y', "Do not ask for confirmation")
        .flag("no-verify", 0, "Skip re-hashing right before delete/hardlink/clone");

    parser.add("organize", "Reorganize a folder by rules (dry run unless --apply)")
        .positional("path")
        .option("preset", 'p', "NAME", "Built-in rule set (default \"By file type\"; see `presets`)")
        .option("rules", 'r', "FILE", "Rule set from a JSON file")
        .option("dest", 0, "DIR", "Put organized files under this folder (default: the folder itself)")
        .flag("ai", 0, "Ask a local Ollama model to suggest the rules (sends names and sizes, never contents)")
        .option("goal", 0, "TEXT", "With --ai: what you want, e.g. \"sort camera uploads by year\"")
        .option("model", 0, "NAME", "With --ai: Ollama model (default llama3.2)")
        .option("ollama", 0, "URL", "With --ai: Ollama address (default http://localhost:11434)")
        .option("top", 'n', "N", "Moves to print (default 30)")
        .flag("apply", 0, "Actually move the files (journaled, undoable)")
        .flag("yes", 'y', "Do not ask for confirmation");

    parser.add("presets", "List built-in organize rule sets").option("show", 0, "NAME", "Print a preset as editable JSON");

    parser.add("clean", "Free space from caches, logs and other junk (dry run unless --apply)")
        .option("only", 0, "IDS", "Comma-separated target ids (see the ID column)")
        .flag("all", 0, "Include targets that are off by default (Trash, backups, old downloads, ...)")
        .flag("trash", 0, "Move caches to the Trash instead of deleting them permanently")
        .flag("apply", 0, "Actually clean (journaled)")
        .flag("yes", 'y', "Do not ask for confirmation");

    parser.add("insights", "Large old files, idle project junk and empty folders under a path")
        .positional("path")
        .option("older", 0, "DAYS", "Not modified for this many days (default 365)")
        .option("min-size", 0, "SIZE", "Minimum size for large files (default 100M)");

    parser.add("nas", "Manage NAS connections: add, list, remove, test")
        .positional("action")
        .optionalPositional("name")
        .option("host", 0, "HOST", "Host name, IP or ~/.ssh/config alias (default: the name)")
        .option("user", 'u', "USER", "SSH user")
        .option("port", 0, "PORT", "SSH port")
        .option("key", 0, "FILE", "Private key file (default: your ssh agent/keys)")
        .option("root", 0, "DIR", "Default folder on the NAS, e.g. /volume1 (default /)");

    parser.add("serve", "Run the local REST API (for scripts and agents)")
        .option("port", 0, "PORT", "Port (default 8765)")
        .option("host", 0, "ADDR", "Listen address (default 127.0.0.1 — this computer only)")
        .option("token", 0, "TOKEN", "Bearer token (default: generated and saved in the app data folder)");

    parser.add("mcp", "Run as an MCP server on stdio (for Claude and other MCP clients)");

    parser.add("history", "List journaled operations")
        .option("limit", 0, "N", "How many batches (default 30)")
        .flag("verbose", 'v', "Show the operations of the latest batch");

    parser.add("undo", "Revert a batch of operations").positional("batch").flag("yes", 'y', "Do not ask for confirmation");
    parser.add("resume", "Finish an interrupted batch").positional("batch").flag("yes", 'y', "Do not ask for confirmation");

    Globals g;
    cli::Parsed p;
    try {
        p = parser.parse(argc, argv);
    } catch (const cli::UsageError& e) {
        std::fprintf(stderr, "error: %s\n\n", e.what());
        parser.printHelp();
        return 2;
    }
    if (p.has("version")) {
        std::printf("storagectl %s\n", kVersion);
        return 0;
    }
    if (p.has("help") || p.command.empty()) {
        parser.printHelp(p.command);
        return 0;
    }
    g.db = p.value("db");
    g.nas = p.value("nas");
    g.json = p.has("json");

    try {
        if (p.command == "scan") return cmdScan(p, g);
        if (p.command == "du") return cmdDu(p, g);
        if (p.command == "scans") return cmdScans(p, g);
        if (p.command == "dupes") return cmdDupes(p, g);
        if (p.command == "history") return cmdHistory(p, g);
        if (p.command == "organize") return cmdOrganize(p, g);
        if (p.command == "presets") return cmdPresets(p, g);
        if (p.command == "clean") return cmdClean(p, g);
        if (p.command == "insights") return cmdInsights(p, g);
        if (p.command == "nas") return cmdNas(p, g);
        if (p.command == "serve") return cmdServe(p, g);
        if (p.command == "mcp") return cmdMcp(p, g);
        if (p.command == "undo") return cmdUndoResume(p, g, true);
        if (p.command == "resume") return cmdUndoResume(p, g, false);
    } catch (const cli::UsageError& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    } catch (const std::exception& e) {
        if (g.json) std::cout << Json::object({{"error", e.what()}}).dump() << "\n";
        else std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
