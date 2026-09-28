#ifndef TS_HTTPS_RETRY_H
#define TS_HTTPS_RETRY_H
#include <stdbool.h>
#include <stdint.h>
/* Pure policy shared by the coordinator and host tests. Times are monotonic ms. */
typedef struct {
    uint32_t generation, control;
    bool ready;
    unsigned failures;
    int64_t due_ms;
} ts_https_retry_t;
static inline bool ts_https_retry_due(ts_https_retry_t *r, uint32_t generation,
        uint32_t control, bool desired, bool ready, int64_t now)
{
    if (generation != r->generation || control != r->control || (ready && !r->ready)) {
        r->failures = 0; r->due_ms = 0;
    }
    r->generation = generation; r->control = control; r->ready = ready;
    return desired && ready && r->failures < 4 && now >= r->due_ms;
}
static inline void ts_https_retry_failed(ts_https_retry_t *r, int64_t now)
{
    static const int delay[] = {1000, 5000, 15000};
    if (r->failures < 3) r->due_ms = now + delay[r->failures];
    ++r->failures;
}
#endif
