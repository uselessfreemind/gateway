#pragma once
#include "model.hpp"
#include <condition_variable>
#include <deque>
#include <thread>
struct sqlite3;
namespace scada {
class Archive {
    sqlite3 *db_{};
    std::string path_;
    mutable std::mutex db_mutex_;

  public:
    explicit Archive(const std::string &path);
    ~Archive();
    Archive(const Archive &) = delete;
    void append(const std::vector<Sample> &samples);
    std::int64_t purge(std::int64_t now);
    Json history(const std::string &controller, const std::string &tag, std::int64_t from, std::int64_t to,
                 int limit, std::int64_t after = 0);
    Json trend(const std::string &controller, const std::string &tag, std::int64_t from, std::int64_t to,
               std::int64_t bucket);
    std::int64_t count();
};
class ArchiveQueue {
    Archive &archive_;
    SharedState &state_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::vector<Sample>> queue_;
    std::size_t size_{};
    std::uint64_t next_ticket_{}, turn_{};
    bool closing_{}, failed_{};
    std::jthread writer_;
    void run();

  public:
    ArchiveQueue(Archive &archive, SharedState &state, std::size_t capacity);
    ~ArchiveQueue();
    std::uint64_t request_ticket();
    bool try_push(const std::vector<Sample> &samples, std::uint64_t ticket);
    std::size_t size();
    void finish();
};
} // namespace scada
