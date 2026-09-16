#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>

static ServerMetrics g_metrics;

void metrics_init(void) {
    g_metrics.requests_total = 0;
    g_metrics.requests_success = 0;
    g_metrics.requests_error = 0;
    g_metrics.cache_hits = 0;
    g_metrics.cache_misses = 0;
    g_metrics.bytes_transferred = 0;
    g_metrics.active_connections = 0;
    pthread_mutex_init(&g_metrics.lock, NULL);
}

void metrics_increment_requests(void) {
    pthread_mutex_lock(&g_metrics.lock);
    g_metrics.requests_total++;
    pthread_mutex_unlock(&g_metrics.lock);
}

void metrics_increment_success(void) {
    pthread_mutex_lock(&g_metrics.lock);
    g_metrics.requests_success++;
    pthread_mutex_unlock(&g_metrics.lock);
}

void metrics_increment_errors(void) {
    pthread_mutex_lock(&g_metrics.lock);
    g_metrics.requests_error++;
    pthread_mutex_unlock(&g_metrics.lock);
}

void metrics_increment_cache_hits(void) {
    pthread_mutex_lock(&g_metrics.lock);
    g_metrics.cache_hits++;
    pthread_mutex_unlock(&g_metrics.lock);
}

void metrics_increment_cache_misses(void) {
    pthread_mutex_lock(&g_metrics.lock);
    g_metrics.cache_misses++;
    pthread_mutex_unlock(&g_metrics.lock);
}

void metrics_add_bytes(size_t bytes) {
    pthread_mutex_lock(&g_metrics.lock);
    g_metrics.bytes_transferred += bytes;
    pthread_mutex_unlock(&g_metrics.lock);
}

void metrics_set_active_connections(int32_t count) {
    pthread_mutex_lock(&g_metrics.lock);
    g_metrics.active_connections = count;
    pthread_mutex_unlock(&g_metrics.lock);
}

void metrics_increment_active_connections(void) {
    pthread_mutex_lock(&g_metrics.lock);
    g_metrics.active_connections++;
    pthread_mutex_unlock(&g_metrics.lock);
}

void metrics_decrement_active_connections(void) {
    pthread_mutex_lock(&g_metrics.lock);
    if (g_metrics.active_connections > 0) {
        g_metrics.active_connections--;
    }
    pthread_mutex_unlock(&g_metrics.lock);
}

char* metrics_export_prometheus(void) {
    // Need approx 1024 bytes
    char* buf = (char*)malloc(1024);
    if (!buf) return NULL;
    
    pthread_mutex_lock(&g_metrics.lock);
    snprintf(buf, 1024,
        "# HELP proxy_requests_total Total number of HTTP requests processed\n"
        "# TYPE proxy_requests_total counter\n"
        "proxy_requests_total %lu\n"
        "# HELP proxy_requests_success Total number of successful requests\n"
        "# TYPE proxy_requests_success counter\n"
        "proxy_requests_success %lu\n"
        "# HELP proxy_requests_error Total number of failed requests\n"
        "# TYPE proxy_requests_error counter\n"
        "proxy_requests_error %lu\n"
        "# HELP proxy_cache_hits Total number of cache hits\n"
        "# TYPE proxy_cache_hits counter\n"
        "proxy_cache_hits %lu\n"
        "# HELP proxy_cache_misses Total number of cache misses\n"
        "# TYPE proxy_cache_misses counter\n"
        "proxy_cache_misses %lu\n"
        "# HELP proxy_bytes_transferred Total bytes transferred\n"
        "# TYPE proxy_bytes_transferred counter\n"
        "proxy_bytes_transferred %lu\n"
        "# HELP proxy_active_connections Current number of active client connections\n"
        "# TYPE proxy_active_connections gauge\n"
        "proxy_active_connections %d\n",
        (unsigned long)g_metrics.requests_total,
        (unsigned long)g_metrics.requests_success,
        (unsigned long)g_metrics.requests_error,
        (unsigned long)g_metrics.cache_hits,
        (unsigned long)g_metrics.cache_misses,
        (unsigned long)g_metrics.bytes_transferred,
        g_metrics.active_connections);
    pthread_mutex_unlock(&g_metrics.lock);
    
    return buf;
}
