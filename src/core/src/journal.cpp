#include "storage/core/journal.hpp"

#include <sqlite3.h>

#include <chrono>
#include <stdexcept>

#include "storage/core/paths.hpp"

namespace storage {

namespace {

int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

[[noreturn]] void fail(sqlite3* db, const char* what) {
    throw std::runtime_error(std::string("journal: ") + what + ": " + (db ? sqlite3_errmsg(db) : "unknown error"));
}

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) fail(db, "prepare");
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt& bind(int i, int64_t v) {
        sqlite3_bind_int64(s_, i, v);
        return *this;
    }
    Stmt& bind(int i, const std::string& v) {
        sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    bool step() {
        const int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        fail(db_, "step");
    }
    void run() {
        step();
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
    }
    int64_t i64(int c) const { return sqlite3_column_int64(s_, c); }
    std::string text(int c) const {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s_, c));
        return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(s_, c))) : std::string();
    }

private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

}  // namespace

std::filesystem::path Journal::defaultPath() { return appDataDir() / "journal.db"; }

Journal::Journal(const std::filesystem::path& dbPath) {
    std::error_code ec;
    if (dbPath.has_parent_path()) std::filesystem::create_directories(dbPath.parent_path(), ec);
    const std::string path = toUtf8(dbPath);
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "cannot open";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("journal: open " + path + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 10000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=FULL");  // every state change is on disk before the file operation runs
    exec(R"sql(
        CREATE TABLE IF NOT EXISTS batches(
            id       INTEGER PRIMARY KEY AUTOINCREMENT,
            created  INTEGER NOT NULL,
            finished INTEGER NOT NULL DEFAULT 0,
            kind     TEXT    NOT NULL,
            title    TEXT    NOT NULL,
            volume   TEXT    NOT NULL,
            state    TEXT    NOT NULL
        );
        CREATE TABLE IF NOT EXISTS ops(
            id      INTEGER PRIMARY KEY AUTOINCREMENT,
            batch   INTEGER NOT NULL REFERENCES batches(id),
            seq     INTEGER NOT NULL,
            action  TEXT    NOT NULL,
            src     TEXT    NOT NULL,
            dst     TEXT    NOT NULL DEFAULT '',
            ref     TEXT    NOT NULL DEFAULT '',
            size    INTEGER NOT NULL DEFAULT 0,
            mtime   INTEGER NOT NULL DEFAULT 0,
            hash    TEXT    NOT NULL DEFAULT '',
            state   TEXT    NOT NULL,
            detail  TEXT    NOT NULL DEFAULT '',
            updated INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS ops_batch ON ops(batch, seq);
    )sql");
}

Journal::~Journal() { sqlite3_close(db_); }

void Journal::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "exec failed";
        sqlite3_free(err);
        throw std::runtime_error("journal: " + msg);
    }
}

int64_t Journal::createBatch(const std::string& kind, const std::string& title, const std::string& volume,
                             const std::vector<JournalOp>& ops) {
    exec("BEGIN IMMEDIATE");
    try {
        Stmt b(db_, "INSERT INTO batches(created,kind,title,volume,state) VALUES(?1,?2,?3,?4,?5)");
        b.bind(1, now()).bind(2, kind).bind(3, title).bind(4, volume).bind(5, std::string(BatchState::Planned)).run();
        const int64_t id = sqlite3_last_insert_rowid(db_);
        Stmt o(db_,
               "INSERT INTO ops(batch,seq,action,src,dst,ref,size,mtime,hash,state,updated) "
               "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
        int seq = 0;
        for (const auto& op : ops) {
            o.bind(1, id)
                .bind(2, seq++)
                .bind(3, op.action)
                .bind(4, op.src)
                .bind(5, op.dst)
                .bind(6, op.ref)
                .bind(7, static_cast<int64_t>(op.size))
                .bind(8, op.mtime)
                .bind(9, op.hash)
                .bind(10, std::string(OpState::Pending))
                .bind(11, now())
                .run();
        }
        exec("COMMIT");
        return id;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<JournalOp> Journal::ops(int64_t batch) {
    std::vector<JournalOp> out;
    Stmt s(db_, "SELECT id,batch,seq,action,src,dst,ref,size,mtime,hash,state,detail FROM ops WHERE batch=?1 ORDER BY seq");
    s.bind(1, batch);
    while (s.step()) {
        JournalOp op;
        op.id = s.i64(0);
        op.batch = s.i64(1);
        op.seq = static_cast<int>(s.i64(2));
        op.action = s.text(3);
        op.src = s.text(4);
        op.dst = s.text(5);
        op.ref = s.text(6);
        op.size = static_cast<uint64_t>(s.i64(7));
        op.mtime = s.i64(8);
        op.hash = s.text(9);
        op.state = s.text(10);
        op.detail = s.text(11);
        out.push_back(std::move(op));
    }
    return out;
}

void Journal::setOp(int64_t opId, const std::string& state, const std::string& detail, const std::string* dst) {
    if (dst) {
        Stmt s(db_, "UPDATE ops SET state=?2, detail=?3, dst=?4, updated=?5 WHERE id=?1");
        s.bind(1, opId).bind(2, state).bind(3, detail).bind(4, *dst).bind(5, now()).run();
    } else {
        Stmt s(db_, "UPDATE ops SET state=?2, detail=?3, updated=?4 WHERE id=?1");
        s.bind(1, opId).bind(2, state).bind(3, detail).bind(4, now()).run();
    }
}

void Journal::setBatchState(int64_t batch, const std::string& state) {
    const bool final = state == BatchState::Done || state == BatchState::Undone;
    Stmt s(db_, "UPDATE batches SET state=?2, finished=CASE WHEN ?3 THEN ?4 ELSE finished END WHERE id=?1");
    s.bind(1, batch).bind(2, state).bind(3, final ? 1 : 0).bind(4, now()).run();
}

namespace {
constexpr const char* kBatchSelect =
    "SELECT b.id,b.created,b.finished,b.kind,b.title,b.volume,b.state,"
    " COUNT(o.id),"
    " SUM(o.state='done'), SUM(o.state='failed'), SUM(o.state='skipped'), SUM(o.state='undone'),"
    " COALESCE(SUM(CASE WHEN o.state='done' THEN o.size ELSE 0 END),0)"
    " FROM batches b LEFT JOIN ops o ON o.batch=b.id ";

JournalBatch readBatch(Stmt& s) {
    JournalBatch b;
    b.id = s.i64(0);
    b.created = s.i64(1);
    b.finished = s.i64(2);
    b.kind = s.text(3);
    b.title = s.text(4);
    b.volume = s.text(5);
    b.state = s.text(6);
    b.total = static_cast<int>(s.i64(7));
    b.done = static_cast<int>(s.i64(8));
    b.failed = static_cast<int>(s.i64(9));
    b.skipped = static_cast<int>(s.i64(10));
    b.undone = static_cast<int>(s.i64(11));
    b.bytes = static_cast<uint64_t>(s.i64(12));
    return b;
}
}  // namespace

JournalBatch Journal::batch(int64_t id) {
    Stmt s(db_, (std::string(kBatchSelect) + "WHERE b.id=?1 GROUP BY b.id").c_str());
    s.bind(1, id);
    if (!s.step()) throw std::runtime_error("journal: no batch " + std::to_string(id));
    return readBatch(s);
}

std::vector<JournalBatch> Journal::batches(int limit) {
    std::vector<JournalBatch> out;
    Stmt s(db_, (std::string(kBatchSelect) + "GROUP BY b.id ORDER BY b.id DESC LIMIT ?1").c_str());
    s.bind(1, limit);
    while (s.step()) out.push_back(readBatch(s));
    return out;
}

std::vector<int64_t> Journal::runningBatches(const std::string& volume) {
    std::vector<int64_t> out;
    Stmt s(db_, "SELECT id FROM batches WHERE state=?1 AND (?2='' OR volume=?2) ORDER BY id");
    s.bind(1, std::string(BatchState::Running)).bind(2, volume);
    while (s.step()) out.push_back(s.i64(0));
    return out;
}

std::vector<std::string> Journal::volumesWithRunningBatches() {
    std::vector<std::string> out;
    Stmt s(db_, "SELECT DISTINCT volume FROM batches WHERE state=?1");
    s.bind(1, std::string(BatchState::Running));
    while (s.step()) out.push_back(s.text(0));
    return out;
}

}  // namespace storage
