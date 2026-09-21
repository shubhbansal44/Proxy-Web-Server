#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CacheModule *HEAD = NULL;
size_t CACHE_SIZE = 0;
// We rely on Main.c to initialize the LOCK or we do it in Cache_init
// Let's have a dedicated lock initialized here for safety
pthread_mutex_t LOCK;

void Cache_init() {
    pthread_mutex_init(&LOCK, NULL);
    HEAD = NULL;
    CACHE_SIZE = 0;
}

CacheModule *FindCache(char *URL) {
    CacheModule *RESPONSE = NULL;
    pthread_mutex_lock(&LOCK);
    if (HEAD != NULL) {
        RESPONSE = HEAD;
        while (RESPONSE != NULL) {
            if (!strcmp(RESPONSE->URL, URL)) {
                RESPONSE->UPTIME = time(NULL);
                break;
            }
            RESPONSE = RESPONSE->NEXT;
        }
    }
    pthread_mutex_unlock(&LOCK);
    return RESPONSE;
}

void RemoveCache() {
    pthread_mutex_lock(&LOCK);
    if (!HEAD) {
        pthread_mutex_unlock(&LOCK);
        return;
    }

    CacheModule *prev = NULL;
    CacheModule *cur = HEAD;
    CacheModule *lru_prev = NULL;
    CacheModule *lru = HEAD;

    while (cur) {
        if (cur->UPTIME < lru->UPTIME) {
            lru = cur;
            lru_prev = prev;
        }
        prev = cur;
        cur = cur->NEXT;
    }

    if (lru == HEAD) {
        HEAD = HEAD->NEXT;
    } else {
        lru_prev->NEXT = lru->NEXT;
    }

    CACHE_SIZE -= (sizeof(CacheModule) + strlen(lru->URL) + lru->LENGTH + 1);
    free(lru->DATA);
    free(lru->URL);
    free(lru);
    pthread_mutex_unlock(&LOCK);
}

int AddCache(char *DATA, int SIZE, char *URL) {
    pthread_mutex_lock(&LOCK);
    size_t ELEMENT_SIZE = (size_t)SIZE + strlen(URL) + sizeof(CacheModule) + 1;
    
    // Check if element is larger than allowed cache size / element size
    if (ELEMENT_SIZE > (size_t)g_config.max_element_size) {
        pthread_mutex_unlock(&LOCK);
        return 0;
    }
    
    // Ensure we have enough space
    // Need to unlock before calling RemoveCache to avoid deadlock since RemoveCache locks too
    while (CACHE_SIZE + ELEMENT_SIZE > (size_t)g_config.max_cache_size && HEAD != NULL) {
        pthread_mutex_unlock(&LOCK);
        RemoveCache();
        pthread_mutex_lock(&LOCK);
    }
    
    // Check again in case max_cache_size is too small for even one element
    if (CACHE_SIZE + ELEMENT_SIZE > (size_t)g_config.max_cache_size) {
        pthread_mutex_unlock(&LOCK);
        return 0;
    }

    CacheModule *CACHE = (CacheModule *)malloc(sizeof(CacheModule));
    if (!CACHE) {
        pthread_mutex_unlock(&LOCK);
        return 0;
    }
    
    CACHE->DATA = (char *)malloc(SIZE + 1);
    if (!CACHE->DATA) {
        free(CACHE);
        pthread_mutex_unlock(&LOCK);
        return 0;
    }
    
    memcpy(CACHE->DATA, DATA, SIZE);
    CACHE->DATA[SIZE] = '\0';
    
    CACHE->URL = (char *)malloc(strlen(URL) + 1);
    if (!CACHE->URL) {
        free(CACHE->DATA);
        free(CACHE);
        pthread_mutex_unlock(&LOCK);
        return 0;
    }
    strcpy(CACHE->URL, URL);
    
    CACHE->UPTIME = time(NULL);
    CACHE->NEXT = HEAD;
    CACHE->LENGTH = SIZE;
    HEAD = CACHE;
    CACHE_SIZE += ELEMENT_SIZE;
    
    pthread_mutex_unlock(&LOCK);
    return 1;
}

void Cache_clear() {
    pthread_mutex_lock(&LOCK);
    CacheModule *cur = HEAD;
    while (cur) {
        CacheModule *next = cur->NEXT;
        free(cur->DATA);
        free(cur->URL);
        free(cur);
        cur = next;
    }
    HEAD = NULL;
    CACHE_SIZE = 0;
    pthread_mutex_unlock(&LOCK);
}
