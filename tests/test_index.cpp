#include "check.hpp"

#include "TempTree.hpp"
#include "storage/core/index.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"

using namespace storage;

TEST_CASE("index save/load round-trips a tree") {
    TempTree t;
    t.file("a/1.bin", 10);
    t.file("a/2.bin", 20);
    t.file("b/c/3.bin", 300);

    LocalFileSystem lfs;
    auto tree = Scanner(lfs, ScanOptions::defaultsFor(toUtf8(t.root()))).run();

    TempTree dbDir;
    Index index(dbDir.root() / "index.db");
    const int64_t id = index.save(*tree);
    CHECK(id > 0);
    CHECK(tree->id == id);

    auto loaded = index.load(id);
    REQUIRE(loaded);
    CHECK(loaded->root->name == tree->root->name);
    CHECK(loaded->root->size == 330);
    CHECK(loaded->root->files == 3);
    CHECK(loaded->root->dirs == 3);
    CHECK(loaded->complete == tree->complete);

    Node* n = loaded->find(toUtf8(t.root() / "b" / "c" / "3.bin"));
    REQUIRE(n);
    CHECK(n->size == 300);
    CHECK(n->parent->name == "c");

    const auto list = index.scans();
    REQUIRE(list.size() == 1);
    CHECK(list[0].bytes == 330);
    CHECK(index.findScan(tree->root->name) == id);
}

TEST_CASE("saving the same root replaces the previous scan") {
    TempTree t;
    t.file("f.bin", 5);
    LocalFileSystem lfs;
    TempTree dbDir;
    Index index(dbDir.root() / "index.db");

    auto first = Scanner(lfs, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
    const int64_t a = index.save(*first);
    t.file("g.bin", 7);
    auto second = Scanner(lfs, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
    const int64_t b = index.save(*second);

    CHECK(a != b);
    CHECK(index.load(a) == nullptr);
    REQUIRE(index.scans().size() == 1);
    CHECK(index.load(b)->root->size == 12);

    index.remove(b);
    CHECK(index.scans().empty());
}
