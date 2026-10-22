#include <algorithm>
#include <string>

#include "TempTree.hpp"
#include "check.hpp"
#include "storage/core/duplicates.hpp"
#include "storage/core/index.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"
#include "storage/core/sha256.hpp"

using namespace storage;
namespace fs = std::filesystem;

namespace {

std::string pattern(size_t n, uint32_t seed) {
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (auto& c : s) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x);
    }
    return s;
}

void write(const TempTree& t, const std::string& rel, const std::string& data) {
    const auto p = t.root() / fs::path(rel);
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

std::unique_ptr<ScanTree> scan(const TempTree& t) {
    LocalFileSystem lfs;
    return Scanner(lfs, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
}

std::vector<DupGroup> find(const TempTree& t, DupOptions o = {}, Index* cache = nullptr) {
    auto tree = scan(t);
    LocalFileSystem lfs;
    return DuplicateFinder(lfs, o, cache).run(*tree->root);
}

std::vector<std::string> names(const DupGroup& g) {
    std::vector<std::string> out;
    for (const auto& f : g.files) out.push_back(fs::path(fromUtf8(f.path)).filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("duplicates: identical files are grouped, look-alikes are not") {
    TempTree t;
    const std::string a = pattern(5000, 1);
    write(t, "one/a.bin", a);
    write(t, "two/a-copy.bin", a);
    write(t, "three/deep/a-again.bin", a);
    write(t, "same-size-different.bin", pattern(5000, 2));
    write(t, "unique.bin", pattern(1234, 3));
    write(t, "empty1", "");
    write(t, "empty2", "");

    auto groups = find(t);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].size == 5000);
    CHECK(groups[0].files.size() == 3);
    CHECK(groups[0].reclaimable() == 10000);
    CHECK(groups[0].hash == Sha256::hexOf(a.data(), a.size()));
    CHECK(names(groups[0]) == std::vector<std::string>{"a-again.bin", "a-copy.bin", "a.bin"});
}

TEST_CASE("duplicates: large files differing only in the middle are not duplicates") {
    TempTree t;
    std::string big = pattern(400 * 1024, 7);
    write(t, "x.bin", big);
    big[200 * 1024] ^= 0x5A;  // outside the first/last 64 KiB sampled by the partial hash
    write(t, "y.bin", big);
    write(t, "z.bin", big);

    auto groups = find(t);
    REQUIRE(groups.size() == 1);
    CHECK(names(groups[0]) == std::vector<std::string>{"y.bin", "z.bin"});
}

TEST_CASE("duplicates: groups are ordered by reclaimable space and minSize filters") {
    TempTree t;
    const std::string small = pattern(100, 4), large = pattern(9000, 5);
    for (int i = 0; i < 4; ++i) write(t, "s" + std::to_string(i), small);  // 300 reclaimable
    for (int i = 0; i < 2; ++i) write(t, "l" + std::to_string(i), large);  // 9000 reclaimable

    auto groups = find(t);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].size == 9000);
    CHECK(groups[1].size == 100);

    DupOptions o;
    o.minSize = 1000;
    auto filtered = find(t, o);
    REQUIRE(filtered.size() == 1);
    CHECK(filtered[0].size == 9000);
}

TEST_CASE("duplicates: excluded folder names and hidden files") {
    TempTree t;
    const std::string data = pattern(3000, 9);
    write(t, "keep/a", data);
    write(t, ".git/objects/a", data);
    write(t, "node/.hidden", data);
    write(t, "keep/b", data);

    DupOptions o;
    o.excludeNames = {".git"};
    o.skipHidden = true;
    auto groups = find(t, o);
    REQUIRE(groups.size() == 1);
    CHECK(names(groups[0]) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("duplicates: byte verification keeps true duplicates") {
    TempTree t;
    const std::string data = pattern(300 * 1024, 11);
    write(t, "a", data);
    write(t, "b", data);
    DupOptions o;
    o.verifyBytes = true;
    auto groups = find(t, o);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].files.size() == 2);
}

#ifndef _WIN32
TEST_CASE("duplicates: hard links to one inode are not duplicates") {
    TempTree t;
    const std::string data = pattern(4000, 12);
    write(t, "original", data);
    fs::create_hard_link(t.root() / "original", t.root() / "link");
    CHECK(find(t).empty());

    write(t, "real-copy", data);
    auto groups = find(t);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].files.size() == 2);  // one name for the linked inode + the real copy
}
#endif

TEST_CASE("duplicates: hashes are cached and reused") {
    TempTree t;
    const std::string data = pattern(500 * 1024, 13);
    write(t, "a", data);
    write(t, "b", data);
    TempTree dbDir;
    Index index(dbDir.root() / "index.db");

    auto first = find(t, {}, &index);
    REQUIRE(first.size() == 1);

    std::vector<HashRecord> recs{{first[0].files[0].path, first[0].size, first[0].files[0].mtime, {}, {}}};
    index.lookupHashes("local", recs);
    CHECK(recs[0].full == first[0].hash);
    CHECK(!recs[0].partial.empty());

    // A stale size invalidates the cache entry.
    std::vector<HashRecord> stale{{first[0].files[0].path, first[0].size + 1, first[0].files[0].mtime, {}, {}}};
    index.lookupHashes("local", stale);
    CHECK(stale[0].full.empty());

    auto second = find(t, {}, &index);
    REQUIRE(second.size() == 1);
    CHECK(second[0].hash == first[0].hash);
}
