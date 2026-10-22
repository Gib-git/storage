// Exercises SshFileSystem's remote protocol against a local POSIX shell (`sh`),
// so no SSH server is needed. CI additionally runs it over real ssh to localhost
// when STORAGE_TEST_SSH_HOST is set.
#ifndef _WIN32

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "TempTree.hpp"
#include "check.hpp"
#include "storage/core/duplicates.hpp"
#include "storage/core/executor.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"
#include "storage/core/ssh_fs.hpp"

using namespace storage;
namespace fs = std::filesystem;

namespace {

NasConfig testConfig() {
    NasConfig c;
    c.name = "test";
    c.host = "localhost";
    if (const char* host = std::getenv("STORAGE_TEST_SSH_HOST"); host && *host) c.host = host;
    else c.transport = {"sh"};
    return c;
}

std::unique_ptr<SshFileSystem> connected() {
    auto fs = std::make_unique<SshFileSystem>(testConfig(), 3);
    std::string msg;
    const auto ec = fs->connect(msg);
    if (ec) std::fprintf(stderr, "  connect failed: %s (%s)\n", msg.c_str(), ec.message().c_str());
    return ec ? nullptr : std::move(fs);
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("ssh: connects and detects remote tools") {
    auto r = connected();
    REQUIRE(r);
    CHECK(!r->caps().system.empty());
    CHECK((r->caps().gnuFind || !r->caps().stat.empty()));
    CHECK(!r->caps().sha.empty());
    std::printf("      (remote: %s, find=%s, stat=%s, sha=%s)\n", r->caps().system.c_str(), r->caps().gnuFind ? "gnu" : "other",
                r->caps().stat.c_str(), r->caps().sha.c_str());

    std::string out;
    std::error_code ec;
    CHECK(r->run("printf 'a\\nb'; exit 3", out, ec) == 3);
    CHECK(out == "a\nb");
    CHECK(r->run("echo still alive", out, ec) == 0);  // `exit` ran in a subshell
}

TEST_CASE("ssh: stat, list and scan match the local filesystem") {
    auto r = connected();
    REQUIRE(r);
    TempTree t;
    t.file("plain.txt", 100);
    t.file("with space/it's \"quoted\".bin", 2000);
    t.file("unicodé/ファイル.jpg", 3000);
    t.file("deep/a/b/c.dat", 40000);
    t.dir("empty");

    DirEntry e;
    REQUIRE(!r->stat(toUtf8(t.root() / "plain.txt"), e));
    CHECK(e.kind == NodeKind::File);
    CHECK(e.size == 100);
    CHECK(r->stat(toUtf8(t.root() / "missing"), e) == std::errc::no_such_file_or_directory);

    std::vector<DirEntry> list;
    REQUIRE(!r->list(toUtf8(t.root()), list));
    std::vector<std::string> names;
    for (const auto& d : list) names.push_back(d.name);
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"deep", "empty", "plain.txt", "unicodé", "with space"});

    LocalFileSystem local;
    auto remoteTree = Scanner(*r, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
    auto localTree = Scanner(local, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
    CHECK(remoteTree->volume == r->volumeName());
    CHECK(remoteTree->root->size == localTree->root->size);
    CHECK(remoteTree->root->files == localTree->root->files);
    CHECK(remoteTree->root->dirs == localTree->root->dirs);
    CHECK(remoteTree->find(toUtf8(t.root() / "with space" / "it's \"quoted\".bin")) != nullptr);
}

TEST_CASE("ssh: hashes match local hashes") {
    auto r = connected();
    REQUIRE(r);
    TempTree t;
    std::string big(300 * 1024, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 7 + i / 1000);
    const auto p = t.root() / "big.bin";
    std::ofstream(p, std::ios::binary) << big;
    const auto s = t.file("small.txt", 10, 's');

    LocalFileSystem local;
    for (const auto& f : {p, s}) {
        std::string a, b;
        REQUIRE(!local.hashFull(toUtf8(f), a, nullptr, nullptr));
        REQUIRE(!r->hashFull(toUtf8(f), b, nullptr, nullptr));
        CHECK(a == b);
        const uint64_t size = fs::file_size(f);
        REQUIRE(!local.hashPartial(toUtf8(f), size, a));
        REQUIRE(!r->hashPartial(toUtf8(f), size, b));
        CHECK(a == b);
    }
    bool equal = false;
    REQUIRE(!r->sameContent(toUtf8(p), toUtf8(p), equal, nullptr));
    CHECK(equal);
    REQUIRE(!r->sameContent(toUtf8(p), toUtf8(s), equal, nullptr));
    CHECK(!equal);
    std::string head;
    REQUIRE(!r->readHead(toUtf8(p), 1000, head));
    CHECK(head == big.substr(0, 1000));
}

TEST_CASE("ssh: moves never overwrite; trash and restore round-trip") {
    auto r = connected();
    REQUIRE(r);
    TempTree t;
    const auto a = t.file("a.txt", 10, 'a');
    const auto b = t.file("b.txt", 20, 'b');
    CHECK(r->renameNoReplace(toUtf8(a), toUtf8(b)) == std::errc::file_exists);
    CHECK(read(b) == std::string(20, 'b'));
    REQUIRE(!r->makeDirs(toUtf8(t.root() / "x" / "y")));
    REQUIRE(!r->renameNoReplace(toUtf8(a), toUtf8(t.root() / "x" / "y" / "a.txt")));
    CHECK(!fs::exists(a));
    CHECK(read(t.root() / "x" / "y" / "a.txt") == std::string(10, 'a'));

    std::string where;
    REQUIRE(!r->trash(toUtf8(b), where));
    CHECK(!fs::exists(b));
    CHECK(where.find(".storage-trash") != std::string::npos);
    REQUIRE(!r->restore(where, toUtf8(b)));
    CHECK(read(b) == std::string(20, 'b'));

    CHECK(r->removeTree("/") == std::errc::invalid_argument);
    CHECK(r->removeTree("/tmp") == std::errc::invalid_argument);
}

TEST_CASE("ssh: duplicates and journaled moves work end to end") {
    auto r = connected();
    REQUIRE(r);
    TempTree t, j;
    t.file("one/photo.jpg", 5000, 'p');
    t.file("two/photo copy.jpg", 5000, 'p');
    t.file("two/other.jpg", 5000, 'o');
    auto tree = Scanner(*r, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
    auto groups = DuplicateFinder(*r, {}).run(*tree->root);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].files.size() == 2);

    for (bool forceCopy : {false, true}) {
        const auto src = t.root() / (forceCopy ? "two/other.jpg" : "one/photo.jpg");
        const auto dst = t.root() / "sorted" / (forceCopy ? "other.jpg" : "photo.jpg");
        DirEntry e;
        REQUIRE(!r->stat(toUtf8(src), e));
        JournalOp op;
        op.action = Action::Move;
        op.src = toUtf8(src);
        op.dst = toUtf8(dst);
        op.size = e.size;
        op.mtime = e.mtime;
        Journal journal(j.root() / "journal.db");
        const int64_t batch = journal.createBatch("organize", "ssh test", r->volumeName(), {op});
        ExecOptions o;
        o.forceCopy = forceCopy;
        Executor ex(*r, journal, o);
        const auto res = ex.run(batch);
        CHECK(res.done == 1);
        CHECK(!fs::exists(src));
        CHECK(fs::file_size(dst) == 5000);
        CHECK(ex.undo(batch).done == 1);
        CHECK(fs::exists(src));
    }
}

#endif
