#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

// A throwaway directory tree that removes itself.
class TempTree {
public:
    TempTree() {
        std::mt19937_64 rng(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        root_ = std::filesystem::temp_directory_path() / ("storage-test-" + std::to_string(rng()));
        std::filesystem::create_directories(root_);
        root_ = std::filesystem::canonical(root_);
    }
    ~TempTree() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const std::filesystem::path& root() const { return root_; }

    std::filesystem::path file(const std::string& rel, size_t bytes, char fill = 'x') const {
        const auto p = root_ / std::filesystem::path(rel);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << std::string(bytes, fill);
        return p;
    }

    std::filesystem::path dir(const std::string& rel) const {
        const auto p = root_ / std::filesystem::path(rel);
        std::filesystem::create_directories(p);
        return p;
    }

private:
    std::filesystem::path root_;
};
