#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL
} LogLevelEnum;

typedef enum {
    LOG_FORMAT_PLAIN = 0,
    LOG_FORMAT_SQUID,
    LOG_FORMAT_CLF,
    LOG_FORMAT_JSON
} LogFormat;

typedef struct {
    LogLevelEnum level;
    LogFormat format;
    char log_file[512];
    bool use_stdout;
    bool use_syslog;
    
    // File rotation
    bool rotation_enabled;
    int max_file_size_mb;
    
} LoggerConfig;

bool logger_init(const LoggerConfig* config);
void logger_shutdown(void);
void logger_log(LogLevelEnum level, const char* file, int line, const char* func, const char* fmt, ...);
void logger_access_log(const char* client_ip, int status_code, size_t response_size, 
                       const char* method, const char* url, double elapsed_ms, 
                       const char* content_type, const char* trace_id);

#define LOG_DEBUG(...) logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...)  logger_log(LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...)  logger_log(LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(...) logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_CRITICAL(...) logger_log(LOG_LEVEL_CRITICAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // LOGGER_H
