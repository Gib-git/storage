#include "storage/core/volumes.hpp"

#include <map>
#include <memory>
#include <mutex>

#include "storage/core/ssh_fs.hpp"

namespace storage {

FileSystem* filesystemFor(const std::string& volume) {
    static LocalFileSystem local;
    if (isLocalVolume(volume)) return &local;

    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<SshFileSystem>> remotes;
    std::lock_guard lk(mu);
    if (auto it = remotes.find(volume); it != remotes.end()) return it->second.get();
    for (const auto& cfg : loadNasConfigs()) {
        if (cfg.volume() != volume) continue;
        auto fs = std::make_unique<SshFileSystem>(cfg);
        FileSystem* raw = fs.get();
        remotes.emplace(volume, std::move(fs));
        return raw;
    }
    return nullptr;
}

}  // namespace storage
