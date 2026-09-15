# Ticket T009: Authentication & Access Control - Proxy-Authorization (Basic)

## Overview
Implement HTTP proxy authentication using the `Proxy-Authorization` header to restrict proxy access to authorized clients only. This addresses the current unrestricted access model identified in the codebase analysis.

## Goals
- Add Proxy-Authorization challenge-response flow (RFC 7235)
- Implement Basic authentication over TLS (credentials must not traverse unencrypted links)
- Support file-based and system user authentication (htpasswd, PAM)
- Add per-user access control policies (ACL)
- Implement credential caching to reduce authentication latency

## Technical Specifications

### Authentication Methods
- **Basic Auth**: Base64-encoded username:password in Proxy-Authorization header
  - Must enforce use over TLS (CONNECT tunnel or local-only) to prevent credential exposure
- **File-based**: htpasswd-style credential files (supports bcrypt, SHA, crypt)
- **System**: Pluggable Authentication Modules (PAM) integration for Linux
- **Caching**: Short-lived cache (default: 30s) for authenticated sessions

### Access Control
- **User-based ACL**: Map authenticated users to allowed/denied destinations
- **Group-based ACL**: Support group membership for policy application
- **Anonymous Access**: Optional fallback for unauthenticated users (with restrictions)

### Implementation Details
- **Challenge Flow**: 
  1. Client sends request without credentials
  2. Proxy responds with `407 Proxy Authentication Required` and `Proxy-Authenticate: Basic realm="..."`
  3. Client retries with `Proxy-Authorization: Basic <base64>` header
  4. Proxy validates credentials and applies user-specific ACL
- **Credential Storage**: Support bcrypt, SHA-256, and crypt(3) hashing for file-based auth
- **ACL Engine**: Evaluate user/group against allow/deny lists before forwarding requests
- **Session Cache**: Thread-safe LRU cache of recent authentications per client IP

### Performance Considerations
- Cache authentication results to avoid repeated PAM/file lookups
- Non-blocking I/O for PAM authentication to prevent thread blocking
- Efficient credential hashing (bcrypt with low cost factor for local files)

### Testing Requirements
- Authentication flow compliance (407 challenge, retry with credentials)
- Credential validation against htpasswd files
- ACL enforcement per user/group
- Session caching effectiveness tests
- Security tests for credential exposure prevention

### Dependencies
- Password hashing library (libcrypt, OpenSSL for bcrypt)
- PAM development headers (Linux) or equivalent
- Base64 encoding/decoding (can use existing parser utilities)

### Rollout Plan
1. Phase 1: Basic authentication with htpasswd file support
2. Phase 2: Per-user ACL engine
3. Phase 3: PAM integration and session caching
4. Phase 4: Advanced auth (OAuth2 proxy tokens, client certificates)

### Tickets Blocked By
- T011: Configuration File Support (credential paths, realm settings)

---
**Priority**: HIGH
**Estimated Effort**: 3-4 weeks
**Owner**: Security Engineering Team
