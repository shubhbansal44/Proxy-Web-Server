# Proxy Web Server Project Documentation

## Overview
This project is a multi-threaded HTTP proxy server implemented in C/C++ with support for modern protocols, caching, authentication, and observability features.

## Project Structure
```
.
├── Main.c              # Main server logic, socket setup, threading
├── proxy_parse.c       # HTTP request parsing
├── cache.c             # LRU cache implementation
├── config.c            # Configuration parsing
├── auth.c              # Authentication and IP-based access control
├── logger.c            # Structured logging system
├── metrics.c           # Prometheus metrics collection
├── admin.c             # Administrative HTTP endpoint
├── tls_tunnel.c        # HTTPS CONNECT tunneling (TLS)
├── http2.c             # HTTP/2 protocol support
├── http3.c             # HTTP/3 protocol support (QUIC)
├── proxy_parse.h       # Header for proxy parsing
├── cache.h             # Header for cache
├── config.h            # Header for configuration
├── auth.h              # Header for authentication
├── logger.h            # Header for logging
├── metrics.h           # Header for metrics
├── admin.h             # Header for admin endpoint
├── tls_tunnel.h        # Header for TLS tunneling
├── http2.h             # Header for HTTP/2
├── http3.h             # Header for HTTP/3
├── proxy.conf          # Example configuration file
├── makefile            # Build instructions
├── tickets/            # Feature specification tickets
│   ├── ticket001.md    # HTTPS CONNECT Tunneling
│   ├── ticket002.md    # HTTP/2 and HTTP/3 Support
│   ├── ticket003.md    # Granular Access Control & Authentication
│   ├── ticket004.md    # Logging, Monitoring & Observability
│   ├── ticket005.md    # Advanced Caching System
│   ├── ticket006.md    # Load Balancing & Traffic Management
│   ├── ticket007.md    # Content Filtering & Security
│   ├── ticket008.md    # WebSocket Support
│   ├── ticket009.md    # IPv6 Support
│   ├── ticket010.md    # Performance Optimization & Scaling
│   ├── ticket011.md    # Developer Experience & Tooling
│   ├── ticket012.md    # Documentation & Standards Compliance
│   └── ticket013.md    # Deployment & Operations
└── tests/              # Test suite
    ├── mock_server.c
    ├── test_proxy_parse.c
    ├── test_config.c
    ├── test_cache.c
    └── run_tests.sh
```

## Build & Execution
### Dependencies
- GCC/G++ compiler
- POSIX threads (`-pthread`)
- OpenSSL (`-lssl -lcrypto`) for TLS
- nghttp2 (`-lnghttp2`) for HTTP/2
- quiche (built from `deps/quiche`) for HTTP/3

### Build Commands
```bash
# Build the proxy server
make

# Build and run tests
make test

# Build with coverage instrumentation
make coverage

# Clean build artifacts
make clean

# Create source tarball
make tar
```

### Running the Proxy
```bash
# Start the proxy with default configuration (listens on port 8080)
./proxy

# Start with a custom configuration file
./proxy /path/to/custom.conf
```

## Configuration
The proxy uses a configuration file (default: `proxy.conf` in the current directory) with the following properties:

| Property | Type | Description |
|----------|------|-------------|
| `port` | integer | TCP port to listen on (default: 8080) |
| `threads` | integer | Number of worker threads (default: number of CPU cores) |
| `cache_size_mb` | integer | Maximum cache size in megabytes (default: 100) |
| `auth_file` | string | Path to password file for basic auth (format: `user:password`) |
| `acl_file` | string | Path to ACL file (format: `allow|deny IP/CIDR`) |
| `log_level` | string | Logging level: `DEBUG`, `INFO`, `WARN`, `ERROR` (default: `INFO`) |
| `log_file` | string | Path to log file (if empty, logs to stdout) |
| `metrics_port` | integer | Port for Prometheus metrics endpoint (default: 9090) |
| `admin_port` | integer | Port for administrative API (default: 9091) |
| `enable_http2` | boolean | Enable HTTP/2 support (default: true) |
| `enable_http3` | boolean | Enable HTTP/3 support (default: true) |
| `tls_cert_file` | string | Path to TLS certificate for HTTPS tunneling |
| `tls_key_file` | string | Path to TLS private key |

Example `proxy.conf`:
```
port 8080
threads 4
cache_size_mb 256
auth_file /etc/proxy/passwd
acl_file /etc/proxy/acl
log_level INFO
log_file /var/log/proxy.log
metrics_port 9090
admin_port 9091
enable_http2 true
enable_http3 true
tls_cert_file /etc/proxy/cert.pem
tls_key_file /etc/proxy/key.pem
```

## Implemented Features

### ✅ Completed Tickets

#### Ticket T001: HTTPS CONNECT Tunneling
- **Status**: Complete
- **Implemented**: 
  - CONNECT method handling (RFC 7231 Section 6.3.6)
  - TLS context management using OpenSSL
  - SNI (Server Name Indication) support
  - Bidirectional encrypted tunnel between client and origin server
  - TLS 1.2/1.3 support with session resumption
  - Configurable timeouts and connection limits
  - Metrics for active/successful/failed tunnels

#### Ticket T002: HTTP/2 and HTTP/3 Support
- **Status**: Complete
- **Implemented**:
  - HTTP/2 support via nghttp2 library (frame multiplexing, HPACK compression)
  - HTTP/3 support via quiche library (QUIC transport, QPACK compression)
  - ALPN negotiation for automatic protocol selection
  - Stream management for concurrent requests
  - Connection reuse across protocols
  - Performance optimizations for high-latency networks

#### Ticket T003: Granular Access Control & Authentication
- **Status**: Complete (Basic Implementation)
- **Implemented**:
  - HTTP Basic authentication with secure credential handling
  - IP-based access control (allow/deny lists with CIDR support)
  - Basic policy evaluation engine (first-match)
  - Authentication token caching (short TTL)
  - Audit logging for access decisions
  - *Note*: Advanced methods (LDAP, OAuth2, SAML, client certificates) are not implemented and remain future scope.

#### Ticket T004: Logging, Monitoring & Observability
- **Status**: Complete
- **Implemented**:
  - Structured JSON logging with log levels (DEBUG, INFO, WARN, ERROR)
  - Size-based log rotation
  - Prometheus metrics endpoint (`/metrics`)
  - Key metrics: request counters, latency histograms, cache hit/miss ratios, active connections
  - Health check endpoints (`/health/live`, `/health/ready`)
  - Administrative API (`/admin/*`) for runtime configuration and stats
  - Basic alerting via log thresholds

#### Ticket T005: Advanced Caching System
- **Status**: Complete (Hybrid Memory Cache)
- **Implemented**:
  - LRU cache with configurable memory size
  - HTTP caching compliance (RFC 7234): Cache-Control, ETag, Expires, Vary headers
  - Conditional request support (If-None-Match, If-Modified-Since)
  - Cache metadata indexing for O(1) lookups
  - Cache hit/miss metrics
  - *Note*: Disk-backed tier and advanced eviction policies (LFU, TTL-based) are not implemented and remain future scope.

#### Ticket T011: Developer Experience & Tooling
- **Status**: Complete
- **Implemented**:
  - Comprehensive test suite (unit and integration tests)
  - Makefile with build, test, coverage, and packaging targets
  - Source tarball creation
  - Code coverage reporting via gcov
  - Clear directory structure and modular design

### 🔄 In Progress / Partial Tickets

#### Ticket T009: IPv6 Support
- **Status**: Partial
- **Implemented**:
  - Dual-stack socket binding (IPv4 and IPv6) in `Main.c`
  - Basic IPv6 address handling in connection acceptance
- **Remaining**:
  - Full IPv6 testing in all code paths
  - IPv6-specific ACL improvements
  - Path MTU discovery considerations

#### Ticket T010: Performance Optimization & Scaling
- **Status**: Partial
- **Implemented**:
  - Thread-per-worker model with configurable thread count
  - Connection pooling for HTTP/2 and HTTP/3
  - Efficient data structures (hash tables, LRU lists)
  - Non-blocking I/O for logging and metrics
- **Remaining**:
  - Advanced connection reuse algorithms
  - CPU affinity and NUMA optimizations
  - Kernel-level optimizations (SO_REUSEPORT, etc.)
  - Load testing and benchmarking framework

### 📋 Future Scope (Incomplete Tickets)

#### Ticket T006: Load Balancing & Traffic Management
- **Planned Features**:
  - Round-robin, least-connections, and IP-hash load balancing algorithms
  - Health checks for upstream servers
  - Circuit breaker patterns
  - Rate limiting and throttling
  - Traffic shaping and QoS policies

#### Ticket T007: Content Filtering & Security
- **Planned Features**:
  - URL-based filtering (blacklists/whitelists)
  - MIME type filtering
  - Malware scanning integration (ICAP)
  - SafeSearch enforcement
  - Data loss prevention (DLP) patterns
  - SSL/TLS inspection with custom certificate authority

#### Ticket T008: WebSocket Support
- **Planned Features**:
  - WebSocket protocol (RFC 6455) upgrading
  - Bidirectional full-duplex communication
  - WebSocket proxying with proper timeout handling
  - WebSocket-specific metrics and logging
  - Origin-based access control for WebSocket connections

#### Ticket T012: Documentation & Standards Compliance
- **Status**: In Progress
- **Planned Features**:
  - Complete API documentation (this document)
  - RFC compliance documentation for all implemented standards
  - User guides and administrator manuals
  - Deployment and operational procedures
  - Compliance with HTTP/1.1, HTTP/2, HTTP/3, TLS, and WebSocket standards

#### Ticket T013: Deployment & Operations
- **Planned Features**:
  - Docker containerization
  - Kubernetes Helm charts
  - Systemd service scripts
  - Log aggregation configurations (ELK/Fluentd)
  - Monitoring dashboards (Grafana)
  - Ansible/Puppet modules for configuration management
  - Automated testing in CI/CD pipelines