#ifndef CACHE_H
#define CACHE_H

#include <time.h>
#include <pthread.h>
#include "config.h"

typedef struct CacheModule {
    char *DATA;
    int LENGTH;
    char *URL;
    time_t UPTIME;
    struct CacheModule *NEXT;
} CacheModule;

extern CacheModule *HEAD;
extern size_t CACHE_SIZE;
extern pthread_mutex_t LOCK;
extern ProxyConfig g_config; // assuming it's used for limits

// Public API
void Cache_init();
CacheModule *FindCache(char *URL);
int AddCache(char *DATA, int SIZE, char *URL);
void RemoveCache();
void Cache_clear(); // for testing

#endif
