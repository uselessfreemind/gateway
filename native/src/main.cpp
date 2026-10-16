#include "scada/engine.hpp"
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
static std::atomic<bool> interrupted{};
static void signal_handler(int) {
    interrupted = true;
}
int main(int argc, char **argv) {
    try {
        std::string path, summary, database;
        bool demo = false;
        unsigned controllers = 2, tags = 10;
        int interval = 10, seconds = 0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto next = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("Missing argument");
                return std::string(argv[i]);
            };
            if (arg == "--config")
                path = next();
            else if (arg == "--demo")
                demo = true;
            else if (arg == "--controllers")
                controllers = static_cast<unsigned>(std::stoul(next()));
            else if (arg == "--tags")
                tags = static_cast<unsigned>(std::stoul(next()));
            else if (arg == "--interval")
                interval = std::stoi(next());
            else if (arg == "--run-for")
                seconds = std::stoi(next());
            else if (arg == "--summary")
                summary = next();
            else if (arg == "--database")
                database = next();
            else
                throw std::invalid_argument("Unknown argument: " + arg);
        }
        if (seconds < 0)
            throw std::invalid_argument("Invalid duration");
        if (!demo && path.empty()) {
            std::cerr
                << "Usage: scada_gateway --config site.json [--run-for seconds --summary result.json]\n      "
                   " scada_gateway --demo [--controllers 2 --tags 10 --interval 10]\n";
            return 2;
        }
        auto config = demo ? scada::demo_config(controllers, tags, interval) : scada::load_config(path);
        if (!database.empty())
            config.database = database;
        const bool diagnostics = config.diagnostics;
        scada::Engine engine(std::move(config));
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        engine.start();
        if (diagnostics)
            std::clog << "Gateway started\n" << std::flush;
        auto start = std::chrono::steady_clock::now();
        while (!interrupted &&
               (seconds == 0 || std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        engine.stop();
        if (diagnostics)
            std::clog << "Gateway stopped; writing summary\n" << std::flush;
        auto report = engine.snapshot();
        report["archiveRows"] = engine.archived_rows();
        if (!summary.empty()) {
            std::ofstream out(summary);
            out << report.dump(2);
            if (!out)
                throw std::runtime_error("Cannot write summary");
        }
        std::cout << "controllers=" << report["controllers"].size() << " latest=" << report["totalLatest"]
                  << " archived=" << report["archiveRows"] << " backpressure=" << report["backpressureEvents"]
                  << "\n";
        return report["archiveError"].get<std::string>().empty() ? 0 : 1;
    } catch (const std::exception &ex) {
        std::cerr << "Fatal: " << ex.what() << "\n";
        return 1;
    }
}
