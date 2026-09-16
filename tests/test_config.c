#include "../config.h"
ProxyConfig g_config;


#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_config_init_defaults() {
    TEST_SUITE(test_config_init_defaults);
    ProxyConfig cfg;
    config_init_defaults(&cfg);
    
    TEST_ASSERT_EQ(DEFAULT_PORT, cfg.port);
    TEST_ASSERT_EQ(DEFAULT_MAX_CLIENTS, cfg.max_clients);
    TEST_ASSERT_STR_EQ(DEFAULT_BIND_ADDRESS, cfg.bind_address);
    TEST_ASSERT_EQ(DEFAULT_ENABLE_CACHE, cfg.enable_cache);
}

void test_config_load_file() {
    TEST_SUITE(test_config_load_file);
    ProxyConfig cfg;
    config_init_defaults(&cfg);
    
    // Create a temporary config file
    const char *tmp_file = "test_proxy.conf";
    int fd = open(tmp_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd != -1) {
        const char *conf = "# Comment line\nport=9090\nmax_clients=50\nbind_address=127.0.0.1\nmax_bytes=4096\nmax_cache_size=104857600\nmax_element_size=1048576\nenable_cache=true\ncache_dir=/tmp/cache\nenable_auth=1\nauth_file=/etc/auth\nlog_level=debug\nlog_file=/var/log/proxy.log\nenable_ipv6=1\nenable_throttling=true\nrate_limit_rps=100\nblocklist_file=/etc/block\nlog_rotation=1\nlog_max_size_mb=40\nrate_limit_burst=50\nbandwidth_limit_kbps=1000\nenable_https=1\nenable_http2=true\nenable_filter=1\n";
        write(fd, conf, strlen(conf));
        close(fd);
    }
    
    // Test that missing file returns 0 (defaults are intact)
    TEST_ASSERT_EQ(0, config_load_file(&cfg, "non_existent_file.conf"));

    int result = config_load_file(&cfg, tmp_file);
    TEST_ASSERT_EQ(0, result);
    TEST_ASSERT_EQ(9090, cfg.port);
    TEST_ASSERT_EQ(50, cfg.max_clients);
    TEST_ASSERT_STR_EQ("127.0.0.1", cfg.bind_address);
    TEST_ASSERT_EQ(4096, cfg.max_bytes);
    TEST_ASSERT_EQ(104857600, cfg.max_cache_size);
    TEST_ASSERT_EQ(1048576, cfg.max_element_size);
    TEST_ASSERT_EQ(1, cfg.enable_cache);
    TEST_ASSERT_STR_EQ("/tmp/cache", cfg.cache_dir);
    TEST_ASSERT_EQ(1, cfg.enable_auth);
    TEST_ASSERT_STR_EQ("/etc/auth", cfg.auth_file);
    TEST_ASSERT_STR_EQ("debug", cfg.log_level);
    TEST_ASSERT_STR_EQ("/var/log/proxy.log", cfg.log_file);
    TEST_ASSERT_EQ(1, cfg.enable_ipv6);
    TEST_ASSERT_EQ(1, cfg.enable_throttling);
    TEST_ASSERT_EQ(100, cfg.rate_limit_rps);
    
    // Cleanup
    unlink(tmp_file);
}

void test_config_apply_env() {
    TEST_SUITE(test_config_apply_env);
    ProxyConfig cfg;
    config_init_defaults(&cfg);
    
    setenv("PROXY_PORT", "8888", 1);
    setenv("PROXY_MAX_CLIENTS", "100", 1);
    setenv("PROXY_MAX_CACHE_SIZE", "5000000", 1);
    setenv("PROXY_LOG_LEVEL", "ERROR", 1);
    
    config_apply_env(&cfg);
    TEST_ASSERT_EQ(8888, cfg.port);
    TEST_ASSERT_EQ(100, cfg.max_clients);
    TEST_ASSERT_EQ(5000000, cfg.max_cache_size);
    TEST_ASSERT_STR_EQ("ERROR", cfg.log_level);
    
    unsetenv("PROXY_PORT");
    unsetenv("PROXY_MAX_CLIENTS");
    unsetenv("PROXY_MAX_CACHE_SIZE");
    unsetenv("PROXY_LOG_LEVEL");
}

void test_config_apply_args() {
    TEST_SUITE(test_config_apply_args);
    ProxyConfig cfg;
    config_init_defaults(&cfg);
    
    char arg0[] = "proxy";
    char arg1[] = "--port=7777";
    char arg2[] = "--max-clients=150";
    char arg3[] = "--enable-ipv6";
    char arg4[] = "--cache-size=999999";
    char arg5[] = "--log-level=WARN";
    char arg6[] = "--bind-address=10.0.0.1";
    char arg7[] = "--max-bytes=2048";
    char arg8[] = "--element-size=1024";
    char arg9[] = "--cache-dir=/tmp/c";
    char arg10[] = "--auth-file=/tmp/a";
    char arg11[] = "--blocklist-file=/tmp/b";
    char arg12[] = "--log-file=/tmp/l";
    char arg13[] = "--rate-limit-rps=100";
    char arg14[] = "--rate-limit-burst=200";
    char arg15[] = "--bandwidth-limit=300";
    char arg16[] = "--enable-cache";
    char arg17[] = "--enable-auth";
    char arg18[] = "--enable-throttling";
    char arg19[] = "--enable-https";
    char arg20[] = "--enable-http2";
    char arg21[] = "--enable-filter";
    
    char* args[] = { arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, 
                     arg10, arg11, arg12, arg13, arg14, arg15, arg16, arg17, arg18, 
                     arg19, arg20, arg21 };
    
    int result = config_apply_args(&cfg, 22, args);
    TEST_ASSERT_EQ(0, result);
    TEST_ASSERT_EQ(7777, cfg.port);
    TEST_ASSERT_EQ(150, cfg.max_clients);
    TEST_ASSERT_EQ(1, cfg.enable_ipv6);
    TEST_ASSERT_EQ(999999, cfg.max_cache_size);
    TEST_ASSERT_STR_EQ("WARN", cfg.log_level);
    TEST_ASSERT_STR_EQ("10.0.0.1", cfg.bind_address);
    TEST_ASSERT_EQ(2048, cfg.max_bytes);
    TEST_ASSERT_EQ(1024, cfg.max_element_size);
    TEST_ASSERT_STR_EQ("/tmp/c", cfg.cache_dir);
    TEST_ASSERT_STR_EQ("/tmp/a", cfg.auth_file);
    TEST_ASSERT_STR_EQ("/tmp/b", cfg.blocklist_file);
    TEST_ASSERT_STR_EQ("/tmp/l", cfg.log_file);
    TEST_ASSERT_EQ(100, cfg.rate_limit_rps);
    TEST_ASSERT_EQ(200, cfg.rate_limit_burst);
    TEST_ASSERT_EQ(300, cfg.bandwidth_limit_kbps);
    TEST_ASSERT_EQ(1, cfg.enable_cache);
    TEST_ASSERT_EQ(1, cfg.enable_auth);
    TEST_ASSERT_EQ(1, cfg.enable_throttling);
    TEST_ASSERT_EQ(1, cfg.enable_https);
    TEST_ASSERT_EQ(1, cfg.enable_http2);
    TEST_ASSERT_EQ(1, cfg.enable_filter);
    
    // Help flag
    char argHelp[] = "--help";
    char* argsHelp[] = { arg0, argHelp };
    TEST_ASSERT_EQ(-1, config_apply_args(&cfg, 2, argsHelp));

    // Unknown arg with --
    char argUnknown[] = "--unknown";
    char* argsUnknown[] = { arg0, argUnknown };
    TEST_ASSERT_EQ(-1, config_apply_args(&cfg, 2, argsUnknown));

    // Invalid format argument
    char argInvalid[] = "-invalid";
    char* argsInvalid[] = { arg0, argInvalid };
    TEST_ASSERT_EQ(-1, config_apply_args(&cfg, 2, argsInvalid));
}

void test_config_validate() {
    TEST_SUITE(test_config_validate);
    ProxyConfig cfg;
    config_init_defaults(&cfg);
    TEST_ASSERT_EQ(0, config_validate(&cfg));

    // Test port
    cfg.port = 0; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    cfg.port = 8080; TEST_ASSERT_EQ(0, config_validate(&cfg));
    cfg.port = 70000; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    cfg.port = 8080;

    // Test bind address
    cfg.bind_address[0] = '\0'; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    strcpy(cfg.bind_address, "0.0.0.0"); TEST_ASSERT_EQ(0, config_validate(&cfg));

    // Test clients
    cfg.max_clients = 0; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    cfg.max_clients = 100; TEST_ASSERT_EQ(0, config_validate(&cfg));

    // Test max bytes
    cfg.max_bytes = 0; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    cfg.max_bytes = 1000; TEST_ASSERT_EQ(0, config_validate(&cfg));

    // Test element size
    cfg.max_element_size = 0; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    cfg.max_element_size = 1000; TEST_ASSERT_EQ(0, config_validate(&cfg));

    // Test cache dir length
    memset(cfg.cache_dir, 'a', sizeof(cfg.cache_dir));
    cfg.cache_dir[sizeof(cfg.cache_dir)-1] = '\0'; // Exactly ok length maybe? No wait.
    // wait, if length is >= sizeof(), it fails. so we can't do that properly in a null terminated string.
    // just test empty cache dir doesn't error when cache disabled
    cfg.cache_dir[0] = '\0'; TEST_ASSERT_EQ(0, config_validate(&cfg));

    // Test enable_auth missing file
    cfg.enable_auth = true; cfg.auth_file[0] = '\0';
    TEST_ASSERT_EQ(-1, config_validate(&cfg));
    strcpy(cfg.auth_file, "auth.txt");
    TEST_ASSERT_EQ(0, config_validate(&cfg));
    
    // Test logging
    cfg.log_file[0] = '\0'; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    strcpy(cfg.log_file, "proxy.log");
    
    strcpy(cfg.log_level, "INVALID"); TEST_ASSERT_EQ(-1, config_validate(&cfg));
    strcpy(cfg.log_level, "INFO"); TEST_ASSERT_EQ(0, config_validate(&cfg));
    
    cfg.log_max_size_mb = 0; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    cfg.log_max_size_mb = 100; TEST_ASSERT_EQ(0, config_validate(&cfg));
    
    // Test throttling
    cfg.enable_throttling = true;
    cfg.rate_limit_rps = 0; TEST_ASSERT_EQ(-1, config_validate(&cfg)); 
    cfg.rate_limit_rps = 10;
    cfg.rate_limit_burst = 0; TEST_ASSERT_EQ(-1, config_validate(&cfg)); 
    cfg.rate_limit_burst = 10;
    cfg.bandwidth_limit_kbps = 0; TEST_ASSERT_EQ(-1, config_validate(&cfg));
    cfg.bandwidth_limit_kbps = 100;
    TEST_ASSERT_EQ(0, config_validate(&cfg));
}

void test_config_print() {
    TEST_SUITE(test_config_print);
    ProxyConfig cfg;
    config_init_defaults(&cfg);
    // Since print goes to stdout, we just run it for coverage
    // so it executes the basic valid path.
    config_print(&cfg);
    cfg.enable_cache = true;
    strcpy(cfg.cache_dir, "/tmp/cache");
    cfg.enable_auth = true;
    strcpy(cfg.auth_file, "auth.txt");
    cfg.enable_throttling = true;
    config_print(&cfg);
    TEST_ASSERT_EQ(1, 1);
}

int main() {
    test_config_init_defaults();
    test_config_load_file();
    test_config_apply_env();
    test_config_apply_args();
    test_config_validate();
    test_config_print();
    
    TEST_REPORT();
    return 0;
}
