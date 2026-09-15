# Proxy Web Server Analysis

Based on a review of the codebase (`Main.c`, `proxy_parse.c`, `ipv6.c`, etc.), here is an analysis of the current scope, architecture, and actionable ideas for expanding the project.

## Current Project Scope & Architecture

The project is a **Multithreaded HTTP Proxy Server** written in C. Its primary purpose is to act as an intermediary for HTTP requests, enhancing performance through caching and handling multiple clients concurrently.

### Key Features:
1. **Multithreaded Connection Handling**: 
   - Utilizes POSIX threads (`pthread_create`) to handle multiple client connections simultaneously.
   - Restricts the maximum number of concurrent connections using Semaphores (`sem_init(&SEMAPHORE, 0, MAX_CLIENTS)`).
2. **LRU (Least Recently Used) Caching Mechanism**:
   - Implements an in-memory cache mechanism with custom structs (`CacheModule`).
   - Uses a linked-list structure with Mutex locks (`pthread_mutex_init`) to make cache access thread-safe.
   - Evicts the least recently used requests when the `CACHE_SIZE` limit is reached to free up memory.
3. **Custom HTTP Parsing**:
   - Includes a custom HTTP parser (`proxy_parse.h/c`) to parse request lines, extract headers, method, protocol, host, and port.
4. **Network Sockets**:
   - Built using standard POSIX socket APIs (`socket`, `bind`, `listen`, `accept`).
   - Currently hardcoded for `AF_INET` (IPv4) addressing in the main proxy logic.

---

## How to Expand the Project

To evolve this project from a basic academic/learning proxy into a robust, production-like network utility, consider the following expansions:

### 1. HTTPS Protocol Support
Modern web traffic is predominantly HTTPS. The current server seems designed to parse flat HTTP. 
- **Action**: Implement support for the HTTP `CONNECT` method to establish a TCP tunnel between the client and the destination server for encrypted HTTPS traffic.

### 2. Full IPv6 Integration
The presence of `ipv6.c` suggests experimentation with IPv6, but `Main.c` forces `AF_INET` (IPv4).
- **Action**: Upgrade socket creation and binding to use `getaddrinfo()` with `AF_UNSPEC` so the proxy can seamlessly handle both IPv4 and IPv6 traffic.

### 3. Advanced Caching Capabilities
The current custom LRU cache is completely memory-bound and does not seem to respect standard HTTP cache headers.
- **Action**: Add logic to parse and respect `Cache-Control` (e.g., `no-cache`, `max-age`), `ETag`, and `Expires` headers. 
- **Action**: Implement a **disk-based cache** persistence layer so the cache survives server restarts and can hold larger assets without exhausting RAM.

### 4. Content Filtering & Ad-Blocking
Proxies are commonly used to block undesirable content.
- **Action**: Introduce a blocklist of domains/IPs (like malicious sites or ad networks). When a client requests a blocked host, the proxy can immediately return a `403 Forbidden` or a custom HTML block page.

### 5. Rate Limiting and Bandwidth Throttling
Prevent single clients from monopolizing proxy resources.
- **Action**: Implement token-bucket or leaky-bucket algorithms to artificially delay or drop packets for IP addresses that exceed a certain data transfer rate.

### 6. Client Authentication
Currently, any client that can reach the port can use the proxy.
- **Action**: Implement the `Proxy-Authenticate` challenge. Require clients to send a `Proxy-Authorization: Basic <base64>` header to restrict access to authorized users only.

### 7. Logging and Monitoring Dashboards
Visibility into proxy traffic is essential.
- **Action**: Write detailed access logs to a file (in standard Squid access log format).
- **Action**: Expose a mini web dashboard on a special port or route (e.g., `http://proxy.local/status`) that displays real-time metrics: Cache Hit Ratio, Active Connections, Total Bandwidth Used.

### 8. Configuration File Support
Avoid recompilation for changing basic parameters.
- **Action**: Create a `proxy.conf` file parser to configure dynamic values like `PORT_NUMBER`, `MAX_CLIENTS`, `CACHE_SIZE`, and blocklist file paths.
