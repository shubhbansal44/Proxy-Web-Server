#include "tls_tunnel.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <pthread.h>
#include "logger.h"
#include "config.h"
#include "metrics.h"

// OpenSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bn.h>

static SSL_CTX *server_ctx = NULL;
static SSL_CTX *client_ctx_out = NULL;
static EVP_PKEY *global_pkey = NULL;
static X509 *default_x509 = NULL;

// Very basic linked list for cert caching
typedef struct CertCacheNode {
    char *hostname;
    X509 *cert;
    struct CertCacheNode *next;
} CertCacheNode;

static CertCacheNode *cert_cache_head = NULL;
static pthread_mutex_t cert_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Generate X509 cert for specific hostname using global_pkey
static X509* generate_cert_for_host(const char *hostname) {
    X509 *x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L); // 1 year

    X509_set_pubkey(x509, global_pkey);

    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "C",  MBSTRING_ASC, (unsigned char *)"US", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O",  MBSTRING_ASC, (unsigned char *)"ProxyCo", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char *)hostname, -1, -1, 0);
    
    // Add SAN
    X509_EXTENSION *ext;
    char san_str[256];
    snprintf(san_str, sizeof(san_str), "DNS:%s", hostname);
    ext = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, san_str);
    if (ext) {
        X509_add_ext(x509, ext, -1);
        X509_EXTENSION_free(ext);
    }

    X509_set_issuer_name(x509, name);
    X509_sign(x509, global_pkey, EVP_sha256());

    return x509;
}

static X509* get_or_create_cert(const char *hostname) {
    if (!hostname || strlen(hostname) == 0) return default_x509;

    pthread_mutex_lock(&cert_cache_mutex);
    CertCacheNode *curr = cert_cache_head;
    while (curr) {
        if (strcmp(curr->hostname, hostname) == 0) {
            pthread_mutex_unlock(&cert_cache_mutex);
            return curr->cert;
        }
        curr = curr->next;
    }

    // not found, generate new
    X509 *new_cert = generate_cert_for_host(hostname);
    if (new_cert) {
        CertCacheNode *new_node = (CertCacheNode*)malloc(sizeof(CertCacheNode));
        new_node->hostname = strdup(hostname);
        new_node->cert = new_cert; // we keep a reference
        new_node->next = cert_cache_head;
        cert_cache_head = new_node;
    } else {
        new_cert = default_x509;
    }
    pthread_mutex_unlock(&cert_cache_mutex);

    return new_cert;
}

// SNI callback to switch certificate
static int sni_callback(SSL *s, int *al, void *arg) {
    const char *hostname = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
    if (hostname) {
        LOG_INFO("SNI callback triggered for: %s", (char*)hostname);
        X509 *cert = get_or_create_cert(hostname);
        if (cert) {
            SSL_use_certificate(s, cert);
        }
    }
    return SSL_TLSEXT_ERR_OK;
}

void InitOpenSSL() {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
#endif

    server_ctx = SSL_CTX_new(TLS_server_method());
    if (!server_ctx) {
        LOG_ERROR("Unable to create SSL context");
        return;
    }

    client_ctx_out = SSL_CTX_new(TLS_client_method());
    if (g_config.tls_verify_peer) {
        SSL_CTX_set_verify(client_ctx_out, SSL_VERIFY_PEER, NULL);
        if (g_config.ca_bundle_file[0] != '\0') {
            SSL_CTX_load_verify_locations(client_ctx_out, g_config.ca_bundle_file, NULL);
        } else {
            SSL_CTX_set_default_verify_paths(client_ctx_out);
        }
    }

    // Generate loop-global key pair once
    LOG_INFO("Generating global RSA key for proxy certificates (this may take a moment)...");
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
    EVP_PKEY_keygen(pctx, &global_pkey);
    EVP_PKEY_CTX_free(pctx);

    default_x509 = generate_cert_for_host("localhost");

    SSL_CTX_use_certificate(server_ctx, default_x509);
    SSL_CTX_use_PrivateKey(server_ctx, global_pkey);
    
    // SNI callback assignment
    SSL_CTX_set_tlsext_servername_callback(server_ctx, sni_callback);
}

void CleanupOpenSSL() {
    if (server_ctx) {
        SSL_CTX_free(server_ctx);
    }
    if (client_ctx_out) {
        SSL_CTX_free(client_ctx_out);
    }
    if (default_x509) {
        X509_free(default_x509);
    }
    if (global_pkey) {
        EVP_PKEY_free(global_pkey);
    }

    pthread_mutex_lock(&cert_cache_mutex);
    CertCacheNode *curr = cert_cache_head;
    while (curr) {
        CertCacheNode *next = curr->next;
        free(curr->hostname);
        X509_free(curr->cert);
        free(curr);
        curr = next;
    }
    cert_cache_head = NULL;
    pthread_mutex_unlock(&cert_cache_mutex);

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_cleanup();
#endif
}

int HandleConnect(int client_socket, const char *host, int port) {
    int end_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (end_socket < 0) return -1;

    struct hostent *host_info = gethostbyname(host);
    if (!host_info) {
        LOG_ERROR("HandleConnect: Failed to resolve %s", host);
        close(end_socket);
        return -1;
    }

    struct sockaddr_in end_addr;
    memset(&end_addr, 0, sizeof(end_addr));
    end_addr.sin_family = AF_INET;
    end_addr.sin_port = htons(port);
    memcpy(&end_addr.sin_addr, host_info->h_addr_list[0], host_info->h_length);

    if (connect(end_socket, (struct sockaddr *)&end_addr, sizeof(end_addr)) < 0) {
        LOG_ERROR("HandleConnect: connect failed");
        close(end_socket);
        return -1;
    }

    const char *conn_est = "HTTP/1.1 200 Connection Established\r\n\r\n";
    send(client_socket, conn_est, strlen(conn_est), 0);

    SSL *client_ssl = SSL_new(server_ctx);
    SSL_set_fd(client_ssl, client_socket);

    // Provide default cert based on CONNECT host (fallback if SNI not provided)
    X509 *host_cert = get_or_create_cert(host);
    if (host_cert) {
        SSL_use_certificate(client_ssl, host_cert);
    }

    SSL *server_ssl = SSL_new(client_ctx_out);
    SSL_set_fd(server_ssl, end_socket);

    if (SSL_accept(client_ssl) <= 0) {
        LOG_ERROR("SSL_accept failed");
        SSL_free(client_ssl);
        SSL_free(server_ssl);
        close(end_socket);
        return -1;
    }

    SSL_set_tlsext_host_name(server_ssl, host);
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    if (g_config.tls_verify_peer) {
        SSL_set1_host(server_ssl, host);
    }
#endif
    if (SSL_connect(server_ssl) <= 0) {
        LOG_ERROR("SSL_connect failed to target server");
        SSL_free(client_ssl);
        SSL_free(server_ssl);
        close(end_socket);
        return -1;
    }

    set_nonblock(client_socket);
    set_nonblock(end_socket);

    struct pollfd fds[2];
    fds[0].fd = client_socket;
    fds[0].events = POLLIN;
    fds[1].fd = end_socket;
    fds[1].events = POLLIN;

    char buf[16384];
    int active = 1;

    while (active) {
        int ret = poll(fds, 2, 5000);
        if (ret < 0) break;
        
        if ((fds[0].revents & POLLIN) || SSL_pending(client_ssl)) {
            while (1) {
                int bytes = SSL_read(client_ssl, buf, sizeof(buf));
                if (bytes > 0) {
                    int wbytes = SSL_write(server_ssl, buf, bytes);
                    if (wbytes > 0) metrics_add_bytes(wbytes);
                    if (wbytes <= 0) { active = 0; break; }
                } else {
                    int err = SSL_get_error(client_ssl, bytes);
                    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                        active = 0;
                    }
                    break;
                }
            }
        }

        if ((fds[1].revents & POLLIN) || SSL_pending(server_ssl)) {
            while (1) {
                int bytes = SSL_read(server_ssl, buf, sizeof(buf));
                if (bytes > 0) {
                    int wbytes = SSL_write(client_ssl, buf, bytes);
                    if (wbytes > 0) metrics_add_bytes(wbytes);
                    if (wbytes <= 0) { active = 0; break; }
                } else {
                    int err = SSL_get_error(server_ssl, bytes);
                    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                        active = 0;
                    }
                    break;
                }
            }
        }
        
        if ((fds[0].revents & (POLLERR | POLLHUP)) || (fds[1].revents & (POLLERR | POLLHUP))) {
            break;
        }
    }

    SSL_shutdown(client_ssl);
    SSL_shutdown(server_ssl);

    SSL_free(client_ssl);
    SSL_free(server_ssl);
    close(end_socket);
    return 0;
}
