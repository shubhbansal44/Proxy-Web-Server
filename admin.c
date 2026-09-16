#include "admin.h"
#include "metrics.h"
#include "logger.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int g_admin_socket = -1;
static pthread_t g_admin_thread;
static volatile bool g_admin_running = false;

static void handle_admin_request(int client_sock) {
    char buf[2048];
    ssize_t bytes_read = recv(client_sock, buf, sizeof(buf) - 1, 0);
    if (bytes_read <= 0) {
        close(client_sock);
        return;
    }
    buf[bytes_read] = '\0';
    
    // Parse method and path
    char method[16], path[256];
    if (sscanf(buf, "%15s %255s", method, path) != 2) {
        close(client_sock);
        return;
    }
    
    char *response = (char*)malloc(8192); // Increase response buffer for HTML
    if (!response) {
        close(client_sock);
        return;
    }
    
    if (strcmp(path, "/metrics") == 0) {
        char* metrics = metrics_export_prometheus();
        if (metrics) {
            snprintf(response, 8192, 
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/plain\r\n"
                     "Connection: close\r\n\r\n%s", metrics);
            send(client_sock, response, strlen(response), 0);
            free(metrics);
        } else {
            const char* err = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
            send(client_sock, err, strlen(err), 0);
        }
    } else if (strcmp(path, "/health") == 0) {
        const char* health = "{\"status\": \"UP\"}";
        snprintf(response, 8192,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Connection: close\r\n\r\n%s", health);
        send(client_sock, response, strlen(response), 0);
    } else if (strcmp(path, "/status") == 0) {
        char* metrics = metrics_export_prometheus();
        snprintf(response, 8192,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html\r\n"
                 "Connection: close\r\n\r\n"
                 "<html><head><title>Proxy Status</title></head>"
                 "<body style='font-family:sans-serif; padding:20px;'>"
                 "<h1>Proxy Server Status</h1>"
                 "<h2>Configuration</h2>"
                 "<ul>"
                 "<li>Port: %d</li>"
                 "<li>Max Clients: %d</li>"
                 "<li>Caching Enabled: %s</li>"
                 "</ul>"
                 "<h2>Live Metrics</h2>"
                 "<pre style='background:#f4f4f4; padding:10px;'>%s</pre>"
                 "</body></html>",
                 g_config.port, g_config.max_clients, 
                 g_config.enable_cache ? "Yes" : "No", 
                 metrics ? metrics : "Unavailable");
        if (metrics) free(metrics);
        send(client_sock, response, strlen(response), 0);
    } else {
        const char* not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send(client_sock, not_found, strlen(not_found), 0);
    }
    
    free(response);
    close(client_sock);
}

static void* admin_thread_func(void* arg) {
    while (g_admin_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(g_admin_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_sock >= 0) {
            handle_admin_request(client_sock);
        }
    }
    return NULL;
}

bool admin_server_start(const AdminConfig* config) {
    if (!config || !config->enabled) return false;
    
    g_admin_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_admin_socket < 0) {
        LOG_ERROR("Failed to create admin socket");
        return false;
    }
    
    int reuse = 1;
    setsockopt(g_admin_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config->port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(g_admin_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Failed to bind admin server to port %d", config->port);
        close(g_admin_socket);
        return false;
    }
    
    if (listen(g_admin_socket, 10) < 0) {
        LOG_ERROR("Failed to listen on admin socket");
        close(g_admin_socket);
        return false;
    }
    
    g_admin_running = true;
    if (pthread_create(&g_admin_thread, NULL, admin_thread_func, NULL) != 0) {
        LOG_ERROR("Failed to create admin thread");
        g_admin_running = false;
        close(g_admin_socket);
        return false;
    }
    
    LOG_INFO("Admin server started on port %d", config->port);
    return true;
}

void admin_server_stop(void) {
    g_admin_running = false;
    if (g_admin_socket >= 0) {
        shutdown(g_admin_socket, SHUT_RDWR);
        close(g_admin_socket);
        g_admin_socket = -1;
    }
    pthread_join(g_admin_thread, NULL);
}
