# Ticket T011: Configuration File Support

## Overview
Replace hardcoded configuration values and command-line-only settings with a structured configuration file system. The current implementation requires command-line arguments and has no way to configure caching behavior, limits, or feature toggles without code changes.

## Goals
- Implement a configuration file parser (INI-style or YAML)
- Support environment variable overrides for containerized deployments
- Add configuration validation at startup
- Enable hot-reload of runtime-configurable settings
- Provide a default configuration template with all available options

## Technical Specifications

### Configuration Sources (in precedence order)
1. **Default values** (compiled-in defaults)
2. **Configuration file** (e.g., `/etc/proxy/proxy.conf` or `~/.config/proxy/proxy.conf`)
3. **Environment variables** (e.g., `PROXY_PORT`, `PROXY_MAX_CLIENTS`)
4. **Command-line arguments** (override all other sources)

### Configurable Parameters
- **Network**: Listen port, bind address, IPv6 enable flag
- **Limits**: Max concurrent clients, max request size, cache size, element size
- **Caching**: Cache enabled/disabled, cache directories, retention policies
- **Security**: Auth enabled, credential file path, blocklist paths
- **Logging**: Log file path, log level, log format, rotation settings
- **Throttling**: Rate limit thresholds, burst sizes, bandwidth caps
- **Features**: Enable/disable HTTPS CONNECT, HTTP/2, etc.

### Implementation Details
- **File Format**: Simple INI-style (key=value pairs, section headers) for minimal dependencies
- **Parser**: Custom or lightweight parser (avoid heavy YAML/JSON dependencies for core config)
- **Validation**: Schema validation with type checking and range bounds
- **Hot-Reload**: Watch config file for changes (inotify on Linux) and reload safe parameters
- **Template**: Provide example configuration file with all options documented

### Performance Considerations
- Parse configuration once at startup (negligible overhead)
- Hot-reload should only re-apply safe parameters (no full restart)
- Cache parsed configuration values in memory for fast access

### Testing Requirements
- Config file parsing correctness (all supported formats)
- Environment variable override precedence
- Validation of invalid/out-of-range values
- Hot-reload behavior for supported parameters
- Default fallback when config file is missing

### Dependencies
- Standard C library (stdio, stdlib)
- Optional: inotify for hot-reload on Linux

### Rollout Plan
1. Phase 1: Basic INI-style config file with startup parsing and validation
2. Phase 2: Environment variable overrides and command-line integration
3. Phase 3: Hot-reload for runtime-configurable settings
4. Phase 4: Configuration template and documentation

### Tickets Blocked By
- (none - this is a foundational ticket)

---
**Priority**: HIGH
**Estimated Effort**: 1-2 weeks
**Owner**: Core Engineering Team
