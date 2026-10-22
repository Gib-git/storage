#include "storage/core/scanner.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_set>

namespace storage {

namespace {

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct InodeKey {
    uint64_t dev, ino;
    bool operator==(const InodeKey& o) const { return dev == o.dev && ino == o.ino; }
};
struct InodeHash {
    size_t operator()(const InodeKey& k) const { return std::hash<uint64_t>()(k.ino * 1000003ULL ^ k.dev); }
};

struct Work {
    Node* node;
    std::string path;
};

}  // namespace

ScanOptions ScanOptions::defaultsFor(const std::string& root) {
    ScanOptions o;
    o.root = root;
#if defined(__APPLE__)
    // The sealed system volume and the data volume are stitched together with
    // firmlinks, so crossing devices is required and /System/Volumes must be
    // skipped or everything under /Users etc. is counted twice.
    o.stayOnDevice = false;
    o.excludes = {"/System/Volumes", "/Volumes", "/dev", "/Network", "/net", "/home"};
#elif defined(_WIN32)
    o.stayOnDevice = false;  // drive letters are separate roots; junctions are never followed
#else
    o.stayOnDevice = true;
    o.excludes = {"/proc", "/sys", "/dev", "/run"};
#endif
    return o;
}

struct Scanner::Impl {
    Impl(FileSystem& f, ScanOptions o) : fs(f), opt(std::move(o)), excludes(opt.excludes.begin(), opt.excludes.end()) {}

    FileSystem& fs;
    ScanOptions opt;
    std::unordered_set<std::string> excludes;
    uint64_t rootDev = 0;

    std::atomic<bool> cancelFlag{false};
    std::atomic<uint64_t> files{0}, dirs{0}, bytes{0}, errors{0};

    mutable std::mutex currentMu;
    std::string current;

    std::mutex queueMu;
    std::condition_variable queueCv;
    std::deque<Work> queue;
    size_t active = 0;

    std::mutex linkMu;
    std::unordered_set<InodeKey, InodeHash> seenLinks;

    void worker() {
        std::vector<DirEntry> entries;
        std::vector<Work> found;
        for (;;) {
            Work w;
            {
                std::unique_lock lk(queueMu);
                queueCv.wait(lk, [&] { return !queue.empty() || active == 0; });
                if (queue.empty()) return;
                w = std::move(queue.front());
                queue.pop_front();
                ++active;
            }

            found.clear();
            if (!cancelFlag.load(std::memory_order_relaxed)) process(w, entries, found);

            {
                std::lock_guard lk(queueMu);
                for (auto& f : found) queue.push_back(std::move(f));
                --active;
            }
            queueCv.notify_all();
        }
    }

    void process(Work& w, std::vector<DirEntry>& entries, std::vector<Work>& found) {
        {
            std::lock_guard lk(currentMu);
            current = w.path;
        }
        if (fs.list(w.path, entries)) {
            w.node->flags |= NodeFlag::Unreadable;
            errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        w.node->children.reserve(entries.size());
        uint64_t localFiles = 0, localDirs = 0, localBytes = 0;
        for (auto& e : entries) {
            auto child = std::make_unique<Node>();
            child->name = std::move(e.name);
            child->parent = w.node;
            child->kind = e.kind;
            child->size = e.size;
            child->mtime = e.mtime;
            child->dev = e.dev;
            child->ino = e.ino;
            child->nlink = e.nlink;

            if (child->kind == NodeKind::Directory) {
                ++localDirs;
                std::string childPath = fs.join(w.path, child->name);
                const bool otherDevice = opt.stayOnDevice && e.dev != 0 && e.dev != rootDev;
                if (otherDevice || excludes.count(childPath)) {
                    child->flags |= NodeFlag::Excluded;
                } else {
                    found.push_back({child.get(), std::move(childPath)});
                }
            } else if (child->kind == NodeKind::File) {
                ++localFiles;
                bool repeat = false;
                if (e.nlink > 1 && e.ino != 0) {
                    std::lock_guard lk(linkMu);
                    repeat = !seenLinks.insert({e.dev, e.ino}).second;
                }
                if (repeat) child->flags |= NodeFlag::HardlinkRepeat;
                else localBytes += e.size;
            }
            w.node->children.push_back(std::move(child));
        }
        files.fetch_add(localFiles, std::memory_order_relaxed);
        dirs.fetch_add(localDirs, std::memory_order_relaxed);
        bytes.fetch_add(localBytes, std::memory_order_relaxed);
    }
};

Scanner::Scanner(FileSystem& fs, ScanOptions options) : d(std::make_unique<Impl>(fs, std::move(options))) {}
Scanner::~Scanner() = default;

void Scanner::cancel() { d->cancelFlag.store(true); }
bool Scanner::cancelled() const { return d->cancelFlag.load(); }

ScanProgress Scanner::progress() const {
    ScanProgress p;
    p.files = d->files.load(std::memory_order_relaxed);
    p.dirs = d->dirs.load(std::memory_order_relaxed);
    p.bytes = d->bytes.load(std::memory_order_relaxed);
    p.errors = d->errors.load(std::memory_order_relaxed);
    std::lock_guard lk(d->currentMu);
    p.current = d->current;
    return p;
}

std::unique_ptr<ScanTree> Scanner::run() {
    auto tree = std::make_unique<ScanTree>();
    tree->volume = d->fs.volumeName();
    tree->started = nowSeconds();
    tree->root = std::make_unique<Node>();
    Node* root = tree->root.get();
    root->name = d->opt.root;

    DirEntry rootEntry;
    if (auto ec = d->fs.stat(d->opt.root, rootEntry)) {
        root->kind = NodeKind::Directory;
        root->flags |= NodeFlag::Unreadable;
        d->errors = 1;
        tree->finished = nowSeconds();
        tree->finalize();
        return tree;
    }
    root->kind = rootEntry.kind;
    root->size = rootEntry.size;
    root->mtime = rootEntry.mtime;
    root->dev = rootEntry.dev;
    root->ino = rootEntry.ino;
    d->rootDev = rootEntry.dev;

    if (root->isDir()) {
        d->queue.push_back({root, d->opt.root});
        unsigned n = d->opt.threads ? d->opt.threads : std::thread::hardware_concurrency();
        n = std::clamp(n, 1u, 16u);
        std::vector<std::thread> pool;
        pool.reserve(n);
        for (unsigned i = 0; i < n; ++i) pool.emplace_back([this] { d->worker(); });
        for (auto& t : pool) t.join();
    } else if (root->kind == NodeKind::File) {
        d->files = 1;
        d->bytes = root->size;
    }

    tree->complete = !d->cancelFlag.load();
    tree->finished = nowSeconds();
    tree->finalize();
    return tree;
}

}  // namespace storage
