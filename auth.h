#ifndef PROXY_AUTH_H
#define PROXY_AUTH_H

#include <stdbool.h>
#include "config.h"

// Initialize authentication and IP ACLs based on config
// Loads files into memory if they exist
void auth_init(const ProxyConfig *cfg);

// Checks if an IP string (e.g. "127.0.0.1") is allowed
// Returns true if allowed, false if denied.
// Deny takes precedence, allow is checked if provide.
bool check_ip_allowed(const char *ip);

// Checks if the provided Basic Auth header value is valid.
// e.g. auth_header = "Basic dXNlcm5hbWU6cGFzc3dvcmQ="
bool check_basic_auth(const char *auth_header);

void auth_cleanup();

#endif
