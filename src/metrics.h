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
    
    // HTTP/2 metrics
    uint64_t http2_requests_total;
    uint64_t http2_bytes_transferred;
    
    // Gauges
    int32_t active_connections;
    int32_t http2_active_connections;
    
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

// HTTP/2 Specific Metrics
void metrics_increment_http2_requests(void);
void metrics_add_http2_bytes(size_t bytes);
void metrics_increment_http2_active_connections(void);
void metrics_decrement_http2_active_connections(void);

// Prometheus format export
char* metrics_export_prometheus(void);

#ifdef __cplusplus
}
#endif

#endif // METRICS_H
