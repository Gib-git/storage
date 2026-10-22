#include "storage/core/index.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

namespace storage {

namespace {

constexpr int kSchemaVersion = 1;

[[noreturn]] void fail(sqlite3* db, const char* what) {
    throw std::runtime_error(std::string("index: ") + what + ": " + (db ? sqlite3_errmsg(db) : "unknown error"));
}

// Minimal RAII prepared statement.
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
        reset();
    }
    void reset() {
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
    }
    int64_t i64(int col) const { return sqlite3_column_int64(s_, col); }
    std::string text(int col) const {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s_, col));
        return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(s_, col))) : std::string();
    }

private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

}  // namespace

Index::Index(const std::filesystem::path& dbPath) {
    std::error_code ec;
    if (dbPath.has_parent_path()) std::filesystem::create_directories(dbPath.parent_path(), ec);
    const auto u8 = dbPath.u8string();
    const std::string path(u8.begin(), u8.end());
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "cannot open";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("index: open " + path + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");

    int version = 0;
    {
        Stmt s(db_, "PRAGMA user_version");
        if (s.step()) version = static_cast<int>(s.i64(0));
    }
    if (version < 1) {
        exec(R"sql(
            CREATE TABLE IF NOT EXISTS scans(
                id       INTEGER PRIMARY KEY AUTOINCREMENT,
                root     TEXT    NOT NULL,
                volume   TEXT    NOT NULL DEFAULT 'local',
                started  INTEGER NOT NULL,
                finished INTEGER NOT NULL,
                files    INTEGER NOT NULL DEFAULT 0,
                dirs     INTEGER NOT NULL DEFAULT 0,
                bytes    INTEGER NOT NULL DEFAULT 0,
                errors   INTEGER NOT NULL DEFAULT 0,
                complete INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS scans_root ON scans(volume, root);
            CREATE TABLE IF NOT EXISTS nodes(
                scan_id INTEGER NOT NULL REFERENCES scans(id) ON DELETE CASCADE,
                id      INTEGER NOT NULL,
                parent  INTEGER NOT NULL,
                name    TEXT    NOT NULL,
                kind    INTEGER NOT NULL,
                size    INTEGER NOT NULL,
                mtime   INTEGER NOT NULL,
                dev     INTEGER NOT NULL,
                ino     INTEGER NOT NULL,
                nlink   INTEGER NOT NULL,
                flags   INTEGER NOT NULL,
                PRIMARY KEY(scan_id, id)
            ) WITHOUT ROWID;
        )sql");
    }
    if (version < kSchemaVersion) exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
}

Index::~Index() {
    sqlite3_close(db_);
}

void Index::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "exec failed";
        sqlite3_free(err);
        throw std::runtime_error("index: " + msg);
    }
}

int64_t Index::save(ScanTree& tree) {
    if (!tree.root) throw std::runtime_error("index: empty tree");
    const Node& root = *tree.root;

    exec("BEGIN IMMEDIATE");
    try {
        {
            Stmt old(db_, "DELETE FROM scans WHERE volume=?1 AND root=?2");
            old.bind(1, tree.volume).bind(2, root.name).run();
        }
        {
            Stmt ins(db_,
                     "INSERT INTO scans(root,volume,started,finished,files,dirs,bytes,errors,complete) "
                     "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
            ins.bind(1, root.name)
                .bind(2, tree.volume)
                .bind(3, tree.started)
                .bind(4, tree.finished)
                .bind(5, static_cast<int64_t>(root.files))
                .bind(6, static_cast<int64_t>(root.dirs))
                .bind(7, static_cast<int64_t>(root.size))
                .bind(8, static_cast<int64_t>(root.errors))
                .bind(9, tree.complete ? 1 : 0)
                .run();
        }
        const int64_t scanId = sqlite3_last_insert_rowid(db_);

        // Pre-order ids guarantee parent id < child id, which load() relies on.
        Stmt ins(db_,
                 "INSERT INTO nodes(scan_id,id,parent,name,kind,size,mtime,dev,ino,nlink,flags) "
                 "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
        int64_t nextId = 0;
        std::vector<std::pair<const Node*, int64_t>> stack{{&root, -1}};
        while (!stack.empty()) {
            auto [n, parent] = stack.back();
            stack.pop_back();
            const int64_t id = nextId++;
            ins.bind(1, scanId)
                .bind(2, id)
                .bind(3, parent)
                .bind(4, n->name)
                .bind(5, static_cast<int64_t>(n->kind))
                .bind(6, n->isDir() ? 0 : static_cast<int64_t>(n->size))
                .bind(7, n->mtime)
                .bind(8, static_cast<int64_t>(n->dev))
                .bind(9, static_cast<int64_t>(n->ino))
                .bind(10, static_cast<int64_t>(n->nlink))
                .bind(11, static_cast<int64_t>(n->flags))
                .run();
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.emplace_back(it->get(), id);
        }
        exec("COMMIT");
        tree.id = scanId;
        return scanId;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<ScanInfo> Index::scans() {
    std::vector<ScanInfo> out;
    Stmt s(db_,
           "SELECT id,root,volume,started,finished,files,dirs,bytes,errors,complete FROM scans "
           "ORDER BY finished DESC");
    while (s.step()) {
        ScanInfo i;
        i.id = s.i64(0);
        i.root = s.text(1);
        i.volume = s.text(2);
        i.started = s.i64(3);
        i.finished = s.i64(4);
        i.files = static_cast<uint64_t>(s.i64(5));
        i.dirs = static_cast<uint64_t>(s.i64(6));
        i.bytes = static_cast<uint64_t>(s.i64(7));
        i.errors = static_cast<uint64_t>(s.i64(8));
        i.complete = s.i64(9) != 0;
        out.push_back(std::move(i));
    }
    return out;
}

int64_t Index::findScan(const std::string& root, const std::string& volume) {
    Stmt s(db_, "SELECT id FROM scans WHERE volume=?1 AND root=?2 ORDER BY finished DESC LIMIT 1");
    s.bind(1, volume).bind(2, root);
    return s.step() ? s.i64(0) : 0;
}

std::unique_ptr<ScanTree> Index::load(int64_t scanId) {
    auto tree = std::make_unique<ScanTree>();
    {
        Stmt s(db_, "SELECT volume,started,finished,complete FROM scans WHERE id=?1");
        s.bind(1, scanId);
        if (!s.step()) return nullptr;
        tree->volume = s.text(0);
        tree->started = s.i64(1);
        tree->finished = s.i64(2);
        tree->complete = s.i64(3) != 0;
        tree->id = scanId;
    }

    std::vector<Node*> byId;
    Stmt s(db_,
           "SELECT id,parent,name,kind,size,mtime,dev,ino,nlink,flags FROM nodes WHERE scan_id=?1 ORDER BY id");
    s.bind(1, scanId);
    while (s.step()) {
        auto n = std::make_unique<Node>();
        const int64_t id = s.i64(0);
        const int64_t parent = s.i64(1);
        n->name = s.text(2);
        n->kind = static_cast<NodeKind>(s.i64(3));
        n->size = static_cast<uint64_t>(s.i64(4));
        n->mtime = s.i64(5);
        n->dev = static_cast<uint64_t>(s.i64(6));
        n->ino = static_cast<uint64_t>(s.i64(7));
        n->nlink = static_cast<uint32_t>(s.i64(8));
        n->flags = static_cast<uint8_t>(s.i64(9));

        if (static_cast<size_t>(id) != byId.size()) throw std::runtime_error("index: corrupt node ids");
        byId.push_back(n.get());
        if (parent < 0) {
            tree->root = std::move(n);
        } else {
            if (static_cast<size_t>(parent) >= byId.size()) throw std::runtime_error("index: corrupt parent id");
            Node* p = byId[static_cast<size_t>(parent)];
            n->parent = p;
            p->children.push_back(std::move(n));
        }
    }
    if (!tree->root) return nullptr;
    tree->finalize();
    return tree;
}

void Index::remove(int64_t scanId) {
    Stmt s(db_, "DELETE FROM scans WHERE id=?1");
    s.bind(1, scanId).run();
}

}  // namespace storage
