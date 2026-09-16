#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_IPS 1024
#define MAX_USERS 1024
#define MAX_CREDS_LEN 256
#define MAX_IP_LEN 64

static char allow_ips[MAX_IPS][MAX_IP_LEN];
static int allow_ips_count = 0;

static char deny_ips[MAX_IPS][MAX_IP_LEN];
static int deny_ips_count = 0;

static char auth_users[MAX_USERS][MAX_CREDS_LEN];
static int auth_users_count = 0;

static bool auth_enabled = false;

static const int b64index[256] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 62, 63, 62, 62, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61,  0,  0,  0,  0,  0,  0,
    0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,  0,  0,  0,  0,  0,
    0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

static int b64decode(const char *in, unsigned char *out, size_t out_max) {
    size_t len = strlen(in);
    if (len == 0) return 0;
    
    int pad = in[len - 1] == '=' ? (in[len - 2] == '=' ? 2 : 1) : 0;
    size_t j = 0;
    for (size_t i = 0; i < len && j < out_max; i += 4) {
        int n = b64index[(int)in[i]] << 18 |
                b64index[(int)in[i+1]] << 12 |
                b64index[(int)in[i+2]] << 6 |
                b64index[(int)in[i+3]];
        out[j++] = n >> 16;
        if (j < out_max && i + 4 - pad > i + 1) out[j++] = n >> 8 & 0xFF;
        if (j < out_max && i + 4 - pad > i + 2) out[j++] = n & 0xFF;
    }
    if (j < out_max) out[j] = '\0';
    return j;
}

static void load_list(const char *filename, char dest[][MAX_IP_LEN], int *count, int max_items) {
    if (!filename || strlen(filename) == 0) return;
    FILE *f = fopen(filename, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && *count < max_items) {
        char *p = line;
        while (isspace(*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        char *end = p + strlen(p) - 1;
        while (end > p && isspace(*end)) *end-- = '\0';
        
        strncpy(dest[*count], p, MAX_IP_LEN - 1);
        dest[*count][MAX_IP_LEN - 1] = '\0';
        (*count)++;
    }
    fclose(f);
}

void auth_init(const ProxyConfig *cfg) {
    allow_ips_count = 0;
    deny_ips_count = 0;
    auth_users_count = 0;
    auth_enabled = cfg->enable_auth;
    
    if (strlen(cfg->allow_ip_file) > 0) {
        load_list(cfg->allow_ip_file, allow_ips, &allow_ips_count, MAX_IPS);
    }
    if (strlen(cfg->deny_ip_file) > 0) {
        load_list(cfg->deny_ip_file, deny_ips, &deny_ips_count, MAX_IPS);
    }
    
    if (auth_enabled && strlen(cfg->auth_file) > 0) {
        FILE *f = fopen(cfg->auth_file, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f) && auth_users_count < MAX_USERS) {
                char *p = line;
                while (isspace(*p)) p++;
                if (*p == '\0' || *p == '#') continue;
                char *end = p + strlen(p) - 1;
                while (end > p && isspace(*end)) *end-- = '\0';
                
                // Format: basic auth expects base64(user:pass) but we can store "user:pass" plaintext in the file
                strncpy(auth_users[auth_users_count], p, MAX_CREDS_LEN - 1);
                auth_users[auth_users_count][MAX_CREDS_LEN - 1] = '\0';
                auth_users_count++;
            }
            fclose(f);
        }
    }
}

#include <netinet/in.h>
#include <arpa/inet.h>

// Helpers for IP matching
static bool ip_matches(const char *ip, const char *rule) {
    if (strcmp(ip, rule) == 0) return true;

    const char *slash = strchr(rule, '/');
    if (!slash) return false;

    char subnet_str[64];
    strncpy(subnet_str, rule, slash - rule);
    subnet_str[slash - rule] = '\0';

    int prefix = atoi(slash + 1);
    if (prefix < 0 || prefix > 32) return false;

    struct in_addr ip_addr, subnet_addr;
    if (inet_pton(AF_INET, ip, &ip_addr) != 1) return false;
    if (inet_pton(AF_INET, subnet_str, &subnet_addr) != 1) return false;

    uint32_t ip_int = ntohl(ip_addr.s_addr);
    uint32_t subnet_int = ntohl(subnet_addr.s_addr);
    uint32_t mask = (prefix == 0) ? 0 : (~0U) << (32 - prefix);

    return (ip_int & mask) == (subnet_int & mask);
}

bool check_ip_allowed(const char *ip) {
    if (!ip) return false;
    
    // Check deny list first
    for (int i = 0; i < deny_ips_count; i++) {
        if (ip_matches(ip, deny_ips[i])) {
            return false;
        }
    }
    
    // If allow list is present, check against it
    if (allow_ips_count > 0) {
        for (int i = 0; i < allow_ips_count; i++) {
            if (ip_matches(ip, allow_ips[i])) {
                return true;
            }
        }
        return false; // Not in allow list
    }
    
    return true; // No allow list -> open
}

bool check_basic_auth(const char *auth_header) {
    if (!auth_enabled) return true;
    if (!auth_header) return false;

    // Must start with "Basic "
    // Format: "Basic dXNlcjpwYXNz"
    const char *prefix = "Basic ";
    if (strncmp(auth_header, prefix, 6) != 0) {
        return false; // not basic auth
    }

    const char *b64_str = auth_header + 6;
    while (*b64_str == ' ') b64_str++; // skip extra spaces if any

    unsigned char decoded[MAX_CREDS_LEN];
    b64decode(b64_str, decoded, sizeof(decoded));
    
    for (int i = 0; i < auth_users_count; i++) {
        if (strcmp((char *)decoded, auth_users[i]) == 0) {
            return true;
        }
    }
    
    return false;
}

void auth_cleanup() {
    allow_ips_count = 0;
    deny_ips_count = 0;
    auth_users_count = 0;
}
