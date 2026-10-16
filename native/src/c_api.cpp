#include "scada/c_api.h"
#include "scada/engine.hpp"
#include <cstdlib>
#include <cstring>
namespace {
thread_local std::string error;
scada::Engine &engine(scada_handle h) {
    if (!h)
        throw std::invalid_argument("Null engine handle");
    return *static_cast<scada::Engine *>(h);
}
char *copy(const scada::Json &j) {
    auto s = j.dump();
    auto p = static_cast<char *>(std::malloc(s.size() + 1));
    if (!p)
        throw std::bad_alloc();
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
template <class F> char *result(F f) {
    try {
        error.clear();
        return copy(f());
    } catch (const std::exception &e) {
        error = e.what();
        return nullptr;
    }
}
scada_handle create(scada::Config c) {
    auto p = std::make_unique<scada::Engine>(std::move(c));
    p->start();
    return p.release();
}
} // namespace
extern "C" {
scada_handle scada_open(const char *path) {
    try {
        error.clear();
        if (!path)
            throw std::invalid_argument("Missing configuration path");
        return create(scada::load_config(path));
    } catch (const std::exception &e) {
        error = e.what();
        return nullptr;
    }
}
scada_handle scada_demo(unsigned controllers, unsigned tags, int interval, const char *database) {
    try {
        error.clear();
        auto c = scada::demo_config(controllers, tags, interval);
        if (database)
            c.database = database;
        return create(std::move(c));
    } catch (const std::exception &e) {
        error = e.what();
        return nullptr;
    }
}
int scada_stop(scada_handle h) {
    try {
        engine(h).stop();
        return 0;
    } catch (const std::exception &e) {
        error = e.what();
        return -1;
    }
}
void scada_close(scada_handle h) {
    try {
        delete static_cast<scada::Engine *>(h);
    } catch (const std::exception &e) {
        error = e.what();
    }
}
char *scada_snapshot(scada_handle h, const char *c) {
    return result([&] { return engine(h).snapshot(c ? c : ""); });
}
char *scada_history(scada_handle h, const char *c, const char *t, int64_t from, int64_t to, int limit,
                    int64_t after) {
    return result([&] {
        if (!c || !t)
            throw std::invalid_argument("Missing controller/tag");
        return engine(h).history(c, t, from, to, limit, after);
    });
}
char *scada_trend(scada_handle h, const char *c, const char *t, int64_t from, int64_t to, int64_t bucket) {
    return result([&] {
        if (!c || !t)
            throw std::invalid_argument("Missing controller/tag");
        return engine(h).trend(c, t, from, to, bucket);
    });
}
const char *scada_last_error() {
    return error.c_str();
}
void scada_free(char *p) {
    std::free(p);
}
}
