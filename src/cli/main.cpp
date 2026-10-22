// storagectl — headless front end to the Storage core. Every command supports
// --Json so scripts, SSH sessions and AI agents can drive it.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <io.h>
#define STORAGE_ISATTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define STORAGE_ISATTY(f) isatty(fileno(f))
#endif

#include "storage/core/format.hpp"
#include "storage/core/index.hpp"
#include "storage/core/json.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"
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
    bool json = false;
};

std::filesystem::path dbPath(const Globals& g) {
    return g.db.empty() ? defaultIndexPath() : fromUtf8(g.db);
}

std::unique_ptr<ScanTree> runScan(const std::string& root, unsigned threads, bool crossMounts, bool quiet) {
    LocalFileSystem fs;
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
    const std::string root = normalizeLocalPath(p.positional[0]);
    auto tree = runScan(root, static_cast<unsigned>(p.number("threads", 0)), p.has("cross-mounts"), g.json);
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
    const std::string target = normalizeLocalPath(p.positional[0]);
    const int depth = static_cast<int>(p.number("depth", 1));
    const size_t top = static_cast<size_t>(std::max(1LL, p.number("top", 20)));

    std::unique_ptr<ScanTree> tree;
    const Node* node = nullptr;
    if (p.has("indexed")) {
        Index index(dbPath(g));
        for (const auto& s : index.scans()) {
            if (s.volume != "local" || target.rfind(s.root, 0) != 0) continue;  // scan must contain target
            tree = index.load(s.id);
            if (tree && (node = tree->find(target))) break;
            tree.reset();
        }
        if (!node) throw std::runtime_error("no indexed scan contains " + target + " (run `storagectl scan` first)");
    } else {
        tree = runScan(target, static_cast<unsigned>(p.number("threads", 0)), p.has("cross-mounts"), g.json);
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

}  // namespace

int main(int argc, char** argv) {
    cli::Parser parser("storagectl", "storagectl — index, deduplicate, organize and clean disks");
    parser.global()
        .option("db", 0, "PATH", "Index database (default: per-user app data directory)")
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
    g.json = p.has("json");

    try {
        if (p.command == "scan") return cmdScan(p, g);
        if (p.command == "du") return cmdDu(p, g);
        if (p.command == "scans") return cmdScans(p, g);
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
