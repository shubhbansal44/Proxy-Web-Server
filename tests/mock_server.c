#include "mock_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

static void *mock_server_thread(void *arg) {
    MockServer *server = (MockServer *)arg;
    struct sockaddr_in address;
    int opt = 1;

    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd == 0) return NULL;

    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(server->port);

    if (bind(server->server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        return NULL;
    }
    
    if (listen(server->server_fd, 3) < 0) {
        return NULL;
    }

    server->is_running = 1;

    while (server->is_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server->server_fd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        int activity = select(server->server_fd + 1, &readfds, NULL, NULL, &timeout);
        
        if (activity > 0 && FD_ISSET(server->server_fd, &readfds)) {
            int new_socket = accept(server->server_fd, NULL, NULL);
            if (new_socket < 0) continue;

            char buffer[4096] = {0};
            read(new_socket, buffer, 4096);
            
            // Just respond with the provided data
            write(new_socket, server->response_data, strlen(server->response_data));
            close(new_socket);
        }
    }

    return NULL;
}

int mock_server_start(MockServer *server, int port, const char *response_data) {
    server->port = port;
    server->is_running = 0;
    server->response_data = strdup(response_data);
    server->expected_request_prefix = NULL;
    
    if (pthread_create(&server->thread_id, NULL, mock_server_thread, server) != 0) {
        return -1;
    }
    
    // Wait briefly for server to start
    usleep(100000); // 100ms
    return 0;
}

void mock_server_stop(MockServer *server) {
    server->is_running = 0;
    pthread_join(server->thread_id, NULL);
    if (server->server_fd > 0) {
        close(server->server_fd);
    }
    if (server->response_data) {
        free(server->response_data);
    }
}
