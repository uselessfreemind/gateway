#include "scada/archive.hpp"
#include <filesystem>
#include <sqlite3.h>
#include <stdexcept>
namespace scada {
namespace {
void check(int rc, sqlite3 *db) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}
void execute(sqlite3 *db, const char *sql) {
    char *error{};
    auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error ? error : "SQLite failure";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}
struct Statement {
    sqlite3_stmt *value{};
    sqlite3 *db;
    Statement(sqlite3 *d, const char *sql) : db(d) {
        check(sqlite3_prepare_v2(db, sql, -1, &value, nullptr), db);
    }
    ~Statement() {
        sqlite3_finalize(value);
    }
    void text(int i, const std::string &s) {
        check(sqlite3_bind_text(value, i, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT), db);
    }
    void integer(int i, std::int64_t n) {
        check(sqlite3_bind_int64(value, i, n), db);
    }
    int step() {
        auto rc = sqlite3_step(value);
        check(rc, db);
        return rc;
    }
};
// Archive viewers use independent WAL readers instead of blocking the writer mutex.
struct ReadConnection {
    sqlite3 *db{};
    explicit ReadConnection(const std::string &path) {
        auto rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            std::string message = db ? sqlite3_errmsg(db) : "Cannot open archive reader";
            if (db)
                sqlite3_close(db);
            throw std::runtime_error(message);
        }
        sqlite3_busy_timeout(db, 5000);
    }
    ~ReadConnection() {
        sqlite3_close(db);
    }
};
std::string text(sqlite3_stmt *s, int col) {
    auto p = sqlite3_column_text(s, col);
    return p ? reinterpret_cast<const char *>(p) : "";
}
Json optional_number(sqlite3_stmt *s, int col) {
    return sqlite3_column_type(s, col) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_double(s, col));
}
void range(Statement &s, const std::string &c, const std::string &t, std::int64_t from, std::int64_t to) {
    if (from < 0 || to < from || to > 253402300799999LL)
        throw std::invalid_argument("Invalid history time range");
    s.text(1, c);
    s.text(2, t);
    s.integer(3, std::max(from, month_cutoff(utc_ms())));
    s.integer(4, to);
}
} // namespace
Archive::Archive(const std::string &path) : path_(path) {
    if (path.empty() || path == ":memory:")
        throw std::invalid_argument("Archive requires a persistent database path");
    auto parent = std::filesystem::path(std::u8string(path.begin(), path.end())).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    auto rc = sqlite3_open_v2(path.c_str(), &db_,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string e = db_ ? sqlite3_errmsg(db_) : "Cannot allocate SQLite connection";
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(e);
    }
    sqlite3_busy_timeout(db_, 5000);
    try {
        execute(db_,
                "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; CREATE TABLE IF NOT EXISTS "
                "native_samples(id INTEGER PRIMARY KEY,controller TEXT NOT NULL,tag TEXT NOT "
                "NULL,timestamp_ms INTEGER NOT NULL,value REAL,quality TEXT NOT NULL,error TEXT); CREATE "
                "INDEX IF NOT EXISTS ix_native_tag_time ON native_samples(controller,tag,timestamp_ms,id); "
                "CREATE INDEX IF NOT EXISTS ix_native_time ON native_samples(timestamp_ms);");
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}
Archive::~Archive() {
    if (db_)
        sqlite3_close(db_);
}
void Archive::append(const std::vector<Sample> &samples) {
    std::lock_guard lock(db_mutex_);
    execute(db_, "BEGIN IMMEDIATE");
    try {
        Statement s(db_, "INSERT INTO native_samples(controller,tag,timestamp_ms,value,quality,error) "
                         "VALUES(?,?,?,?,?,?)");
        for (auto &v : samples) {
            s.text(1, v.controller);
            s.text(2, v.tag);
            s.integer(3, v.timestamp_ms);
            if (v.value)
                check(sqlite3_bind_double(s.value, 4, *v.value), db_);
            else
                check(sqlite3_bind_null(s.value, 4), db_);
            s.text(5, v.quality);
            s.text(6, v.error);
            s.step();
            check(sqlite3_reset(s.value), db_);
            check(sqlite3_clear_bindings(s.value), db_);
        }
        execute(db_, "COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
std::int64_t Archive::purge(std::int64_t now) {
    std::int64_t total{};
    while (true) {
        std::lock_guard lock(db_mutex_);
        Statement s(db_, "DELETE FROM native_samples WHERE id IN (SELECT id FROM native_samples WHERE "
                         "timestamp_ms < ? LIMIT 10000)");
        s.integer(1, month_cutoff(now));
        s.step();
        auto n = sqlite3_changes(db_);
        total += n;
        if (n < 10000)
            break;
    }
    return total;
}
Json Archive::history(const std::string &c, const std::string &t, std::int64_t from, std::int64_t to,
                      int limit, std::int64_t after) {
    if (limit < 1 || limit > 10000 || after < 0)
        throw std::invalid_argument("Invalid history page");
    ReadConnection reader(path_);
    Statement s(reader.db,
                "SELECT id,timestamp_ms,value,quality,error FROM native_samples WHERE controller=? AND "
                "tag=? AND timestamp_ms>=? AND timestamp_ms<=? AND id>? ORDER BY id LIMIT ?");
    range(s, c, t, from, to);
    s.integer(5, after);
    s.integer(6, limit);
    Json rows = Json::array();
    std::int64_t next = after;
    while (s.step() == SQLITE_ROW) {
        next = sqlite3_column_int64(s.value, 0);
        rows.push_back({{"id", next},
                        {"timestampMs", sqlite3_column_int64(s.value, 1)},
                        {"value", optional_number(s.value, 2)},
                        {"quality", text(s.value, 3)},
                        {"error", text(s.value, 4)}});
    }
    return {{"samples", rows}, {"nextAfterId", rows.empty() ? Json(nullptr) : Json(next)}};
}
Json Archive::trend(const std::string &c, const std::string &t, std::int64_t from, std::int64_t to,
                    std::int64_t bucket) {
    if (bucket < 10 || from < 0 || to < from || to > 253402300799999LL || to / bucket - from / bucket > 1999)
        throw std::invalid_argument("Trend supports 10 ms minimum and at most 2000 buckets");
    ReadConnection reader(path_);
    Statement s(
        reader.db,
        "SELECT (timestamp_ms / ?5)*?5 AS bucket,COUNT(*),COUNT(value),MIN(value),MAX(value),AVG(value) "
        "FROM native_samples WHERE controller=?1 AND tag=?2 AND timestamp_ms>=?3 AND timestamp_ms<=?4 "
        "GROUP BY bucket ORDER BY bucket");
    range(s, c, t, from, to);
    s.integer(5, bucket);
    Json rows = Json::array();
    while (s.step() == SQLITE_ROW)
        rows.push_back({{"timestampMs", sqlite3_column_int64(s.value, 0)},
                        {"count", sqlite3_column_int64(s.value, 1)},
                        {"goodCount", sqlite3_column_int64(s.value, 2)},
                        {"min", optional_number(s.value, 3)},
                        {"max", optional_number(s.value, 4)},
                        {"average", optional_number(s.value, 5)}});
    return rows;
}
std::int64_t Archive::count() {
    std::lock_guard lock(db_mutex_);
    Statement s(db_, "SELECT COUNT(*) FROM native_samples");
    s.step();
    return sqlite3_column_int64(s.value, 0);
}
ArchiveQueue::ArchiveQueue(Archive &archive, SharedState &state, std::size_t capacity)
    : archive_(archive), state_(state), capacity_(capacity), writer_([this] { run(); }) {}
ArchiveQueue::~ArchiveQueue() {
    finish();
}
std::uint64_t ArchiveQueue::request_ticket() {
    std::lock_guard lock(mutex_);
    return next_ticket_++;
}
bool ArchiveQueue::try_push(const std::vector<Sample> &samples, std::uint64_t ticket) {
    std::lock_guard lock(mutex_);
    if (closing_ || failed_)
        return false;
    if (ticket != turn_ || samples.size() > capacity_ - size_)
        return false;
    queue_.push_back(samples);
    size_ += samples.size();
    ++turn_;
    changed_.notify_one();
    return true;
}
std::size_t ArchiveQueue::size() {
    std::lock_guard lock(mutex_);
    return size_;
}
void ArchiveQueue::finish() {
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    changed_.notify_all();
    if (writer_.joinable())
        writer_.join();
}
void ArchiveQueue::run() {
    try {
        archive_.purge(utc_ms());
        auto next = std::chrono::steady_clock::now() + std::chrono::hours(1);
        while (true) {
            std::vector<Sample> batch;
            {
                std::unique_lock lock(mutex_);
                changed_.wait_until(lock, next, [this] { return closing_ || !queue_.empty(); });
                if (queue_.empty() && closing_)
                    break;
                while (!queue_.empty() && (batch.empty() || batch.size() + queue_.front().size() <= 10000)) {
                    auto &q = queue_.front();
                    size_ -= q.size();
                    batch.insert(batch.end(), std::make_move_iterator(q.begin()),
                                 std::make_move_iterator(q.end()));
                    queue_.pop_front();
                }
            }
            if (!batch.empty()) {
                archive_.append(batch);
                std::lock_guard lock(state_.mutex);
                state_.archived += batch.size();
            }
            if (std::chrono::steady_clock::now() >= next) {
                archive_.purge(utc_ms());
                next = std::chrono::steady_clock::now() + std::chrono::hours(1);
            }
        }
    } catch (const std::exception &ex) {
        {
            std::lock_guard lock(state_.mutex);
            state_.archive_error = ex.what();
        }
        {
            std::lock_guard lock(mutex_);
            failed_ = true;
            closing_ = true;
        }
        changed_.notify_all();
    }
}
} // namespace scada
