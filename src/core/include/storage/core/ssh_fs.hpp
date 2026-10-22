#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/core/filesystem.hpp"
#include "storage/core/json.hpp"

namespace storage {

// How to reach a NAS. `host` may be an alias from ~/.ssh/config.
struct NasConfig {
    std::string name;
    std::string host;
    std::string user;
    int port = 0;
    std::string identityFile;
    std::string root = "/";  // default folder to scan
    // Testing / advanced: argv that yields a POSIX shell on stdin instead of `ssh ... sh`.
    std::vector<std::string> transport;

    std::string volume() const;  // "ssh://user@host:port"
    std::vector<std::string> command() const;
    json::Value toJson() const;
    static NasConfig fromJson(const json::Value& v);
};

// Saved NAS connections (appDataDir()/nas.json).
std::vector<NasConfig> loadNasConfigs();
void saveNasConfigs(const std::vector<NasConfig>& list);

struct RemoteCaps {
    std::string system;       // uname -s
    bool gnuFind = false;     // find -printf
    std::string stat;         // "gnu", "bsd" or ""
    std::string sha;          // "sha256sum", "sha256", "shasum" or ""
    bool reflink = false;     // cp --reflink
};

// A filesystem on another machine, driven through one or more persistent shells
// over the system `ssh` client. Thread-safe: concurrent calls use separate sessions.
class SshFileSystem final : public FileSystem {
public:
    explicit SshFileSystem(NasConfig config, int sessions = 4);
    ~SshFileSystem() override;

    // Open the first session and detect remote tools. Errors carry ssh's own message.
    // Safe to call repeatedly and from any thread; every operation also connects on demand.
    std::error_code connect(std::string& message);
    bool connected() const { return connected_; }
    const RemoteCaps& caps() const { return caps_; }
    const NasConfig& config() const { return cfg_; }

    // Run an arbitrary command; returns its exit code (-1 on transport failure).
    int run(const std::string& command, std::string& output, std::error_code& ec);

    std::error_code stat(const std::string& path, DirEntry& out) override;
    std::error_code list(const std::string& dir, std::vector<DirEntry>& out) override;
    std::string join(const std::string& dir, const std::string& name) const override;
    char separator() const override { return '/'; }
    std::string volumeName() const override { return cfg_.volume(); }
    std::unique_ptr<ScanTree> walkTree(const std::string& root, const std::atomic<bool>& cancel,
                                       std::atomic<uint64_t>& files, std::atomic<uint64_t>& dirs,
                                       std::atomic<uint64_t>& bytes) override;

    std::error_code hashPartial(const std::string& path, uint64_t size, std::string& out) override;
    std::error_code hashFull(const std::string& path, std::string& out, const std::atomic<bool>* cancel,
                             std::atomic<uint64_t>* bytesRead) override;
    std::error_code sameContent(const std::string& a, const std::string& b, bool& equal,
                                const std::atomic<bool>* cancel) override;
    std::error_code trash(const std::string& path, std::string& location) override;
    std::error_code restore(const std::string& location, const std::string& original) override;
    std::error_code removeFile(const std::string& path) override;
    std::error_code replaceWithHardlink(const std::string& target, const std::string& source) override;
    std::error_code replaceWithClone(const std::string& target, const std::string& source) override;
    std::error_code replaceWithCopy(const std::string& target, const std::string& source) override;
    std::error_code makeDirs(const std::string& dir) override;
    std::error_code renameNoReplace(const std::string& from, const std::string& to) override;
    std::error_code copyNew(const std::string& from, const std::string& to, const std::atomic<bool>* cancel) override;
    std::error_code readHead(const std::string& path, size_t max, std::string& out) override;
    std::error_code removeDir(const std::string& dir) override;
    std::error_code removeTree(const std::string& dir) override;

private:
    class Session;
    Session* acquire();
    void release(Session* s);
    int runStream(const std::string& command, const std::function<void(const char*, size_t)>& sink, std::error_code& ec);
    std::error_code simple(const std::string& command);
    std::string statCommand(const std::string& quotedPaths) const;  // records for paths, NUL- or newline-separated
    std::string hashCommand(const std::string& quotedPath) const;
    std::string trashDirFor(const std::string& path);

    NasConfig cfg_;
    RemoteCaps caps_;
    std::mutex connectMu_;
    std::atomic<bool> connected_{false};
    int maxSessions_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::vector<Session*> idle_;
    std::mutex trashMu_;
    std::unordered_map<std::string, std::string> trashDirs_;  // parent dir -> trash dir
};

}  // namespace storage
