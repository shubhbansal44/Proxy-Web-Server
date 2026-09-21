#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#define CONFIG_MAX_LINE 1024
#define CONFIG_SEPARATOR "="

/**
 * Initialize configuration with compiled-in defaults.
 */
void config_init_defaults(ProxyConfig *cfg) {
    if (!cfg) return;
    
    /* Network */
    cfg->port = DEFAULT_PORT;
    strncpy(cfg->bind_address, DEFAULT_BIND_ADDRESS, sizeof(cfg->bind_address) - 1);
    cfg->bind_address[sizeof(cfg->bind_address) - 1] = '\0';
    cfg->enable_ipv6 = DEFAULT_ENABLE_IPV6;
    
    /* Limits */
    cfg->max_clients = DEFAULT_MAX_CLIENTS;
    cfg->max_bytes = DEFAULT_MAX_BYTES;
    cfg->max_cache_size = DEFAULT_MAX_CACHE_SIZE;
    cfg->max_element_size = DEFAULT_MAX_ELEMENT_SIZE;
    
    /* Protocol Limits */
    cfg->max_http1_connections = DEFAULT_MAX_HTTP1_CONNECTIONS;
    cfg->max_http2_connections = DEFAULT_MAX_HTTP2_CONNECTIONS;
    cfg->max_http3_connections = DEFAULT_MAX_HTTP3_CONNECTIONS;
    
    /* Caching */
    cfg->enable_cache = DEFAULT_ENABLE_CACHE;
    cfg->cache_dir[0] = '\0';
    
    /* Security */
    cfg->enable_auth = DEFAULT_ENABLE_AUTH;
    cfg->auth_file[0] = '\0';
    cfg->blocklist_file[0] = '\0';
    cfg->allow_ip_file[0] = '\0';
    cfg->deny_ip_file[0] = '\0';
    
    /* Logging */
    strncpy(cfg->log_file, DEFAULT_LOG_FILE, sizeof(cfg->log_file) - 1);
    cfg->log_file[sizeof(cfg->log_file) - 1] = '\0';
    strncpy(cfg->log_level, DEFAULT_LOG_LEVEL, sizeof(cfg->log_level) - 1);
    cfg->log_level[sizeof(cfg->log_level) - 1] = '\0';
    strncpy(cfg->log_format, DEFAULT_LOG_FORMAT, sizeof(cfg->log_format) - 1);
    cfg->log_format[sizeof(cfg->log_format) - 1] = '\0';
    cfg->log_rotation = false;
    cfg->log_max_size_mb = 10;
    
    cfg->enable_admin = DEFAULT_ENABLE_ADMIN;
    cfg->admin_port = DEFAULT_ADMIN_PORT;
    
    /* Throttling */
    cfg->enable_throttling = DEFAULT_ENABLE_THROTTLING;
    cfg->rate_limit_rps = 100;
    cfg->rate_limit_burst = 20;
    cfg->bandwidth_limit_kbps = 1000;
    
    /* Features */
    cfg->enable_https = DEFAULT_ENABLE_HTTPS;
    cfg->tls_verify_peer = true;
    cfg->ca_bundle_file[0] = '\0';
    cfg->crl_file[0] = '\0';
    cfg->enable_http2 = DEFAULT_ENABLE_HTTP2;
    cfg->enable_filter = DEFAULT_ENABLE_FILTER;
}

/**
 * Trim whitespace from the beginning and end of a string.
 */
static void trim_whitespace(char *str) {
    char *end;
    
    // Trim leading space
    while (isspace(*str)) str++;
    
    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) end--;
    
    // Write new null terminator
    *(end + 1) = '\0';
}

/**
 * Parse a simple INI-style line: key = value
 * Returns 0 on success, -1 on failure to parse.
 */
static int parse_config_line(const char *line, char *key, size_t key_size, 
                            char *value, size_t value_size) {
    const char *sep = strchr(line, '=');
    if (!sep) return -1;
    
    size_t key_len = sep - line;
    if (key_len >= key_size) return -1;
    
    memcpy(key, line, key_len);
    key[key_len] = '\0';
    trim_whitespace(key);
    
    const char *val_start = sep + 1;
    while (isspace(*val_start)) val_start++;
    
    size_t val_len = strlen(val_start);
    if (val_len >= value_size) return -1;
    
    memcpy(value, val_start, val_len);
    value[val_len] = '\0';
    trim_whitespace(value);
    
    return 0;
}

/**
 * Load configuration from a file.
 * Returns 0 on success, -1 on error (file not found is not fatal - uses defaults).
 */
int config_load_file(ProxyConfig *cfg, const char *path) {
    FILE *fp;
    char line[CONFIG_MAX_LINE];
    char key[256];
    char value[512];
    
    if (!cfg || !path) return -1;
    
    // Try to open the file
    fp = fopen(path, "r");
    if (!fp) {
        // File not found is not fatal - we'll use defaults
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    
    // Parse each line
    while (fgets(line, sizeof(line), fp)) {
        
        // Skip empty lines and comments
        char *trimmed = line;
        while (isspace(*trimmed)) trimmed++;
        if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';') {
            continue;
        }
        
        // Parse key = value
        if (parse_config_line(trimmed, key, sizeof(key), value, sizeof(value)) == 0) {
            // Handle known configuration keys
            if (strcmp(key, "port") == 0) {
                cfg->port = atoi(value);
            } else if (strcmp(key, "bind_address") == 0) {
                strncpy(cfg->bind_address, value, sizeof(cfg->bind_address) - 1);
                cfg->bind_address[sizeof(cfg->bind_address) - 1] = '\0';
            } else if (strcmp(key, "enable_ipv6") == 0) {
                cfg->enable_ipv6 = (strcasecmp(value, "true") == 0 || 
                                   strcmp(value, "1") == 0);
            } else if (strcmp(key, "max_clients") == 0) {
                cfg->max_clients = atoi(value);
            } else if (strcmp(key, "max_bytes") == 0) {
                cfg->max_bytes = atoi(value);
            } else if (strcmp(key, "max_cache_size") == 0) {
                cfg->max_cache_size = atoll(value);
            } else if (strcmp(key, "max_element_size") == 0) {
                cfg->max_element_size = atoi(value);
            } else if (strcmp(key, "enable_cache") == 0) {
                cfg->enable_cache = (strcasecmp(value, "true") == 0 || 
                                    strcmp(value, "1") == 0);
            } else if (strcmp(key, "cache_dir") == 0) {
                strncpy(cfg->cache_dir, value, sizeof(cfg->cache_dir) - 1);
                cfg->cache_dir[sizeof(cfg->cache_dir) - 1] = '\0';
            } else if (strcmp(key, "enable_auth") == 0) {
                cfg->enable_auth = (strcasecmp(value, "true") == 0 || 
                                   strcmp(value, "1") == 0);
            } else if (strcmp(key, "auth_file") == 0) {
                strncpy(cfg->auth_file, value, sizeof(cfg->auth_file) - 1);
                cfg->auth_file[sizeof(cfg->auth_file) - 1] = '\0';
            } else if (strcmp(key, "blocklist_file") == 0) {
                strncpy(cfg->blocklist_file, value, sizeof(cfg->blocklist_file) - 1);
                cfg->blocklist_file[sizeof(cfg->blocklist_file) - 1] = '\0';
            } else if (strcmp(key, "allow_ip_file") == 0) {
                strncpy(cfg->allow_ip_file, value, sizeof(cfg->allow_ip_file) - 1);
                cfg->allow_ip_file[sizeof(cfg->allow_ip_file) - 1] = '\0';
            } else if (strcmp(key, "deny_ip_file") == 0) {
                strncpy(cfg->deny_ip_file, value, sizeof(cfg->deny_ip_file) - 1);
                cfg->deny_ip_file[sizeof(cfg->deny_ip_file) - 1] = '\0';
            } else if (strcmp(key, "log_file") == 0) {
                strncpy(cfg->log_file, value, sizeof(cfg->log_file) - 1);
                cfg->log_file[sizeof(cfg->log_file) - 1] = '\0';
            } else if (strcmp(key, "log_level") == 0) {
                strncpy(cfg->log_level, value, sizeof(cfg->log_level) - 1);
                cfg->log_level[sizeof(cfg->log_level) - 1] = '\0';
            } else if (strcmp(key, "log_format") == 0) {
                strncpy(cfg->log_format, value, sizeof(cfg->log_format) - 1);
                cfg->log_format[sizeof(cfg->log_format) - 1] = '\0';
            } else if (strcmp(key, "enable_admin") == 0) {
                cfg->enable_admin = (strcasecmp(value, "true") == 0 || 
                                    strcmp(value, "1") == 0);
            } else if (strcmp(key, "admin_port") == 0) {
                cfg->admin_port = atoi(value);
            } else if (strcmp(key, "log_rotation") == 0) {
                cfg->log_rotation = (strcasecmp(value, "true") == 0 || 
                                    strcmp(value, "1") == 0);
            } else if (strcmp(key, "log_max_size_mb") == 0) {
                cfg->log_max_size_mb = atoi(value);
            } else if (strcmp(key, "max_http1_connections") == 0) {
                cfg->max_http1_connections = atoi(value);
            } else if (strcmp(key, "max_http2_connections") == 0) {
                cfg->max_http2_connections = atoi(value);
            } else if (strcmp(key, "max_http3_connections") == 0) {
                cfg->max_http3_connections = atoi(value);
            } else if (strcmp(key, "enable_throttling") == 0) {
                cfg->enable_throttling = (strcasecmp(value, "true") == 0 || 
                                         strcmp(value, "1") == 0);
            } else if (strcmp(key, "rate_limit_rps") == 0) {
                cfg->rate_limit_rps = atoi(value);
            } else if (strcmp(key, "rate_limit_burst") == 0) {
                cfg->rate_limit_burst = atoi(value);
            } else if (strcmp(key, "bandwidth_limit_kbps") == 0) {
                cfg->bandwidth_limit_kbps = atoi(value);
            } else if (strcmp(key, "enable_https") == 0) {
                cfg->enable_https = (strcasecmp(value, "true") == 0 || 
                                    strcmp(value, "1") == 0);
            } else if (strcmp(key, "enable_http2") == 0) {
                cfg->enable_http2 = (strcasecmp(value, "true") == 0 || 
                                    strcmp(value, "1") == 0);
            } else if (strcmp(key, "enable_http3") == 0) {
                cfg->enable_http3 = (strcasecmp(value, "true") == 0 || 
                                    strcmp(value, "1") == 0);
            } else if (strcmp(key, "tls_verify_peer") == 0) {
                cfg->tls_verify_peer = (strcasecmp(value, "true") == 0 || 
                                       strcmp(value, "1") == 0);
            } else if (strcmp(key, "ca_bundle_file") == 0) {
                strncpy(cfg->ca_bundle_file, value, sizeof(cfg->ca_bundle_file) - 1);
                cfg->ca_bundle_file[sizeof(cfg->ca_bundle_file) - 1] = '\0';
            } else if (strcmp(key, "crl_file") == 0) {
                strncpy(cfg->crl_file, value, sizeof(cfg->crl_file) - 1);
                cfg->crl_file[sizeof(cfg->crl_file) - 1] = '\0';
            } else if (strcmp(key, "enable_filter") == 0) {
                cfg->enable_filter = (strcasecmp(value, "true") == 0 || 
                                     strcmp(value, "1") == 0);
            }
        }
        // Unknown keys are ignored for forward compatibility
    }
    
    fclose(fp);
    return 0;
}

/**
 * Apply environment variable overrides.
 * Recognized variables: PROXY_PORT, PROXY_MAX_CLIENTS, PROXY_MAX_CACHE_SIZE,
 * PROXY_LOG_LEVEL, PROXY_BIND_ADDRESS, PROXY_CONFIG_FILE
 */
void config_apply_env(ProxyConfig *cfg) {
    if (!cfg) return;
    
    const char *env_val;
    
    // Network
    if ((env_val = getenv("PROXY_PORT")) != NULL) {
        cfg->port = atoi(env_val);
    }
    if ((env_val = getenv("PROXY_BIND_ADDRESS")) != NULL) {
        strncpy(cfg->bind_address, env_val, sizeof(cfg->bind_address) - 1);
        cfg->bind_address[sizeof(cfg->bind_address) - 1] = '\0';
    }
    if ((env_val = getenv("PROXY_ENABLE_IPV6")) != NULL) {
        cfg->enable_ipv6 = (strcasecmp(env_val, "true") == 0 || 
                           strcmp(env_val, "1") == 0);
    }
    
    // Limits
    if ((env_val = getenv("PROXY_MAX_CLIENTS")) != NULL) {
        cfg->max_clients = atoi(env_val);
    }
    if ((env_val = getenv("PROXY_MAX_BYTES")) != NULL) {
        cfg->max_bytes = atoi(env_val);
    }
    if ((env_val = getenv("PROXY_MAX_HTTP1_CONNECTIONS")) != NULL) {
        cfg->max_http1_connections = atoi(env_val);
    }
    if ((env_val = getenv("PROXY_MAX_HTTP2_CONNECTIONS")) != NULL) {
        cfg->max_http2_connections = atoi(env_val);
    }
    if ((env_val = getenv("PROXY_MAX_HTTP3_CONNECTIONS")) != NULL) {
        cfg->max_http3_connections = atoi(env_val);
    }
    if ((env_val = getenv("PROXY_MAX_CACHE_SIZE")) != NULL) {
        cfg->max_cache_size = atoll(env_val);
    }
    if ((env_val = getenv("PROXY_MAX_ELEMENT_SIZE")) != NULL) {
        cfg->max_element_size = atoi(env_val);
    }
    
    // Caching
    if ((env_val = getenv("PROXY_ENABLE_CACHE")) != NULL) {
        cfg->enable_cache = (strcasecmp(env_val, "true") == 0 || 
                            strcmp(env_val, "1") == 0);
    }
    if ((env_val = getenv("PROXY_CACHE_DIR")) != NULL) {
        strncpy(cfg->cache_dir, env_val, sizeof(cfg->cache_dir) - 1);
        cfg->cache_dir[sizeof(cfg->cache_dir) - 1] = '\0';
    }
    
    // Security
    if ((env_val = getenv("PROXY_ENABLE_AUTH")) != NULL) {
        cfg->enable_auth = (strcasecmp(env_val, "true") == 0 || 
                           strcmp(env_val, "1") == 0);
    }
    if ((env_val = getenv("PROXY_AUTH_FILE")) != NULL) {
        strncpy(cfg->auth_file, env_val, sizeof(cfg->auth_file) - 1);
        cfg->auth_file[sizeof(cfg->auth_file) - 1] = '\0';
    }
    if ((env_val = getenv("PROXY_BLOCKLIST_FILE")) != NULL) {
        strncpy(cfg->blocklist_file, env_val, sizeof(cfg->blocklist_file) - 1);
        cfg->blocklist_file[sizeof(cfg->blocklist_file) - 1] = '\0';
    }
    
    // Logging
    if ((env_val = getenv("PROXY_LOG_FILE")) != NULL) {
        strncpy(cfg->log_file, env_val, sizeof(cfg->log_file) - 1);
        cfg->log_file[sizeof(cfg->log_file) - 1] = '\0';
    }
    if ((env_val = getenv("PROXY_LOG_LEVEL")) != NULL) {
        strncpy(cfg->log_level, env_val, sizeof(cfg->log_level) - 1);
        cfg->log_level[sizeof(cfg->log_level) - 1] = '\0';
    }
    if ((env_val = getenv("PROXY_LOG_ROTATION")) != NULL) {
        cfg->log_rotation = (strcasecmp(env_val, "true") == 0 || 
                            strcmp(env_val, "1") == 0);
    }
    if ((env_val = getenv("PROXY_LOG_MAX_SIZE_MB")) != NULL) {
        cfg->log_max_size_mb = atoi(env_val);
    }
    
    // Throttling
    if ((env_val = getenv("PROXY_ENABLE_THROTTLING")) != NULL) {
        cfg->enable_throttling = (strcasecmp(env_val, "true") == 0 || 
                                 strcmp(env_val, "1") == 0);
    }
    if ((env_val = getenv("PROXY_RATE_LIMIT_RPS")) != NULL) {
        cfg->rate_limit_rps = atoi(env_val);
    }
    if ((env_val = getenv("PROXY_RATE_LIMIT_BURST")) != NULL) {
        cfg->rate_limit_burst = atoi(env_val);
    }
    if ((env_val = getenv("PROXY_BANDWIDTH_LIMIT_KBPS")) != NULL) {
        cfg->bandwidth_limit_kbps = atoi(env_val);
    }
    
    // Features
    if ((env_val = getenv("PROXY_ENABLE_HTTPS")) != NULL) {
        cfg->enable_https = (strcasecmp(env_val, "true") == 0 || 
                            strcmp(env_val, "1") == 0);
    }
    if ((env_val = getenv("PROXY_ENABLE_HTTP2")) != NULL) {
        cfg->enable_http2 = (strcasecmp(env_val, "true") == 0 || 
                            strcmp(env_val, "1") == 0);
    }
    if ((env_val = getenv("PROXY_ENABLE_HTTP3")) != NULL) {
        cfg->enable_http3 = (strcasecmp(env_val, "true") == 0 || 
                            strcmp(env_val, "1") == 0);
    }
    if ((env_val = getenv("PROXY_TLS_VERIFY_PEER")) != NULL) {
        cfg->tls_verify_peer = (strcasecmp(env_val, "true") == 0 || 
                               strcmp(env_val, "1") == 0);
    }
    if ((env_val = getenv("PROXY_CA_BUNDLE_FILE")) != NULL) {
        strncpy(cfg->ca_bundle_file, env_val, sizeof(cfg->ca_bundle_file) - 1);
        cfg->ca_bundle_file[sizeof(cfg->ca_bundle_file) - 1] = '\0';
    }
    if ((env_val = getenv("PROXY_CRL_FILE")) != NULL) {
        strncpy(cfg->crl_file, env_val, sizeof(cfg->crl_file) - 1);
        cfg->crl_file[sizeof(cfg->crl_file) - 1] = '\0';
    }
    if ((env_val = getenv("PROXY_ENABLE_FILTER")) != NULL) {
        cfg->enable_filter = (strcasecmp(env_val, "true") == 0 || 
                             strcmp(env_val, "1") == 0);
    }
}

/**
 * Apply command-line argument overrides.
 * Supports: --port=N, --max-clients=N, --cache-size=N, --log-level=LEVEL, --allow-ip-file=FILE, --deny-ip-file=FILE
 * Returns 0 on success, -1 if invalid argument.
 */
int config_apply_args(ProxyConfig *cfg, int argc, char *argv[]) {
    if (!cfg || !argv) return -1;
    
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            char *arg = argv[i] + 2; // Skip "--"
            char *eq = strchr(arg, '=');
            
            if (!eq) {
                // Boolean flags like --enable-auth
                if (strcmp(arg, "help") == 0 || strcmp(arg, "h") == 0) {
                    printf("Usage: %s [options]\n"
                           "Options:\n"
                           "  --port=N                Set proxy port (default: %d)\n"
                           "  --bind-address=IP       Set bind address (default: %s)\n"
                           "  --enable-ipv6           Enable IPv6 support\n"
                           "  --max-clients=N         Max concurrent clients (default: %d)\n"
                           "  --max-bytes=N           Max request size (default: %d)\n"
                           "  --cache-size=N          Max cache size in bytes (default: %d)\n"
                           "  --element-size=N        Max cache element size (default: %d)\n"
                           "  --enable-cache          Enable caching\n"
                           "  --cache-dir=DIR         Cache directory\n"
                           "  --enable-auth           Enable authentication\n"
                           "  --auth-file=FILE        Auth file path\n"
                           "  --blocklist-file=FILE   Blocklist file path\n"
                           "  --allow-ip-file=FILE    File with allowed IP addresses\n"
                           "  --deny-ip-file=FILE     File with denied IP addresses\n"
                           "  --log-file=FILE         Log file path (default: %s)\n"
                           "  --log-level=LEVEL       Log level (DEBUG,INFO,WARN,ERROR)\n"
                           "  --enable-throttling     Enable rate limiting\n"
                           "  --rate-limit-rps=N      Requests per second limit\n"
                           "  --rate-limit-burst=N    Burst allowance\n"
                           "  --bandwidth-limit=N     Bandwidth limit in kbps\n"
                           "  --enable-https          Enable HTTPS CONNECT\n"
                           "  --enable-http2          Enable HTTP/2 support\n"
                           "  --enable-http3          Enable HTTP/3 support\n"
                           "  --tls-verify-peer=0/1   Verify TLS peers (default: 1)\n"
                           "  --ca-bundle-file=FILE   CA bundle file for TLS\n"
                           "  --crl-file=FILE         CRL file for TLS\n"
                           "  --enable-filter         Enable content filtering\n",
                           argv[0], DEFAULT_PORT, DEFAULT_BIND_ADDRESS,
                           DEFAULT_MAX_CLIENTS, DEFAULT_MAX_BYTES,
                           DEFAULT_MAX_CACHE_SIZE, DEFAULT_MAX_ELEMENT_SIZE,
                           DEFAULT_LOG_FILE);
                    return -1; // Special return to indicate help was printed
                }
                
                // Boolean flags
                if (strcmp(arg, "enable-cache") == 0) {
                    cfg->enable_cache = true;
                } else if (strcmp(arg, "enable-auth") == 0) {
                    cfg->enable_auth = true;
                } else if (strcmp(arg, "enable-ipv6") == 0) {
                    cfg->enable_ipv6 = true;
                } else if (strcmp(arg, "enable-throttling") == 0) {
                    cfg->enable_throttling = true;
                } else if (strcmp(arg, "enable-https") == 0) {
                    cfg->enable_https = true;
                } else if (strcmp(arg, "enable-http2") == 0) {
                    cfg->enable_http2 = true;
                } else if (strcmp(arg, "enable-http3") == 0) {
                    cfg->enable_http3 = true;
                } else if (strcmp(arg, "enable-filter") == 0) {
                    cfg->enable_filter = true;
                } else {
                    fprintf(stderr, "Unknown option: %s\n", argv[i]);
                    return -1;
                }
            } else {
                // Key=value format
                *eq = '\0'; // Temporarily split the string
                char *key = arg;
                char *value = eq + 1;
                
                if (strcmp(key, "port") == 0) {
                    cfg->port = atoi(value);
                } else if (strcmp(key, "bind-address") == 0) {
                    strncpy(cfg->bind_address, value, sizeof(cfg->bind_address) - 1);
                    cfg->bind_address[sizeof(cfg->bind_address) - 1] = '\0';
                } else if (strcmp(key, "max-clients") == 0) {
                    cfg->max_clients = atoi(value);
                } else if (strcmp(key, "max-bytes") == 0) {
                    cfg->max_bytes = atoi(value);
                } else if (strcmp(key, "max-http1-connections") == 0) {
                    cfg->max_http1_connections = atoi(value);
                } else if (strcmp(key, "max-http2-connections") == 0) {
                    cfg->max_http2_connections = atoi(value);
                } else if (strcmp(key, "max-http3-connections") == 0) {
                    cfg->max_http3_connections = atoi(value);
                } else if (strcmp(key, "cache-size") == 0) {
                    cfg->max_cache_size = atoll(value);
                } else if (strcmp(key, "element-size") == 0) {
                    cfg->max_element_size = atoi(value);
                } else if (strcmp(key, "cache-dir") == 0) {
                    strncpy(cfg->cache_dir, value, sizeof(cfg->cache_dir) - 1);
                    cfg->cache_dir[sizeof(cfg->cache_dir) - 1] = '\0';
                } else if (strcmp(key, "auth-file") == 0) {
                    strncpy(cfg->auth_file, value, sizeof(cfg->auth_file) - 1);
                    cfg->auth_file[sizeof(cfg->auth_file) - 1] = '\0';
                } else if (strcmp(key, "blocklist-file") == 0) {
                    strncpy(cfg->blocklist_file, value, sizeof(cfg->blocklist_file) - 1);
                    cfg->blocklist_file[sizeof(cfg->blocklist_file) - 1] = '\0';
                } else if (strcmp(key, "allow-ip-file") == 0) {
                    strncpy(cfg->allow_ip_file, value, sizeof(cfg->allow_ip_file) - 1);
                    cfg->allow_ip_file[sizeof(cfg->allow_ip_file) - 1] = '\0';
                } else if (strcmp(key, "deny-ip-file") == 0) {
                    strncpy(cfg->deny_ip_file, value, sizeof(cfg->deny_ip_file) - 1);
                    cfg->deny_ip_file[sizeof(cfg->deny_ip_file) - 1] = '\0';
                } else if (strcmp(key, "log-file") == 0) {
                    strncpy(cfg->log_file, value, sizeof(cfg->log_file) - 1);
                    cfg->log_file[sizeof(cfg->log_file) - 1] = '\0';
                } else if (strcmp(key, "log-level") == 0) {
                    strncpy(cfg->log_level, value, sizeof(cfg->log_level) - 1);
                    cfg->log_level[sizeof(cfg->log_level) - 1] = '\0';
                } else if (strcmp(key, "rate-limit-rps") == 0) {
                    cfg->rate_limit_rps = atoi(value);
                } else if (strcmp(key, "rate-limit-burst") == 0) {
                    cfg->rate_limit_burst = atoi(value);
                } else if (strcmp(key, "bandwidth-limit") == 0) {
                    cfg->bandwidth_limit_kbps = atoi(value);
                } else if (strcmp(key, "tls-verify-peer") == 0) {
                    cfg->tls_verify_peer = (strcasecmp(value, "true") == 0 || 
                                           strcmp(value, "1") == 0);
                } else if (strcmp(key, "ca-bundle-file") == 0) {
                    strncpy(cfg->ca_bundle_file, value, sizeof(cfg->ca_bundle_file) - 1);
                    cfg->ca_bundle_file[sizeof(cfg->ca_bundle_file) - 1] = '\0';
                } else if (strcmp(key, "crl-file") == 0) {
                    strncpy(cfg->crl_file, value, sizeof(cfg->crl_file) - 1);
                    cfg->crl_file[sizeof(cfg->crl_file) - 1] = '\0';
                } else if (strcmp(key, "config") == 0) {
                    // Handled early in Main.c
                } else {
                    fprintf(stderr, "Unknown option: %s\n", argv[i]);
                    *eq = '='; // Restore the string
                    return -1;
                }
                *eq = '='; // Restore the string
            }
        } else {
            fprintf(stderr, "Invalid argument format: %s (expected --option or --option=value)\n", argv[i]);
            return -1;
        }
    }
    
    return 0;
}

/**
 * Validate configuration values.
 * Returns 0 on success, -1 on validation failure.
 */
int config_validate(const ProxyConfig *cfg) {
    if (!cfg) return -1;
    
    // Validate network settings
    if (cfg->port < 1 || cfg->port > 65535) {
        fprintf(stderr, "Invalid port: %d (must be 1-65535)\n", cfg->port);
        return -1;
    }
    
    if (strlen(cfg->bind_address) == 0) {
        fprintf(stderr, "Bind address cannot be empty\n");
        return -1;
    }
    
    // Validate limits
    if (cfg->max_clients < 1) {
        fprintf(stderr, "Max clients must be >= 1\n");
        return -1;
    }
    
    if (cfg->max_bytes < 1) {
        fprintf(stderr, "Max bytes must be >= 1\n");
        return -1;
    }
    
    if (cfg->max_cache_size < 0) {
        fprintf(stderr, "Max cache size cannot be negative\n");
        return -1;
    }
    
    if (cfg->max_element_size < 1) {
        fprintf(stderr, "Max element size must be >= 1\n");
        return -1;
    }
    
    // Validate cache directory if set
    if (strlen(cfg->cache_dir) > 0) {
        // Basic check - we could do more thorough validation
        // For now just ensure it's not too long
        if (strlen(cfg->cache_dir) >= sizeof(cfg->cache_dir)) {
            fprintf(stderr, "Cache directory path too long\n");
            return -1;
        }
    }
    
    // Validate auth settings
    if (cfg->enable_auth) {
        if (strlen(cfg->auth_file) == 0) {
            fprintf(stderr, "Auth file must be specified when auth is enabled\n");
            return -1;
        }
        if (strlen(cfg->auth_file) >= sizeof(cfg->auth_file)) {
            fprintf(stderr, "Auth file path too long\n");
            return -1;
        }
    }
    
    if (strlen(cfg->blocklist_file) >= sizeof(cfg->blocklist_file)) {
        fprintf(stderr, "Blocklist file path too long\n");
        return -1;
    }
    
    // Validate logging
    if (strlen(cfg->log_file) == 0) {
        fprintf(stderr, "Log file cannot be empty\n");
        return -1;
    }
    
    if (strlen(cfg->log_file) >= sizeof(cfg->log_file)) {
        fprintf(stderr, "Log file path too long\n");
        return -1;
    }
    
    // Validate log level
    int level = (int)log_level_from_string(cfg->log_level);
    if (level < 0) {
        fprintf(stderr, "Invalid log level: %s (must be DEBUG, INFO, WARN, or ERROR)\n", 
                cfg->log_level);
        return -1;
    }
    
    if (cfg->log_max_size_mb < 1) {
        fprintf(stderr, "Log max size must be >= 1 MB\n");
        return -1;
    }
    
    // Validate throttling
    if (cfg->enable_throttling) {
        if (cfg->rate_limit_rps < 1) {
            fprintf(stderr, "Rate limit RPS must be >= 1\n");
            return -1;
        }
        
        if (cfg->rate_limit_burst < 1) {
            fprintf(stderr, "Rate limit burst must be >= 1\n");
            return -1;
        }
        
        if (cfg->bandwidth_limit_kbps < 1) {
            fprintf(stderr, "Bandwidth limit must be >= 1 kbps\n");
            return -1;
        }
    }
    
    return 0;
}

/**
 * Print configuration to stdout (for debugging).
 */
void config_print(const ProxyConfig *cfg) {
    if (!cfg) return;
    
    printf("=== Proxy Configuration ===\n");
    
    printf("Network:\n");
    printf("  Port: %d\n", cfg->port);
    printf("  Bind Address: %s\n", cfg->bind_address);
    printf("  IPv6 Enabled: %s\n", cfg->enable_ipv6 ? "true" : "false");
    
    printf("Limits:\n");
    printf("  Max Clients: %d\n", cfg->max_clients);
    printf("  Max Request Size: %zu bytes\n", cfg->max_bytes);
    printf("  Max HTTP/1 Conns: %d\n", cfg->max_http1_connections);
    printf("  Max HTTP/2 Conns: %d\n", cfg->max_http2_connections);
    printf("  Max HTTP/3 Conns: %d\n", cfg->max_http3_connections);
    printf("  Max Cache Size: %zu bytes (%zu MB)\n", 
           cfg->max_cache_size, cfg->max_cache_size / (1024 * 1024));
    printf("  Max Element Size: %zu bytes\n", cfg->max_element_size);
    
    printf("Caching:\n");
    printf("  Enabled: %s\n", cfg->enable_cache ? "true" : "false");
    if (strlen(cfg->cache_dir) > 0) {
        printf("  Cache Directory: %s\n", cfg->cache_dir);
    }
    
    printf("Security:\n");
    printf("  Auth Enabled: %s\n", cfg->enable_auth ? "true" : "false");
    if (strlen(cfg->auth_file) > 0) {
        printf("  Auth File: %s\n", cfg->auth_file);
    }
    if (strlen(cfg->blocklist_file) > 0) {
        printf("  Blocklist File: %s\n", cfg->blocklist_file);
    }
    if (strlen(cfg->allow_ip_file) > 0) {
        printf("  Allow IP File: %s\n", cfg->allow_ip_file);
    }
    if (strlen(cfg->deny_ip_file) > 0) {
        printf("  Deny IP File: %s\n", cfg->deny_ip_file);
    }
    
    printf("Logging:\n");
    printf("  Log File: %s\n", cfg->log_file);
    printf("  Log Level: %s\n", cfg->log_level);
    printf("  Log Rotation: %s\n", cfg->log_rotation ? "true" : "false");
    printf("  Max Log Size: %d MB\n", cfg->log_max_size_mb);
    
    printf("Throttling:\n");
    printf("  Enabled: %s\n", cfg->enable_throttling ? "true" : "false");
    if (cfg->enable_throttling) {
        printf("  Rate Limit: %d req/s (burst: %d)\n", 
               cfg->rate_limit_rps, cfg->rate_limit_burst);
        printf("  Bandwidth Limit: %d kbps\n", cfg->bandwidth_limit_kbps);
    }
    
    printf("Features:\n");
    printf("  HTTPS CONNECT: %s\n", cfg->enable_https ? "true" : "false");
    printf("  TLS Verify Peer: %s\n", cfg->tls_verify_peer ? "true" : "false");
    if (cfg->ca_bundle_file[0]) printf("  CA Bundle File: %s\n", cfg->ca_bundle_file);
    if (cfg->crl_file[0]) printf("  CRL File: %s\n", cfg->crl_file);
    printf("  HTTP/2: %s\n", cfg->enable_http2 ? "true" : "false");
    printf("  HTTP/3: %s\n", cfg->enable_http3 ? "true" : "false");
    printf("  Content Filtering: %s\n", cfg->enable_filter ? "true" : "false");
    printf("==========================\n");
}

/**
 * Convert log level string to LogLevel enum.
 */
LogLevel log_level_from_string(const char *level) {
    if (!level) return LOG_INFO;
    
    if (strcasecmp(level, "DEBUG") == 0) return LOG_DEBUG;
    if (strcasecmp(level, "INFO") == 0) return LOG_INFO;
    if (strcasecmp(level, "WARN") == 0) return LOG_WARN;
    if (strcasecmp(level, "ERROR") == 0) return LOG_ERROR;
    
    return (LogLevel)-1; // Invalid
}

/**
 * Convert LogLevel enum to string.
 */
const char *log_level_to_string(LogLevel level) {
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        default:        return "UNKNOWN";
    }
}
#include <sys/inotify.h>
#include <pthread.h>
#include "logger.h"

static void *hot_reload_thread(void *arg) {
    char *config_path = (char *)arg;
    int fd = inotify_init();
    if (fd < 0) {
        LOG_ERROR("hot_reload: inotify_init failed");
        free(config_path);
        return NULL;
    }
    
    int wd = inotify_add_watch(fd, config_path, IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO);
    // Note: Since IN_MOVED_TO and VIM saving can trigger dir changes, 
    // basic inotify directly on the file might get IN_IGNORED if file is replaced.
    // Real robust watcher watches the dir. For simple requirements, we just watch the file and if we get IN_IGNORED we re-add.
    
    if (wd < 0) {
        LOG_WARN("hot_reload: could not watch config file %s", config_path);
        close(fd);
        free(config_path);
        return NULL;
    }

    char buffer[1024];
    while (1) {
        int length = read(fd, buffer, sizeof(buffer));
        if (length < 0) break;
        
        int i = 0;
        int should_reload = 0;
        int watch_removed = 0;
        
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                should_reload = 1;
            }
            if (event->mask & (IN_IGNORED)) {
                watch_removed = 1;
            }
            i += sizeof(struct inotify_event) + event->len;
        }

        if (watch_removed) {
            // File was probably replaced by sed/vim. Need to wait a tiny bit and re-add watch.
            usleep(100000); // 100ms
            wd = inotify_add_watch(fd, config_path, IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO);
            if (wd >= 0) {
                should_reload = 1;
            }
        }
        
        if (should_reload) {
            LOG_INFO("Config file changed, hot-reloading safe parameters...");
            ProxyConfig new_cfg;
            
            // Start from current config to preserve non-loaded states (e.g. CLI args for non-safe parts)
            memcpy(&new_cfg, &g_config, sizeof(ProxyConfig));
            
            // Initialize a fresh one to parse into, then merge safe fields
            ProxyConfig parsed_cfg;
            config_init_defaults(&parsed_cfg);
            if (config_load_file(&parsed_cfg, config_path) == 0) {
                config_apply_env(&parsed_cfg);
                
                // --- Apply Safe Parameters ---
                strncpy(g_config.log_level, parsed_cfg.log_level, sizeof(g_config.log_level));
                g_config.max_cache_size = parsed_cfg.max_cache_size;
                g_config.max_element_size = parsed_cfg.max_element_size;
                g_config.enable_cache = parsed_cfg.enable_cache;
                g_config.enable_throttling = parsed_cfg.enable_throttling;
                g_config.rate_limit_rps = parsed_cfg.rate_limit_rps;
                g_config.rate_limit_burst = parsed_cfg.rate_limit_burst;
                g_config.bandwidth_limit_kbps = parsed_cfg.bandwidth_limit_kbps;
                g_config.enable_filter = parsed_cfg.enable_filter;
                strncpy(g_config.log_file, parsed_cfg.log_file, sizeof(g_config.log_file));
                
                // Update logger level runtime
                LogLevelEnum l_enum = LOG_LEVEL_INFO;
                if (!strcmp(g_config.log_level, "DEBUG")) l_enum = LOG_LEVEL_DEBUG;
                else if (!strcmp(g_config.log_level, "WARN")) l_enum = LOG_LEVEL_WARN;
                else if (!strcmp(g_config.log_level, "ERROR")) l_enum = LOG_LEVEL_ERROR;
                else if (!strcmp(g_config.log_level, "CRITICAL")) l_enum = LOG_LEVEL_CRITICAL;
                logger_set_level(l_enum);
                
                LOG_INFO("Hot-reload completed.");
            }
        }
    }
    
    close(fd);
    free(config_path);
    return NULL;
}

int config_start_hot_reload(const char *config_path) {
    if (!config_path) return -1;
    char *path = strdup(config_path);
    pthread_t tid;
    if (pthread_create(&tid, NULL, hot_reload_thread, path) != 0) {
        free(path);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
