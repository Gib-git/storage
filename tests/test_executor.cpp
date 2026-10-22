#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "TempTree.hpp"
#include "check.hpp"
#include "storage/core/duplicates.hpp"
#include "storage/core/executor.hpp"
#include "storage/core/journal.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/sha256.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace storage;
namespace fs = std::filesystem;

namespace {

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string u8(const fs::path& p) { return toUtf8(p); }

JournalOp opFor(const std::string& action, const fs::path& src, const fs::path& ref = {}, const fs::path& dst = {}) {
    LocalFileSystem lfs;
    DirEntry e;
    lfs.stat(u8(src), e);
    JournalOp op;
    op.action = action;
    op.src = u8(src);
    op.ref = ref.empty() ? std::string() : u8(ref);
    op.dst = dst.empty() ? std::string() : u8(dst);
    op.size = e.size;
    op.mtime = e.mtime;
    return op;
}

// Every file's contents under `root`, keyed by relative path (ignores the journal).
std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> out;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        out[fs::relative(e.path(), root).generic_string()] = read(e.path());
    }
    return out;
}

std::multiset<std::string> contents(const std::map<std::string, std::string>& snap) {
    std::multiset<std::string> out;
    for (auto& [k, v] : snap) out.insert(v);
    return out;
}

}  // namespace

TEST_CASE("executor: planDedupe never removes every copy") {
    DupGroup g;
    g.size = 3;
    g.hash = "h";
    g.files = {{"/a", 3, 1, 0, 0}, {"/b", 3, 2, 0, 0}};
    bool threw = false;
    try {
        planDedupe({g}, {{true, true}}, Action::Trash);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    auto ops = planDedupe({g}, {{false, true}}, Action::Delete);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].src == "/b");
    CHECK(ops[0].ref == "/a");
    CHECK(planDedupe({g}, {{false, false}}, Action::Delete).empty());
}

TEST_CASE("executor: keep rules pick the expected copy") {
    DupGroup g;
    g.files = {{"/x/long/path/f", 1, 300, 0, 0}, {"/x/f", 1, 100, 0, 0}, {"/keep/f", 1, 200, 0, 0}};
    CHECK(chooseKeeper(g, KeepRule::Newest) == 0);
    CHECK(chooseKeeper(g, KeepRule::Oldest) == 1);
    CHECK(chooseKeeper(g, KeepRule::ShortestPath) == 1);
    CHECK(chooseKeeper(g, KeepRule::LongestPath) == 0);
    CHECK(chooseKeeper(g, KeepRule::Alphabetical) == 2);
    CHECK(chooseKeeper(g, KeepRule::InFolder, {"/keep"}) == 2);
    CHECK(chooseKeeper(g, KeepRule::InFolder, {"/kee"}) == 1);  // prefix of a name is not a folder match
    CHECK(parseKeepRule("newest") == KeepRule::Newest);
}

TEST_CASE("executor: delete is refused when contents changed behind its back") {
    TempTree t, j;
    const auto a = t.file("a.bin", 5000, 'q');
    const auto b = t.file("b.bin", 5000, 'q');
    Journal journal(j.root() / "journal.db");
    LocalFileSystem lfs;
    const auto op = opFor(Action::Delete, b, a);

    // Same size, same mtime, different bytes: only a content re-check notices.
    const auto mtime = fs::last_write_time(b);
    t.file("b.bin", 5000, 'z');
    fs::last_write_time(b, mtime);

    const int64_t batch = journal.createBatch("dedupe", "test", "local", {op});
    auto r = Executor(lfs, journal).run(batch);
    CHECK(r.skipped == 1);
    CHECK(fs::exists(b));
    CHECK(read(b) == std::string(5000, 'z'));
    CHECK(journal.ops(batch)[0].detail == "contents no longer identical");
}

TEST_CASE("executor: changed size, missing keeper and missing file are skipped") {
    TempTree t, j;
    const auto a = t.file("a", 100, 'k');
    const auto b = t.file("b", 100, 'k');
    const auto c = t.file("c", 100, 'k');
    const auto d = t.file("d", 100, 'k');
    Journal journal(j.root() / "journal.db");
    LocalFileSystem lfs;
    auto changed = opFor(Action::Delete, b, a);
    auto noKeeper = opFor(Action::Delete, c, t.root() / "gone");
    auto gone = opFor(Action::Delete, d, a);
    t.file("b", 150, 'k');
    fs::remove(d);
    const int64_t batch = journal.createBatch("dedupe", "test", "local", {changed, noKeeper, gone});
    auto r = Executor(lfs, journal).run(batch);
    CHECK(r.skipped == 3);
    CHECK(r.done == 0);
    CHECK(fs::exists(b));
    CHECK(fs::exists(c));
}

#ifndef _WIN32
TEST_CASE("executor: hardlink dedupe and undo") {
    TempTree t, j;
    const auto a = t.file("a.bin", 70000, 'h');
    const auto b = t.file("sub/b.bin", 70000, 'h');
    Journal journal(j.root() / "journal.db");
    LocalFileSystem lfs;
    const int64_t batch = journal.createBatch("dedupe", "test", "local", {opFor(Action::Hardlink, b, a)});
    Executor ex(lfs, journal);
    auto r = ex.run(batch);
    CHECK(r.done == 1);
    CHECK(fs::equivalent(a, b));
    CHECK(fs::hard_link_count(a) == 2);
    CHECK(!fs::exists(u8(b) + FileSystem::kTempSuffix));

    auto u = ex.undo(batch);
    CHECK(u.done == 1);
    CHECK(!fs::equivalent(a, b));
    CHECK(read(b) == std::string(70000, 'h'));
    CHECK(journal.batch(batch).state == BatchState::Undone);
}

TEST_CASE("executor: trash and restore") {
    TempTree t, j;
    const auto a = t.file("keep.txt", 64, 't');
    const auto b = t.file("trash-me.txt", 64, 't');
    Journal journal(j.root() / "journal.db");
    LocalFileSystem lfs;
    const int64_t batch = journal.createBatch("dedupe", "test", "local", {opFor(Action::Trash, b, a)});
    Executor ex(lfs, journal);
    auto r = ex.run(batch);
    REQUIRE(r.done == 1);
    CHECK(!fs::exists(b));
    const auto op = journal.ops(batch)[0];
    REQUIRE(!op.dst.empty());
    CHECK(fs::exists(fromUtf8(op.dst)));

    auto u = ex.undo(batch);  // also keeps the developer's Trash clean
    CHECK(u.done == 1);
    CHECK(read(b) == std::string(64, 't'));
    CHECK(!fs::exists(fromUtf8(op.dst)));
}
#endif

TEST_CASE("executor: moves never overwrite and undo puts files back") {
    for (bool forceCopy : {false, true}) {
        TempTree t, j;
        const auto src = t.file("in/photo.jpg", 200000, 'p');
        const auto taken = t.file("out/taken.jpg", 10, 'x');
        const auto other = t.file("in/other.jpg", 300, 'o');
        Journal journal(j.root() / "journal.db");
        LocalFileSystem lfs;
        ExecOptions o;
        o.forceCopy = forceCopy;
        const auto dst = t.root() / "out" / "2026" / "photo.jpg";
        const int64_t batch = journal.createBatch(
            "organize", "test", "local",
            {opFor(Action::Move, src, {}, dst), opFor(Action::Move, other, {}, taken)});
        Executor ex(lfs, journal, o);
        auto r = ex.run(batch);
        CHECK(r.done == 1);
        CHECK(r.skipped == 1);  // destination exists: refused, nothing overwritten
        CHECK(!fs::exists(src));
        CHECK(read(dst) == std::string(200000, 'p'));
        CHECK(read(taken) == std::string(10, 'x'));
        CHECK(read(other) == std::string(300, 'o'));

        auto u = ex.undo(batch);
        CHECK(u.done == 1);
        CHECK(read(src) == std::string(200000, 'p'));
        CHECK(!fs::exists(dst));
    }
}

#ifndef _WIN32
// Kill a child process mid-operation at every fault point, then recover and finish.
// Afterwards every original file's bytes must exist exactly once, in a valid place.
TEST_CASE("executor: survives being killed at any point (fork + _exit)") {
    const char* points[] = {"started", "copied", "published", "renamed"};
    for (bool forceCopy : {true, false}) {
        for (const char* point : points) {
            TempTree t, j;
            for (int i = 0; i < 4; ++i) t.file("src/f" + std::to_string(i) + ".bin", 50000 + i * 1000, char('a' + i));
            t.file("dup/a.bin", 50000, 'a');  // duplicate of src/f0.bin for a hardlink op
            const auto before = contents(snapshot(t.root()));

            std::vector<JournalOp> ops;
            for (int i = 0; i < 4; ++i)
                ops.push_back(opFor(Action::Move, t.root() / "src" / ("f" + std::to_string(i) + ".bin"), {},
                                    t.root() / "dst" / ("f" + std::to_string(i) + ".bin")));
            ops.push_back(opFor(Action::Hardlink, t.root() / "dup" / "a.bin", t.root() / "dst" / "f0.bin"));
            ops.back().size = 50000;
            int64_t batch;
            {
                Journal journal(j.root() / "journal.db");
                batch = journal.createBatch("organize", "crash test", "local", ops);
            }

            const pid_t pid = fork();
            REQUIRE(pid >= 0);
            if (pid == 0) {
                Journal journal(j.root() / "journal.db");
                LocalFileSystem lfs;
                ExecOptions o;
                o.forceCopy = forceCopy;
                o.faultHook = [point](const JournalOp& op, const char* p) {
                    if (op.seq == 2 && std::string(p) == point) _exit(42);  // die mid-batch, no cleanup
                };
                Executor(lfs, journal, o).run(batch);
                _exit(0);
            }
            int status = 0;
            waitpid(pid, &status, 0);

            Journal journal(j.root() / "journal.db");
            LocalFileSystem lfs;
            const auto recovered = Executor::recover(lfs, journal);
            const bool crashed = WIFEXITED(status) && WEXITSTATUS(status) == 42;
            const std::string pt = point;
            const bool reachable = pt == "started" || (forceCopy ? pt == "copied" || pt == "published" : pt == "renamed");
            CHECK(crashed == reachable);  // the kill really happened wherever that point exists
            CHECK(recovered.size() == (crashed ? 1u : 0u));
            Executor(lfs, journal).run(batch);

            const auto after = snapshot(t.root());
            CHECK(contents(after) == before);  // nothing lost, nothing duplicated
            for (int i = 0; i < 4; ++i) {
                CHECK(after.count("dst/f" + std::to_string(i) + ".bin") == 1);
                CHECK(after.count("src/f" + std::to_string(i) + ".bin") == 0);
            }
            for (const auto& [path, _] : after) CHECK(path.find(FileSystem::kTempSuffix) == std::string::npos);
            CHECK(journal.batch(batch).state == BatchState::Done);
            CHECK(journal.batch(batch).failed == 0);
        }
    }
}
#endif
