#pragma once
#include <stdint.h>
#if defined(_WIN32)
#if defined(SCADA_EXPORTS)
#define SCADA_API __declspec(dllexport)
#else
#define SCADA_API __declspec(dllimport)
#endif
#else
#define SCADA_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef void *scada_handle;
/* UTF-8. Returned JSON strings belong to the caller; always release with scada_free. */
SCADA_API scada_handle scada_open(const char *configuration_path);
SCADA_API scada_handle scada_demo(unsigned controllers, unsigned tags, int interval_ms,
                                  const char *database_path);
SCADA_API int scada_stop(scada_handle handle);
SCADA_API void scada_close(scada_handle handle);
SCADA_API char *scada_snapshot(scada_handle handle, const char *controller);
SCADA_API char *scada_history(scada_handle handle, const char *controller, const char *tag, int64_t from_ms,
                              int64_t to_ms, int limit, int64_t after_id);
SCADA_API char *scada_trend(scada_handle handle, const char *controller, const char *tag, int64_t from_ms,
                            int64_t to_ms, int64_t bucket_ms);
SCADA_API const char *scada_last_error(void);
SCADA_API void scada_free(char *value);
#ifdef __cplusplus
}
#endif
