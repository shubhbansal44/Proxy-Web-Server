#include "http3.h"
#include "logger.h"
#include "config.h"
#include <quiche.h>
#include <uthash.h>
#include <sys/socket.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <string.h>

#define LOCAL_CONN_ID_LEN 16
#define MAX_DATAGRAM_SIZE 1350
#define MAX_TOKEN_LEN (sizeof("quiche") - 1 + sizeof(struct sockaddr_storage) + QUICHE_MAX_CONN_ID_LEN)

struct conn_io {
    int sock;
    uint8_t cid[LOCAL_CONN_ID_LEN];
    quiche_conn *conn;
    quiche_h3_conn *http3;

    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;

    UT_hash_handle hh;
};

static quiche_config *g_quic_config = NULL;
static quiche_h3_config *g_http3_config = NULL;
static struct conn_io *g_conns = NULL;
static int g_udp_socket = -1;

extern ProxyConfig g_config;

static void mint_token(const uint8_t *dcid, size_t dcid_len,
                       struct sockaddr_storage *addr, socklen_t addr_len,
                       uint8_t *token, size_t *token_len) {
    memcpy(token, "quiche", sizeof("quiche") - 1);
    memcpy(token + sizeof("quiche") - 1, addr, addr_len);
    memcpy(token + sizeof("quiche") - 1 + addr_len, dcid, dcid_len);
    *token_len = sizeof("quiche") - 1 + addr_len + dcid_len;
}

static bool validate_token(const uint8_t *token, size_t token_len,
                           struct sockaddr_storage *addr, socklen_t addr_len,
                           uint8_t *odcid, size_t *odcid_len) {
    if ((token_len < sizeof("quiche") - 1) ||
         memcmp(token, "quiche", sizeof("quiche") - 1) != 0) {
        return false;
    }
    
    token += sizeof("quiche") - 1;
    token_len -= sizeof("quiche") - 1;
    if ((token_len < addr_len) || memcmp(token, addr, addr_len) != 0) {
        return false;
    }

    token += addr_len;
    token_len -= addr_len;

    if (*odcid_len < token_len) {
        return false;
    }

    memcpy(odcid, token, token_len);
    *odcid_len = token_len;

    return true;
}

static uint8_t *gen_cid(uint8_t *cid, size_t cid_len) {
    // Basic random implementation (use secure random in production)
    for (size_t i = 0; i < cid_len; i++) {
        cid[i] = rand() % 256;
    }
    return cid;
}

static void flush_egress(struct conn_io *conn_io) {
    static uint8_t out[MAX_DATAGRAM_SIZE];
    quiche_send_info send_info;
    while (1) {
        ssize_t written = quiche_conn_send(conn_io->conn, out, sizeof(out), &send_info);

        if (written == QUICHE_ERR_DONE) {
            break; // No more data
        }

        if (written < 0) {
            LOG_ERROR("HTTP/3 error sending packet: %zd", written);
            // Handle error, cleanup connection, etc.
            return;
        }

        ssize_t sent = sendto(conn_io->sock, out, written, 0,
                              (struct sockaddr *) &send_info.to,
                              send_info.to_len);
        if (sent != written) {
            LOG_ERROR("HTTP/3 failed to sendto: %s", strerror(errno));
            return;
        }
    }
}

void *http3_worker_thread(void *arg) {
    LOG_INFO("HTTP/3 QUIC worker thread started");
    
    uint8_t buf[65535];
    uint8_t out[MAX_DATAGRAM_SIZE];
    
    struct pollfd fds[1];
    fds[0].fd = g_udp_socket;
    fds[0].events = POLLIN;

    while (1) {
        int timeout_ms = -1;
        // Ideally should iterate all conns to find min timeout, but hardcode 500 for demo
        struct conn_io *conn_io, *tmp;
        uint64_t min_timeout = UINT64_MAX;
        
        HASH_ITER(hh, g_conns, conn_io, tmp) {
            quiche_conn_on_timeout(conn_io->conn);
            flush_egress(conn_io);
            if (quiche_conn_is_closed(conn_io->conn)) {
                HASH_DEL(g_conns, conn_io);
                quiche_conn_free(conn_io->conn);
                if (conn_io->http3) quiche_h3_conn_free(conn_io->http3);
                free(conn_io);
                continue;
            }
            uint64_t conn_timeout = quiche_conn_timeout_as_nanos(conn_io->conn);
            if (conn_timeout < min_timeout) min_timeout = conn_timeout;
        }
        
        if (min_timeout != UINT64_MAX) {
            timeout_ms = min_timeout / 1000000;
        }

        int poll_res = poll(fds, 1, (timeout_ms > 0 && timeout_ms < 500) ? timeout_ms : 500);

        if (poll_res < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("HTTP/3 Worker Poll failed");
            break;
        }
        
        if (poll_res > 0 && (fds[0].revents & POLLIN)) {
            struct sockaddr_storage peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);

            ssize_t read = recvfrom(g_udp_socket, buf, sizeof(buf), 0,
                                    (struct sockaddr *) &peer_addr,
                                    &peer_addr_len);

            if (read < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
                LOG_ERROR("HTTP/3 Worker UDP read failed: %s", strerror(errno));
                continue;
            }

            uint8_t type;
            uint32_t version;
            uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
            size_t scid_len = sizeof(scid);
            uint8_t dcid[QUICHE_MAX_CONN_ID_LEN];
            size_t dcid_len = sizeof(dcid);
            uint8_t odcid[QUICHE_MAX_CONN_ID_LEN];
            size_t odcid_len = sizeof(odcid);
            uint8_t token[MAX_TOKEN_LEN];
            size_t token_len = sizeof(token);

            int rc = quiche_header_info(buf, read, LOCAL_CONN_ID_LEN, &version,
                                        &type, scid, &scid_len, dcid, &dcid_len,
                                        token, &token_len);
            if (rc < 0) {
                LOG_WARN("HTTP/3 failed to parse header: %d", rc);
                continue;
            }

            HASH_FIND(hh, g_conns, dcid, dcid_len, conn_io);

            if (conn_io == NULL) {
                if (!quiche_version_is_supported(version)) {
                    ssize_t written = quiche_negotiate_version(scid, scid_len, dcid, dcid_len, out, sizeof(out));
                    if (written > 0) {
                        sendto(g_udp_socket, out, written, 0, (struct sockaddr *) &peer_addr, peer_addr_len);
                    }
                    continue;
                }

                if (token_len == 0) {
                    mint_token(dcid, dcid_len, &peer_addr, peer_addr_len, token, &token_len);
                    uint8_t new_cid[LOCAL_CONN_ID_LEN];
                    if (gen_cid(new_cid, LOCAL_CONN_ID_LEN) != NULL) {
                        ssize_t written = quiche_retry(scid, scid_len, dcid, dcid_len,
                                                       new_cid, LOCAL_CONN_ID_LEN,
                                                       token, token_len,
                                                       version, out, sizeof(out));

                        if (written > 0) {
                            sendto(g_udp_socket, out, written, 0, (struct sockaddr *) &peer_addr, peer_addr_len);
                        }
                    }
                    continue;
                }

                if (!validate_token(token, token_len, &peer_addr, peer_addr_len, odcid, &odcid_len)) continue;

                quiche_conn *conn = quiche_conn_new_with_tls(scid, scid_len, odcid, odcid_len,
                                                             (struct sockaddr *)&peer_addr, peer_addr_len,
                                                             (struct sockaddr *)&peer_addr, peer_addr_len, 
                                                             g_quic_config, NULL, false); // Removed passing SSL_CTX because quiche manages boringSSL internally

                if (conn == NULL) {
                    LOG_ERROR("HTTP/3 quiche_conn_new_with_tls failed");
                    continue;
                }

                conn_io = (struct conn_io *)calloc(1, sizeof(*conn_io));
                conn_io->sock = g_udp_socket;
                memcpy(conn_io->cid, scid, scid_len);
                conn_io->conn = conn;
                memcpy(&conn_io->peer_addr, &peer_addr, peer_addr_len);
                conn_io->peer_addr_len = peer_addr_len;

                HASH_ADD(hh, g_conns, cid, scid_len, conn_io);
            }

            quiche_recv_info recv_info = {
                (struct sockaddr *)&peer_addr,
                peer_addr_len,
                (struct sockaddr *)&peer_addr, // local and peer addr are normally matched to from/to
                peer_addr_len,
            };

            ssize_t done = quiche_conn_recv(conn_io->conn, buf, read, &recv_info);
            
            if (done < 0) {
                LOG_ERROR("HTTP/3 quiche_conn_recv failed: %zd", done);
                continue;
            }
            
            if (quiche_conn_is_established(conn_io->conn)) {
                if (conn_io->http3 == NULL) {
                    conn_io->http3 = quiche_h3_conn_new_with_transport(conn_io->conn, g_http3_config);
                    if (!conn_io->http3) {
                        LOG_ERROR("HTTP/3 failed to create H3 connection");
                    }
                }
                
                // Process HTTP/3 streams 
                if (conn_io->http3 != NULL) {
                    // This is where HTTP/3 flow goes: poll events and read/write stream requests. 
                    // Basic placeholder to process H3 and do echoes (full proxy not fully implemented here yet)
                    quiche_h3_event *ev;
                    while (1) {
                        int64_t s = quiche_h3_conn_poll(conn_io->http3, conn_io->conn, &ev);
                        if (s < 0) break;

                        switch (quiche_h3_event_type(ev)) {
                            case QUICHE_H3_EVENT_HEADERS: {
                                int rc = quiche_h3_event_for_each_header(ev, [](uint8_t *name, size_t name_len,
                                                                                uint8_t *value, size_t value_len,
                                                                                void *argp) -> int {
                                    LOG_INFO("HTTP/3 Header: %.*s: %.*s", (int)name_len, name, (int)value_len, value);
                                    return 0;
                                }, NULL);
                                if (rc != 0) {
                                    LOG_ERROR("Failed to process headers");
                                }

                                quiche_h3_header headers[] = {
                                    {
                                        .name = (const uint8_t *)":status",
                                        .name_len = sizeof(":status") - 1,
                                        .value = (const uint8_t *)"200",
                                        .value_len = sizeof("200") - 1,
                                    },
                                    {
                                        .name = (const uint8_t *)"server",
                                        .name_len = sizeof("server") - 1,
                                        .value = (const uint8_t *)"quiche",
                                        .value_len = sizeof("quiche") - 1,
                                    },
                                    {
                                        .name = (const uint8_t *)"content-length",
                                        .name_len = sizeof("content-length") - 1,
                                        .value = (const uint8_t *)"14",
                                        .value_len = sizeof("14") - 1,
                                    },
                                };

                                quiche_h3_send_response(conn_io->http3, conn_io->conn, s, headers, 3, false);
                                quiche_h3_send_body(conn_io->http3, conn_io->conn, s, (const uint8_t *)"HTTP/3 Echo!\r\n", 14, true);
                                break;
                            }
                            case QUICHE_H3_EVENT_DATA: {
                                // Drain data if any
                                break;
                            }
                            case QUICHE_H3_EVENT_FINISHED:
                                break;
                            case QUICHE_H3_EVENT_RESET:
                                break;
                            case QUICHE_H3_EVENT_PRIORITY_UPDATE:
                                break;
                            case QUICHE_H3_EVENT_GOAWAY:
                                break;
                        }
                        quiche_h3_event_free(ev);
                    }
                }
            }

            flush_egress(conn_io);
        }
    }
    
    return NULL;
}

void start_http3_server(int port) {
    LOG_INFO("Configuring HTTP/3 QUIC Context...");

    g_quic_config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!g_quic_config) {
        LOG_ERROR("Failed to create quiche config");
        return;
    }
    
    quiche_config_set_application_protos(g_quic_config, (uint8_t *)"\x02h3", 3);
    
    quiche_config_set_max_idle_timeout(g_quic_config, 5000);
    quiche_config_set_max_recv_udp_payload_size(g_quic_config, MAX_DATAGRAM_SIZE);
    quiche_config_set_max_send_udp_payload_size(g_quic_config, MAX_DATAGRAM_SIZE);
    quiche_config_set_initial_max_data(g_quic_config, 10000000);
    quiche_config_set_initial_max_stream_data_bidi_local(g_quic_config, 1000000);
    quiche_config_set_initial_max_stream_data_bidi_remote(g_quic_config, 1000000);
    quiche_config_set_initial_max_stream_data_uni(g_quic_config, 1000000);
    quiche_config_set_initial_max_streams_bidi(g_quic_config, 100);
    quiche_config_set_initial_max_streams_uni(g_quic_config, 100);
    quiche_config_set_disable_active_migration(g_quic_config, true);
    
    // Set a dummy certificate since we don't have a real PEM configuration in the struct, or if we do? 
    quiche_config_load_cert_chain_from_pem_file(g_quic_config, "server.crt"); // assuming it exists
    quiche_config_load_priv_key_from_pem_file(g_quic_config, "server.key");
    
    g_http3_config = quiche_h3_config_new();
    if (!g_http3_config) {
        LOG_ERROR("Failed to create quiche H3 config");
        return;
    }

    g_udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_socket < 0) {
        LOG_ERROR("HTTP/3 failed to create UDP socket");
        return;
    }

    int reuse = 1;
    setsockopt(g_udp_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Non blocking socket
    int flags = fcntl(g_udp_socket, F_GETFL, 0);
    fcntl(g_udp_socket, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_udp_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("HTTP/3 failed to bind UDP on port %d", port);
        return;
    }

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, http3_worker_thread, NULL);
    pthread_detach(thread_id);

    LOG_INFO("HTTP/3 QUIC worker initialized and listening on UDP port %d", port);
}
