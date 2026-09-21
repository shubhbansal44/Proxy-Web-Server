#ifndef CACHE_H
#define CACHE_H

#include <time.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>
#include "config.h"

#define HASH_TABLE_SIZE 1024

// Eviction policy enums
typedef enum {
    EVICT_LRU = 0,
    EVICT_LFU,
    EVICT_TTL
} CacheEvictionPolicy;

typedef struct CacheModule {
    char *URL;             // Key
    
    // Tiered storage indicator
    bool is_on_disk;
    char *disk_path;
    char *DATA;            // In-memory or mmap'd data
    int LENGTH;            // Size of data
    
    // Metadata for eviction 
    time_t UPTIME;         // Last access for LRU
    int access_count;      // For LFU
    time_t expires_at;     // For TTL/HTTP Cache-Control max-age
    
    struct CacheModule *NEXT;      // Hash chain
    struct CacheModule *prev_lru;  // Doubly-linked tracking
    struct CacheModule *next_lru;
} CacheModule;

extern size_t CACHE_SIZE;
extern pthread_mutex_t LOCK;
extern CacheModule *HASH_TABLE[HASH_TABLE_SIZE];

// Configuration globals for cache behaviour
extern CacheEvictionPolicy g_eviction_policy;
extern char g_cache_dir[256];

// Public API
void Cache_init();
CacheModule *FindCache(char *URL);
int AddCache(char *DATA, int SIZE, char *URL);
void RemoveCache();
void Cache_clear(); 

// Advanced APIs for ticket 005
void Cache_purge(const char *url);
void Cache_set_eviction_policy(CacheEvictionPolicy policy);

#endif
