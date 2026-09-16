#ifndef PROXY_CONFIG_H
#define PROXY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Default configuration values */
#define DEFAULT_PORT              8080
#define DEFAULT_MAX_CLIENTS       10
#define DEFAULT_MAX_BYTES         4096
#define DEFAULT_MAX_CACHE_SIZE    (200 * 1024 * 1024)
#define DEFAULT_MAX_ELEMENT_SIZE  (10 * 1024)
#define DEFAULT_BIND_ADDRESS      "0.0.0.0"
#define DEFAULT_LOG_LEVEL         "INFO"
#define DEFAULT_LOG_FILE          "proxy.log"
#define DEFAULT_LOG_FORMAT        "SQUID"
#define DEFAULT_ENABLE_ADMIN      true
#define DEFAULT_ADMIN_PORT        8081
#define DEFAULT_CONFIG_FILE       "/etc/proxy/proxy.conf"

/* Feature flags */
#define DEFAULT_ENABLE_HTTPS      true
#define DEFAULT_ENABLE_HTTP2      false
#define DEFAULT_ENABLE_IPV6       false
#define DEFAULT_ENABLE_AUTH       false
#define DEFAULT_ENABLE_CACHE      true
#define DEFAULT_ENABLE_FILTER     false
#define DEFAULT_ENABLE_THROTTLING false

/* Logging levels */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} LogLevel;

/* Configuration structure */
typedef struct {
    /* Network */
    int    port;
    char   bind_address[256];
    bool   enable_ipv6;

    /* Limits */
    int    max_clients;
    size_t max_bytes;
    size_t max_cache_size;
    size_t max_element_size;

    /* Caching */
    bool   enable_cache;
    char   cache_dir[512];

    /* Security */
    bool   enable_auth;
    char   auth_file[512];
    char   blocklist_file[512];
    char   allow_ip_file[512];
    char   deny_ip_file[512];

    /* Logging */
    char   log_file[512];
    char   log_level[32];
    char   log_format[32];
    bool   log_rotation;
    int    log_max_size_mb;

    /* Admin */
    bool   enable_admin;
    int    admin_port;

    /* Throttling */
    bool   enable_throttling;
    int    rate_limit_rps;
    int    rate_limit_burst;
    int    bandwidth_limit_kbps;

    /* Features */
    bool   enable_https;
    bool   enable_http2;
    bool   enable_filter;
} ProxyConfig;

extern ProxyConfig g_config;

/* Configuration source precedence:
 * 1. Compiled-in defaults
 * 2. Configuration file
 * 3. Environment variables
 * 4. Command-line arguments
 */

/**
 * Initialize configuration with compiled-in defaults.
 */
void config_init_defaults(ProxyConfig *cfg);

/**
 * Load configuration from a file.
 * Returns 0 on success, -1 on error (file not found is not fatal - uses defaults).
 */
int config_load_file(ProxyConfig *cfg, const char *path);

/**
 * Apply environment variable overrides.
 * Recognized variables: PROXY_PORT, PROXY_MAX_CLIENTS, PROXY_MAX_CACHE_SIZE,
 * PROXY_LOG_LEVEL, PROXY_BIND_ADDRESS, PROXY_CONFIG_FILE
 */
void config_apply_env(ProxyConfig *cfg);

/**
 * Apply command-line argument overrides.
 * Supports: --port=N, --max-clients=N, --cache-size=N, --log-level=LEVEL
 * Returns 0 on success, -1 if invalid argument.
 */
int config_apply_args(ProxyConfig *cfg, int argc, char *argv[]);

/**
 * Validate configuration values.
 * Returns 0 on success, -1 on validation failure.
 */
int config_validate(const ProxyConfig *cfg);

/**
 * Print configuration to stdout (for debugging).
 */
void config_print(const ProxyConfig *cfg);

/**
 * Convert log level string to LogLevel enum.
 */
LogLevel log_level_from_string(const char *level);

/**
 * Convert LogLevel enum to string.
 */
const char *log_level_to_string(LogLevel level);

#endif /* PROXY_CONFIG_H */