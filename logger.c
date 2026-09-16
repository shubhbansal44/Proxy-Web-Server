#include "logger.h"
#include <syslog.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>

static LoggerConfig g_logger_config = {
    .level = LOG_LEVEL_INFO,
    .format = LOG_FORMAT_PLAIN,
    .log_file = {0},
    .use_stdout = true,
    .use_syslog = false,
    .rotation_enabled = false,
    .max_file_size_mb = 10
};

static FILE* g_log_fp = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char* level_to_string(LogLevelEnum level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

static void rotate_log_if_needed() {
    if (!g_logger_config.rotation_enabled || g_log_fp == NULL) return;
    
    struct stat st;
    if (fstat(fileno(g_log_fp), &st) == 0) {
        off_t max_size = g_logger_config.max_file_size_mb * 1024 * 1024;
        if (st.st_size >= max_size) {
            fclose(g_log_fp);
            char backup[1024];
            snprintf(backup, sizeof(backup), "%s.1", g_logger_config.log_file);
            rename(g_logger_config.log_file, backup);
            g_log_fp = fopen(g_logger_config.log_file, "a");
        }
    }
}

bool logger_init(const LoggerConfig* config) {
    if (config) {
        g_logger_config = *config;
    }
    
    pthread_mutex_lock(&g_log_mutex);
    if (g_logger_config.use_syslog) {
        openlog("proxy-web-server", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
    
    if (strlen(g_logger_config.log_file) > 0) {
        g_log_fp = fopen(g_logger_config.log_file, "a");
        if (!g_log_fp) {
            pthread_mutex_unlock(&g_log_mutex);
            fprintf(stderr, "Failed to open log file: %s\n", g_logger_config.log_file);
            return false;
        }
    }
    pthread_mutex_unlock(&g_log_mutex);
    return true;
}

void logger_shutdown(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    if (g_logger_config.use_syslog) {
        closelog();
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void logger_log(LogLevelEnum level, const char* file, int line, const char* func, const char* fmt, ...) {
    if (level < g_logger_config.level) return;
    
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    
    pthread_mutex_lock(&g_log_mutex);
    
    rotate_log_if_needed();
    
    if (g_logger_config.format == LOG_FORMAT_JSON) {
        // Simple JSON escaping could be added here
        char json_buf[4096];
        snprintf(json_buf, sizeof(json_buf), 
                "{\"timestamp\":\"%s\",\"level\":\"%s\",\"file\":\"%s:%d\",\"func\":\"%s\",\"msg\":\"%s\"}\n",
                time_buf, level_to_string(level), file, line, func, buffer);
        if (g_logger_config.use_stdout) {
            fprintf(stdout, "%s", json_buf);
            fflush(stdout);
        }
        if (g_log_fp) {
            fprintf(g_log_fp, "%s", json_buf);
            fflush(g_log_fp);
        }
    } else {
        char txt_buf[4096];
        snprintf(txt_buf, sizeof(txt_buf), "[%s] [%s] [%s:%d] %s\n", 
                 time_buf, level_to_string(level), file, line, buffer);
                 
        if (g_logger_config.use_stdout) {
            FILE* out = (level >= LOG_LEVEL_ERROR) ? stderr : stdout;
            fprintf(out, "%s", txt_buf);
            fflush(out);
        }
        if (g_log_fp) {
            fprintf(g_log_fp, "%s", txt_buf);
            fflush(g_log_fp);
        }
    }
    
    if (g_logger_config.use_syslog) {
        int syslog_level = LOG_INFO;
        switch (level) {
            case LOG_LEVEL_DEBUG: syslog_level = LOG_DEBUG; break;
            case LOG_LEVEL_INFO:  syslog_level = LOG_INFO; break;
            case LOG_LEVEL_WARN:  syslog_level = LOG_WARNING; break;
            case LOG_LEVEL_ERROR: syslog_level = LOG_ERR; break;
            case LOG_LEVEL_CRITICAL: syslog_level = LOG_CRIT; break;
        }
        syslog(syslog_level, "[%s:%d] %s", file, line, buffer);
    }
    
    pthread_mutex_unlock(&g_log_mutex);
}

void logger_access_log(const char* client_ip, int status_code, size_t response_size, 
                       const char* method, const char* url, double elapsed_ms, 
                       const char* content_type, const char* trace_id) {
                       
    time_t now = time(NULL);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    if (!content_type) content_type = "-";
    if (!client_ip) client_ip = "-";
    if (!method) method = "-";
    if (!url) url = "-";
    if (!trace_id) trace_id = "-";
    
    pthread_mutex_lock(&g_log_mutex);
    rotate_log_if_needed();
    
    char log_entry[4096];
    
    if (g_logger_config.format == LOG_FORMAT_SQUID) {
        // timestamp elapsed client_ip action/code size method url identity hierarchy/from content_type [trace_id]
        // Squid timestamp is standard unix time with ms
        double squid_time = (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0);
        snprintf(log_entry, sizeof(log_entry), 
                 "%.3f %6.0f %s %s/%d %zu %s %s - DIRECT/%s %s [%s]\n",
                 squid_time, elapsed_ms, client_ip, 
                 status_code >= 400 ? "ERR" : "TCP_MISS", 
                 status_code, response_size, method, url, 
                 "-", content_type, trace_id); // simplified hierarchy
                 
    } else if (g_logger_config.format == LOG_FORMAT_JSON) {
        struct tm* tm_info = gmtime(&now);
        char time_buf[64];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", tm_info);
        
        snprintf(log_entry, sizeof(log_entry),
                 "{\"type\":\"access\",\"timestamp\":\"%s\",\"client_ip\":\"%s\",\"status\":%d,\"size\":%zu,\"method\":\"%s\",\"url\":\"%s\",\"elapsed_ms\":%.2f,\"content_type\":\"%s\",\"trace_id\":\"%s\"}\n",
                 time_buf, client_ip, status_code, response_size, method, url, elapsed_ms, content_type, trace_id);
                 
    } else { // CLF or Plain
        struct tm* tm_info = localtime(&now);
        char time_buf[64];
        strftime(time_buf, sizeof(time_buf), "%d/%b/%Y:%H:%M:%S %z", tm_info);
        
        snprintf(log_entry, sizeof(log_entry), 
                 "%s - - [%s] \"%s %s HTTP/1.1\" %d %zu \"-\" \"-\" [%s]\n",
                 client_ip, time_buf, method, url, status_code, response_size, trace_id);
    }
    
    if (g_logger_config.use_stdout) {
        fprintf(stdout, "%s", log_entry);
        fflush(stdout);
    }
    
    if (g_log_fp) {
        fprintf(g_log_fp, "%s", log_entry);
        fflush(g_log_fp);
    }
    
    pthread_mutex_unlock(&g_log_mutex);
}
