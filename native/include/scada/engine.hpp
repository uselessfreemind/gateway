#pragma once
#include "archive.hpp"
#include "protocol.hpp"
#include <atomic>
namespace scada {
class HsmsServer;
class Engine {
    Config config_;
    SharedState state_;
    Archive archive_;
    ArchiveQueue queue_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_{asio::make_work_guard(io_)};
    std::vector<std::vector<std::vector<std::size_t>>> plans_;
    std::vector<std::jthread> threads_;
    std::vector<std::shared_ptr<PlcClient>> clients_;
    std::vector<std::shared_ptr<asio::steady_timer>> timers_;
    std::shared_ptr<HsmsServer> hsms_;
    std::atomic<bool> stopping_{false};
    bool started_{};
#ifdef _WIN32
    bool timer_resolution_{};
#endif
    asio::awaitable<void> poll(std::size_t index, std::shared_ptr<PlcClient> client,
                               std::shared_ptr<asio::steady_timer> timer);

  public:
    explicit Engine(Config config);
    ~Engine();
    void start();
    void stop();
    Json snapshot(const std::string &controller = "") const;
    Json history(const std::string &c, const std::string &t, std::int64_t from, std::int64_t to, int limit,
                 std::int64_t after) {
        return archive_.history(c, t, from, to, limit, after);
    }
    Json trend(const std::string &c, const std::string &t, std::int64_t from, std::int64_t to,
               std::int64_t bucket) {
        return archive_.trend(c, t, from, to, bucket);
    }
    std::int64_t archived_rows() {
        return archive_.count();
    }
};
} // namespace scada
