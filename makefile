CC=g++
CFLAGS= -g -Wall -pthread
COVERAGE_FLAGS= -fprofile-arcs -ftest-coverage
HTTP2_LIBS= -lnghttp2
QUICHE_DIR=deps/quiche
QUICHE_LIBS=-L$(QUICHE_DIR)/target/release -Ltarget/release -lquiche -ldl -lrt -pthread -Wl,-rpath,$(QUICHE_DIR)/target/release -Wl,-rpath,target/release
SRC_DIR=src

all: proxy

proxy: $(SRC_DIR)/Main.c $(SRC_DIR)/config.c $(SRC_DIR)/cache.c $(SRC_DIR)/proxy_parse.c $(SRC_DIR)/auth.c $(SRC_DIR)/logger.c $(SRC_DIR)/metrics.c $(SRC_DIR)/admin.c $(SRC_DIR)/tls_tunnel.c $(SRC_DIR)/http2.c $(SRC_DIR)/http3.c
	$(CC) $(CFLAGS) -o proxy_parse.o -c $(SRC_DIR)/proxy_parse.c
	$(CC) $(CFLAGS) -o cache.o -c $(SRC_DIR)/cache.c
	$(CC) $(CFLAGS) -o config.o -c $(SRC_DIR)/config.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o auth.o -c $(SRC_DIR)/auth.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o logger.o -c $(SRC_DIR)/logger.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o metrics.o -c $(SRC_DIR)/metrics.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o admin.o -c $(SRC_DIR)/admin.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o tls_tunnel.o -c $(SRC_DIR)/tls_tunnel.c -I$(SRC_DIR) -I$(QUICHE_DIR)/quiche/include
	$(CC) $(CFLAGS) -o http2.o -c $(SRC_DIR)/http2.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o http3.o -c $(SRC_DIR)/http3.c -I$(SRC_DIR) -I$(QUICHE_DIR)/quiche/include
	$(CC) $(CFLAGS) -o proxy.o -c $(SRC_DIR)/Main.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o proxy proxy_parse.o config.o cache.o auth.o logger.o metrics.o admin.o tls_tunnel.o http2.o http3.o proxy.o -lssl -lcrypto $(HTTP2_LIBS) $(QUICHE_LIBS)

test: proxy
	mkdir -p tests/bin
	$(CC) $(CFLAGS) -o proxy_parse.o -c $(SRC_DIR)/proxy_parse.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o config.o -c $(SRC_DIR)/config.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o cache.o -c $(SRC_DIR)/cache.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) -o mock_server.o -c tests/mock_server.c
	$(CC) $(CFLAGS) -o tests/bin/test_proxy_parse proxy_parse.o config.o cache.o logger.o tests/test_proxy_parse.c -I$(SRC_DIR) -I.
	$(CC) $(CFLAGS) -o tests/bin/test_config proxy_parse.o config.o cache.o logger.o tests/test_config.c -I$(SRC_DIR) -I.
	$(CC) $(CFLAGS) -o tests/bin/test_cache cache.o config.o logger.o tests/test_cache.c -I$(SRC_DIR) -I.
	$(CC) $(CFLAGS) -o tests/bin/test_integration tests/test_integration.c mock_server.o proxy_parse.o config.o cache.o logger.o -I$(SRC_DIR) -I.
	./tests/run_tests.sh

coverage: clean
	mkdir -p tests/bin
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o proxy_parse.o -c $(SRC_DIR)/proxy_parse.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o config.o -c $(SRC_DIR)/config.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o cache.o -c $(SRC_DIR)/cache.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o mock_server.o -c tests/mock_server.c
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o auth.o -c $(SRC_DIR)/auth.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o logger.o -c $(SRC_DIR)/logger.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o metrics.o -c $(SRC_DIR)/metrics.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o admin.o -c $(SRC_DIR)/admin.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tls_tunnel.o -c $(SRC_DIR)/tls_tunnel.c -I$(SRC_DIR) -I$(QUICHE_DIR)/quiche/include
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o http2.o -c $(SRC_DIR)/http2.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o http3.o -c $(SRC_DIR)/http3.c -I$(SRC_DIR) -I$(QUICHE_DIR)/quiche/include
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tests/bin/test_proxy_parse proxy_parse.o config.o cache.o logger.o tests/test_proxy_parse.c -I$(SRC_DIR) -I.
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tests/bin/test_config proxy_parse.o config.o cache.o logger.o tests/test_config.c -I$(SRC_DIR) -I.
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tests/bin/test_cache cache.o config.o logger.o tests/test_cache.c -I$(SRC_DIR) -I.
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o proxy.o -c $(SRC_DIR)/Main.c -I$(SRC_DIR)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o proxy proxy_parse.o config.o cache.o auth.o logger.o metrics.o admin.o tls_tunnel.o http2.o http3.o proxy.o -lssl -lcrypto $(HTTP2_LIBS) $(QUICHE_LIBS)
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -o tests/bin/test_integration tests/test_integration.c mock_server.o proxy_parse.o config.o cache.o logger.o -I$(SRC_DIR) -I.
	./tests/run_tests.sh
	gcov $(SRC_DIR)/proxy_parse.c $(SRC_DIR)/config.c $(SRC_DIR)/cache.c $(SRC_DIR)/auth.c $(SRC_DIR)/logger.c $(SRC_DIR)/metrics.c $(SRC_DIR)/admin.c

clean:
	rm -f proxy *.o *.gcno *.gcda *.gcov
	rm -rf tests/bin

tar:
	tar -cvzf ass1.tgz $(SRC_DIR)/Main.c README makefile $(SRC_DIR)/proxy_parse.c $(SRC_DIR)/proxy_parse.h $(SRC_DIR)/cache.c $(SRC_DIR)/cache.h $(SRC_DIR)/config.c $(SRC_DIR)/config.h $(SRC_DIR)/auth.c $(SRC_DIR)/auth.h $(SRC_DIR)/logger.c $(SRC_DIR)/logger.h $(SRC_DIR)/metrics.c $(SRC_DIR)/metrics.h $(SRC_DIR)/admin.c $(SRC_DIR)/admin.h






