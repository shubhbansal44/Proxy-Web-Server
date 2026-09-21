#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>

size_t CACHE_SIZE = 0;
pthread_mutex_t LOCK;
CacheModule *HASH_TABLE[HASH_TABLE_SIZE];

CacheModule *list_head = NULL;
CacheModule *list_tail = NULL;

CacheEvictionPolicy g_eviction_policy = EVICT_LRU;
char g_cache_dir[256] = "/tmp/proxy_cache";

static unsigned long hash_url(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % HASH_TABLE_SIZE;
}

static void list_remove(CacheModule *node) {
    if (node->prev_lru) node->prev_lru->next_lru = node->next_lru;
    else list_head = node->next_lru;
    
    if (node->next_lru) node->next_lru->prev_lru = node->prev_lru;
    else list_tail = node->prev_lru;
}

static void list_append(CacheModule *node) {
    node->next_lru = NULL;
    node->prev_lru = list_tail;
    if (list_tail) list_tail->next_lru = node;
    else list_head = node;
    list_tail = node;
}

static void list_move_to_end(CacheModule *node) {
    list_remove(node);
    list_append(node);
}

void Cache_init() {
    pthread_mutex_init(&LOCK, NULL);
    for (int i = 0; i < HASH_TABLE_SIZE; i++) HASH_TABLE[i] = NULL;
    list_head = NULL;
    list_tail = NULL;
    CACHE_SIZE = 0;
    
    struct stat st = {0};
    if (stat(g_cache_dir, &st) == -1) {
        mkdir(g_cache_dir, 0700);
    }
}

CacheModule *FindCache(char *URL) {
    pthread_mutex_lock(&LOCK);
    unsigned long h = hash_url(URL);
    CacheModule *curr = HASH_TABLE[h];
    time_t now = time(NULL);
    
    while (curr) {
        if (!strcmp(curr->URL, URL)) {
            // Check TTL expiration
            if (curr->expires_at > 0 && curr->expires_at < now) {
                pthread_mutex_unlock(&LOCK);
                Cache_purge(URL); 
                return NULL;
            }
            
            curr->UPTIME = now;
            curr->access_count++;
            
            if (g_eviction_policy == EVICT_LRU) {
                list_move_to_end(curr);
            }
            pthread_mutex_unlock(&LOCK);
            return curr;
        }
        curr = curr->NEXT;
    }
    pthread_mutex_unlock(&LOCK);
    return NULL;
}

void RemoveCache() {    
    if (!list_head) return;
    
    CacheModule *victim = list_head;
    
    if (g_eviction_policy == EVICT_LFU) {
        CacheModule *curr = list_head;
        CacheModule *lfu = list_head;
        while (curr) {
            if (curr->access_count < lfu->access_count) {
                lfu = curr;
            }
            curr = curr->next_lru;
        }
        victim = lfu;
    } else if (g_eviction_policy == EVICT_TTL) {
        CacheModule *curr = list_head;
        CacheModule *earliest = list_head;
        while (curr) {
            if (curr->expires_at < earliest->expires_at) {
                earliest = curr;
            }
            curr = curr->next_lru;
        }
        victim = earliest;
    }

    // Remove from linked list
    list_remove(victim);
    
    // Remove from Hash
    unsigned long h = hash_url(victim->URL);
    if (HASH_TABLE[h] == victim) {
        HASH_TABLE[h] = victim->NEXT;
    } else {
        CacheModule *curr = HASH_TABLE[h];
        while (curr && curr->NEXT != victim) {
            curr = curr->NEXT;
        }
        if (curr) curr->NEXT = victim->NEXT;
    }

    size_t ELEMENT_SIZE = (size_t)victim->LENGTH + strlen(victim->URL) + sizeof(CacheModule) + 1;
    CACHE_SIZE -= ELEMENT_SIZE;
    
    if (victim->is_on_disk) {
        munmap(victim->DATA, victim->LENGTH);
        unlink(victim->disk_path);
        free(victim->disk_path);
    } else {
        free(victim->DATA);
    }
    
    free(victim->URL);
    free(victim);
}

void Cache_purge(const char *url) {
    pthread_mutex_lock(&LOCK);
    unsigned long h = hash_url(url);
    CacheModule *curr = HASH_TABLE[h];
    CacheModule *prev = NULL;
    
    while (curr) {
        if (!strcmp(curr->URL, url)) {
            if (prev) prev->NEXT = curr->NEXT;
            else HASH_TABLE[h] = curr->NEXT;
            
            list_remove(curr);
            size_t ELEMENT_SIZE = (size_t)curr->LENGTH + strlen(curr->URL) + sizeof(CacheModule) + 1;
            CACHE_SIZE -= ELEMENT_SIZE;
            
            if (curr->is_on_disk) {
                munmap(curr->DATA, curr->LENGTH);
                unlink(curr->disk_path);
                free(curr->disk_path);
            } else {
                free(curr->DATA);
            }
            free(curr->URL);
            free(curr);
            break;
        }
        prev = curr;
        curr = curr->NEXT;
    }
    pthread_mutex_unlock(&LOCK);
}

void Cache_clear() {
    pthread_mutex_lock(&LOCK);
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        CacheModule *curr = HASH_TABLE[i];
        while (curr) {
            CacheModule *next = curr->NEXT;
            if (curr->is_on_disk) {
                munmap(curr->DATA, curr->LENGTH);
                unlink(curr->disk_path);
                free(curr->disk_path);
            } else {
                free(curr->DATA);
            }
            free(curr->URL);
            free(curr);
            curr = next;
        }
        HASH_TABLE[i] = NULL;
    }
    list_head = NULL;
    list_tail = NULL;
    CACHE_SIZE = 0;
    pthread_mutex_unlock(&LOCK);
}

int AddCache(char *DATA, int SIZE, char *URL) {
    // Basic HTTP Cache compliance checks before locking
    if (strcasestr(DATA, "Cache-Control: no-store") || strcasestr(DATA, "Cache-Control: no-cache")) {
        return 0; // Not cacheable
    }

    int max_age = -1;
    char *cc = strcasestr(DATA, "Cache-Control:");
    if (cc) {
        char *ma = strcasestr(cc, "max-age=");
        if (ma) {
            max_age = atoi(ma + 8);
        }
    }

    pthread_mutex_lock(&LOCK);
    size_t ELEMENT_SIZE = (size_t)SIZE + strlen(URL) + sizeof(CacheModule) + 1;
    
    if (ELEMENT_SIZE > (size_t)g_config.max_element_size) {
        pthread_mutex_unlock(&LOCK);
        return 0;
    }
    
    while (CACHE_SIZE + ELEMENT_SIZE > (size_t)g_config.max_cache_size && list_head != NULL) {
        RemoveCache();
    }
    
    if (CACHE_SIZE + ELEMENT_SIZE > (size_t)g_config.max_cache_size) {
        pthread_mutex_unlock(&LOCK);
        return 0;
    }

    CacheModule *CACHE = (CacheModule *)calloc(1, sizeof(CacheModule));
    if (!CACHE) {
        pthread_mutex_unlock(&LOCK);
        return 0;
    }
    
    CACHE->is_on_disk = false;
    CACHE->LENGTH = SIZE;
    
    // Disk-tier logic based on size heuristic (e.g. > 128KB goes to disk)
    if (SIZE > 128 * 1024) {
        char disk_path[512];
        snprintf(disk_path, sizeof(disk_path), "%s/cache_%lu_%ld.bin", g_cache_dir, hash_url(URL), (long)time(NULL));
        int fd = open(disk_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            if (write(fd, DATA, SIZE) == SIZE) {
                CACHE->DATA = (char *)mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (CACHE->DATA != MAP_FAILED) {
                    CACHE->is_on_disk = true;
                    CACHE->disk_path = strdup(disk_path);
                } else {
                    CACHE->DATA = NULL;
                }
            }
            close(fd);
        }
    }
    
    if (!CACHE->is_on_disk) {
        CACHE->DATA = (char *)malloc(SIZE + 1);
        if (CACHE->DATA) {
            memcpy(CACHE->DATA, DATA, SIZE);
            CACHE->DATA[SIZE] = '\0';
        } else {
            free(CACHE);
            pthread_mutex_unlock(&LOCK);
            return 0;
        }
    }

    CACHE->URL = strdup(URL);
    CACHE->UPTIME = time(NULL);
    CACHE->access_count = 1;
    if (max_age > 0) {
        CACHE->expires_at = time(NULL) + max_age;
    } else {
        CACHE->expires_at = time(NULL) + 3600; // Default 1 hr
    }
    
    unsigned long h = hash_url(URL);
    CACHE->NEXT = HASH_TABLE[h];
    HASH_TABLE[h] = CACHE;
    
    list_append(CACHE);
    
    CACHE_SIZE += ELEMENT_SIZE;
    
    pthread_mutex_unlock(&LOCK);
    return 1;
}

void Cache_set_eviction_policy(CacheEvictionPolicy policy) {
    pthread_mutex_lock(&LOCK);
    g_eviction_policy = policy;
    pthread_mutex_unlock(&LOCK);
}
