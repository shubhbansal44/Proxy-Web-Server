#ifndef MOCK_SERVER_H
#define MOCK_SERVER_H

#include <pthread.h>

typedef struct {
    int port;
    int server_fd;
    pthread_t thread_id;
    int is_running;
    char *expected_request_prefix;
    char *response_data;
} MockServer;

/* Start a mock server on the specified port.
 * If expected_request_prefix is not NULL, the server will check if the request matches it.
 * It will respond with response_data.
 * Returns 0 on success.
 */
int mock_server_start(MockServer *server, int port, const char *response_data);

/* Stop the mock server */
void mock_server_stop(MockServer *server);

#endif
