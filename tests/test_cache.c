#include <unistd.h>
#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"
#include "src/cache.h"
#include <string.h>

ProxyConfig g_config;

void test_cache_add_find() {
    TEST_SUITE(test_cache_add_find);
    
    // Setup
    g_config.max_cache_size = 1024 * 1024;
    g_config.max_element_size = 1024;
    Cache_init();
    
    const char *url = "http://example.com";
    const char *data = "Hello World!";
    
    // Add to cache
    int res = AddCache((char*)data, strlen(data), (char*)url);
    TEST_ASSERT_EQ(1, res);
    
    // Find in cache
    CacheModule *found = FindCache((char*)url);
    TEST_ASSERT(found != NULL);
    if (found) {
        TEST_ASSERT_STR_EQ(data, found->DATA);
        TEST_ASSERT_EQ(strlen(data), found->LENGTH);
        TEST_ASSERT_STR_EQ(url, found->URL);
    }
    
    Cache_clear();
}

void test_cache_lru_eviction() {
    TEST_SUITE(test_cache_lru_eviction);
    
    // Setup for strict capacity
    g_config.max_cache_size = 100; // very small
    g_config.max_element_size = 100;
    Cache_init();
    
    // We add elements that will exceed the size.
    // Adding 3 elements of roughly 80 bytes each
    // Element size = SIZE + strlen(URL) + sizeof(CacheModule) + 1
    // sizeof(CacheModule) is usually 40 bytes.
    char data[20] = "data";
    char url1[20] = "url1";
    char url2[20] = "url2";
    char url3[20] = "url3";
    
    AddCache(data, 4, url1);
    sleep(1); // Ensure different UPTIME
    AddCache(data, 4, url2);
    sleep(1);
    AddCache(data, 4, url3);
    
    // url1 should be evicted because it's LRU and total cache size exceeds max_cache_size
    CacheModule *c1 = FindCache(url1);
    TEST_ASSERT(c1 == NULL);
    
    CacheModule *c3 = FindCache(url3);
    TEST_ASSERT(c3 != NULL);
    
    Cache_clear();
}

int main() {
    test_cache_add_find();
    test_cache_lru_eviction();
    TEST_REPORT();
    return 0;
}
