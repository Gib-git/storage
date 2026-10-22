#include "check.hpp"

#include "TempTree.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace storage;
namespace fs = std::filesystem;

namespace {
std::unique_ptr<ScanTree> scan(const fs::path& root, unsigned threads = 4) {
    LocalFileSystem lfs;
    ScanOptions o = ScanOptions::defaultsFor(toUtf8(root));
    o.threads = threads;
    return Scanner(lfs, o).run();
}
}  // namespace

TEST_CASE("scanner aggregates sizes and counts") {
    TempTree t;
    t.file("a.txt", 100);
    t.file("docs/b.pdf", 2000);
    t.file("docs/deep/c.bin", 30000);
    t.file("docs/deep/d.bin", 400);
    t.dir("empty");

    for (unsigned threads : {1u, 4u, 16u}) {
        auto tree = scan(t.root(), threads);
        REQUIRE(tree->complete);
        const Node& r = *tree->root;
        CHECK(r.size == 32500);
        CHECK(r.files == 4);
        CHECK(r.dirs == 3);
        CHECK(r.errors == 0);

        // Children are sorted largest first.
        REQUIRE(r.children.size() == 3);
        CHECK(r.children[0]->name == "docs");
        CHECK(r.children[0]->size == 32400);
        CHECK(r.children[1]->name == "a.txt");
        CHECK(r.children[2]->name == "empty");
        CHECK(r.children[2]->size == 0);
    }
}

TEST_CASE("find resolves paths and Node::path round-trips") {
    TempTree t;
    t.file("x/y/z.txt", 10);
    auto tree = scan(t.root());
    const std::string rootPath = toUtf8(t.root());
    const std::string target = toUtf8(t.root() / "x" / "y" / "z.txt");

    Node* n = tree->find(target);
    REQUIRE(n);
    CHECK(n->name == "z.txt");
    CHECK(n->size == 10);
    CHECK(fs::path(fromUtf8(n->path(nativeSeparator()))) == fs::path(fromUtf8(target)));
    CHECK(tree->find(rootPath) == tree->root.get());
    CHECK(tree->find(rootPath + "/nope") == nullptr);
    CHECK(tree->find("/definitely/elsewhere") == nullptr);
}

TEST_CASE("scanner handles a missing root") {
    TempTree t;
    auto tree = scan(t.root() / "missing");
    CHECK(tree->root->errors == 1);
    CHECK(tree->root->size == 0);
}

#ifndef _WIN32
TEST_CASE("hardlinks are counted once and symlinks are not followed") {
    TempTree t;
    const auto a = t.file("a.bin", 5000);
    fs::create_hard_link(a, t.root() / "a-link.bin");
    t.file("target/big.bin", 70000);
    fs::create_directory_symlink(t.root() / "target", t.root() / "loop");

    auto tree = scan(t.root());
    const Node& r = *tree->root;
    CHECK(r.size == 75000);  // the hardlink and the symlinked directory add nothing
    CHECK(r.files == 3);     // both hardlink names are still listed as files

    Node* link = tree->find(toUtf8(t.root() / "loop"));
    REQUIRE(link);
    CHECK(link->kind == NodeKind::Symlink);
    CHECK(link->children.empty());
}

TEST_CASE("unreadable directories are reported, not fatal") {
    if (::geteuid() == 0) SKIP("root can read everything");
    TempTree t;
    t.file("ok.txt", 10);
    const auto locked = t.dir("locked");
    t.file("locked/secret.txt", 99);
    fs::permissions(locked, fs::perms::none);

    auto tree = scan(t.root());
    fs::permissions(locked, fs::perms::owner_all);
    CHECK(tree->root->errors == 1);
    CHECK(tree->root->size == 10);
    Node* n = tree->find(toUtf8(locked));
    REQUIRE(n);
    CHECK((n->flags & NodeFlag::Unreadable));
}
#endif

TEST_CASE("excluded paths are listed but not descended into") {
    TempTree t;
    t.file("keep/a.bin", 100);
    t.file("skip/b.bin", 900);

    LocalFileSystem lfs;
    ScanOptions o = ScanOptions::defaultsFor(toUtf8(t.root()));
    o.excludes = {toUtf8(t.root() / "skip")};
    auto tree = Scanner(lfs, o).run();
    CHECK(tree->root->size == 100);
    Node* skip = tree->find(toUtf8(t.root() / "skip"));
    REQUIRE(skip);
    CHECK((skip->flags & NodeFlag::Excluded));
}
