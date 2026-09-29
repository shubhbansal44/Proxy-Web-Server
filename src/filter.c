#include "filter.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <regex.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#define MAX_URL_PATTERNS 1000
#define MAX_IP_BLOCKS 1000
#define MAX_IP_ALLOWS 1000
#define MAX_CONTENT_TYPES 100

// Trie node for domain matching (reversed domain for easier suffix matching)
typedef struct TrieNode {
    struct TrieNode *children[256];
    bool is_end_of_domain;
} TrieNode;

// CIDR matching structure
typedef struct {
    struct in_addr addr;
    struct in_addr mask;
    int is_ipv6; // For now simplify to IPv4 mostly, but we can support IPv6 later
    struct in6_addr addr6;
    struct in6_addr mask6;
} CidrBlock;

static TrieNode *domain_trie_root = NULL;
static CidrBlock allow_ips[MAX_IP_ALLOWS];
static int allow_ips_count = 0;
static CidrBlock deny_ips[MAX_IP_BLOCKS];
static int deny_ips_count = 0;
static regex_t url_patterns[MAX_URL_PATTERNS];
static int url_patterns_count = 0;
static char content_type_blocklist[MAX_CONTENT_TYPES][128];
static int content_type_blocklist_count = 0;

static char block_page_html[8192];
static const char *default_block_page = "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>";

static pthread_rwlock_t filter_lock = PTHREAD_RWLOCK_INITIALIZER;
static bool filter_enabled = false;
static ProxyConfig saved_cfg;

// Hot-reload: inotify watch descriptors
static int inotify_fd = -1;
static int inotify_wd_domain = -1;
static int inotify_wd_ip_allow = -1;
static int inotify_wd_ip_deny = -1;
static int inotify_wd_url = -1;
static int inotify_wd_ct = -1;
static int inotify_wd_block_page = -1;
static volatile bool hot_reload_running = false;
static pthread_t hot_reload_thread;

// Utility functions
static TrieNode *trie_create_node() {
    TrieNode *node = (TrieNode*)calloc(1, sizeof(TrieNode));
    return node;
}

static void trie_free(TrieNode *node) {
    if (!node) return;
    for (int i = 0; i < 256; i++) {
        if (node->children[i]) {
            trie_free(node->children[i]);
        }
    }
    free(node);
}

// Insert domain (reversed) into trie
static void trie_insert(TrieNode *root, const char *domain) {
    int len = strlen(domain);
    TrieNode *curr = root;
    for (int i = len - 1; i >= 0; i--) {
        unsigned char c = (unsigned char)tolower(domain[i]);
        if (!curr->children[c]) {
            curr->children[c] = trie_create_node();
        }
        curr = curr->children[c];
    }
    curr->is_end_of_domain = true;
}

// Parse CIDR "192.168.1.0/24" or "192.168.1.5"
static bool parse_cidr(const char *str, CidrBlock *block) {
    char copy[256];
    strncpy(copy, str, sizeof(copy)-1);
    copy[sizeof(copy)-1] = '\0';
    char *slash = strchr(copy, '/');
    int prefix = -1;
    if (slash) {
        *slash = '\0';
        prefix = atoi(slash + 1);
    }
    
    // Try IPv4
    if (inet_pton(AF_INET, copy, &block->addr) == 1) {
        block->is_ipv6 = 0;
        if (prefix < 0 || prefix > 32) prefix = 32;
        block->mask.s_addr = htonl(~((1ULL << (32 - prefix)) - 1));
        block->addr.s_addr &= block->mask.s_addr;
        return true;
    }
    // Try IPv6
    if (inet_pton(AF_INET6, copy, &block->addr6) == 1) {
        block->is_ipv6 = 1;
        if (prefix < 0 || prefix > 128) prefix = 128;
        // Construct IPv6 mask
        memset(&block->mask6, 0, sizeof(block->mask6));
        for (int i = 0; i < prefix / 8; i++) {
            block->mask6.s6_addr[i] = 0xff;
        }
        if (prefix % 8 != 0) {
            block->mask6.s6_addr[prefix / 8] = (unsigned char)(0xff << (8 - (prefix % 8)));
        }
        for (int i = 0; i < 16; i++) {
            block->addr6.s6_addr[i] &= block->mask6.s6_addr[i];
        }
        return true;
    }
    return false;
}

static void load_domain_list(const char *path) {
    if (!path || strlen(path) == 0) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char *end = p + strlen(p) - 1;
        while (end > p && isspace((unsigned char)*end)) *end-- = '\0';
        trie_insert(domain_trie_root, p);
    }
    fclose(f);
}

static void load_ip_list(const char *path, CidrBlock *arr, int *count, int max_size) {
    if (!path || strlen(path) == 0) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && *count < max_size) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char *end = p + strlen(p) - 1;
        while (end > p && isspace((unsigned char)*end)) *end-- = '\0';
        if (parse_cidr(p, &arr[*count])) {
            (*count)++;
        }
    }
    fclose(f);
}

static void load_content_type_list(const char *path) {
    if (!path || strlen(path) == 0) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    content_type_blocklist_count = 0;
    while (fgets(line, sizeof(line), f) && content_type_blocklist_count < MAX_CONTENT_TYPES) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char *end = p + strlen(p) - 1;
        while (end > p && isspace((unsigned char)*end)) *end-- = '\0';
        strncpy(content_type_blocklist[content_type_blocklist_count], p, sizeof(content_type_blocklist[0]) - 1);
        content_type_blocklist[content_type_blocklist_count][sizeof(content_type_blocklist[0]) - 1] = '\0';
        content_type_blocklist_count++;
    }
    fclose(f);
}

static void load_url_list(const char *path) {
    if (!path || strlen(path) == 0) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) && url_patterns_count < MAX_URL_PATTERNS) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char *end = p + strlen(p) - 1;
        while (end > p && isspace((unsigned char)*end)) *end-- = '\0';
        if (regcomp(&url_patterns[url_patterns_count], p, REG_EXTENDED | REG_NOSUB | REG_ICASE) == 0) {
            url_patterns_count++;
        }
    }
    fclose(f);
}

static void filter_clear() {
    if (domain_trie_root) {
        trie_free(domain_trie_root);
    }
    domain_trie_root = trie_create_node();
    allow_ips_count = 0;
    deny_ips_count = 0;
    for (int i = 0; i < url_patterns_count; i++) {
        regfree(&url_patterns[i]);
    }
    url_patterns_count = 0;
}

static void load_block_page(const char *path) {
    strncpy(block_page_html, default_block_page, sizeof(block_page_html) - 1);
    block_page_html[sizeof(block_page_html) - 1] = '\0';
    
    if (!path || strlen(path) == 0) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    
    size_t read_bytes = fread(block_page_html, 1, sizeof(block_page_html) - 1, f);
    block_page_html[read_bytes] = '\0';
    fclose(f);
}

static void *hot_reload_thread_fn(void *arg) {
    (void)arg;
    while (hot_reload_running) {
        char event_buf[4096];
        int n = read(inotify_fd, event_buf, sizeof(event_buf));
        if (n > 0 && hot_reload_running) {
            filter_reload();
        }
    }
    return NULL;
}

static int setup_inotify_watches(const ProxyConfig *cfg) {
    if (inotify_fd < 0) inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd < 0) return -1;
    inotify_wd_domain = inotify_add_watch(inotify_fd, cfg->blocklist_file ? cfg->blocklist_file : "/dev/null", IN_MODIFY | IN_CLOSE_WRITE);
    inotify_wd_url = inotify_add_watch(inotify_fd, cfg->url_blocklist_file ? cfg->url_blocklist_file : "/dev/null", IN_MODIFY | IN_CLOSE_WRITE);
    inotify_wd_ct = inotify_add_watch(inotify_fd, cfg->content_type_blocklist_file ? cfg->content_type_blocklist_file : "/dev/null", IN_MODIFY | IN_CLOSE_WRITE);
    return 0;
}

void filter_init(const ProxyConfig *cfg) {
    saved_cfg = *cfg;
    filter_enabled = cfg->enable_filter;
    if (!filter_enabled) return;
    pthread_rwlock_wrlock(&filter_lock);
    filter_clear();
    load_domain_list(cfg->blocklist_file);
    load_ip_list(cfg->allow_ip_file, allow_ips, &allow_ips_count, MAX_IP_ALLOWS);
    load_ip_list(cfg->deny_ip_file, deny_ips, &deny_ips_count, MAX_IP_BLOCKS);
    load_url_list(cfg->url_blocklist_file);
    load_content_type_list(cfg->content_type_blocklist_file);
    load_block_page(cfg->block_page_file);
    setup_inotify_watches(cfg);
    pthread_rwlock_unlock(&filter_lock);
    hot_reload_running = true;
    pthread_create(&hot_reload_thread, NULL, hot_reload_thread_fn, NULL);
}

void filter_reload(void) {
    if (!filter_enabled) return;
    pthread_rwlock_wrlock(&filter_lock);
    filter_clear();
    load_domain_list(saved_cfg.blocklist_file);
    load_ip_list(saved_cfg.allow_ip_file, allow_ips, &allow_ips_count, MAX_IP_ALLOWS);
    load_ip_list(saved_cfg.deny_ip_file, deny_ips, &deny_ips_count, MAX_IP_BLOCKS);
    load_url_list(saved_cfg.url_blocklist_file);
    load_content_type_list(saved_cfg.content_type_blocklist_file);
    load_block_page(saved_cfg.block_page_file);
    pthread_rwlock_unlock(&filter_lock);
    LOG_INFO("Filter lists reloaded");
}

void filter_cleanup(void) {
    hot_reload_running = false;
    if (hot_reload_thread) pthread_join(hot_reload_thread, NULL);
    if (inotify_fd >= 0) { close(inotify_fd); inotify_fd = -1; }
    if (!filter_enabled) return;
    pthread_rwlock_wrlock(&filter_lock);
    if (domain_trie_root) {
        trie_free(domain_trie_root);
        domain_trie_root = NULL;
    }
    for (int i = 0; i < url_patterns_count; i++) {
        regfree(&url_patterns[i]);
    }
    url_patterns_count = 0;
    pthread_rwlock_unlock(&filter_lock);
}

bool filter_is_domain_blocked(const char *domain) {
    if (!filter_enabled || !domain || !domain_trie_root) return false;
    pthread_rwlock_rdlock(&filter_lock);
    int len = strlen(domain);
    TrieNode *curr = domain_trie_root;
    bool blocked = false;
    for (int i = len - 1; i >= 0; i--) {
        unsigned char c = (unsigned char)tolower(domain[i]);
        if (!curr->children[c]) {
            break;
        }
        curr = curr->children[c];
        // If we hit an end of domain and either it's the full string matched
        // or the character preceding it in the domain string is a '.'
        if (curr->is_end_of_domain) {
            if (i == 0 || domain[i - 1] == '.') {
                blocked = true;
                break;
            }
        }
    }
    pthread_rwlock_unlock(&filter_lock);
    return blocked;
}

bool filter_is_url_blocked(const char *url) {
    if (!filter_enabled || !url) return false;
    pthread_rwlock_rdlock(&filter_lock);
    bool blocked = false;
    for (int i = 0; i < url_patterns_count; i++) {
        if (regexec(&url_patterns[i], url, 0, NULL, 0) == 0) {
            blocked = true;
            break;
        }
    }
    pthread_rwlock_unlock(&filter_lock);
    return blocked;
}

bool filter_is_ip_blocked(struct sockaddr *sa) {
    if (!filter_enabled || !sa) return false;
    
    // Convert sa to check
    struct in_addr addr4;
    struct in6_addr addr6;
    int is_ipv6 = 0;
    
    if (sa->sa_family == AF_INET) {
        addr4 = ((struct sockaddr_in *)sa)->sin_addr;
    } else if (sa->sa_family == AF_INET6) {
        addr6 = ((struct sockaddr_in6 *)sa)->sin6_addr;
        is_ipv6 = 1;
    } else {
        return false;
    }
    
    pthread_rwlock_rdlock(&filter_lock);
    
    // Check allowlist first (bypass blocklist if allowed)
    bool allowed = false;
    for (int i = 0; i < allow_ips_count; i++) {
        if (allow_ips[i].is_ipv6 == is_ipv6) {
            if (!is_ipv6) {
                if ((addr4.s_addr & allow_ips[i].mask.s_addr) == allow_ips[i].addr.s_addr) {
                    allowed = true; break;
                }
            } else {
                bool match = true;
                for (int j = 0; j < 16; j++) {
                    if ((addr6.s6_addr[j] & allow_ips[i].mask6.s6_addr[j]) != allow_ips[i].addr6.s6_addr[j]) {
                        match = false; break;
                    }
                }
                if (match) { allowed = true; break; }
            }
        }
    }
    
    if (allowed) {
        pthread_rwlock_unlock(&filter_lock);
        return false;
    }
    
    // Check blocklist
    bool blocked = false;
    for (int i = 0; i < deny_ips_count; i++) {
        if (deny_ips[i].is_ipv6 == is_ipv6) {
            if (!is_ipv6) {
                if ((addr4.s_addr & deny_ips[i].mask.s_addr) == deny_ips[i].addr.s_addr) {
                    blocked = true; break;
                }
            } else {
                bool match = true;
                for (int j = 0; j < 16; j++) {
                    if ((addr6.s6_addr[j] & deny_ips[i].mask6.s6_addr[j]) != deny_ips[i].addr6.s6_addr[j]) {
                        match = false; break;
                    }
                }
                if (match) { blocked = true; break; }
            }
        }
    }
    
    pthread_rwlock_unlock(&filter_lock);
    return blocked;
}

bool filter_is_content_type_blocked(const char *content_type) {
    if (!filter_enabled || !content_type || content_type_blocklist_count == 0) return false;
    pthread_rwlock_rdlock(&filter_lock);
    bool blocked = false;
    char buf[128];
    strncpy(buf, content_type, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *token = strtok(buf, ",");
    while (token && !blocked) {
        char *trim = token;
        while (isspace((unsigned char)*trim)) trim++;
        char *end = trim + strlen(trim) - 1;
        while (end > trim && isspace((unsigned char)*end)) *end-- = '\0';
        for (int i = 0; i < content_type_blocklist_count && !blocked; i++) {
            if (strstr(trim, content_type_blocklist[i]) != NULL) {
                blocked = true;
                break;
            }
        }
        token = strtok(NULL, ",");
    }
    pthread_rwlock_unlock(&filter_lock);
    return blocked;
}

const char* filter_get_block_page(void) {
    if (!filter_enabled) return default_block_page;
    return block_page_html;
}
