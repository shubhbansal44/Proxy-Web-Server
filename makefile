CC=g++
CFLAGS= -g -Wall -pthread
COVERAGE_FLAGS= -fprofile-arcs -ftest-coverage

all: proxy

proxy: Main.c config.c cache.c proxy_parse.c auth.c logger.c metrics.c admin.c tls_tunnel.c
	$(CC) $(CFLAGS) -o proxy_parse.o -c proxy_parse.c
	$(CC) $(CFLAGS) -o cache.o -c cache.c
	$(CC) $(CFLAGS) -o config.o -c config.c
	$(CC) $(CFLAGS) -o auth.o -c auth.c
	$(CC) $(CFLAGS) -o logger.o -c logger.c
	$(CC) $(CFLAGS) -o metrics.o -c metrics.c
	$(CC) $(CFLAGS) -o admin.o -c admin.c
	$(CC) $(CFLAGS) -o tls_tunnel.o -c tls_tunnel.c
	$(CC) $(CFLAGS) -o proxy.o -c Main.c
	$(CC) $(CFLAGS) -o proxy proxy_parse.o config.o cache.o auth.o logger.o metrics.o admin.o tls_tunnel.o proxy.o -lssl -lcrypto

test: proxy
	mkdir -p tests/bin
	$(CC) $(CFLAGS) -o proxy_parse.o -c proxy_parse.c
	$(CC) $(CFLAGS) -o config.o -c config.c
	$(CC) $(CFLAGS) -o cache.o -c cache.c
	$(CC) $(CFLAGS) -o mock_server.o -c tests/mock_server.c
	$(CC) $(CFLAGS) -o tests/bin/test_proxy_parse proxy_parse.o config.o cache.o tests/test_proxy_parse.c -I.
	$(CC) $(CFLAGS) -o tests/bin/test_config proxy_parse.o config.o cache.o tests/test_config.c -I.
	$(CC) $(CFLAGS) -o tests/bin/test_cache cache.o config.o tests/test_cache.c -I.
	$(CC) $(CFLAGS) -o tests/bin/test_integration tests/test_integration.c mock_server.o proxy_parse.o config.o cache.o -I.
	./tests/run_tests.sh

coverage: clean
	mkdir -p tests/bin
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o proxy_parse.o -c proxy_parse.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o config.o -c config.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o cache.o -c cache.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o mock_server.o -c tests/mock_server.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o auth.o -c auth.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o logger.o -c logger.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o metrics.o -c metrics.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o admin.o -c admin.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tests/bin/test_proxy_parse proxy_parse.o config.o cache.o tests/test_proxy_parse.c -I.
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tests/bin/test_config proxy_parse.o config.o cache.o tests/test_config.c -I.
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tests/bin/test_cache cache.o config.o tests/test_cache.c -I.
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o proxy.o -c Main.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o proxy proxy_parse.o config.o cache.o auth.o logger.o metrics.o admin.o tls_tunnel.o proxy.o -lssl -lcrypto
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tests/bin/test_integration tests/test_integration.c mock_server.o proxy_parse.o config.o cache.o -I.
	./tests/run_tests.sh
	gcov proxy_parse.c config.c cache.c auth.c logger.c metrics.c admin.c

clean:
	rm -f proxy *.o *.gcno *.gcda *.gcov
	rm -rf tests/bin

tar:
	tar -cvzf ass1.tgz Main.c README makefile proxy_parse.c proxy_parse.h cache.c cache.h config.c config.h auth.c auth.h logger.c logger.h metrics.c metrics.h admin.c admin.h




