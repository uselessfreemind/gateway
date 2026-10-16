#include "scada/engine.hpp"
#include "scada/secs.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string("Failed: ") + #x);                                          \
    } while (false)
template <class F> void throws(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    REQUIRE(caught);
}
int main() {
    try {
        using namespace scada;
        Tag tag;
        tag.address = "D:100";
        tag.type = Type::f32;
        auto a = fins_address(tag);
        REQUIRE(a.area == 0x82 && a.count == 2);
        Controller c;
        c.id = "c";
        tag.id = "t";
        tag.svid = 1;
        c.tags = {tag};
        auto request = fins_request(c, tag, 2, 7);
        REQUIRE(request[10] == 1 && request[11] == 1);
        Bytes reply(request.begin(), request.begin() + 12);
        reply[0] = 0xc0;
        for (int i = 0; i < 3; ++i) {
            reply[3 + i] = request[6 + i];
            reply[6 + i] = request[3 + i];
        }
        reply.insert(reply.end(), {0, 0, 0, 0, 0x3f, 0x80});
        REQUIRE(fins_decode(c, {0}, 7, reply)[0].value == 1);
        throws([&] { fins_decode(c, {0}, 8, reply); });
        tag.address = "D:65535";
        throws([&] { fins_address(tag); });
        tag.address = "Machine.Real[300]";
        auto cip = cip_read(tag);
        REQUIRE(cip[0] == 0x4c);
        Bytes real{0xcc, 0, 0, 0, 0xca, 0, 0, 0, 0x80, 0x3f};
        REQUIRE(cip_value(Type::f32, real) == 1);
        throws([&] { cip_value(Type::i16, real); });
        throws([] { cip_path("Tag;Write(1)"); });
        auto item = Item::L({Item::U(42), Item::F(12.5), Item::A("CJ2"), Item::L({Item::B(0)})});
        auto wire = item.encode();
        REQUIRE(Item::decode(wire).encode() == wire);
        wire.pop_back();
        throws([&] { Item::decode(wire); });
        Frame f{0, 1, 3, 0, true, 42, Item::L({Item::U(1)}).encode()};
        auto decoded = Frame::decode(f.encode());
        REQUIRE(decoded.system == 42 && decoded.w && decoded.body == f.body);
        using namespace std::chrono;
        auto march = sys_days{year{2024} / March / 31} + hours{12};
        auto cutoff = month_cutoff(duration_cast<milliseconds>(march.time_since_epoch()).count());
        REQUIRE(sys_time<milliseconds>{milliseconds{cutoff}} ==
                sys_days{year{2024} / February / 29} + hours{12});
        auto config = demo_config(500, 700, 10);
        REQUIRE(config.controllers.size() == 500);
        config.controllers.push_back(config.controllers[0]);
        throws([&] { validate(config); });
        auto path =
            std::filesystem::temp_directory_path() / ("scada-test-" + std::to_string(utc_ms()) + ".db");
        {
            Archive archive(path.string());
            auto now = utc_ms();
            archive.append({{"c", "t", 1, month_cutoff(now) - 1, 1, "Good", ""},
                            {"c", "t", 1, now, 10, "Good", ""},
                            {"c", "t", 1, now + 1, 20, "Good", ""},
                            {"c", "t", 1, now + 2, {}, "Bad", "timeout"}});
            REQUIRE(archive.count() == 4);
            auto page = archive.history("c", "t", 0, now + 10, 2);
            REQUIRE(page["samples"].size() == 2);
            auto next = archive.history("c", "t", 0, now + 10, 2, page["nextAfterId"]);
            REQUIRE(next["samples"].size() == 1 && next["samples"][0]["value"].is_null());
            auto trend = archive.trend("c", "t", now, now + 10, 1000000);
            std::int64_t total{}, good{};
            for (auto &b : trend) {
                total += b["count"].get<std::int64_t>();
                good += b["goodCount"].get<std::int64_t>();
            }
            REQUIRE(total == 3 && good == 2);
            REQUIRE(archive.purge(now) == 1);
            REQUIRE(archive.purge(now) == 0);
            REQUIRE(archive.count() == 3);
            SharedState queue_state;
            {
                ArchiveQueue queue(archive, queue_state, 1000);
                auto first = queue.request_ticket();
                auto second = queue.request_ticket();
                std::vector<Sample> sample{{"c", "t", 1, now, 30, "Good", ""}};
                REQUIRE(!queue.try_push(sample, second));
                REQUIRE(queue.try_push(sample, first));
                REQUIRE(queue.try_push(sample, second));
                queue.finish();
                REQUIRE(queue_state.archived == 2);
            }
            REQUIRE(archive.count() == 5);
            std::atomic<bool> done{};
            std::exception_ptr writer_error;
            std::jthread writer([&] {
                try {
                    for (int batch = 0; batch < 4; ++batch) {
                        std::vector<Sample> rows(1000, Sample{"parallel", "t", 2, now, 5, "Good", ""});
                        archive.append(rows);
                    }
                } catch (...) {
                    writer_error = std::current_exception();
                }
                done = true;
            });
            do {
                auto view = archive.trend("parallel", "t", now, now + 10, 1000000);
                std::uint64_t committed{};
                for (auto &bucket : view)
                    committed += bucket["count"].get<std::uint64_t>();
                REQUIRE(committed % 1000 == 0); // A viewer must not see a partial transaction.
            } while (!done);
            writer.join();
            if (writer_error)
                std::rethrow_exception(writer_error);
            auto committed = archive.history("parallel", "t", now, now + 10, 10000);
            REQUIRE(committed["samples"].size() == 4000);

            throws([&] { archive.trend("c", "t", now, now + 10000000, 10); });
        }
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + "-wal");
        std::filesystem::remove(path.string() + "-shm");
        Config cfg = demo_config(1, 1, 10);
        SharedState state;
        state.latest[1] = {"demo0", "tag0", 1, utc_ms(), 12.5, "Good", ""};
        GemModel gem(cfg, state);
        gem.handle(1, 13, Item::L());
        auto status = gem.handle(1, 3, Item::L({Item::U(1)}));
        REQUIRE(status.first == 4 && status.second->list[0].format == 0x80);
        state.latest[1].timestamp_ms = 0;
        REQUIRE(gem.handle(1, 3, Item::L({Item::U(1)})).second->list[0].list.empty());
        REQUIRE(gem.handle(2, 41, Item::L()).second->list[0].data[0] == 1);
        std::cout << "PASS: codecs, bounds, month expiry, archive pages/trends, 500x700 configuration, GEM "
                     "status/read-only commands\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
