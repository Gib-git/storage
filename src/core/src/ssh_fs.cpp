#include "storage/core/ssh_fs.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <unordered_set>

#include "storage/core/paths.hpp"
#include "storage/core/process.hpp"

namespace storage {

namespace {

std::string q(const std::string& s) { return shellQuote(s); }

std::string randomToken() {
    std::random_device rd;
    std::mt19937_64 rng((uint64_t(rd()) << 32) ^ rd() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    static const char* hex = "0123456789abcdef";
    std::string t = "__STORAGE_";
    for (int i = 0; i < 24; ++i) t.push_back(hex[rng() & 15]);
    return t;
}

// Map a failed remote command's output to an errno-style error.
std::error_code remoteError(int code, const std::string& out) {
    if (code == 17 || out.find("File exists") != std::string::npos) return std::make_error_code(std::errc::file_exists);
    if (code == 18 || out.find("ross-device") != std::string::npos) return std::make_error_code(std::errc::cross_device_link);
    if (out.find("No such file") != std::string::npos) return std::make_error_code(std::errc::no_such_file_or_directory);
    if (out.find("ermission denied") != std::string::npos || out.find("not permitted") != std::string::npos)
        return std::make_error_code(std::errc::permission_denied);
    if (out.find("not empty") != std::string::npos) return std::make_error_code(std::errc::directory_not_empty);
    if (out.find("No space") != std::string::npos) return std::make_error_code(std::errc::no_space_on_device);
    return std::make_error_code(std::errc::io_error);
}

std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of('/');
    if (s == std::string::npos) return ".";
    return s == 0 ? "/" : p.substr(0, s);
}

std::string nameOf(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Parse one stat record "type|size|mtime|dev|ino|nlink|path" (separator `sep`).
bool parseRecord(const std::string& rec, char sep, const std::string& style, DirEntry& e, std::string& path) {
    size_t pos = 0;
    std::string f[6];
    for (int i = 0; i < 6; ++i) {
        const size_t n = rec.find(sep, pos);
        if (n == std::string::npos) return false;
        f[i] = rec.substr(pos, n - pos);
        pos = n + 1;
    }
    path = rec.substr(pos);
    const std::string& t = f[0];
    if (style == "find") {  // %y: f d l ...
        e.kind = t == "d" ? NodeKind::Directory : t == "f" ? NodeKind::File : t == "l" ? NodeKind::Symlink : NodeKind::Other;
    } else {  // GNU %F / BSD %HT: "regular file", "Directory", "Symbolic Link", ...
        std::string l = t;
        for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        e.kind = l.find("directory") != std::string::npos ? NodeKind::Directory
                 : l.find("regular") != std::string::npos ? NodeKind::File
                 : l.find("symbolic") != std::string::npos ? NodeKind::Symlink
                                                           : NodeKind::Other;
    }
    try {
        e.size = e.kind == NodeKind::File ? std::stoull(f[1]) : 0;
        e.mtime = static_cast<int64_t>(std::stod(f[2]));
        e.dev = std::stoull(f[3]);
        e.ino = std::stoull(f[4]);
        e.nlink = static_cast<uint32_t>(std::stoul(f[5]));
    } catch (const std::exception&) {
        return false;
    }
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    e.name = nameOf(path);
    return true;
}

}  // namespace

// --- Config ------------------------------------------------------------------------

std::string NasConfig::volume() const {
    std::string v = "ssh://";
    if (!user.empty()) v += user + "@";
    v += host;
    if (port) v += ":" + std::to_string(port);
    return v;
}

std::vector<std::string> NasConfig::command() const {
    if (!transport.empty()) return transport;
    std::vector<std::string> c = {"ssh", "-T",
                                  "-o", "BatchMode=yes",
                                  "-o", "ConnectTimeout=10",
                                  "-o", "ServerAliveInterval=15",
                                  "-o", "StrictHostKeyChecking=accept-new"};
    if (port) {
        c.push_back("-p");
        c.push_back(std::to_string(port));
    }
    if (!identityFile.empty()) {
        c.push_back("-i");
        c.push_back(identityFile);
    }
    c.push_back(user.empty() ? host : user + "@" + host);
    c.push_back("sh");
    return c;
}

json::Value NasConfig::toJson() const {
    json::Value v = json::Value::object({{"name", name}, {"host", host}, {"user", user}, {"port", port},
                                         {"identityFile", identityFile}, {"root", root}});
    if (!transport.empty()) {
        json::Value t = json::Value::array();
        for (const auto& a : transport) t.push_back(a);
        v["transport"] = t;
    }
    return v;
}

NasConfig NasConfig::fromJson(const json::Value& v) {
    NasConfig c;
    c.name = v.get("name").asString();
    c.host = v.get("host").asString();
    c.user = v.get("user").asString();
    c.port = static_cast<int>(v.get("port").asInt(0));
    c.identityFile = v.get("identityFile").asString();
    c.root = v.get("root").asString("/");
    for (const auto& a : v.get("transport").items()) c.transport.push_back(a.asString());
    if (c.name.empty()) c.name = c.host;
    return c;
}

std::vector<NasConfig> loadNasConfigs() {
    std::vector<NasConfig> out;
    std::ifstream f(appDataDir() / "nas.json", std::ios::binary);
    if (!f) return out;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        const json::Value all = json::Value::parse(ss.str());  // must outlive the loop below
        for (const auto& v : all.items()) out.push_back(NasConfig::fromJson(v));
    } catch (const std::exception&) {
    }
    return out;
}

void saveNasConfigs(const std::vector<NasConfig>& list) {
    json::Value a = json::Value::array();
    for (const auto& c : list) a.push_back(c.toJson());
    const auto path = appDataDir() / "nas.json";
    const auto tmp = appDataDir() / "nas.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << a.dump(2);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

// --- Session -----------------------------------------------------------------------

class SshFileSystem::Session {
public:
    explicit Session(std::vector<std::string> argv) : argv_(std::move(argv)), token_(randomToken()) {}

    std::error_code start(std::string& message) {
        if (auto ec = proc_.start(argv_)) {
            message = "could not start " + argv_.front() + ": " + ec.message();
            return ec;
        }
        std::string out;
        std::error_code ec;
        const int code = run("echo ok", [&](const char* p, size_t n) { out.append(p, n); }, ec);
        if (ec || code != 0 || out.find("ok") == std::string::npos) {
            message = out.empty() ? std::string("connection failed") : out;  // ssh's own error text
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
            proc_.wait(true);
            return ec ? ec : std::make_error_code(std::errc::connection_refused);
        }
        alive_ = true;
        return {};
    }

    bool alive() const { return alive_; }

    // Runs `command` in a subshell; streams its combined output to `sink`; returns the exit code.
    int run(const std::string& command, const std::function<void(const char*, size_t)>& sink, std::error_code& ec) {
        const std::string script = "( " + command + "\n) </dev/null 2>&1; printf '\\n%s %d\\n' '" + token_ + "' \"$?\"\n";
        if ((ec = proc_.write(script))) {
            alive_ = false;
            return -1;
        }
        const std::string marker = "\n" + token_ + " ";
        std::string buf;
        char chunk[65536];
        for (;;) {
            const size_t n = proc_.read(chunk, sizeof chunk, ec);
            if (n == 0) {
                alive_ = false;
                if (!ec) ec = std::make_error_code(std::errc::connection_aborted);
                if (!buf.empty()) sink(buf.data(), buf.size());
                return -1;
            }
            buf.append(chunk, n);
            const size_t m = buf.find(marker);
            if (m != std::string::npos) {
                const size_t eol = buf.find('\n', m + marker.size());
                if (eol == std::string::npos) continue;  // exit code still arriving
                if (m) sink(buf.data(), m);
                return std::atoi(buf.c_str() + m + marker.size());
            }
            // Deliver everything that cannot be the start of the marker.
            if (buf.size() > marker.size() + 16) {
                const size_t keep = marker.size() + 16;
                sink(buf.data(), buf.size() - keep);
                buf.erase(0, buf.size() - keep);
            }
        }
    }

private:
    std::vector<std::string> argv_;
    std::string token_;
    Process proc_;
    bool alive_ = false;
};

// --- SshFileSystem -------------------------------------------------------------------

SshFileSystem::SshFileSystem(NasConfig config, int sessions) : cfg_(std::move(config)), maxSessions_(std::max(1, sessions)) {}
SshFileSystem::~SshFileSystem() = default;

std::error_code SshFileSystem::connect(std::string& message) {
    std::lock_guard connectLock(connectMu_);
    if (connected_) {
        message = caps_.system;
        return {};
    }
    auto s = std::make_unique<Session>(cfg_.command());
    if (auto ec = s->start(message)) return ec;

    std::string out;
    std::error_code ec;
    s->run("uname -s; "
           "if find / -maxdepth 0 -printf '' >/dev/null 2>&1; then echo find=gnu; fi; "
           "if stat -c %s / >/dev/null 2>&1; then echo stat=gnu; elif stat -f %z / >/dev/null 2>&1; then echo stat=bsd; fi; "
           "for c in sha256sum sha256 shasum; do if command -v $c >/dev/null 2>&1; then echo sha=$c; break; fi; done; "
           "if cp --help 2>&1 | grep -q reflink; then echo reflink=1; fi",
           [&](const char* p, size_t n) { out.append(p, n); }, ec);
    if (ec) {
        message = "connected, but could not inspect the remote system";
        return ec;
    }
    RemoteCaps caps;
    std::istringstream lines(out);
    std::string line;
    bool first = true;
    while (std::getline(lines, line)) {
        if (first) {
            caps.system = line;
            first = false;
        } else if (line == "find=gnu") caps.gnuFind = true;
        else if (line.rfind("stat=", 0) == 0) caps.stat = line.substr(5);
        else if (line.rfind("sha=", 0) == 0) caps.sha = line.substr(4);
        else if (line == "reflink=1") caps.reflink = true;
    }
    if (!caps.gnuFind && caps.stat.empty()) {
        message = "the remote system has neither GNU find nor a usable stat";
        return std::make_error_code(std::errc::not_supported);
    }
    caps_ = caps;
    {
        std::lock_guard lk(mu_);
        idle_.push_back(s.get());
        sessions_.push_back(std::move(s));
    }
    connected_ = true;
    cv_.notify_one();
    message = caps_.system;
    return {};
}

SshFileSystem::Session* SshFileSystem::acquire() {
    if (!connected_) {
        std::string message;
        if (connect(message)) return nullptr;
    }
    std::unique_lock lk(mu_);
    for (;;) {
        // Drop dead sessions so a broken connection gets re-established.
        idle_.erase(std::remove_if(idle_.begin(), idle_.end(), [](Session* s) { return !s->alive(); }), idle_.end());
        if (!idle_.empty()) {
            Session* s = idle_.back();
            idle_.pop_back();
            return s;
        }
        size_t alive = 0;
        for (const auto& s : sessions_) alive += s->alive();
        if (alive < static_cast<size_t>(maxSessions_)) {
            auto s = std::make_unique<Session>(cfg_.command());
            lk.unlock();
            std::string msg;
            const bool ok = !s->start(msg);
            lk.lock();
            if (!ok) return nullptr;
            sessions_.push_back(std::move(s));
            return sessions_.back().get();
        }
        cv_.wait(lk);
    }
}

void SshFileSystem::release(Session* s) {
    {
        std::lock_guard lk(mu_);
        if (s->alive()) idle_.push_back(s);
    }
    cv_.notify_one();
}

int SshFileSystem::runStream(const std::string& command, const std::function<void(const char*, size_t)>& sink,
                             std::error_code& ec) {
    Session* s = acquire();
    if (!s) {
        ec = std::make_error_code(std::errc::connection_refused);
        return -1;
    }
    const int code = s->run(command, sink, ec);
    release(s);
    return code;
}

int SshFileSystem::run(const std::string& command, std::string& output, std::error_code& ec) {
    output.clear();
    return runStream(command, [&](const char* p, size_t n) { output.append(p, n); }, ec);
}

std::error_code SshFileSystem::simple(const std::string& command) {
    std::string out;
    std::error_code ec;
    const int code = run(command, out, ec);
    if (ec) return ec;
    return code == 0 ? std::error_code{} : remoteError(code, out);
}

std::string SshFileSystem::join(const std::string& dir, const std::string& name) const {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

// Records: "type|size|mtime|dev|ino|nlink|path", newline-separated; GNU find uses tabs + NUL.
std::string SshFileSystem::statCommand(const std::string& quotedPaths) const {
    if (caps_.gnuFind) return "find " + quotedPaths + " -maxdepth 0 -printf '%y\\t%s\\t%T@\\t%D\\t%i\\t%n\\t%p\\0'";
    if (caps_.stat == "gnu") return "stat -c '%F|%s|%Y|%d|%i|%h|%n' -- " + quotedPaths;
    return "stat -f '%HT|%z|%m|%d|%i|%l|%N' -- " + quotedPaths;
}

std::error_code SshFileSystem::stat(const std::string& path, DirEntry& out) {
    std::string res;
    std::error_code ec;
    const int code = run(statCommand(q(path)), res, ec);
    if (ec) return ec;
    if (code != 0) return remoteError(code, res);
    const char sep = caps_.gnuFind ? '\t' : '|';
    if (!res.empty() && (res.back() == '\0' || res.back() == '\n')) res.pop_back();
    std::string p;
    if (!parseRecord(res, sep, caps_.gnuFind ? "find" : "stat", out, p)) return std::make_error_code(std::errc::io_error);
    return {};
}

std::error_code SshFileSystem::list(const std::string& dir, std::vector<DirEntry>& out) {
    out.clear();
    const std::string cmd =
        caps_.gnuFind ? "find " + q(dir) + " -mindepth 1 -maxdepth 1 -printf '%y\\t%s\\t%T@\\t%D\\t%i\\t%n\\t%p\\0'"
                      : "cd " + q(dir) + " && for f in .* *; do case \"$f\" in .|..) continue;; esac; [ -e \"$f\" ] || [ -L \"$f\" ] || continue; " +
                            statCommand("\"$PWD/$f\"") + "; done";
    std::string res;
    std::error_code ec;
    const int code = run(cmd, res, ec);
    if (ec) return ec;
    if (code != 0) return remoteError(code, res);
    const char recSep = caps_.gnuFind ? '\0' : '\n';
    const char sep = caps_.gnuFind ? '\t' : '|';
    size_t pos = 0;
    while (pos < res.size()) {
        size_t end = res.find(recSep, pos);
        if (end == std::string::npos) end = res.size();
        DirEntry e;
        std::string p;
        if (end > pos && parseRecord(res.substr(pos, end - pos), sep, caps_.gnuFind ? "find" : "stat", e, p)) out.push_back(std::move(e));
        pos = end + 1;
    }
    return {};
}

std::unique_ptr<ScanTree> SshFileSystem::walkTree(const std::string& root, const std::atomic<bool>& cancel,
                                                  std::atomic<uint64_t>& files, std::atomic<uint64_t>& dirs,
                                                  std::atomic<uint64_t>& bytes) {
    auto tree = std::make_unique<ScanTree>();
    tree->root = std::make_unique<Node>();
    tree->root->name = root;
    tree->root->kind = NodeKind::Directory;

    // Snapshot views and our own quarantine would double-count everything.
    const std::string prune = "\\( -name .zfs -o -name '#snapshot' -o -name .storage-trash \\) -prune -o";
    std::string cmd;
    if (caps_.gnuFind) cmd = "find " + q(root) + " -xdev " + prune + " -printf '%y\\t%s\\t%T@\\t%D\\t%i\\t%n\\t%p\\0'";
    else cmd = "find " + q(root) + " -xdev " + prune + " -exec " +
               (caps_.stat == "gnu" ? std::string("stat -c '%F|%s|%Y|%d|%i|%h|%n'") : std::string("stat -f '%HT|%z|%m|%d|%i|%l|%N'")) +
               " {} +";
    const char recSep = caps_.gnuFind ? '\0' : '\n';
    const char sep = caps_.gnuFind ? '\t' : '|';
    const std::string style = caps_.gnuFind ? "find" : "stat";

    std::string normRoot = root;
    while (normRoot.size() > 1 && normRoot.back() == '/') normRoot.pop_back();
    std::unordered_map<std::string, Node*> dirsByPath{{normRoot, tree->root.get()}};
    std::unordered_set<std::string> seenInodes;
    uint64_t rootDev = 0;
    bool sawRoot = false;
    std::string pending;

    auto handle = [&](const std::string& rec) {
        DirEntry e;
        std::string path;
        if (!parseRecord(rec, sep, style, e, path)) return;
        if (path == normRoot) {
            tree->root->mtime = e.mtime;
            tree->root->dev = e.dev;
            rootDev = e.dev;
            sawRoot = true;
            return;
        }
        auto parentIt = dirsByPath.find(dirOf(path));
        if (parentIt == dirsByPath.end()) return;
        auto node = std::make_unique<Node>();
        node->name = e.name;
        node->parent = parentIt->second;
        node->kind = e.kind;
        node->size = e.size;
        node->mtime = e.mtime;
        node->dev = e.dev;
        node->ino = e.ino;
        node->nlink = e.nlink;
        if (e.kind == NodeKind::Directory) {
            dirs.fetch_add(1, std::memory_order_relaxed);
            dirsByPath.emplace(path, node.get());
        } else if (e.kind == NodeKind::File) {
            files.fetch_add(1, std::memory_order_relaxed);
            if (e.nlink > 1 && !seenInodes.insert(std::to_string(e.dev) + ":" + std::to_string(e.ino)).second)
                node->flags |= NodeFlag::HardlinkRepeat;
            else bytes.fetch_add(e.size, std::memory_order_relaxed);
        }
        parentIt->second->children.push_back(std::move(node));
    };

    std::error_code ec;
    runStream(cmd, [&](const char* p, size_t n) {
        if (cancel.load(std::memory_order_relaxed)) return;
        pending.append(p, n);
        size_t start = 0;
        for (size_t i; (i = pending.find(recSep, start)) != std::string::npos; start = i + 1) handle(pending.substr(start, i - start));
        pending.erase(0, start);
    }, ec);
    if (!pending.empty()) handle(pending);
    (void)rootDev;
    if (!sawRoot) tree->root->flags |= NodeFlag::Unreadable;
    return tree;
}

std::string SshFileSystem::hashCommand(const std::string& quotedPath) const {
    if (caps_.sha == "sha256") return "sha256 -q " + quotedPath;
    if (caps_.sha == "shasum") return "shasum -a 256 " + quotedPath;
    return "sha256sum " + quotedPath;
}

namespace {
std::error_code firstHash(int code, const std::string& out, const std::error_code& ec, std::string& hash) {
    if (ec) return ec;
    if (code != 0) return remoteError(code, out);
    // GNU tools prefix a backslash when the name needed escaping.
    size_t i = out.size() > 0 && out[0] == '\\' ? 1 : 0;
    if (out.size() < i + 64) return std::make_error_code(std::errc::io_error);
    hash = out.substr(i, 64);
    for (char c : hash)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::make_error_code(std::errc::io_error);
    return {};
}
}  // namespace

std::error_code SshFileSystem::hashFull(const std::string& path, std::string& out, const std::atomic<bool>* cancel,
                                        std::atomic<uint64_t>*) {
    if (cancel && cancel->load()) return std::make_error_code(std::errc::operation_canceled);
    std::string res;
    std::error_code ec;
    const int code = run(hashCommand("-- " + q(path)), res, ec);
    if (caps_.sha == "sha256") {  // BSD sha256 does not take "--"
        res.clear();
        const int c2 = run(hashCommand(q(path)), res, ec);
        return firstHash(c2, res, ec, out);
    }
    return firstHash(code, res, ec, out);
}

std::error_code SshFileSystem::hashPartial(const std::string& path, uint64_t size, std::string& out) {
    if (size <= 2 * kPartialChunk) return hashFull(path, out, nullptr, nullptr);
    const std::string n = std::to_string(kPartialChunk);
    const std::string sha = caps_.sha == "sha256" ? "sha256 -q" : caps_.sha == "shasum" ? "shasum -a 256" : "sha256sum";
    std::string res;
    std::error_code ec;
    const int code = run("{ head -c " + n + " " + q(path) + " && tail -c " + n + " " + q(path) + "; } | " + sha, res, ec);
    return firstHash(code, res, ec, out);
}

std::error_code SshFileSystem::sameContent(const std::string& a, const std::string& b, bool& equal, const std::atomic<bool>*) {
    std::string res;
    std::error_code ec;
    const int code = run("cmp -s " + q(a) + " " + q(b), res, ec);
    if (ec) return ec;
    if (code > 1) return remoteError(code, res);
    equal = code == 0;
    return {};
}

std::error_code SshFileSystem::readHead(const std::string& path, size_t max, std::string& out) {
    std::error_code ec;
    const int code = run("head -c " + std::to_string(max) + " " + q(path), out, ec);
    if (ec) return ec;
    return code == 0 ? std::error_code{} : remoteError(code, out);
}

std::error_code SshFileSystem::removeFile(const std::string& path) { return simple("rm -f -- " + q(path) + " && [ ! -e " + q(path) + " ]"); }
std::error_code SshFileSystem::removeDir(const std::string& dir) { return simple("rmdir -- " + q(dir)); }

std::error_code SshFileSystem::removeTree(const std::string& dir) {
    // Refuse anything that is not a reasonably deep absolute path.
    if (dir.empty() || dir[0] != '/' || std::count(dir.begin(), dir.end(), '/') < 2 || dir.find("/..") != std::string::npos)
        return std::make_error_code(std::errc::invalid_argument);
    return simple("rm -rf -- " + q(dir));
}

std::error_code SshFileSystem::makeDirs(const std::string& dir) { return simple("mkdir -p -- " + q(dir)); }

std::error_code SshFileSystem::renameNoReplace(const std::string& from, const std::string& to) {
    // A hard link refuses to overwrite and is atomic; fall back to mv for folders and
    // filesystems without hard links. Exit 17 = exists, 18 = different filesystem.
    const std::string F = q(from), T = q(to);
    return simple("[ -e " + T + " ] || [ -L " + T + " ] && exit 17; "
                  "if [ -d " + F + " ] && [ ! -L " + F + " ]; then mv -- " + F + " " + T + "; exit $?; fi; "
                  "err=$(ln -- " + F + " " + T + " 2>&1) && { rm -f -- " + F + "; exit 0; }; "
                  "case \"$err\" in *ross-device*) exit 18;; esac; "
                  "[ -e " + T + " ] && exit 17; mv -- " + F + " " + T);
}

std::error_code SshFileSystem::copyNew(const std::string& from, const std::string& to, const std::atomic<bool>*) {
    const std::string T = q(to);
    return simple("[ -e " + T + " ] && exit 17; cp -p -- " + q(from) + " " + T + " && { sync -- " + T +
                  " 2>/dev/null || sync; }");
}

std::error_code SshFileSystem::replaceWithHardlink(const std::string& target, const std::string& source) {
    const std::string tmp = q(target + kTempSuffix);
    return simple("rm -f -- " + tmp + "; ln -- " + q(source) + " " + tmp + " && mv -f -- " + tmp + " " + q(target) +
                  " || { rm -f -- " + tmp + "; exit 1; }");
}

std::error_code SshFileSystem::replaceWithClone(const std::string& target, const std::string& source) {
    if (!caps_.reflink) return std::make_error_code(std::errc::operation_not_supported);
    const std::string tmp = q(target + kTempSuffix);
    const auto ec = simple("rm -f -- " + tmp + "; cp --reflink=always -p -- " + q(source) + " " + tmp + " && mv -f -- " + tmp +
                           " " + q(target) + " || { rm -f -- " + tmp + "; exit 95; }");
    return ec == std::errc::io_error ? std::make_error_code(std::errc::operation_not_supported) : ec;
}

std::error_code SshFileSystem::replaceWithCopy(const std::string& target, const std::string& source) {
    const std::string tmp = q(target + kTempSuffix);
    return simple("rm -f -- " + tmp + "; cp -p -- " + q(source) + " " + tmp + " && { sync -- " + tmp +
                  " 2>/dev/null || sync; } && mv -f -- " + tmp + " " + q(target) + " || { rm -f -- " + tmp + "; exit 1; }");
}

// Quarantine at the top of the same filesystem, so trashing is a cheap rename.
std::string SshFileSystem::trashDirFor(const std::string& path) {
    const std::string parent = dirOf(path);
    {
        std::lock_guard lk(trashMu_);
        if (auto it = trashDirs_.find(parent); it != trashDirs_.end()) return it->second;
    }
    // Highest writable folder on the same filesystem (usually the share or the home folder),
    // so moving into the quarantine is a rename, never a copy.
    const std::string devOf = caps_.stat == "bsd" ? "stat -f %d" : "stat -c %d";
    std::string out;
    std::error_code ec;
    run("d=" + q(parent) + "; dev=$(" + devOf + " \"$d\"); "
        "while [ \"$d\" != / ]; do up=$(dirname \"$d\"); "
        "[ \"$(" + devOf + " \"$up\" 2>/dev/null)\" = \"$dev\" ] && [ -w \"$up\" ] || break; d=$up; done; "
        "printf %s \"$d\"",
        out, ec);
    std::string dir;
    if (!ec && !out.empty() && out[0] == '/') dir = (out == "/" ? std::string() : out) + "/.storage-trash";
    else dir = parent + "/.storage-trash";
    std::lock_guard lk(trashMu_);
    trashDirs_[parent] = dir;
    return dir;
}

std::error_code SshFileSystem::trash(const std::string& path, std::string& location) {
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    const std::string dest = trashDirFor(path) + "/" + stamp + path;
    if (auto ec = makeDirs(dirOf(dest))) return ec;
    if (auto ec = renameNoReplace(path, dest)) return ec;
    location = dest;
    return {};
}

std::error_code SshFileSystem::restore(const std::string& location, const std::string& original) {
    if (location.empty()) return std::make_error_code(std::errc::operation_not_supported);
    if (auto ec = makeDirs(dirOf(original))) return ec;
    return renameNoReplace(location, original);
}

}  // namespace storage
