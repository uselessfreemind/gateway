#include "scada/protocol.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <numeric>
#include <regex>
#include <stdexcept>
namespace scada {
static void need(std::span<const std::uint8_t> b, std::size_t p, std::size_t n) {
    if (p > b.size() || n > b.size() - p)
        throw std::runtime_error("Truncated protocol message");
}
std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t p) {
    need(b, p, 2);
    return static_cast<std::uint16_t>((b[p] << 8) | b[p + 1]);
}
std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t p) {
    return (static_cast<std::uint32_t>(be16(b, p)) << 16) | be16(b, p + 2);
}
std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t p) {
    need(b, p, 2);
    return static_cast<std::uint16_t>(b[p] | (b[p + 1] << 8));
}
static std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t p) {
    return le16(b, p) | (static_cast<std::uint32_t>(le16(b, p + 2)) << 16);
}
void put_be16(Bytes &b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put_be32(Bytes &b, std::uint32_t v) {
    put_be16(b, static_cast<std::uint16_t>(v >> 16));
    put_be16(b, static_cast<std::uint16_t>(v));
}
void put_le16(Bytes &b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
static void put_le32(Bytes &b, std::uint32_t v) {
    put_le16(b, static_cast<std::uint16_t>(v));
    put_le16(b, static_cast<std::uint16_t>(v >> 16));
}
static std::uint32_t number(const std::string &s) {
    std::uint32_t v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size())
        throw std::invalid_argument("Invalid address number");
    return v;
}
FinsAddress fins_address(const Tag &t) {
    auto colon = t.address.find(':');
    if (colon == std::string::npos)
        throw std::invalid_argument("FINS address needs area:word[.bit]");
    auto area = t.address.substr(0, colon);
    auto value = t.address.substr(colon + 1);
    auto dot = value.find('.');
    bool bits = t.type == Type::boolean;
    if (bits != (dot != std::string::npos))
        throw std::invalid_argument("Only BOOL needs a FINS bit suffix");
    auto word = number(value.substr(0, dot));
    auto bit = bits ? number(value.substr(dot + 1)) : 0;
    static const std::map<std::string, std::uint8_t> areas{
        {"D", 0x82}, {"CIO", 0xb0}, {"W", 0xb1}, {"H", 0xb2}, {"A", 0xb3}};
    auto code = areas.at(area);
    if (bits)
        code = static_cast<std::uint8_t>(code - 0x80);
    int count = t.type == Type::f64                                                   ? 4
                : (t.type == Type::i32 || t.type == Type::u32 || t.type == Type::f32) ? 2
                                                                                      : 1;
    if (word > 65535 || bit > 15 || word + count > 65536)
        throw std::invalid_argument("FINS address out of range");
    return {code, static_cast<std::uint8_t>(bit), static_cast<std::uint16_t>(word),
            static_cast<std::uint16_t>(count), bits};
}
Bytes fins_request(const Controller &c, const Tag &first, std::uint16_t count, std::uint8_t sid) {
    auto a = fins_address(first);
    if (count < 1 || count > 400)
        throw std::invalid_argument("Invalid FINS read count");
    Bytes b{0x80,
            0,
            2,
            c.destination_network,
            c.destination_node,
            c.destination_unit,
            c.source_network,
            c.source_node,
            c.source_unit,
            sid,
            1,
            1,
            a.area};
    put_be16(b, a.word);
    b.push_back(a.bit);
    put_be16(b, count);
    return b;
}
static double fins_value(const Tag &t, const Controller &c, std::span<const std::uint8_t> data) {
    if (t.type == Type::boolean) {
        if (data.size() != 1 || data[0] > 1)
            throw std::runtime_error("Invalid FINS BOOL");
        return data[0];
    }
    auto words = data.size() / 2;
    std::uint64_t n{};
    for (std::size_t i = 0; i < words; ++i)
        n = (n << 16) | be16(data, 2 * (c.low_word_first ? words - 1 - i : i));
    switch (t.type) {
    case Type::i16:
        return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(n));
    case Type::u16:
        return static_cast<std::uint16_t>(n);
    case Type::i32:
        return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(n));
    case Type::u32:
        return static_cast<std::uint32_t>(n);
    case Type::f32:
        return std::bit_cast<float>(static_cast<std::uint32_t>(n));
    case Type::f64:
        return std::bit_cast<double>(n);
    default:
        throw std::runtime_error("Unsupported FINS type");
    }
}
std::vector<Sample> fins_decode(const Controller &c, const std::vector<std::size_t> &tags, std::uint8_t sid,
                                std::span<const std::uint8_t> reply) {
    need(reply, 0, 14);
    if ((reply[0] & 0xc0) != 0xc0 || reply[9] != sid || reply[10] != 1 || reply[11] != 1 ||
        reply[3] != c.source_network || reply[4] != c.source_node || reply[5] != c.source_unit ||
        reply[6] != c.destination_network || reply[7] != c.destination_node || reply[8] != c.destination_unit)
        throw std::runtime_error("FINS response correlation mismatch");
    if (auto end = be16(reply, 12); end)
        throw std::runtime_error("FINS end code " + std::to_string(end));
    auto first = fins_address(c.tags.at(tags.at(0)));
    auto last = first.position();
    for (auto i : tags) {
        auto a = fins_address(c.tags[i]);
        last = std::max(last, a.position() + a.count);
    }
    auto width = first.bits ? 1 : 2;
    if (reply.size() != 14 + static_cast<std::size_t>(last - first.position()) * width)
        throw std::runtime_error("Invalid FINS response length");
    std::vector<Sample> result;
    for (auto i : tags) {
        auto &t = c.tags[i];
        auto a = fins_address(t);
        auto value =
            fins_value(t, c,
                       reply.subspan(14 + static_cast<std::size_t>(a.position() - first.position()) * width,
                                     a.count * width));
        result.push_back({c.id, t.id, t.svid, utc_ms(), value, "Good", ""});
    }
    return result;
}
Bytes cip_path(const std::string &address) {
    static const std::regex pattern(R"(^([A-Za-z_][A-Za-z0-9_]*)(?:\[([0-9]+)\])?$)");
    Bytes b;
    std::size_t start{};
    while (true) {
        auto end = address.find('.', start);
        auto part = address.substr(start, end == std::string::npos ? end : end - start);
        std::smatch m;
        if (!std::regex_match(part, m, pattern) || m[1].length() > 255)
            throw std::invalid_argument("Invalid CIP symbolic path");
        auto name = m[1].str();
        b.push_back(0x91);
        b.push_back(static_cast<std::uint8_t>(name.size()));
        b.insert(b.end(), name.begin(), name.end());
        if (name.size() % 2)
            b.push_back(0);
        if (m[2].matched) {
            auto index = number(m[2]);
            if (index <= 255) {
                b.push_back(0x28);
                b.push_back(static_cast<std::uint8_t>(index));
            } else if (index <= 65535) {
                b.insert(b.end(), {0x29, 0});
                put_le16(b, static_cast<std::uint16_t>(index));
            } else {
                b.insert(b.end(), {0x2a, 0});
                put_le32(b, index);
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (b.size() > 440)
        throw std::invalid_argument("CIP path exceeds request budget");
    return b;
}
Bytes cip_read(const Tag &t) {
    auto path = cip_path(t.address);
    Bytes b{0x4c, static_cast<std::uint8_t>(path.size() / 2)};
    b.insert(b.end(), path.begin(), path.end());
    put_le16(b, 1);
    return b;
}
double cip_value(Type type, std::span<const std::uint8_t> b) {
    need(b, 0, 4);
    if (b[0] != 0xcc || b[2])
        throw std::runtime_error("CIP Read Tag status " + std::to_string(b[2]));
    std::size_t p = 4 + b[3] * 2;
    auto code = le16(b, p);
    p += 2;
    static const std::map<Type, int> codes{{Type::boolean, 0xc1}, {Type::i16, 0xc3}, {Type::u16, 0xc7},
                                           {Type::i32, 0xc4},     {Type::u32, 0xc8}, {Type::f32, 0xca},
                                           {Type::f64, 0xcb}};
    auto size = type == Type::boolean                    ? 1
                : type == Type::i16 || type == Type::u16 ? 2
                : type == Type::f64                      ? 8
                                                         : 4;
    if (code != codes.at(type) || b.size() != p + size)
        throw std::runtime_error("CIP tag type/length mismatch");
    switch (type) {
    case Type::boolean:
        return b[p] ? 1 : 0;
    case Type::i16:
        return std::bit_cast<std::int16_t>(le16(b, p));
    case Type::u16:
        return le16(b, p);
    case Type::i32:
        return std::bit_cast<std::int32_t>(le32(b, p));
    case Type::u32:
        return le32(b, p);
    case Type::f32:
        return std::bit_cast<float>(le32(b, p));
    case Type::f64:
        return std::bit_cast<double>(static_cast<std::uint64_t>(le32(b, p)) |
                                     (static_cast<std::uint64_t>(le32(b, p + 4)) << 32));
    }
    throw std::runtime_error("Unsupported CIP type");
}
std::vector<std::vector<std::size_t>> read_plan(const Controller &c) {
    std::vector<std::vector<std::size_t>> out;
    std::vector<std::size_t> group, order(c.tags.size());
    std::iota(order.begin(), order.end(), 0);
    if (c.protocol == Protocol::fins)
        std::sort(order.begin(), order.end(), [&](auto i, auto j) {
            auto a = fins_address(c.tags[i]), b = fins_address(c.tags[j]);
            return std::pair{a.area, a.position()} < std::pair{b.area, b.position()};
        });
    int first{}, last{}, area = -1, request = 8, response = 6;
    for (auto i : order) {
        if (c.protocol == Protocol::fins) {
            auto a = fins_address(c.tags[i]);
            if (!group.empty() &&
                (area != a.area || a.position() + a.count - first > 400 || a.position() > last + 16)) {
                out.push_back(std::move(group));
                group.clear();
            }
            if (group.empty()) {
                first = a.position();
                last = first;
                area = a.area;
            }
            last = std::max(last, a.position() + a.count);
        } else if (c.protocol == Protocol::cip) {
            auto n = static_cast<int>(cip_read(c.tags[i]).size()) + 2;
            if (!group.empty() && (request + n > 480 || response + 16 > 480)) {
                out.push_back(std::move(group));
                group.clear();
                request = 8;
                response = 6;
            }
            request += n;
            response += 16;
        }
        group.push_back(i);
    }
    if (!group.empty())
        out.push_back(std::move(group));
    return out;
}
// All socket and deadline handlers execute on the same per-controller strand.
class Deadline {
    std::shared_ptr<asio::steady_timer> timer_;

  public:
    template <class Socket>
    Deadline(const std::shared_ptr<Socket> &socket, int ms)
        : timer_(std::make_shared<asio::steady_timer>(socket->get_executor())) {
        timer_->expires_after(std::chrono::milliseconds(ms));
        timer_->async_wait([socket](asio::error_code ec) {
            if (!ec) {
                asio::error_code ignored;
                socket->close(ignored);
            }
        });
    }
    ~Deadline() {
        timer_->cancel();
    }
};
class FinsClient final : public PlcClient {
    const Controller &c_;
    std::shared_ptr<asio::ip::udp::socket> socket_;
    std::uint8_t sid_{};

  public:
    FinsClient(asio::any_io_executor e, const Controller &c)
        : c_(c), socket_(std::make_shared<asio::ip::udp::socket>(e)) {}
    void close() override {
        asio::error_code ec;
        socket_->close(ec);
    }
    asio::awaitable<std::vector<Sample>> read(const std::vector<std::size_t> &tags) override {
        Deadline deadline(socket_, c_.timeout_ms);
        try {
            if (!socket_->is_open()) {
                socket_->open(asio::ip::udp::v4());
                socket_->connect({asio::ip::make_address_v4(c_.host), c_.port});
            }
            auto first = fins_address(c_.tags[tags[0]]);
            int end = first.position();
            for (auto i : tags) {
                auto a = fins_address(c_.tags[i]);
                end = std::max(end, a.position() + a.count);
            }
            auto sid = ++sid_;
            auto request =
                fins_request(c_, c_.tags[tags[0]], static_cast<std::uint16_t>(end - first.position()), sid);
            co_await socket_->async_send(asio::buffer(request), asio::use_awaitable);
            Bytes reply(2048);
            while (true) {
                auto n = co_await socket_->async_receive(asio::buffer(reply), asio::use_awaitable);
                if (n >= 10 && reply[9] != sid)
                    continue;
                reply.resize(n);
                break;
            }
            co_return fins_decode(c_, tags, sid, reply);
        } catch (...) {
            close();
            throw;
        }
    }
};
class CipClient final : public PlcClient {
    const Controller &c_;
    std::shared_ptr<asio::ip::tcp::socket> socket_;
    std::uint32_t session_{}, context_{};
    std::map<std::size_t, Bytes> requests_;
    asio::awaitable<Bytes> exchange(std::uint16_t command, const Bytes &payload) {
        Bytes tx;
        put_le16(tx, command);
        put_le16(tx, static_cast<std::uint16_t>(payload.size()));
        put_le32(tx, session_);
        put_le32(tx, 0);
        auto id = ++context_;
        put_le32(tx, id);
        put_le32(tx, 0);
        put_le32(tx, 0);
        tx.insert(tx.end(), payload.begin(), payload.end());
        co_await asio::async_write(*socket_, asio::buffer(tx), asio::use_awaitable);
        Bytes header(24);
        co_await asio::async_read(*socket_, asio::buffer(header), asio::use_awaitable);
        auto size = le16(header, 2);
        auto returned = le32(header, 4);
        if (le16(header, 0) != command || le32(header, 8) || le32(header, 12) != id || le32(header, 16) ||
            le32(header, 20) || size > 8192)
            throw std::runtime_error("Invalid EtherNet/IP encapsulation reply");
        if (command == 0x65) {
            if (!returned)
                throw std::runtime_error("Invalid session");
            session_ = returned;
        } else if (returned != session_)
            throw std::runtime_error("Session mismatch");
        Bytes body(size);
        co_await asio::async_read(*socket_, asio::buffer(body), asio::use_awaitable);
        co_return body;
    }
    asio::awaitable<Bytes> send_cip(const Bytes &cip) {
        if (!socket_->is_open()) {
            co_await socket_->async_connect({asio::ip::make_address_v4(c_.host), c_.port},
                                            asio::use_awaitable);
            socket_->set_option(asio::ip::tcp::no_delay(true));
            Bytes reg{1, 0, 0, 0};
            auto reply = co_await exchange(0x65, reg);
            if (reply != reg)
                throw std::runtime_error("Invalid RegisterSession reply");
        }
        Bytes cpf(6, 0);
        put_le16(cpf, 2);
        put_le16(cpf, 0);
        put_le16(cpf, 0);
        put_le16(cpf, 0xb2);
        put_le16(cpf, static_cast<std::uint16_t>(cip.size()));
        cpf.insert(cpf.end(), cip.begin(), cip.end());
        auto reply = co_await exchange(0x6f, cpf);
        need(reply, 0, 8);
        if (le32(reply, 0))
            throw std::runtime_error("Invalid CIP interface handle");
        auto count = le16(reply, 6);
        std::size_t p = 8;
        Bytes data;
        for (int i = 0; i < count; ++i) {
            auto id = le16(reply, p), n = le16(reply, p + 2);
            p += 4;
            need(reply, p, n);
            if (id == 0xb2) {
                if (!data.empty())
                    throw std::runtime_error("Duplicate CPF data");
                data.assign(reply.begin() + static_cast<std::ptrdiff_t>(p),
                            reply.begin() + static_cast<std::ptrdiff_t>(p + n));
            }
            p += n;
        }
        if (p != reply.size() || data.empty())
            throw std::runtime_error("Invalid CPF response");
        co_return data;
    }

  public:
    CipClient(asio::any_io_executor e, const Controller &c)
        : c_(c), socket_(std::make_shared<asio::ip::tcp::socket>(e)) {
        for (const auto &tags : read_plan(c_)) {
            std::vector<Bytes> requests;
            for (auto i : tags)
                requests.push_back(cip_read(c_.tags[i]));
            Bytes request;
            if (tags.size() == 1)
                request = requests[0];
            else {
                request = {0x0a, 2, 0x20, 2, 0x24, 1};
                put_le16(request, static_cast<std::uint16_t>(tags.size()));
                std::size_t offset = 2 + 2 * tags.size();
                for (auto &r : requests) {
                    put_le16(request, static_cast<std::uint16_t>(offset));
                    offset += r.size();
                }
                for (auto &r : requests)
                    request.insert(request.end(), r.begin(), r.end());
            }
            requests_.emplace(tags.front(), std::move(request));
        }
    }
    void close() override {
        asio::error_code ec;
        socket_->close(ec);
        session_ = 0;
    }
    asio::awaitable<std::vector<Sample>> read(const std::vector<std::size_t> &tags) override {
        Deadline deadline(socket_, c_.timeout_ms);
        try {
            const auto &request = requests_.at(tags.front());
            auto response = co_await send_cip(request);
            std::vector<Sample> samples;
            std::size_t base{};
            if (tags.size() > 1) {
                need(response, 0, 4);
                if (response[0] != 0x8a || (response[2] != 0 && response[2] != 0x1e))
                    throw std::runtime_error("CIP Multiple Service unsupported or failed");
                base = 4 + response[3] * 2;
                if (le16(response, base) != tags.size())
                    throw std::runtime_error("CIP service count mismatch");
            }
            for (std::size_t k = 0; k < tags.size(); ++k) {
                auto &t = c_.tags[tags[k]];
                Sample s{c_.id, t.id, t.svid, utc_ms(), {}, "Good", ""};
                std::size_t from = 0, to = response.size();
                if (tags.size() > 1) {
                    auto a = le16(response, base + 2 + 2 * k);
                    auto b = k + 1 < tags.size() ? le16(response, base + 4 + 2 * k) : response.size() - base;
                    if (a < 2 + tags.size() * 2 || b < a || base + b > response.size())
                        throw std::runtime_error("Invalid CIP offsets");
                    from = base + a;
                    to = base + b;
                }
                try {
                    s.value = cip_value(t.type, std::span{response}.subspan(from, to - from));
                } catch (const std::exception &ex) {
                    s.quality = "Bad";
                    s.error = ex.what();
                }
                samples.push_back(std::move(s));
            }
            co_return samples;
        } catch (...) {
            close();
            throw;
        }
    }
};
class SimulatedClient final : public PlcClient {
    const Controller &c_;

  public:
    explicit SimulatedClient(const Controller &c) : c_(c) {}
    void close() override {}
    asio::awaitable<std::vector<Sample>> read(const std::vector<std::size_t> &tags) override {
        std::vector<Sample> out;
        auto now = utc_ms();
        for (auto i : tags) {
            auto &t = c_.tags[i];
            out.push_back({c_.id, t.id, t.svid, now,
                           20 + std::sin(static_cast<double>(now) / 1000.0 + static_cast<double>(i) / 10.0),
                           "Good", ""});
        }
        co_return out;
    }
};
std::shared_ptr<PlcClient> make_plc(asio::any_io_executor e, const Controller &c) {
    if (c.protocol == Protocol::fins)
        return std::make_shared<FinsClient>(e, c);
    if (c.protocol == Protocol::cip)
        return std::make_shared<CipClient>(e, c);
    return std::make_shared<SimulatedClient>(c);
}
} // namespace scada
