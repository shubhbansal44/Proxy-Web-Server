#ifndef FILTER_H
#define FILTER_H

#include "config.h"
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>

void filter_init(const ProxyConfig *cfg);
void filter_cleanup(void);
void filter_reload(void);

// Returns true if the domain matches any in the blocklist
bool filter_is_domain_blocked(const char *domain);

// Returns true if the URL matches any regex pattern in the blocklist
bool filter_is_url_blocked(const char *url);

// Returns true if the IP matches any in the deny list and NOT in the allow list
bool filter_is_ip_blocked(struct sockaddr *sa);

// Returns true if the content type matches any in the blocklist
bool filter_is_content_type_blocked(const char *content_type);

// Return custom block page HTML
const char* filter_get_block_page(void);

#endif
