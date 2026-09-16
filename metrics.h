#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Counters
    uint64_t requests_total;
    uint64_t requests_success;
    uint64_t requests_error;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t bytes_transferred;
    
    // Gauges
    int32_t active_connections;
    
    // Mutex
    pthread_mutex_t lock;
} ServerMetrics;

void metrics_init(void);
void metrics_increment_requests(void);
void metrics_increment_success(void);
void metrics_increment_errors(void);
void metrics_increment_cache_hits(void);
void metrics_increment_cache_misses(void);
void metrics_add_bytes(size_t bytes);
void metrics_set_active_connections(int32_t count);
void metrics_increment_active_connections(void);
void metrics_decrement_active_connections(void);

// Prometheus format export
char* metrics_export_prometheus(void);

#ifdef __cplusplus
}
#endif

#endif // METRICS_H
