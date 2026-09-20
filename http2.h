#ifndef HTTP2_H
#define HTTP2_H

#include <openssl/ssl.h>
#include <nghttp2/nghttp2.h>

void handle_http2_connection(SSL *client_ssl, SSL *server_ssl, const char *target_host, int target_port, SSL_CTX *upstream_ctx);

#endif /* HTTP2_H */