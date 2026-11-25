#include "scada/engine.hpp"
#include "scada/secs.hpp"
#include <cmath>
#include <iostream>
#ifdef _WIN32
#include <mmsystem.h>
#endif
namespace scada {
Engine::Engine(Config config)
    : config_(std::move(config)), archive_(config_.database),
      queue_(archive_, state_, config_.queue_capacity) {
    validate(config_);
    for (const auto &controller : config_.controllers)
        plans_.push_back(read_plan(controller));
}
Engine::~Engine() {
    stop();
}
void Engine::start() {
    if (started_)
        throw std::logic_error("Engine already started");
    started_ = true;
#ifdef _WIN32
    timer_resolution_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif

    for (std::size_t i = 0; i < config_.controllers.size(); ++i) {
        auto executor = asio::make_strand(io_);
        auto client = make_plc(executor, config_.controllers[i]);
        auto timer = std::make_shared<asio::steady_timer>(executor);
        clients_.push_back(client);
        generations_.push_back(std::make_shared<std::atomic<std::uint64_t>>(0));
        timers_.push_back(timer);
        {
            std::lock_guard lock(state_.mutex);
            state_.controllers[config_.controllers[i].id] = {};
        }
        asio::co_spawn(executor, poll(i, client, timer), [this](std::exception_ptr error) {
            if (error && !stopping_) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception &ex) {
                    std::lock_guard lock(state_.mutex);
                    state_.archive_error = std::string("Acquisition worker failed: ") + ex.what();
                }
                stopping_ = true;
            }
        });
    }
    if (config_.secs.enabled) {
        auto executor = asio::make_strand(io_);
        hsms_ = std::make_shared<HsmsServer>(executor, config_, state_);
        auto server = hsms_;
        asio::co_spawn(executor, server->run(), [this, server](std::exception_ptr e) {
            if (e && !stopping_) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception &ex) {
                    std::lock_guard lock(state_.mutex);
                    state_.secs_error = ex.what();
                }
            }
        });
    }
    for (unsigned i = 0; i < config_.io_threads; ++i)
        threads_.emplace_back([this] { io_.run(); });
}
void Engine::retry(const std::string &controller) {
    if (stopping_) throw std::logic_error("Engine is stopped");
    for (std::size_t i = 0; i < config_.controllers.size(); ++i) {
        if (config_.controllers[i].id != controller) continue;
        auto generation = generations_.at(i);
        auto client = clients_.at(i);
        auto timer = timers_.at(i);
        asio::post(timer->get_executor(), [client, timer, generation] { ++*generation; client->close(); timer->cancel(); });
        return;
    }
    throw std::invalid_argument("Unknown controller");
}
void Engine::restart_sampling() {
    for (const auto &c : config_.controllers) retry(c.id);
}
void Engine::stop() {
    if (!started_)
        return;
    stopping_ = true;
    if (config_.diagnostics)
        std::clog << "Shutdown: cancelling network operations\n" << std::flush;
    for (std::size_t i = 0; i < clients_.size(); ++i) {
        auto client = clients_[i];
        auto timer = timers_[i];
        asio::post(timer->get_executor(), [client, timer] {
            client->close();
            timer->cancel();
        });
    }
    if (hsms_) {
        auto server = hsms_;
        asio::post(io_, [server] { server->close(); });
    }
    work_.reset();
    if (config_.diagnostics)
        std::clog << "Shutdown: joining I/O workers\n" << std::flush;
    for (auto &t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
    if (config_.diagnostics)
        std::clog << "Shutdown: draining archive, queued=" << queue_.size() << "\n" << std::flush;
    queue_.finish();
    if (config_.diagnostics)
        std::clog << "Shutdown: archive drained\n" << std::flush;
#ifdef _WIN32
    if (timer_resolution_) {
        timeEndPeriod(1);
        timer_resolution_ = false;
    }
#endif
    started_ = false;
}
asio::awaitable<void> Engine::poll(std::size_t index, std::shared_ptr<PlcClient> client,
                                   std::shared_ptr<asio::steady_timer> timer) {
    auto &c = config_.controllers[index];
    const auto &plan = plans_.at(index);
    using Clock = std::chrono::steady_clock;
    auto interval = std::chrono::milliseconds(c.interval_ms);
    auto due = Clock::now() + std::chrono::microseconds((index * 997) % 10000);
    Status status;
    std::uint64_t generation = 0;
    unsigned failures = 0;
    asio::error_code timer_error;
    try {
        while (!stopping_) {
            if (generation != generations_[index]->load()) {
                generation = generations_[index]->load();
                client->close();
                failures = 0;
                status.connection = "Connecting";
                status.retry_attempts = 0;
                status.connection_error.clear();
                due = Clock::now();
                std::lock_guard lock(state_.mutex);
                state_.controllers[c.id] = status;
            }
            if (failures >= 4) {
                timer->expires_after(std::chrono::seconds(1));
                co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, timer_error));
                continue;
            }
            timer->expires_at(due);
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, timer_error));
            if (stopping_)
                break;
            if (generation != generations_[index]->load()) continue;
            status.retry_attempts = failures;
            { std::lock_guard lock(state_.mutex); state_.controllers[c.id] = status; }
            auto began = Clock::now();
            status.jitter_ms = std::chrono::duration<double, std::milli>(began - due).count();
            status.last_attempt = utc_ms();
            std::vector<Sample> samples;
            bool interrupted = false;
            for (auto &batch : plan) {
                if (stopping_)
                    break;
                std::vector<Sample> reads;
                if (!interrupted) {
                    try {
                        reads = co_await client->read(batch);
                    } catch (const std::exception &ex) {
                        if (stopping_)
                            break;
                        for (auto i : batch) {
                            auto &t = c.tags[i];
                            reads.push_back({c.id, t.id, t.svid, utc_ms(), {}, "Bad", ex.what()});
                        }
                        interrupted = true;
                        status.connection_error = ex.what();
                        client->close();
                    }
                } else
                    for (auto i : batch) {
                        auto &t = c.tags[i];
                        reads.push_back({c.id,
                                         t.id,
                                         t.svid,
                                         utc_ms(),
                                         {},
                                         "Bad",
                                         "Scan interrupted after transport failure"});
                    }
                for (std::size_t k = 0; k < reads.size(); ++k) {
                    auto &s = reads[k];
                    auto &tag = c.tags[batch[k]];
                    if (s.value) {
                        auto scaled = *s.value * tag.scale + tag.offset;
                        if (std::isfinite(scaled))
                            s.value = scaled;
                        else {
                            s.value.reset();
                            s.quality = "Bad";
                            s.error = "Non-finite value";
                        }
                    }
                    if (s.quality != "Good")
                        ++status.failed_reads;
                    else
                        status.last_success = s.timestamp_ms;
                    samples.push_back(std::move(s));
                }
            }
            if (stopping_)
                break;
            if (generation != generations_[index]->load()) continue;
            if (interrupted) {
                ++failures;
                status.connection = failures >= 4 ? "Aborted" : "Reconnecting";
                status.retry_attempts = std::min(3u, failures - 1);
            } else {
                failures = 0;
                status.connection = "Connected";
                status.retry_attempts = 0;
                status.connection_error.clear();
            }
            { std::lock_guard lock(state_.mutex); state_.controllers[c.id] = status; }
            // Never block an I/O thread on disk. Backpressure yields this controller's coroutine.
            {
                std::lock_guard lock(state_.mutex);
                for (const auto &sample : samples)
                    state_.latest[sample.svid] = sample;
            }
            auto ticket = queue_.request_ticket();
            while (!queue_.try_push(samples, ticket)) {
                if (generation != generations_[index]->load()) { queue_.cancel_ticket(ticket); break; }
                if (stopping_)
                    break;
                {
                    std::lock_guard lock(state_.mutex);
                    ++state_.backpressure;
                    if (!state_.archive_error.empty()) {
                        stopping_ = true;
                        break;
                    }
                }
                timer->expires_after(std::chrono::milliseconds(5));
                co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, timer_error));
            }
            if (stopping_)
                break;
            if (generation != generations_[index]->load()) continue;
            ++status.scans;
            status.scan_ms = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
            auto next = due + interval;
            if (Clock::now() > next) {
                auto late = Clock::now() - next;
                auto missed = 1 + late / interval;
                status.missed_deadlines += static_cast<std::uint64_t>(missed);
                next += interval * missed;
            }
            {
                std::lock_guard lock(state_.mutex);
                state_.controllers[c.id] = status;
            }
            due = failures > 0 ? Clock::now() + std::chrono::seconds(1) : next;
        }
    } catch (const asio::system_error &) {
        if (!stopping_)
            throw;
    }
}
Json Engine::snapshot(const std::string &filter) const {
    Json out;
    std::lock_guard lock(state_.mutex);
    out["archivedSamples"] = state_.archived;
    out["backpressureEvents"] = state_.backpressure;
    out["archiveError"] = state_.archive_error;
    out["totalLatest"] = state_.latest.size();
    out["controllers"] = Json::array();
    out["samples"] = Json::array();
    out["secs"] = {{"connected", state_.connected},
                   {"selected", state_.selected},
                   {"communicating", state_.communicating},
                   {"online", state_.online},
                   {"error", state_.secs_error}};
    auto now = utc_ms();
    for (auto &c : config_.controllers) {
        auto it = state_.controllers.find(c.id);
        auto s = it == state_.controllers.end() ? Status{} : it->second;
        out["controllers"].push_back({{"id", c.id},
                                      {"intervalMs", c.interval_ms},
                                      {"connectionState", s.connection},
                                      {"retryAttempts", s.retry_attempts},
                                      {"connectionError", s.connection_error},
                                      {"tagCount", c.tags.size()},
                                      {"scans", s.scans},
                                      {"failedReads", s.failed_reads},
                                      {"missedDeadlines", s.missed_deadlines},
                                      {"scanMs", s.scan_ms},
                                      {"jitterMs", s.jitter_ms},
                                      {"lastAttemptMs", s.last_attempt},
                                      {"lastSuccessMs", s.last_success}});
        if (!filter.empty() && filter != c.id)
            continue;
        for (auto &t : c.tags) {
            auto it2 = state_.latest.find(t.svid);
            if (it2 == state_.latest.end())
                continue;
            auto v = it2->second;
            if (v.quality == "Good" && now - v.timestamp_ms > 2LL * c.interval_ms + c.timeout_ms) {
                v.value.reset();
                v.quality = "Stale";
                v.error = "No recent sample";
            }
            Json sample = v;
            sample["unit"] = t.unit;
            sample["address"] = t.address;
            sample["analogue"] = t.type != Type::boolean;
            out["samples"].push_back(std::move(sample));
        }
    }
    return out;
}
} // namespace scada
