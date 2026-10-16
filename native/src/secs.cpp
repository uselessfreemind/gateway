#include "scada/secs.hpp"
#include "scada/protocol.hpp"
#include <algorithm>
#include <bit>
#include <deque>
#include <stdexcept>
namespace scada {
Item Item::L(std::vector<Item> children) {
    return {0, std::move(children), {}};
}
Item Item::U(std::uint32_t value) {
    Item i;
    i.format = 0xb0;
    put_be32(i.data, value);
    return i;
}
Item Item::B(std::uint8_t value) {
    return {0x20, {}, Bytes{value}};
}
Item Item::A(const std::string &v) {
    return {0x40, {}, Bytes(v.begin(), v.end())};
}
Item Item::F(double value) {
    Item i;
    i.format = 0x80;
    auto bits = std::bit_cast<std::uint64_t>(value);
    put_be32(i.data, static_cast<std::uint32_t>(bits >> 32));
    put_be32(i.data, static_cast<std::uint32_t>(bits));
    return i;
}
std::uint32_t Item::uint() const {
    if (format != 0xb0 || data.size() != 4)
        throw std::runtime_error("Expected scalar U4");
    return be32(data, 0);
}
bool Item::flag() const {
    if (format != 0x24 || data.size() != 1 || data[0] > 1)
        throw std::runtime_error("Expected scalar BOOLEAN");
    return data[0] != 0;
}
Bytes Item::encode() const {
    auto n = format == 0 ? list.size() : data.size();
    if (n > 0xffffff)
        throw std::runtime_error("SECS item too large");
    unsigned bytes = n <= 255 ? 1 : n <= 65535 ? 2 : 3;
    Bytes b{static_cast<std::uint8_t>(format | bytes)};
    for (unsigned i = 0; i < bytes; ++i)
        b.push_back(static_cast<std::uint8_t>(n >> (8 * (bytes - 1 - i))));
    if (format == 0)
        for (auto &i : list) {
            auto child = i.encode();
            b.insert(b.end(), child.begin(), child.end());
        }
    else
        b.insert(b.end(), data.begin(), data.end());
    return b;
}
static Item parse_item(std::span<const std::uint8_t> b, std::size_t &pos, int depth) {
    if (depth > 16 || pos >= b.size())
        throw std::runtime_error("Invalid SECS nesting");
    auto h = b[pos++];
    unsigned n = h & 3;
    auto format = static_cast<std::uint8_t>(h & 0xfc);
    if (!n || pos + n > b.size())
        throw std::runtime_error("Invalid SECS length");
    std::size_t size{};
    for (unsigned i = 0; i < n; ++i)
        size = (size << 8) | b[pos++];
    Item out;
    out.format = format;
    if (!format) {
        if (size > 10000)
            throw std::runtime_error("SECS list exceeds limit");
        for (std::size_t i = 0; i < size; ++i)
            out.list.push_back(parse_item(b, pos, depth + 1));
    } else {
        if (size > b.size() - pos)
            throw std::runtime_error("Truncated SECS value");
        if (format != 0x20 && format != 0x24 && format != 0x40 && format != 0xb0 && format != 0x80)
            throw std::runtime_error("Unsupported SECS format");
        if ((format == 0xb0 && size % 4) || (format == 0x80 && size % 8))
            throw std::runtime_error("Misaligned SECS numeric data");
        out.data.assign(b.begin() + static_cast<std::ptrdiff_t>(pos),
                        b.begin() + static_cast<std::ptrdiff_t>(pos + size));
        pos += size;
    }
    return out;
}
Item Item::decode(std::span<const std::uint8_t> b) {
    std::size_t pos{};
    auto item = parse_item(b, pos, 0);
    if (pos != b.size())
        throw std::runtime_error("Trailing SECS data");
    return item;
}
Bytes Frame::encode() const {
    if (body.size() > 1048566)
        throw std::runtime_error("HSMS frame exceeds 1 MiB");
    Bytes b;
    put_be32(b, static_cast<std::uint32_t>(body.size() + 10));
    put_be16(b, session);
    b.push_back(static_cast<std::uint8_t>(stream | (w ? 0x80 : 0)));
    b.push_back(function);
    b.push_back(0);
    b.push_back(stype);
    put_be32(b, system);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}
Frame Frame::decode(std::span<const std::uint8_t> b) {
    if (b.size() < 14 || b.size() > 1048580 || be32(b, 0) != b.size() - 4 || b[8] != 0)
        throw std::runtime_error("Invalid HSMS frame");
    return {be16(b, 4),  static_cast<std::uint8_t>(b[6] & 0x7f), b[7], b[9], (b[6] & 0x80) != 0,
            be32(b, 10), Bytes(b.begin() + 14, b.end())};
}
Frame Frame::control(std::uint8_t type, std::uint32_t system, std::uint8_t status) {
    return {0xffff, 0, status, type, false, system, {}};
}
static const std::vector<Item> &items(const Item &i, std::optional<std::size_t> n = {}) {
    if (i.format != 0 || (n && i.list.size() != *n))
        throw std::runtime_error("Invalid SECS list shape");
    return i.list;
}
static std::vector<std::uint32_t> ids(const Item &i) {
    std::vector<std::uint32_t> out;
    for (auto &v : items(i))
        out.push_back(v.uint());
    return out;
}
GemModel::GemModel(const Config &c, SharedState &s) : config_(c), state_(s) {
    for (auto &p : c.controllers)
        for (auto &t : p.tags)
            tags_.emplace(t.svid, std::pair{&p, &t});
}
Item GemModel::value(std::uint32_t id) {
    auto t = tags_.find(id);
    if (t == tags_.end())
        return Item::L();
    std::lock_guard lock(state_.mutex);
    auto v = state_.latest.find(id);
    if (v == state_.latest.end() || v->second.quality != "Good" || !v->second.value ||
        utc_ms() - v->second.timestamp_ms > 2LL * t->second.first->interval_ms + t->second.first->timeout_ms)
        return Item::L();
    return Item::F(*v->second.value);
}
std::pair<std::uint8_t, std::optional<Item>> GemModel::handle(std::uint8_t s, std::uint8_t f,
                                                              const std::optional<Item> &body) {
    auto required = [&]() -> const Item & {
        if (!body)
            throw std::runtime_error("Missing SECS body");
        return *body;
    };
    if (s == 1 && f == 1)
        return {2, Item::L({Item::A("OmronNative"), Item::A("0.1.0")})};
    if (s == 1 && f == 13) {
        items(required(), 0);
        communicating = true;
        return {14, Item::L({Item::B(0), Item::L({Item::A("OmronNative"), Item::A("0.1.0")})})};
    }
    if (!communicating)
        throw std::runtime_error("S1F13 required first");
    if (s == 1 && (f == 3 || f == 11)) {
        auto requested = ids(required());
        if (requested.empty())
            for (auto &[id, _] : tags_)
                requested.push_back(id);
        std::vector<Item> result;
        for (auto id : requested) {
            if (f == 3)
                result.push_back(value(id));
            else {
                auto t = tags_.find(id);
                result.push_back(Item::L(
                    {Item::U(id),
                     Item::A(t == tags_.end() ? "" : t->second.first->id + "." + t->second.second->id),
                     Item::A(t == tags_.end() ? "" : t->second.second->unit)}));
            }
        }
        return {static_cast<std::uint8_t>(f + 1), Item::L(std::move(result))};
    }
    if (s == 1 && f == 15) {
        online = false;
        return {16, Item::B(0)};
    }
    if (s == 1 && f == 17) {
        auto ack = online ? 2 : 0;
        online = true;
        return {18, Item::B(static_cast<std::uint8_t>(ack))};
    }
    if (s == 2 && f == 33) {
        auto &d = items(required(), 2);
        d[0].uint();
        auto &definitions = items(d[1]);
        if (definitions.empty()) {
            reports_.clear();
            linked_.clear();
            return {34, Item::B(0)};
        }
        auto proposed = reports_;
        for (auto &entry : definitions) {
            auto &p = items(entry, 2);
            auto id = p[0].uint();
            auto vids = ids(p[1]);
            if (std::any_of(vids.begin(), vids.end(), [&](auto v) { return !tags_.contains(v); }))
                return {34, Item::B(4)};
            if (vids.empty())
                proposed.erase(id);
            else if (proposed.contains(id))
                return {34, Item::B(3)};
            else
                proposed[id] = std::move(vids);
        }
        std::size_t count{};
        for (auto &[_, v] : proposed)
            count += v.size();
        if (proposed.size() > 100 || count > 10000)
            return {34, Item::B(1)};
        reports_ = std::move(proposed);
        std::erase_if(linked_, [&](auto v) { return !reports_.contains(v); });
        return {34, Item::B(0)};
    }
    if (s == 2 && f == 35) {
        auto &d = items(required(), 2);
        d[0].uint();
        auto &definitions = items(d[1]);
        if (definitions.empty()) {
            linked_.clear();
            return {36, Item::B(0)};
        }
        if (definitions.size() != 1)
            return {36, Item::B(4)};
        auto &link = items(definitions[0], 2);
        if (link[0].uint() != 1001)
            return {36, Item::B(4)};
        auto proposed = ids(link[1]);
        for (auto id : proposed)
            if (!reports_.contains(id))
                return {36, Item::B(5)};
        auto sorted = proposed;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            return {36, Item::B(3)};
        linked_ = std::move(proposed);
        return {36, Item::B(0)};
    }
    if (s == 2 && f == 37) {
        auto &d = items(required(), 2);
        auto flag = d[0].flag();
        for (auto id : ids(d[1]))
            if (id != 1001)
                return {38, Item::B(1)};
        enabled_ = flag;
        return {38, Item::B(0)};
    }
    if (s == 2 && f == 41)
        return {42, Item::L({Item::B(1), Item::L()})};
    return {0, std::nullopt};
}
std::optional<Item> GemModel::event() {
    if (!communicating || !online || !enabled_ || linked_.empty())
        return {};
    std::vector<Item> reports;
    for (auto id : linked_) {
        std::vector<Item> values;
        for (auto vid : reports_.at(id))
            values.push_back(value(vid));
        reports.push_back(Item::L({Item::U(id), Item::L(std::move(values))}));
    }
    return Item::L({Item::U(0), Item::U(1001), Item::L(std::move(reports))});
}
namespace {
class Session : public std::enable_shared_from_this<Session> {
    using Clock = std::chrono::steady_clock;
    struct Pending {
        std::uint8_t stream, function, stype;
        Clock::time_point deadline;
    };
    std::shared_ptr<asio::ip::tcp::socket> socket_;
    const Config &config_;
    SharedState &state_;
    GemModel model_;
    asio::steady_timer timer_;
    std::shared_ptr<asio::steady_timer> frame_timer_;
    std::deque<Bytes> writes_;
    std::map<std::uint32_t, Pending> pending_;
    std::uint32_t system_{};
    bool writing_{}, failed_{}, reader_done_{}, selected_{};
    std::string error_;
    Clock::time_point connected_ = Clock::now(), last_link_ = Clock::now(), last_event_ = Clock::now();
    void fail(const std::string &reason) {
        if (failed_)
            return;
        failed_ = true;
        error_ = reason;
        asio::error_code ec;
        socket_->close(ec);
        timer_.cancel();
        if (frame_timer_)
            frame_timer_->cancel();
    }
    void send(Frame f) {
        writes_.push_back(f.encode());
        if (writing_)
            return;
        writing_ = true;
        auto self = shared_from_this();
        asio::co_spawn(
            socket_->get_executor(),
            [self]() -> asio::awaitable<void> {
                try {
                    while (!self->writes_.empty() && !self->failed_) {
                        auto deadline = std::make_shared<asio::steady_timer>(self->socket_->get_executor());
                        deadline->expires_after(std::chrono::milliseconds(self->config_.secs.t8_ms));
                        auto socket = self->socket_;
                        deadline->async_wait([socket](asio::error_code ec) {
                            if (!ec) {
                                asio::error_code ignored;
                                socket->close(ignored);
                            }
                        });
                        try {
                            co_await asio::async_write(*self->socket_, asio::buffer(self->writes_.front()),
                                                       asio::use_awaitable);
                        } catch (...) {
                            deadline->cancel();
                            throw;
                        }
                        deadline->cancel();
                        self->writes_.pop_front();
                    }
                } catch (const std::exception &e) {
                    self->fail(e.what());
                }
                self->writing_ = false;
            },
            asio::detached);
    }
    void transaction(Frame f, std::uint8_t response_type, int timeout) {
        pending_[f.system] = {f.stream, static_cast<std::uint8_t>(f.function + 1), response_type,
                              Clock::now() + std::chrono::milliseconds(timeout)};
        send(std::move(f));
    }
    asio::awaitable<Frame> receive() {
        Bytes prefix(4);
        co_await asio::async_read(*socket_, asio::buffer(prefix.data(), 1), asio::use_awaitable);
        frame_timer_ = std::make_shared<asio::steady_timer>(socket_->get_executor());
        frame_timer_->expires_after(std::chrono::milliseconds(config_.secs.t8_ms));
        auto socket = socket_;
        frame_timer_->async_wait([socket](asio::error_code ec) {
            if (!ec) {
                asio::error_code ignored;
                socket->close(ignored);
            }
        });
        try {
            co_await asio::async_read(*socket_, asio::buffer(prefix.data() + 1, 3), asio::use_awaitable);
            auto size = be32(prefix, 0);
            if (size < 10 || size > 1048576)
                throw std::runtime_error("Invalid HSMS length");
            auto length = prefix.size();
            prefix.resize(length + size);
            co_await asio::async_read(*socket_, asio::buffer(prefix.data() + length, size),
                                      asio::use_awaitable);
            frame_timer_->cancel();
            co_return Frame::decode(prefix);
        } catch (...) {
            frame_timer_->cancel();
            throw;
        }
    }
    asio::awaitable<void> read_loop() {
        try {
            while (!failed_) {
                auto f = co_await receive();
                if (f.stype) {
                    if (f.session != 0xffff || f.w || !f.body.empty())
                        throw std::runtime_error("Invalid HSMS control frame");
                    if (f.stype == 2 || f.stype == 4 || f.stype == 6) {
                        auto p = pending_.find(f.system);
                        if (p != pending_.end() && p->second.stype == f.stype) {
                            if (f.stype == 2) {
                                if (f.function)
                                    throw std::runtime_error("HSMS select refused");
                                selected_ = true;
                            }
                            pending_.erase(p);
                        }
                    } else if (f.stype == 1) {
                        send(Frame::control(2, f.system, selected_ ? 1 : 0));
                        selected_ = true;
                    } else if (f.stype == 3) {
                        send(Frame::control(4, f.system));
                        selected_ = false;
                        pending_.clear();
                        model_.communicating = model_.online = false;
                        connected_ = Clock::now();
                    } else if (f.stype == 5)
                        send(Frame::control(6, f.system));
                    else
                        throw std::runtime_error("HSMS separate or unsupported control");
                } else {
                    if (!selected_ || f.session != config_.secs.device_id)
                        throw std::runtime_error("HSMS session not selected or device ID mismatch");
                    if (f.function % 2 == 0) {
                        auto p = pending_.find(f.system);
                        if (p != pending_.end() && p->second.stype == 0 && p->second.stream == f.stream &&
                            p->second.function == f.function) {
                            if (f.stream == 6 && f.function == 12) {
                                auto ack = Item::decode(f.body);
                                if (ack.format != 0x20 || ack.data != Bytes{0}) {
                                    std::lock_guard lock(state_.mutex);
                                    state_.secs_error = "Host rejected S6F11";
                                }
                            }
                            pending_.erase(p);
                        }
                    } else {
                        std::pair<std::uint8_t, std::optional<Item>> response;
                        try {
                            response = model_.handle(f.stream, f.function,
                                                     f.body.empty() ? std::nullopt
                                                                    : std::optional{Item::decode(f.body)});
                        } catch (const std::exception &) {
                            response = {0, std::nullopt};
                        }
                        if (f.w)
                            send({config_.secs.device_id, f.stream, response.first, 0, false, f.system,
                                  response.second ? response.second->encode() : Bytes{}});
                    }
                }
                {
                    std::lock_guard lock(state_.mutex);
                    state_.selected = selected_;
                    state_.communicating = model_.communicating;
                    state_.online = model_.online;
                }
            }
        } catch (const std::exception &e) {
            fail(e.what());
        }
        reader_done_ = true;
    }

  public:
    Session(std::shared_ptr<asio::ip::tcp::socket> socket, const Config &config, SharedState &state)
        : socket_(std::move(socket)), config_(config), state_(state), model_(config, state),
          timer_(socket_->get_executor()) {}
    asio::awaitable<void> run() {
        auto self = shared_from_this();
        asio::co_spawn(
            socket_->get_executor(), [self]() -> asio::awaitable<void> { co_await self->read_loop(); },
            asio::detached);
        if (config_.secs.active)
            transaction(Frame::control(1, ++system_), 2, config_.secs.t6_ms);
        try {
            while (!failed_) {
                auto now = Clock::now();
                if (!selected_ && now - connected_ > std::chrono::milliseconds(config_.secs.t7_ms))
                    throw std::runtime_error("HSMS T7 expired");
                for (auto &[_, p] : pending_)
                    if (now > p.deadline)
                        throw std::runtime_error("HSMS T3/T6 transaction expired");
                if (selected_ && now - last_link_ > std::chrono::seconds(30)) {
                    transaction(Frame::control(5, ++system_), 6, config_.secs.t6_ms);
                    last_link_ = now;
                }
                bool pending_event = std::any_of(pending_.begin(), pending_.end(),
                                                 [](auto &p) { return p.second.stype == 0; });
                if (selected_ && !pending_event &&
                    now - last_event_ >= std::chrono::milliseconds(config_.secs.event_ms)) {
                    if (auto event = model_.event())
                        transaction({config_.secs.device_id, 6, 11, 0, true, ++system_, event->encode()}, 0,
                                    config_.secs.t3_ms);
                    last_event_ = now;
                }
                timer_.expires_after(std::chrono::milliseconds(5));
                co_await timer_.async_wait(asio::use_awaitable);
            }
        } catch (const std::exception &e) {
            fail(e.what());
        }
        while (!reader_done_ || writing_) {
            timer_.expires_after(std::chrono::milliseconds(1));
            co_await timer_.async_wait(asio::use_awaitable);
        }
        throw std::runtime_error(error_);
    }
};
} // namespace
HsmsServer::HsmsServer(asio::any_io_executor e, const Config &c, SharedState &s)
    : executor_(e), config_(c), state_(s), timer_(std::make_shared<asio::steady_timer>(e)) {}
void HsmsServer::close() {
    auto self = shared_from_this();
    asio::dispatch(executor_, [self] {
        self->stopped_ = true;
        asio::error_code ec;
        if (self->listener_)
            self->listener_->close(ec);
        if (self->socket_)
            self->socket_->close(ec);
        self->timer_->cancel();
    });
}
asio::awaitable<void> HsmsServer::run() {
    auto &c = config_.secs;
    if (!c.active) {
        listener_ = std::make_shared<asio::ip::tcp::acceptor>(executor_);
        listener_->open(asio::ip::tcp::v4());
        listener_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        listener_->bind({asio::ip::make_address_v4(c.host), c.port});
        listener_->listen(1);
    }
    while (!stopped_) {
        try {
            socket_ = std::make_shared<asio::ip::tcp::socket>(executor_);
            if (c.active) {
                timer_->expires_after(std::chrono::milliseconds(c.t6_ms));
                auto socket = socket_;
                timer_->async_wait([socket](asio::error_code ec) {
                    if (!ec) {
                        asio::error_code ignored;
                        socket->close(ignored);
                    }
                });
                co_await socket_->async_connect({asio::ip::make_address_v4(c.host), c.port},
                                                asio::use_awaitable);
                timer_->cancel();
            } else
                co_await listener_->async_accept(*socket_, asio::use_awaitable);
            socket_->set_option(asio::ip::tcp::no_delay(true));
            {
                std::lock_guard lock(state_.mutex);
                state_.connected = true;
            }
            auto session = std::make_shared<Session>(socket_, config_, state_);
            co_await session->run();
        } catch (const std::exception &ex) {
            timer_->cancel();
            if (!stopped_) {
                std::lock_guard lock(state_.mutex);
                state_.secs_error = ex.what();
            }
        }
        {
            std::lock_guard lock(state_.mutex);
            state_.connected = state_.selected = state_.communicating = state_.online = false;
        }
        if (stopped_)
            break;
        timer_->expires_after(std::chrono::milliseconds(c.reconnect_ms));
        asio::error_code ec;
        co_await timer_->async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
}
} // namespace scada
