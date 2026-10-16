#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace scada {
using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;
inline std::int64_t utc_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
enum class Protocol { fins, cip, simulated };
enum class Type { boolean, i16, u16, i32, u32, f32, f64 };
struct Tag {
    std::string id, address, unit;
    Type type{Type::u16};
    std::uint32_t svid{};
    double scale{1}, offset{};
};
struct Controller {
    std::string id, host{"127.0.0.1"};
    Protocol protocol{Protocol::fins};
    std::uint16_t port{9600};
    int interval_ms{10}, timeout_ms{100};
    std::uint8_t source_network{}, source_node{100}, source_unit{}, destination_network{},
        destination_node{1}, destination_unit{};
    bool low_word_first{true};
    std::vector<Tag> tags;
};
struct SecsConfig {
    bool enabled{};
    bool active{};
    std::string host{"127.0.0.1"};
    std::uint16_t port{5000}, device_id{};
    int t3_ms{45000}, t6_ms{5000}, t7_ms{10000}, t8_ms{5000}, reconnect_ms{5000}, event_ms{1000};
};
struct Config {
    std::string database{"data/scada-native.db"};
    std::size_t queue_capacity{100000};
    unsigned io_threads{4};
    std::vector<Controller> controllers;
    SecsConfig secs;
};
Config load_config(const std::string &path);
Config demo_config(unsigned controllers, unsigned tags, int interval_ms);
void validate(const Config &config);
struct Sample {
    std::string controller, tag;
    std::uint32_t svid{};
    std::int64_t timestamp_ms{};
    std::optional<double> value;
    std::string quality{"Good"}, error;
};
struct Status {
    std::uint64_t scans{}, failed_reads{}, missed_deadlines{};
    double scan_ms{}, jitter_ms{};
    std::int64_t last_attempt{}, last_success{};
};
struct SharedState {
    mutable std::mutex mutex;
    std::map<std::uint32_t, Sample> latest;
    std::map<std::string, Status> controllers;
    std::uint64_t archived{}, backpressure{};
    std::string archive_error, secs_error;
    bool connected{}, selected{}, communicating{}, online{};
};
void to_json(Json &j, const Sample &s);
std::int64_t month_cutoff(std::int64_t now_ms);
} // namespace scada
