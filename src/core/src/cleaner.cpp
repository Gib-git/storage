#include "storage/core/cleaner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "storage/core/organize.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"

namespace storage {

namespace {

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string env(const std::string& name) {
#ifdef _WIN32
    const std::wstring w(name.begin(), name.end());
    const wchar_t* v = _wgetenv(w.c_str());
    return v ? toUtf8(std::filesystem::path(v)) : std::string();
#else
    const char* v = std::getenv(name.c_str());
    return v ? std::string(v) : std::string();
#endif
}

CleanTarget target(std::string id, std::string group, std::string name, std::string description,
                   std::vector<std::string> paths, Safety safety, bool defaultOn, int minAgeDays = 0,
                   std::string hint = {}) {
    CleanTarget t;
    t.id = std::move(id);
    t.group = std::move(group);
    t.name = std::move(name);
    t.description = std::move(description);
    t.paths = std::move(paths);
    t.safety = safety;
    t.defaultOn = defaultOn;
    t.minAgeDays = minAgeDays;
    t.hint = std::move(hint);
    return t;
}

}  // namespace

const char* safetyName(Safety s) {
    switch (s) {
        case Safety::Safe: return "Safe";
        case Safety::Review: return "Review";
        case Safety::Caution: return "Caution";
    }
    return "Review";
}

std::string expandUserPath(const std::string& in) {
    std::string s = in;
    if (!s.empty() && s[0] == '~') {
#ifdef _WIN32
        s = env("USERPROFILE") + s.substr(1);
#else
        s = env("HOME") + s.substr(1);
#endif
    }
    // %VAR% (Windows style; harmless elsewhere)
    for (size_t a = s.find('%'); a != std::string::npos; a = s.find('%', a)) {
        const size_t b = s.find('%', a + 1);
        if (b == std::string::npos) break;
        const std::string value = env(s.substr(a + 1, b - a - 1));
        if (value.empty()) return {};  // unresolved: target does not apply
        s.replace(a, b - a + 1, value);
        a += value.size();
    }
#ifdef _WIN32
    std::replace(s.begin(), s.end(), '/', '\\');
#endif
    return s;
}

std::vector<CleanTarget> cleanCatalogue() {
    using S = Safety;
    std::vector<CleanTarget> t;
#if defined(__APPLE__)
    t.push_back(target("mac.caches", "System", "App caches", "Files apps store to start faster. They are rebuilt automatically.",
                       {"~/Library/Caches/*"}, S::Safe, true, 0, "Quit open apps first for the best result."));
    t.push_back(target("mac.logs", "System", "Logs", "Diagnostic logs written by apps.", {"~/Library/Logs/*"}, S::Safe, true));
    t.push_back(target("mac.mail-downloads", "Apps", "Mail attachments cache",
                       "Copies of attachments you opened in Mail; the originals stay in your mail.",
                       {"~/Library/Containers/com.apple.mail/Data/Library/Mail Downloads/*"}, S::Safe, true));
    t.push_back(target("mac.trash", "System", "Trash", "Items you already moved to the Trash.", {"~/.Trash/*"},
                       S::Caution, false));
    t.back().permanentOnly = true;
    t.push_back(target("mac.ios-backups", "Apps", "iPhone & iPad backups",
                       "Local device backups. Make sure you have another backup (e.g. iCloud) first.",
                       {"~/Library/Application Support/MobileSync/Backup/*"}, S::Caution, false));
    t.push_back(target("xcode.derived", "Developer", "Xcode DerivedData", "Build products and indexes; rebuilt on next build.",
                       {"~/Library/Developer/Xcode/DerivedData/*"}, S::Safe, true, 0, "Quit Xcode first."));
    t.push_back(target("xcode.devicesupport", "Developer", "Xcode device support",
                       "Debug symbols for iOS versions; downloaded again when a device connects.",
                       {"~/Library/Developer/Xcode/iOS DeviceSupport/*", "~/Library/Developer/Xcode/watchOS DeviceSupport/*"},
                       S::Safe, false, 90));
    t.push_back(target("xcode.archives", "Developer", "Xcode archives", "Archived app builds (needed to symbolicate old crash reports).",
                       {"~/Library/Developer/Xcode/Archives/*"}, S::Review, false, 180));
    t.push_back(target("xcode.simcaches", "Developer", "Simulator caches", "Caches for iOS simulators.",
                       {"~/Library/Developer/CoreSimulator/Caches/*"}, S::Safe, true));
    t.push_back(target("dev.npm", "Developer", "npm cache", "Downloaded packages; fetched again when needed.", {"~/.npm/_cacache/*"},
                       S::Safe, true));
    t.push_back(target("dev.gradle", "Developer", "Gradle caches", "Downloaded dependencies and build caches.",
                       {"~/.gradle/caches/*"}, S::Review, false));
    t.push_back(target("dev.cargo", "Developer", "Cargo download cache", "Downloaded crate archives.", {"~/.cargo/registry/cache/*"},
                       S::Safe, true));
    t.push_back(target("downloads.old", "Apps", "Old downloads", "Files in Downloads you haven't touched in 6 months.",
                       {"~/Downloads/*"}, S::Review, false, 180));
#elif defined(_WIN32)
    t.push_back(target("win.temp", "System", "Temporary files", "Leftovers apps and installers did not clean up.",
                       {"%TEMP%/*"}, S::Safe, true, 1, "Files in use are skipped automatically."));
    t.push_back(target("win.systemp", "System", "Windows temporary files", "System-wide temporary files (needs admin rights).",
                       {"%WINDIR%/Temp/*"}, S::Safe, false, 1));
    t.push_back(target("win.crashdumps", "System", "Crash dumps", "Memory dumps written when apps crashed.",
                       {"%LOCALAPPDATA%/CrashDumps/*"}, S::Safe, true));
    t.push_back(target("win.inetcache", "Browsers", "Internet cache", "Cached web content used by Windows components.",
                       {"%LOCALAPPDATA%/Microsoft/Windows/INetCache/*"}, S::Safe, true));
    t.push_back(target("win.thumbs", "System", "Thumbnail cache", "Explorer rebuilds thumbnails when needed.",
                       {"%LOCALAPPDATA%/Microsoft/Windows/Explorer/thumbcache_*.db"}, S::Safe, false));
    t.push_back(target("browser.chrome", "Browsers", "Chrome cache", "Cached web pages and images.",
                       {"%LOCALAPPDATA%/Google/Chrome/User Data/Default/Cache/*"}, S::Safe, true, 0, "Close Chrome first."));
    t.push_back(target("browser.edge", "Browsers", "Edge cache", "Cached web pages and images.",
                       {"%LOCALAPPDATA%/Microsoft/Edge/User Data/Default/Cache/*"}, S::Safe, true, 0, "Close Edge first."));
    t.push_back(target("dev.npm", "Developer", "npm cache", "Downloaded packages; fetched again when needed.",
                       {"%LOCALAPPDATA%/npm-cache/_cacache/*"}, S::Safe, true));
    t.push_back(target("dev.pip", "Developer", "pip cache", "Downloaded Python packages.", {"%LOCALAPPDATA%/pip/Cache/*"}, S::Safe, true));
    t.push_back(target("dev.gradle", "Developer", "Gradle caches", "Downloaded dependencies and build caches.",
                       {"~/.gradle/caches/*"}, S::Review, false));
    t.push_back(target("downloads.old", "Apps", "Old downloads", "Files in Downloads you haven't touched in 6 months.",
                       {"~/Downloads/*"}, S::Review, false, 180));
#else
    t.push_back(target("linux.cache", "System", "App caches", "Files apps store to start faster (~/.cache). Rebuilt automatically.",
                       {"~/.cache/*"}, S::Safe, true, 0, "Close open apps first for the best result."));
    t.push_back(target("linux.thumbs", "System", "Thumbnails", "Image previews; regenerated when needed.", {"~/.thumbnails/*"},
                       S::Safe, true));
    t.push_back(target("linux.trash", "System", "Trash", "Items you already moved to the Trash.",
                       {"~/.local/share/Trash/files/*", "~/.local/share/Trash/info/*"}, S::Caution, false));
    t.back().permanentOnly = true;
    t.push_back(target("dev.npm", "Developer", "npm cache", "Downloaded packages; fetched again when needed.", {"~/.npm/_cacache/*"},
                       S::Safe, true));
    t.push_back(target("dev.gradle", "Developer", "Gradle caches", "Downloaded dependencies and build caches.",
                       {"~/.gradle/caches/*"}, S::Review, false));
    t.push_back(target("dev.cargo", "Developer", "Cargo download cache", "Downloaded crate archives.", {"~/.cargo/registry/cache/*"},
                       S::Safe, true));
    t.push_back(target("downloads.old", "Apps", "Old downloads", "Files in Downloads you haven't touched in 6 months.",
                       {"~/Downloads/*"}, S::Review, false, 180));
#endif
    return t;
}

std::vector<CleanCommand> cleanCommands() {
    std::vector<CleanCommand> c;
#if defined(__APPLE__)
    c.push_back({"Homebrew", "brew cleanup --prune=all", "Remove old versions and cached downloads of Homebrew packages."});
    c.push_back({"Docker", "docker system prune", "Remove stopped containers, unused networks and dangling images."});
    c.push_back({"Time Machine local snapshots", "tmutil listlocalsnapshots /",
                 "List local snapshots; macOS removes them automatically when space runs low."});
    c.push_back({"Xcode simulators", "xcrun simctl delete unavailable", "Delete simulators for runtimes no longer installed."});
#elif defined(_WIN32)
    c.push_back({"Windows Update leftovers", "Dism.exe /online /Cleanup-Image /StartComponentCleanup",
                 "Remove superseded Windows component versions (run as administrator)."});
    c.push_back({"Disk Cleanup", "cleanmgr /lowdisk", "Windows' own cleanup tool, including system files."});
    c.push_back({"Docker", "docker system prune", "Remove stopped containers, unused networks and dangling images."});
#else
    c.push_back({"Package cache (apt)", "sudo apt-get clean", "Remove downloaded .deb packages."});
    c.push_back({"Unused packages (apt)", "sudo apt-get autoremove --purge", "Remove packages nothing depends on any more."});
    c.push_back({"System logs", "sudo journalctl --vacuum-time=2weeks", "Keep only the last two weeks of system logs."});
    c.push_back({"Old snap revisions", "snap list --all", "Disabled snap revisions can be removed with `sudo snap remove NAME --revision=N`."});
    c.push_back({"Unused Flatpak runtimes", "flatpak uninstall --unused", "Remove runtimes no installed app uses."});
    c.push_back({"Docker", "docker system prune", "Remove stopped containers, unused networks and dangling images."});
#endif
    return c;
}

namespace {

bool wildcardMatch(const std::string& pattern, const std::string& name) {
    // Only '*' is supported; enough for patterns like thumbcache_*.db.
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

}  // namespace

std::vector<CleanItem> analyzeCleanTargets(FileSystem& fs, const std::vector<CleanTarget>& targets,
                                           const std::atomic<bool>* cancel) {
    std::vector<CleanItem> out;
    const int64_t now = nowSeconds();
    for (const auto& t : targets) {
        CleanItem item;
        item.target = t;
        for (const auto& raw : t.paths) {
            if (cancel && cancel->load()) return out;
            const std::string expanded = expandUserPath(raw);
            if (expanded.empty()) continue;
            const size_t slash = expanded.find_last_of("/\\");
            if (slash == std::string::npos) continue;
            const std::string pattern = expanded.substr(slash + 1);
            const std::string dir = fs.parentOf(expanded);
            std::vector<DirEntry> children;
            if (fs.list(dir, children)) continue;  // not present on this machine
            for (const auto& c : children) {
                if (!wildcardMatch(pattern, c.name)) continue;
                const std::string path = fs.join(dir, c.name);
                CleanEntry e{path, c.size, c.mtime, c.kind == NodeKind::Directory};
                if (e.isDir) {
                    ScanOptions o = ScanOptions::defaultsFor(path);
                    o.threads = 4;
                    auto tree = Scanner(fs, o).run();
                    e.size = tree->root->size;
                    item.files += tree->root->files;
                    item.errors += tree->root->errors;
                    // Age of a folder = its most recently modified content.
                    int64_t newest = c.mtime;
                    std::vector<const Node*> stack{tree->root.get()};
                    while (!stack.empty()) {
                        const Node* n = stack.back();
                        stack.pop_back();
                        newest = std::max(newest, n->mtime);
                        for (const auto& k : n->children) stack.push_back(k.get());
                    }
                    e.mtime = newest;
                } else {
                    item.files += 1;
                }
                if (t.minAgeDays && now - e.mtime < int64_t(t.minAgeDays) * 86400) continue;
                if (c.kind == NodeKind::Symlink) e.size = 0;
                item.bytes += e.size;
                item.entries.push_back(std::move(e));
            }
        }
        std::sort(item.entries.begin(), item.entries.end(), [](const CleanEntry& a, const CleanEntry& b) { return a.size > b.size; });
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<JournalOp> planClean(const std::vector<CleanItem>& items, bool permanent) {
    std::vector<JournalOp> ops;
    for (const auto& item : items) {
        const bool del = item.target.permanentOnly || (permanent && item.target.safety == Safety::Safe);
        for (const auto& e : item.entries) {
            JournalOp op;
            op.action = del ? (e.isDir ? Action::DeleteTree : Action::Delete) : Action::Trash;
            op.src = e.path;
            op.size = e.size;
            op.mtime = e.isDir ? 0 : e.mtime;  // folders change as apps use them; only files are pinned
            op.detail = item.target.name;
            ops.push_back(std::move(op));
        }
    }
    return ops;
}

// --- Insights ----------------------------------------------------------------------

std::vector<Finding> findLargeOldFiles(const Node& root, char sep, uint64_t minSize, int olderThanDays, size_t limit) {
    std::vector<Finding> out;
    const int64_t cutoff = nowSeconds() - int64_t(olderThanDays) * 86400;
    std::vector<const Node*> stack{&root};
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        if (n->isDir()) {
            if (n != &root && isPackageName(n->name)) continue;
            for (const auto& c : n->children) stack.push_back(c.get());
        } else if (n->kind == NodeKind::File && n->size >= minSize && n->mtime <= cutoff &&
                   !(n->flags & NodeFlag::HardlinkRepeat)) {
            out.push_back({n->path(sep), n->size, n->mtime, false, "large and not modified recently"});
        }
    }
    std::sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.size > b.size; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<Finding> findEmptyFolders(const Node& root, char sep) {
    std::vector<Finding> out;
    std::unordered_map<const Node*, bool> empty;
    std::vector<std::pair<const Node*, bool>> walk{{&root, false}};
    while (!walk.empty()) {
        auto [n, visited] = walk.back();
        walk.pop_back();
        if (!n->isDir()) continue;
        if (!visited) {
            if (n != &root && (isPackageName(n->name) || (n->flags & (NodeFlag::Excluded | NodeFlag::Unreadable)))) {
                empty[n] = false;
                continue;
            }
            walk.emplace_back(n, true);
            for (const auto& c : n->children) walk.emplace_back(c.get(), false);
            continue;
        }
        bool e = true;
        for (const auto& c : n->children) {
            if (c->isDir()) e &= empty.count(c.get()) && empty[c.get()];
            else if (!isJunkFile(c->name)) e = false;
        }
        empty[n] = e;
        // Report only the top of each empty subtree (removing it removes the rest).
        if (e && n != &root) out.push_back({n->path(sep), 0, n->mtime, true, "empty folder"});
    }
    // Drop entries whose parent is also reported.
    std::unordered_set<std::string> reported;
    for (const auto& f : out) reported.insert(f.path);
    std::vector<Finding> top;
    for (const auto& f : out) {
        const size_t s = f.path.find_last_of("/\\");
        if (s != std::string::npos && reported.count(f.path.substr(0, s))) continue;
        top.push_back(f);
    }
    return top;
}

std::vector<Finding> findProjectJunk(const Node& root, char sep, int olderThanDays) {
    static const std::unordered_map<std::string, std::string> kinds = {
        {"node_modules", "Node.js packages (npm install restores them)"},
        {"target", "Rust/Maven build output"},
        {".venv", "Python virtual environment"},
        {"venv", "Python virtual environment"},
        {"__pycache__", "Python bytecode cache"},
        {".gradle", "Gradle project cache"},
        {"DerivedData", "Xcode build output"},
        {".next", "Next.js build output"},
        {".nuxt", "Nuxt build output"},
        {".parcel-cache", "Parcel cache"},
        {".terraform", "Terraform providers"},
    };
    const int64_t cutoff = nowSeconds() - int64_t(olderThanDays) * 86400;
    std::vector<Finding> out;

    // Newest modification time of anything in a subtree, ignoring the junk folders themselves.
    std::unordered_map<const Node*, int64_t> newest;
    std::vector<std::pair<const Node*, bool>> walk{{&root, false}};
    while (!walk.empty()) {
        auto [n, visited] = walk.back();
        walk.pop_back();
        if (!visited) {
            walk.emplace_back(n, true);
            for (const auto& c : n->children) walk.emplace_back(c.get(), false);
            continue;
        }
        int64_t t = n->mtime;
        for (const auto& c : n->children)
            if (!(c->isDir() && kinds.count(c->name))) t = std::max(t, newest[c.get()]);
        newest[n] = t;
    }

    std::vector<const Node*> stack{&root};
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        for (const auto& c : n->children) {
            if (!c->isDir()) continue;
            auto k = kinds.find(c->name);
            if (k != kinds.end()) {
                // "target"/"build"-style names only count inside something that looks like a project.
                const bool projectish = c->name != "target" || std::any_of(n->children.begin(), n->children.end(), [](const auto& s) {
                                            return s->name == "Cargo.toml" || s->name == "pom.xml";
                                        });
                if (projectish && newest[n] <= cutoff && c->size > 0)
                    out.push_back({c->path(sep), c->size, newest[n], true, k->second});
                continue;  // never descend into junk folders
            }
            if (!isPackageName(c->name) || c->name == "node_modules") stack.push_back(c.get());
        }
    }
    std::sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.size > b.size; });
    return out;
}

}  // namespace storage
