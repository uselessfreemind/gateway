#include "scada/model.hpp"
#include "scada/protocol.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
namespace scada {
void to_json(Json &j, const Sample &s) {
    j = {{"controller", s.controller},
         {"tag", s.tag},
         {"svid", s.svid},
         {"timestampMs", s.timestamp_ms},
         {"value", s.value ? Json(*s.value) : Json(nullptr)},
         {"quality", s.quality},
         {"error", s.error}};
}
std::int64_t month_cutoff(std::int64_t now) {
    using namespace std::chrono;
    auto tp = sys_time<milliseconds>{milliseconds{now}};
    auto day = floor<days>(tp);
    year_month_day ymd{day};
    auto target = ymd.year() / ymd.month() - months{1};
    auto last = year_month_day_last{target.year(), month_day_last{target.month()}}.day();
    auto clamped = target.year() / target.month() / std::min(ymd.day(), last);
    return duration_cast<milliseconds>((sys_days{clamped} + (tp - day)).time_since_epoch()).count();
}
static bool id_valid(const std::string &s) {
    return !s.empty() && s.size() <= 80 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
               c == '_';
    });
}
void validate(const Config &config) {
    if (config.controllers.empty() || config.controllers.size() > 500)
        throw std::invalid_argument("Configure 1..500 controllers");
    if (config.database.empty() || config.io_threads < 1 || config.io_threads > 32 ||
        config.queue_capacity < 1000 || config.queue_capacity > 5000000)
        throw std::invalid_argument("Invalid database, worker count or queue capacity");
    std::set<std::string> ids;
    std::set<std::uint32_t> svids;
    for (auto &c : config.controllers) {
        if (c.tags.size() > config.queue_capacity)
            throw std::invalid_argument("Archive queue must hold at least one complete controller scan");
        if (!id_valid(c.id) || !ids.insert(c.id).second || c.tags.empty() || c.tags.size() > 10000 ||
            c.port == 0 || c.interval_ms < 10 || c.interval_ms > 86400000 || c.timeout_ms < 1 ||
            c.timeout_ms > 60000)
            throw std::invalid_argument("Invalid controller: " + c.id);
        asio::ip::make_address_v4(c.host);
        std::set<std::string> tags;
        for (auto &t : c.tags) {
            if (!id_valid(t.id) || !tags.insert(t.id).second || !t.svid || !svids.insert(t.svid).second ||
                !std::isfinite(t.scale) || !std::isfinite(t.offset))
                throw std::invalid_argument("Invalid tag or duplicate SVID: " + t.id);
            if (c.protocol == Protocol::fins)
                fins_address(t);
            else if (c.protocol == Protocol::cip)
                cip_path(t.address);
        }
        read_plan(c);
    }
    auto &s = config.secs;
    if (s.port == 0 || s.device_id > 32767 || s.t3_ms < 1 || s.t3_ms > 120000 || s.t6_ms < 1 ||
        s.t6_ms > 120000 || s.t7_ms < 1 || s.t7_ms > 120000 || s.t8_ms < 1 || s.t8_ms > 120000 ||
        s.reconnect_ms < 1 || s.reconnect_ms > 300000 || s.event_ms < 10 || s.event_ms > 60000)
        throw std::invalid_argument("Invalid HSMS settings");
    asio::ip::make_address_v4(s.host);
}
Config load_config(const std::string &path) {
    std::ifstream input(std::filesystem::path(std::u8string(path.begin(), path.end())));
    if (!input)
        throw std::runtime_error("Cannot open configuration: " + path);
    Json j;
    input >> j;
    Config out;
    out.database = j.value("database", out.database);
    out.diagnostics = j.value("diagnostics", false);
    out.queue_capacity = j.value("queueCapacity", out.queue_capacity);
    out.io_threads = j.value("ioThreads", out.io_threads);
    for (auto &item : j.at("controllers")) {
        Controller c;
        c.id = item.at("id");
        c.host = item.value("host", c.host);
        auto p = item.value("protocol", std::string("FinsUdp"));
        c.protocol = p == "FinsUdp"      ? Protocol::fins
                     : p == "EtherNetIp" ? Protocol::cip
                     : p == "Simulated"  ? Protocol::simulated
                                         : throw std::invalid_argument("Unknown protocol");
        auto port = item.value("port", c.protocol == Protocol::cip ? 44818 : 9600);
        if (port < 1 || port > 65535)
            throw std::invalid_argument("Invalid port");
        c.port = static_cast<std::uint16_t>(port);
        c.interval_ms = item.value("intervalMs", 10);
        c.timeout_ms = item.value("timeoutMs", 100);
        c.low_word_first = item.value("lowWordFirst", true);
        auto byte = [&](const char *k, int d) {
            auto v = item.value(k, d);
            if (v < 0 || v > 255)
                throw std::invalid_argument("Invalid FINS node");
            return static_cast<std::uint8_t>(v);
        };
        c.source_node = byte("sourceNode", 100);
        c.source_network = byte("sourceNetwork", 0);
        c.source_unit = byte("sourceUnit", 0);
        c.destination_node = byte("destinationNode", 1);
        c.destination_network = byte("destinationNetwork", 0);
        c.destination_unit = byte("destinationUnit", 0);
        for (auto &v : item.at("tags")) {
            Tag t;
            t.id = v.at("id");
            t.address = v.value("address", std::string{});
            auto svid = v.at("svid").get<std::int64_t>();
            if (svid <= 0 || svid > UINT32_MAX)
                throw std::invalid_argument("SVID out of range");
            t.svid = static_cast<std::uint32_t>(svid);
            t.unit = v.value("unit", std::string{});
            t.scale = v.value("scale", 1.0);
            t.offset = v.value("offset", 0.0);
            static const std::map<std::string, Type> types{
                {"Bool", Type::boolean}, {"Int16", Type::i16},   {"UInt16", Type::u16}, {"Int32", Type::i32},
                {"UInt32", Type::u32},   {"Float32", Type::f32}, {"Float64", Type::f64}};
            t.type = types.at(v.value("type", std::string("UInt16")));
            c.tags.push_back(std::move(t));
        }
        out.controllers.push_back(std::move(c));
    }
    if (j.contains("secs")) {
        auto &s = j.at("secs");
        auto &o = out.secs;
        o.enabled = s.value("enabled", false);
        auto mode = s.value("mode", std::string("Passive"));
        if (mode != "Active" && mode != "Passive")
            throw std::invalid_argument("Invalid HSMS mode");
        o.active = mode == "Active";
        o.host = s.value("host", o.host);
        int port = s.value("port", 5000), device = s.value("deviceId", 0);
        if (port < 1 || port > 65535 || device < 0 || device > 32767)
            throw std::invalid_argument("Invalid HSMS port/device");
        o.port = static_cast<std::uint16_t>(port);
        o.device_id = static_cast<std::uint16_t>(device);
        o.t3_ms = s.value("t3Ms", o.t3_ms);
        o.t6_ms = s.value("t6Ms", o.t6_ms);
        o.t7_ms = s.value("t7Ms", o.t7_ms);
        o.t8_ms = s.value("t8Ms", o.t8_ms);
        o.reconnect_ms = s.value("reconnectMs", o.reconnect_ms);
        o.event_ms = s.value("eventMs", o.event_ms);
    }
    validate(out);
    return out;
}
Config demo_config(unsigned controllers, unsigned tags, int interval) {
    Config c;
    if (controllers > 500 || tags > 10000)
        throw std::invalid_argument("Demo size exceeds limits");
    for (unsigned i = 0; i < controllers; ++i) {
        Controller p;
        p.id = "demo" + std::to_string(i);
        p.protocol = Protocol::simulated;
        p.interval_ms = interval;
        for (unsigned k = 0; k < tags; ++k) {
            Tag t;
            t.id = "tag" + std::to_string(k);
            t.svid = i * tags + k + 1;
            t.type = Type::f64;
            t.unit = "demo";
            p.tags.push_back(t);
        }
        c.controllers.push_back(std::move(p));
    }
    validate(c);
    return c;
}
} // namespace scada
