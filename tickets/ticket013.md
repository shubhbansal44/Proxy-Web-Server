# Ticket T013: Packaging & Deployment

## Overview
Provide standardized packaging and deployment artifacts to make the proxy easily installable and distributable across different platforms and deployment models (OSS binary releases, Docker containers, managed appliances).

## Goals
- Create distributable binary packages (tar.gz, deb, RPM)
- Add Docker container image for easy deployment
- Implement systemd service unit for Linux
- Provide installation script with dependency checking
- Add version metadata and build timestamps

## Technical Specifications

### Package Formats
- **Tarball**: `.tar.gz` with pre-compiled binary and config template
- **Debian**: `.deb` package with file placement per Debian policy
- **RPM**: `.rpm` for RHEL/CentOS/Fedora distributions
- **Docker**: Multi-stage Dockerfile with minimal runtime image (scratch or Alpine)

### Installation Artifacts
- **Binary**: Compiled proxy executable with version flag (`--version`)
- **Config**: Default config file placed at `/etc/proxy/proxy.conf`
- **Service**: systemd unit file at `/etc/systemd/system/proxy.service`
- **Docs**: Man pages for proxy(8) command

### Docker Container
- **Multi-stage Build**: Build stage with dev dependencies, final stage with just binary
- **Runtime User**: Non-root user for security (proxy:proxy, UID 1000)
- **Config Mount**: Volume mount for user config (`/etc/proxy/proxy.conf`)
- **Log Handling**: stdout/stderr logging with optional file output
- **Healthcheck**: HTTP probe against stats endpoint

### Versioning & Metadata
- **Version String**: Semantic versioning (v0.1.0) embedded in binary
- **Build Info**: Compile date, git commit hash as build flags
- **Release Process**: Tagged releases on GitHub with binary uploads

### Systemd Service
- **Type**: forking or simple
- **User/Group**: Run as `proxy` user
- **Restart**: Always restart on failure
- **Dependencies**: After=network-online.target
- **Limits**: Configure file descriptor and process limits (nofile, nproc)

### Implementation Details
- **Makefile Updates**: Add `install`, `deb`, `rpm`, `docker` targets
- **Version Script**: `scripts/version.sh` to generate version header
- **Packaging Spec**: `packaging/debian/proxy.spec`, `packaging/rpm/proxy.spec`
- **Dockerfiles**: `Dockerfile` (multi-stage) and `Dockerfile.alpine`
- **Install Script**: `scripts/install.sh` with OS detection and dependency check

### Performance Considerations
- Minimal Docker image size (target: <10MB)
- Static linking for single-binary distribution
- Proper signal handling (SIGTERM/SIGINT) in container

### Testing Requirements
- Package installation verification on Ubuntu and CentOS
- Docker container startup and healthcheck
- systemd service starts correctly with custom config
- Binary runs with `--version` and `--help`
- Container exits cleanly on SIGTERM

### Dependencies
- `make` for build orchestration
- Package build tools (dpkg-deb, rpm-build or rpmbuild)
- Docker for container builds

### Rollout Plan
1. Phase 1: Version embedding and tarball release packaging
2. Phase 2: Debian/RPM packaging with systemd service
3. Phase 3: Docker container image with healthcheck
4. Phase 4: Install script and automated release workflow

### Tickets Blocked By
- T011: Configuration File Support (packages need default config)
- T010: Logging & Access Logs (logs need destinations for container)

---
**Priority**: MEDIUM
**Estimated Effort**: 2-3 weeks
**Owner**: DevOps Engineering Team
