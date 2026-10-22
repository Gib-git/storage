#include "storage/core/organize.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <atomic>
#include <mutex>
#include <regex>
#include <thread>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "storage/core/exif.hpp"

namespace storage {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return {};
    return lower(name.substr(dot + 1));
}

std::string stemOf(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return name;
    return name.substr(0, dot);
}

const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                         "July",    "August",   "September", "October", "November", "December"};

std::string two(int v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d", v);
    return b;
}

Category categoryFromName(const std::string& s) {
    for (int i = 0; i <= static_cast<int>(Category::Other); ++i)
        if (lower(categoryName(static_cast<Category>(i))) == lower(s)) return static_cast<Category>(i);
    throw std::invalid_argument("unknown file type '" + s + "'");
}

json::Value strings(const std::vector<std::string>& v) {
    json::Value a = json::Value::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

std::vector<std::string> readStrings(const json::Value& v) {
    std::vector<std::string> out;
    for (const auto& s : v.items()) out.push_back(s.asString());
    return out;
}

}  // namespace

// --- Safety helpers ------------------------------------------------------------------

std::string sanitizeSegment(const std::string& in) {
    std::string s;
    for (unsigned char c : in) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            s.push_back('_');
        else
            s.push_back(static_cast<char>(c));
    }
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.empty() || s == "." || s == "..") return "_";
    static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
                                     "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
                                     "lpt6", "lpt7", "lpt8", "lpt9"};
    const std::string l = lower(stemOf(s));
    for (const char* r : reserved)
        if (l == r) return "_" + s;
    if (s.size() > 200) s.resize(200);  // leave room for parents and " (99)" suffixes
    return s;
}

const char* junkFileNames() { return ".DS_Store, Thumbs.db, desktop.ini"; }

bool isJunkFile(const std::string& name) {
    const std::string l = lower(name);
    return l == ".ds_store" || l == "thumbs.db" || l == "desktop.ini" || l == "._.ds_store";
}

bool isPackageName(const std::string& name) {
    static const std::unordered_set<std::string> names = {".git", ".svn", ".hg", "node_modules", "__pycache__",
                                                          ".venv", "venv", ".idea", ".vscode"};
    static const std::unordered_set<std::string> exts = {
        "app",       "bundle", "framework", "plugin", "kext",    "photoslibrary", "photolibrary", "aplibrary",
        "fcpbundle", "logicx", "band",      "xcodeproj", "xcworkspace", "imovielibrary", "tvlibrary", "musiclibrary",
        "lrdata",    "rtfd",   "pages",     "numbers", "key",     "sparsebundle", "vmwarevm", "pvm", "utm", "docarchive"};
    if (names.count(lower(name))) return true;
    return exts.count(extensionOf(name)) > 0;
}

// --- RuleSet JSON -------------------------------------------------------------------

json::Value RuleSet::toJson() const {
    json::Value rs = json::Value::array();
    for (const auto& r : rules) {
        json::Value m = json::Value::object();
        if (!r.match.categories.empty()) {
            json::Value c = json::Value::array();
            for (Category cat : r.match.categories) c.push_back(categoryName(cat));
            m["types"] = c;
        }
        if (!r.match.extensions.empty()) m["extensions"] = strings(r.match.extensions);
        if (!r.match.nameContains.empty()) m["nameContains"] = r.match.nameContains;
        if (!r.match.nameRegex.empty()) m["nameRegex"] = r.match.nameRegex;
        if (r.match.minSize) m["minSize"] = r.match.minSize;
        if (r.match.maxSize) m["maxSize"] = r.match.maxSize;
        if (r.match.olderThanDays) m["olderThanDays"] = r.match.olderThanDays;
        if (r.match.newerThanDays) m["newerThanDays"] = r.match.newerThanDays;
        json::Value o = json::Value::object({{"name", r.name}, {"match", m}});
        if (r.leave) o["leave"] = true;
        else o["folder"] = r.folder;
        if (!r.rename.empty()) o["rename"] = r.rename;
        rs.push_back(o);
    }
    return json::Value::object({
        {"name", name},
        {"description", description},
        {"rules", rs},
        {"fallbackFolder", fallbackFolder},
        {"dateSource", dateSource == DateSource::Modified ? "modified" : "capture"},
        {"conflict", conflict == ConflictPolicy::Skip ? "skip" : "rename"},
        {"includeSubfolders", includeSubfolders},
        {"removeEmptyFolders", removeEmptyFolders},
        {"includeHidden", includeHidden},
    });
}

RuleSet RuleSet::fromJson(const json::Value& v) {
    if (!v.isObject()) throw std::invalid_argument("rules must be a JSON object");
    RuleSet s;
    s.name = v.get("name").asString("Custom");
    s.description = v.get("description").asString();
    s.fallbackFolder = v.get("fallbackFolder").asString();
    s.dateSource = v.get("dateSource").asString("capture") == "modified" ? DateSource::Modified
                                                                          : DateSource::CaptureThenModified;
    s.conflict = v.get("conflict").asString("rename") == "skip" ? ConflictPolicy::Skip : ConflictPolicy::Rename;
    s.includeSubfolders = v.get("includeSubfolders").asBool(true);
    s.removeEmptyFolders = v.get("removeEmptyFolders").asBool(true);
    s.includeHidden = v.get("includeHidden").asBool(false);
    for (const auto& r : v.get("rules").items()) {
        OrganizeRule rule;
        rule.name = r.get("name").asString("Rule");
        rule.folder = r.get("folder").asString();
        rule.rename = r.get("rename").asString();
        rule.leave = r.get("leave").asBool(false);
        const auto& m = r.get("match");
        for (const auto& t : m.get("types").items()) rule.match.categories.push_back(categoryFromName(t.asString()));
        for (auto e : readStrings(m.get("extensions"))) {
            e = lower(e);
            if (!e.empty() && e[0] == '.') e.erase(0, 1);
            rule.match.extensions.push_back(e);
        }
        rule.match.nameContains = m.get("nameContains").asString();
        rule.match.nameRegex = m.get("nameRegex").asString();
        if (!rule.match.nameRegex.empty()) {
            try {
                std::regex test(rule.match.nameRegex);
            } catch (const std::regex_error&) {
                throw std::invalid_argument("rule '" + rule.name + "': invalid regular expression");
            }
        }
        rule.match.minSize = static_cast<uint64_t>(m.get("minSize").asInt(0));
        rule.match.maxSize = static_cast<uint64_t>(m.get("maxSize").asInt(0));
        rule.match.olderThanDays = static_cast<int>(m.get("olderThanDays").asInt(0));
        rule.match.newerThanDays = static_cast<int>(m.get("newerThanDays").asInt(0));
        if (!rule.leave && rule.folder.empty() && rule.rename.empty())
            throw std::invalid_argument("rule '" + rule.name + "' needs a folder, a rename, or \"leave\": true");
        s.rules.push_back(std::move(rule));
    }
    return s;
}

std::vector<RuleSet> builtinPresets() {
    auto rule = [](std::string name, std::vector<Category> cats, std::string folder) {
        OrganizeRule r;
        r.name = std::move(name);
        r.match.categories = std::move(cats);
        r.folder = std::move(folder);
        return r;
    };
    using C = Category;
    std::vector<RuleSet> out;

    RuleSet byType;
    byType.name = "By file type";
    byType.description = "Images, Videos, Documents, ... one folder per kind of file.";
    byType.fallbackFolder = "{type}";
    out.push_back(byType);

    RuleSet typeYear;
    typeYear.name = "By type, then year";
    typeYear.description = "Images/2024, Documents/2023, ... (photos use the date they were taken).";
    typeYear.fallbackFolder = "{type}/{year}";
    out.push_back(typeYear);

    RuleSet media;
    media.name = "Photos & videos by date";
    media.description = "Photos and videos into Year/Year-Month folders; everything else stays put.";
    media.rules = {rule("Photos", {C::Image}, "Photos/{year}/{year}-{month}"),
                   rule("Videos", {C::Video}, "Videos/{year}/{year}-{month}")};
    out.push_back(media);

    RuleSet yearMonth;
    yearMonth.name = "By year and month";
    yearMonth.description = "2024/03 - March, ... for everything.";
    yearMonth.fallbackFolder = "{year}/{month} - {monthname}";
    out.push_back(yearMonth);

    RuleSet byExt;
    byExt.name = "By extension";
    byExt.description = "PDF, JPG, ZIP, ... one folder per file extension.";
    byExt.fallbackFolder = "{EXT}";
    out.push_back(byExt);

    RuleSet downloads;
    downloads.name = "Tidy Downloads";
    downloads.description = "Installers, archives, documents, media and other files into their own folders.";
    downloads.includeSubfolders = false;
    downloads.removeEmptyFolders = false;
    downloads.rules = {rule("Installers", {C::Application}, "Installers"), rule("Archives", {C::Archive}, "Archives"),
                       rule("Documents", {C::Document}, "Documents"),     rule("Images", {C::Image}, "Images"),
                       rule("Videos", {C::Video}, "Videos"),              rule("Audio", {C::Audio}, "Audio"),
                       rule("Code", {C::Code}, "Code")};
    downloads.fallbackFolder = "Other";
    out.push_back(downloads);
    return out;
}

// --- Planning ---------------------------------------------------------------------

Organizer::Organizer(FileSystem& fs, RuleSet rules) : fs_(fs), rules_(std::move(rules)) {
    for (const auto& r : rules_.rules)
        regexes_.emplace_back(r.match.nameRegex.empty() ? std::string(".*") : r.match.nameRegex,
                              std::regex::ECMAScript | std::regex::icase);
    now_ = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

const OrganizeRule* Organizer::ruleFor(const Node& f) const {
    const std::string ext = extensionOf(f.name);
    const std::string lname = lower(f.name);
    const Category cat = categorize(f.name);
    for (size_t i = 0; i < rules_.rules.size(); ++i) {
        const auto& r = rules_.rules[i];
        const auto& m = r.match;
        if (!m.categories.empty() && std::find(m.categories.begin(), m.categories.end(), cat) == m.categories.end()) continue;
        if (!m.extensions.empty() && std::find(m.extensions.begin(), m.extensions.end(), ext) == m.extensions.end()) continue;
        if (!m.nameContains.empty() && lname.find(lower(m.nameContains)) == std::string::npos) continue;
        if (m.minSize && f.size < m.minSize) continue;
        if (m.maxSize && f.size > m.maxSize) continue;
        const int64_t age = (now_ - f.mtime) / 86400;
        if (m.olderThanDays && age < m.olderThanDays) continue;
        if (m.newerThanDays && age > m.newerThanDays) continue;
        if (!m.nameRegex.empty() && !std::regex_search(f.name, regexes_[i])) continue;
        return &r;
    }
    return nullptr;
}

bool Organizer::usesDates() const {
    auto has = [](const std::string& t) {
        return t.find("{year}") != std::string::npos || t.find("{month") != std::string::npos ||
               t.find("{day}") != std::string::npos;
    };
    if (has(rules_.fallbackFolder)) return true;
    for (const auto& r : rules_.rules)
        if (has(r.folder) || has(r.rename)) return true;
    return false;
}

// Opening each photo costs ~0.1-0.2 ms of latency, so read their EXIF dates on a pool.
void Organizer::prefetchCaptureDates(const std::vector<const Node*>& files) {
    if (rules_.dateSource != DateSource::CaptureThenModified || !usesDates()) return;
    std::vector<const Node*> images;
    for (const Node* f : files)
        if (categorize(f->name) == Category::Image) images.push_back(f);
    if (images.empty()) return;

    const char sep = fs_.separator();
    std::vector<CaptureDate> dates(images.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
        std::string head;
        for (size_t i; (i = next.fetch_add(1)) < images.size();) {
            const std::string path = images[i]->path(sep);
            // The date is almost always within the first few KB; read more only when needed.
            if (fs_.readHead(path, 16 * 1024, head)) continue;
            CaptureDate d = readCaptureDate(head);
            if (!d.valid() && head.size() == 16 * 1024 && !fs_.readHead(path, 128 * 1024, head)) d = readCaptureDate(head);
            dates[i] = d;
        }
    };
    const unsigned n = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < n; ++k) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    for (size_t i = 0; i < images.size(); ++i)
        if (dates[i].valid()) captureDates_[images[i]] = dates[i];
}

std::string Organizer::dateFor(const Node& f, const std::string& path, char token) {
    if (datePath_ != path) {
        datePath_ = path;
        dateY_ = dateM_ = dateD_ = 0;
        if (rules_.dateSource == DateSource::CaptureThenModified && categorize(f.name) == Category::Image) {
            CaptureDate d;
            if (auto it = captureDates_.find(&f); it != captureDates_.end()) d = it->second;
            if (d.valid()) {
                dateY_ = d.year;
                dateM_ = d.month;
                dateD_ = d.day;
            }
        }
        if (!dateY_) {
            const std::time_t t = static_cast<std::time_t>(f.mtime);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            dateY_ = tm.tm_year + 1900;
            dateM_ = tm.tm_mon + 1;
            dateD_ = tm.tm_mday;
        }
    }
    switch (token) {
        case 'y': return std::to_string(dateY_);
        case 'm': return two(dateM_);
        case 'd': return two(dateD_);
        default: return kMonths[std::clamp(dateM_, 1, 12) - 1];
    }
}

std::string Organizer::expand(const std::string& tmpl, const Node& f, const std::string& scopePath) {
    const std::string path = f.path(fs_.separator());
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] != '{') {
            out.push_back(tmpl[i++]);
            continue;
        }
        const size_t close = tmpl.find('}', i);
        if (close == std::string::npos) {
            out.append(tmpl, i, std::string::npos);
            break;
        }
        const std::string token = tmpl.substr(i + 1, close - i - 1);
        std::string v;
        const std::string ext = extensionOf(f.name);
        if (token == "type") v = categoryName(categorize(f.name));
        else if (token == "ext") v = ext.empty() ? "no-extension" : ext;
        else if (token == "EXT") {
            v = ext.empty() ? "NO-EXTENSION" : ext;
            for (auto& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else if (token == "name") v = stemOf(f.name);
        else if (token == "filename") v = f.name;
        else if (token == "year") v = dateFor(f, path, 'y');
        else if (token == "month") v = dateFor(f, path, 'm');
        else if (token == "day") v = dateFor(f, path, 'd');
        else if (token == "monthname") v = dateFor(f, path, 'n');
        else if (token == "size") {
            const uint64_t mb = f.size / (1024 * 1024);
            v = mb < 1 ? "Small (under 1 MB)" : mb < 100 ? "Medium (1-100 MB)" : mb < 1024 ? "Large (100 MB-1 GB)" : "Huge (over 1 GB)";
        } else if (token == "first") {
            const unsigned char c = f.name.empty() ? '_' : static_cast<unsigned char>(f.name[0]);
            v = std::isalpha(c) ? std::string(1, static_cast<char>(std::toupper(c))) : std::isdigit(c) ? "0-9" : "#";
        } else if (token == "parent") {
            v = f.parent && f.parent->parent ? f.parent->name : sanitizeSegment(scopePath);
        } else {
            v = "{" + token + "}";  // unknown tokens are left visible so the preview shows the typo
        }
        // Token values never introduce folder levels.
        for (auto& c : v)
            if (c == '/' || c == '\\') c = '_';
        out += v;
        i = close + 1;
    }
    return out;
}

OrganizePlan Organizer::plan(const Node& scope, const std::string& destination) {
    OrganizePlan plan;
    const char sep = fs_.separator();
    plan.scope = scope.path(sep);
    plan.destination = destination.empty() ? plan.scope : destination;

    // Paths that are already taken (lower-cased: macOS and Windows are case-insensitive by default).
    std::unordered_set<std::string> taken;
    std::unordered_map<std::string, std::string> destDirs;  // key -> path: every target folder and its ancestors
    std::unordered_map<const Node*, bool> moved;

    auto keyOf = [](const std::string& p) { return lower(p); };

    // Nothing can already exist inside a folder that does not exist, so check each
    // destination folder once instead of stat()ing every planned file.
    std::unordered_map<std::string, bool> dirExists;
    auto existsOnDisk = [&](const std::string& dir, const std::string& file) {
        auto it = dirExists.find(dir);
        if (it == dirExists.end()) {
            DirEntry e;
            it = dirExists.emplace(dir, !fs_.stat(dir, e)).first;
        }
        if (!it->second) return false;
        DirEntry e;
        return !fs_.stat(file, e);
    };

    std::vector<const Node*> files;
    std::vector<const Node*> dirsPostOrder;
    std::vector<std::pair<const Node*, bool>> walk{{&scope, false}};
    while (!walk.empty()) {
        auto [n, visited] = walk.back();
        walk.pop_back();
        if (!n->isDir()) continue;
        if (visited) {
            dirsPostOrder.push_back(n);
            continue;
        }
        walk.emplace_back(n, true);
        for (const auto& c : n->children) {
            if (c->isDir()) {
                if (!rules_.includeSubfolders) continue;
                if (isPackageName(c->name) || (c->flags & (NodeFlag::Excluded | NodeFlag::Unreadable))) {
                    plan.protectedSkipped += c->files;
                    continue;
                }
                if (!rules_.includeHidden && !c->name.empty() && c->name[0] == '.') continue;
                walk.emplace_back(c.get(), false);
            } else if (c->kind == NodeKind::File) {
                files.push_back(c.get());
            }
        }
    }

    prefetchCaptureDates(files);

    // Existing files count as taken unless they will move away.
    for (const Node* f : files) taken.insert(keyOf(f->path(sep)));

    for (const Node* f : files) {
        if (f->name.size() >= 12 && f->name.compare(f->name.size() - 12, 12, FileSystem::kTempSuffix) == 0) continue;
        if (isJunkFile(f->name)) continue;
        if (!rules_.includeHidden && !f->name.empty() && f->name[0] == '.') continue;

        const OrganizeRule* rule = ruleFor(*f);
        std::string folderTmpl, renameTmpl, ruleName;
        if (rule) {
            if (rule->leave) {
                ++plan.alreadyInPlace;
                continue;
            }
            folderTmpl = rule->folder;
            renameTmpl = rule->rename;
            ruleName = rule->name;
        } else if (!rules_.fallbackFolder.empty()) {
            folderTmpl = rules_.fallbackFolder;
            ruleName = "Everything else";
        } else {
            ++plan.unmatched;
            continue;
        }

        // Build the destination path segment by segment.
        std::string dir = plan.destination;
        const std::string expanded = expand(folderTmpl, *f, plan.scope);
        size_t start = 0;
        while (start <= expanded.size()) {
            const size_t slash = expanded.find('/', start);
            const std::string seg = expanded.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (!seg.empty()) dir = fs_.join(dir, sanitizeSegment(seg));
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        std::string filename = renameTmpl.empty() ? f->name : sanitizeSegment(expand(renameTmpl, *f, plan.scope));
        if (!renameTmpl.empty() && extensionOf(filename).empty() && !extensionOf(f->name).empty())
            filename += f->name.substr(f->name.rfind('.'));  // keep the extension unless the template set one

        const std::string src = f->path(sep);
        std::string dst = fs_.join(dir, filename);
        if (keyOf(dst) == keyOf(src)) {
            ++plan.alreadyInPlace;
            continue;
        }

        bool renamed = false;
        if (taken.count(keyOf(dst)) || existsOnDisk(dir, dst)) {
            if (rules_.conflict == ConflictPolicy::Skip) {
                ++plan.conflictsSkipped;
                continue;
            }
            const std::string stem = stemOf(filename);
            const std::string ext = filename.size() > stem.size() ? filename.substr(stem.size()) : std::string();
            for (int k = 2;; ++k) {
                const std::string candidate = fs_.join(dir, stem + " (" + std::to_string(k) + ")" + ext);
                if (!taken.count(keyOf(candidate)) && !existsOnDisk(dir, candidate)) {
                    dst = candidate;
                    break;
                }
            }
            renamed = true;
        }
        taken.insert(keyOf(dst));
        moved[f] = true;
        for (std::string d = dir; !d.empty() && destDirs.emplace(keyOf(d), d).second;) {
            const std::string up = fs_.parentOf(d);
            if (up == d) break;
            d = up;
        }
        plan.moves.push_back({src, dst, ruleName, f->size, f->mtime, renamed});
        plan.bytes += f->size;
    }

    // Destination folders that must be created (recorded so undo can remove them again).
    for (const auto& [key, path] : destDirs) {
        auto it = dirExists.find(path);
        DirEntry e;
        if (it != dirExists.end() ? !it->second : static_cast<bool>(fs_.stat(path, e))) plan.createDirs.push_back(path);
    }
    std::sort(plan.createDirs.begin(), plan.createDirs.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });

    // Folders left with nothing but junk once their files move out.
    if (rules_.removeEmptyFolders) {
        std::unordered_map<const Node*, bool> empty;
        for (const Node* d : dirsPostOrder) {
            bool isEmpty = d != &scope;
            for (const auto& c : d->children) {
                if (c->isDir()) isEmpty &= empty.count(c.get()) && empty[c.get()];
                else if (!moved.count(c.get()) && !isJunkFile(c->name)) isEmpty = false;
                if (!isEmpty) break;
            }
            const std::string path = d->path(sep);
            if (destDirs.count(keyOf(path))) isEmpty = false;
            empty[d] = isEmpty;
            if (isEmpty) plan.removeDirs.push_back(path);
        }
    }
    return plan;
}

std::vector<JournalOp> OrganizePlan::toOps() const {
    std::vector<JournalOp> ops;
    ops.reserve(createDirs.size() + moves.size() + removeDirs.size());
    for (const auto& d : createDirs) {
        JournalOp op;
        op.action = Action::MakeDir;
        op.src = d;
        ops.push_back(std::move(op));
    }
    for (const auto& m : moves) {
        JournalOp op;
        op.action = Action::Move;
        op.src = m.src;
        op.dst = m.dst;
        op.size = m.size;
        op.mtime = m.mtime;
        ops.push_back(std::move(op));
    }
    for (const auto& d : removeDirs) {
        JournalOp op;
        op.action = Action::RemoveDir;
        op.src = d;
        ops.push_back(std::move(op));
    }
    return ops;
}

}  // namespace storage
