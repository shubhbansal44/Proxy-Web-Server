#ifndef HTTP2_STREAM_H
#define HTTP2_STREAM_H

#include <openssl/ssl.h>
#include <nghttp2/nghttp2.h>
#include <pthread.h>

typedef struct {
    nghttp2_session *session;
    int client_fd;
    SSL *client_ssl;
    
    char target_host[256];
    int target_port;
    
    int active;
    pthread_mutex_t lock;
} http2_conn_t;

typedef struct {
    int32_t stream_id;
    http2_conn_t *conn;
    
    char *method;
    char *path;
    char *authority;
    char *request_buf;
    size_t request_buf_len;
    size_t request_buf_size;
    
    char *body;
    size_t body_len;
    size_t body_size;
    
    char *response_buf;
    size_t response_buf_len;
    size_t response_buf_size;
} http2_stream_data;

#endif
