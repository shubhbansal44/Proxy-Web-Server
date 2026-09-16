#include "../config.h"
ProxyConfig g_config;
#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"
#include "mock_server.h"
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include "../config.h"


pid_t start_proxy(int port) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        char port_str[32];
        sprintf(port_str, "%d", port);
        
        int fd = open("/dev/null", O_WRONLY);
        if (fd != -1) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        
        execl("./proxy", "proxy", port_str, NULL);
        exit(1);
    }
    usleep(500000); // give proxy time to start
    return pid;
}

void test_forward_proxy() {
    TEST_SUITE(test_forward_proxy);
    
    // Start mock upstream
    MockServer upstream;
    const char *resp = "HTTP/1.0 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
    int res = mock_server_start(&upstream, 9091, resp);
    TEST_ASSERT_EQ(0, res);
    
    // Start proxy
    pid_t proxy_pid = start_proxy(9090);
    TEST_ASSERT(proxy_pid > 0);
    
    // Connect to proxy
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(9090);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        TEST_ASSERT(0 && "Connection to proxy failed");
    } else {
        const char *req = "GET http://127.0.0.1:9091/ HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
        write(sock, req, strlen(req));
        
        char buffer[1024] = {0};
        int valread = read(sock, buffer, 1024);
        TEST_ASSERT(valread > 0);
        
        // Find body in response
        char *body = strstr(buffer, "\r\n\r\n");
        if (body) {
            body += 4;
            TEST_ASSERT_STR_EQ("Hello, World!", body);
        } else {
            TEST_ASSERT(0 && "No body found in response");
        }
        close(sock);
    }
    
    kill(proxy_pid, SIGTERM);
    waitpid(proxy_pid, NULL, 0);
    mock_server_stop(&upstream);
}

int main() {
    test_forward_proxy();
    
    TEST_REPORT();
    return 0;
}
