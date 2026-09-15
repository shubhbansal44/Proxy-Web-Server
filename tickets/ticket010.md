# Ticket T010: Logging & Access Logs - Squid-Compatible Format

## Overview
Implement comprehensive logging infrastructure with Squid-compatible access logs, structured JSON logs for machine processing, and real-time statistics exposure. The current codebase uses only printf for diagnostics with no structured logging.

## Goals
- Add Squid native access log format support
- Implement JSON structured logging for analytics pipelines
- Add real-time statistics endpoint (cache hit ratio, active connections, bandwidth)
- Support log rotation (size-based and time-based) with compression
- Provide configurable log levels (DEBUG, INFO, WARN, ERROR)

## Technical Specifications

### Logging System
- **Squid Format**: `timestamp elapsed remotehost code/status bytes method URL rfc931 peerstatus/peerhost type`
- **JSON Format**: Structured fields for request method, URL, status, bytes, client IP, duration, cache status
- **Log Levels**: DEBUG, INFO, WARN, ERROR with runtime configurability
- **Output Destinations**: File, syslog, stdout, network (UDP/TCP)
- **Rotation**: Size-based rotation with configurable max file size and retention count

### Real-Time Statistics
- **Exposed Metrics**:
  - Total requests served
  - Cache hit ratio (hits/total requests)
  - Active client connections
  - Upstream server connections
  - Total bytes transferred (inbound/outbound)
  - Error counts by type (4xx, 5xx, cache errors)
  - Throttled/blocked requests count
- **Endpoint**: HTTP endpoint (e.g., `/stats`) or Prometheus-compatible metrics export

### Implementation Details
- **Log Buffer**: Thread-safe ring buffer for async log writes
- **Rotation**: Background thread for log rotation and compression (gzip)
- **Filtering**: Per-module log level filtering to reduce noise in production
- **Context Fields**: Include request ID, thread ID, and timestamp for correlation

### Performance Considerations
- Non-blocking log writes to avoid impacting request latency
- Configurable log levels to reduce verbosity in production
- Batch writes for high-throughput scenarios
- Memory-efficient JSON serialization (no full-string copies)

### Testing Requirements
- Log format compliance (Squid, JSON)
- Rotation correctness (file naming, count, compression)
- Statistics accuracy under concurrent load
- Log level filtering verification
- Syslog integration testing

### Dependencies
- Standard C library (stdio, time)
- Gzip library (zlib) for log compression
- Syslog library (syslog.h) for system log integration

### Rollout Plan
1. Phase 1: Access logging with Squid and JSON format support
2. Phase 2: Log rotation and compression
3. Phase 3: Real-time statistics endpoint
4. Phase 4: Syslog integration and advanced filtering

### Tickets Blocked By
- T011: Configuration File Support (log paths, levels, format selection)

---
**Priority**: HIGH
**Estimated Effort**: 2-3 weeks
**Owner**: DevOps Engineering Team
