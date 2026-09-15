# Ticket T012: Testing & Quality Assurance Framework

## Overview
Establish a comprehensive testing framework to ensure reliability, correctness, and maintainability as the proxy evolves. The current codebase has no test suite and uses printf-based diagnostics.

## Goals
- Implement unit tests for core modules (HTTP parser, cache, config, auth)
- Add integration tests for end-to-end HTTP/HTTPS request flows
- Set up continuous integration with automated test execution
- Add static analysis and memory safety checks (valgrind, AddressSanitizer)
- Create test infrastructure for mocking network I/O and upstream servers

## Technical Specifications

### Test Categories
- **Unit Tests**: Test individual functions/modules in isolation
  - HTTP request parsing (`proxy_parse.c`)
  - Cache operations (LRU eviction, lookup, insertion)
  - Configuration parsing and validation
  - Authentication credential validation
  - Rate limiting algorithm correctness
  - Content filter matching logic

- **Integration Tests**: Test full request/response cycles
  - HTTP/1.1 forward proxy requests
  - HTTPS CONNECT tunneling
  - Caching behavior (cache hit, miss, eviction)
  - Authentication challenge flow
  - Content filtering block/allow
  - Rate limiting enforcement

- **Stress/Performance Tests**: Verify behavior under load
  - Concurrent client connections
  - Large request/response bodies
  - High cache turnover scenarios
  - Long-running stability (memory leaks)

### Testing Infrastructure
- **Framework**: Simple test runner in C with expected/actual assertions
  - Macros for assertions: `TEST_ASSERT_EQ`, `TEST_ASSERT_STR_EQ`, `TEST_ASSERT_NE`
  - Test registration via macros or function pointer array
  - Output in TAP-compatible format for CI integration
- **Mocking**: Fake upstream servers using local sockets
- **Test Data**: Pre-built HTTP requests/responses as test fixtures
- **Memory Checking**: Run tests under AddressSanitizer and valgrind

### CI Pipeline
- **Build**: Compile with warnings-as-errors (`-Werror`)
- **Static Analysis**: Run `clang-tidy` or `cppcheck`
- **Tests**: Execute unit and integration tests
- **Memory**: Run key tests under valgrind for 5-minute timeout
- **Coverage**: Generate code coverage report (gcov/lcov)

### Security Testing
- Fuzz testing for HTTP parser (AFL++ or libFuzzer)
- Buffer overflow detection via AddressSanitizer
- Use-after-free detection via Valgrind

### Implementation Details
- Test files alongside source: `test_proxy_parse.c`, `test_cache.c`, etc.
- Mock upstream server in `tests/mock_server.c`
- Test fixtures in `tests/fixtures/`
- CI config: `.github/workflows/ci.yml` (GitHub Actions)
- Test runner script: `tests/run_tests.sh`

### Performance Considerations
- Tests should run fast (under 30 seconds total) for CI feedback
- Separate long-running stress tests into nightly builds
- Use in-memory or tmpfs for test file I/O

### Testing Requirements
- All core modules must have >80% code coverage
- Integration tests must pass on both Linux and macOS
- Memory tests must show zero leaks/errors
- CI pipeline must gate on test failure

### Dependencies
- Test framework (simple C test runner, no external deps for unit tests)
- Check or cmocka (optional, for richer assertions)
- Valgrind (for CI memory checks)
- AddressSanitizer (compiler flag, no external deps)
- AFL++ or libFuzzer (for fuzzing, optional)

### Rollout Plan
1. Phase 1: Unit test framework and parser/cache tests
2. Phase 2: Integration test infrastructure (mock upstream)
3. Phase 3: CI pipeline with build, static analysis, and test execution
4. Phase 4: Fuzz testing, memory checks, and coverage reporting

### Tickets Blocked By
- T011: Configuration File Support (tests need configurable parameters)

---
**Priority**: HIGH
**Estimated Effort**: 3-4 weeks
**Owner**: Quality Engineering Team
