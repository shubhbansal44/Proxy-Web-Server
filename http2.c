#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>

#include "http2.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <nghttp2/nghttp2.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>  /* For snprintf */
#include <pthread.h>
#include "logger.h"
#include "metrics.h"

/* custom_memmem is not standard on all systems, provide a simple implementation */
static void *custom_memmem(const void *haystack, size_t haystacklen,
                    const void *needle, size_t needlelen)
{
    const char *begin;
    const char *const last_possible =
        (const char *)haystack + haystacklen - needlelen;

    if (needlelen == 0)
        /* The first occurrence of the empty string is deemed to occur at
           the beginning of the string.  */
        return (void *)haystack;

    /* Sanity check, otherwise the loop might search through the whole memory. */
    if (haystacklen < needlelen)
        return NULL;

    for (begin = (const char *)haystack; begin <= last_possible; ++begin)
        if ((begin[0] == ((const char *)needle)[0]) &&
            !memcmp((const void *)begin, needle, needlelen))
            return (void *)begin;

    return NULL;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t offset;
} body_payload;

/* Callback for nghttp2 to read data from our buffer */
static ssize_t data_read_callback(nghttp2_session *session,
                                  int32_t stream_id,
                                  uint8_t *buf,
                                  size_t length,
                                  uint32_t *data_flags,
                                  nghttp2_data_source *source,
                                  void *user_data) {
    (void)session;
    (void)stream_id;
    (void)user_data;
    body_payload *payload = (body_payload *)source->ptr;

    if (payload->offset >= payload->len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t remaining = payload->len - payload->offset;
    size_t copy_len = (length < remaining) ? length : remaining;
    memcpy(buf, payload->data + payload->offset, copy_len);
    payload->offset += copy_len;

    return (ssize_t)copy_len;
}

/* HTTP/2 Upstream Connection State Machine */
enum http2_upstream_state {
    HTTP2_UPSTREAM_STATE_INIT,
    HTTP2_UPSTREAM_STATE_CONNECTING,
    HTTP2_UPSTREAM_STATE_SENDING_REQUEST,
    HTTP2_UPSTREAM_STATE_READING_RESPONSE,
    HTTP2_UPSTREAM_STATE_DONE
};

/* Structure to hold per-stream HTTP/2 state */
typedef struct http2_stream {
    int32_t stream_id;
    char *method;
    char *path;
    char *authority;
    int have_method;
    int have_path;
    int have_authority;
    char *request_buf;
    size_t request_buf_len;
    size_t request_buf_size;
    char *headers;
    size_t headers_len;
    size_t headers_size;
    char *body;
    size_t body_len;
    size_t body_size;
    char *response_buf;
    size_t response_buf_len;
    size_t response_buf_size;
    int request_complete;
    int response_complete;
    /* Upstream connection state */
    int upstream_fd;
    SSL *upstream_ssl;
    enum http2_upstream_state upstream_state;
    char *upstream_request;
    size_t upstream_request_len;
    size_t upstream_request_sent;
    int upstream_headers_complete;
    int upstream_status_code;
    char *upstream_response_buf;
    size_t upstream_response_len;
    size_t upstream_response_size;
} http2_stream;

/* Structure to hold per-connection HTTP/2 state */
typedef struct {
    nghttp2_session *session;
    int client_fd;
    int server_fd;
    SSL *client_ssl;
    SSL *server_ssl;
    char target_host[256];
    int target_port;
    http2_stream **streams;
    size_t num_streams;
    size_t max_streams;
    pthread_mutex_t lock;
    size_t active_streams_count;   // number of streams that are request_complete and not response_complete
} http2_conn;

/* Forward declarations */
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);
static ssize_t recv_callback(nghttp2_session *session, uint8_t *data, size_t length, int flags, void *user_data);
static int on_begin_headers_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static http2_stream *http2_get_stream(http2_conn *conn, int32_t stream_id);
static void http2_stream_free(http2_stream *stream);
static int http2_process_stream(http2_conn *conn, http2_stream *stream);

/* Send data over SSL to the client */
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data) {
    http2_conn *conn = (http2_conn *)user_data;
    ssize_t sent = SSL_write(conn->client_ssl, data, length);
    if (sent <= 0) {
        int err = SSL_get_error(conn->client_ssl, sent);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    metrics_add_http2_bytes(sent);
    return sent;
}

/* Receive data from SSL from the client */
static ssize_t recv_callback(nghttp2_session *session, uint8_t *data, size_t length, int flags, void *user_data) {
    http2_conn *conn = (http2_conn *)user_data;
    ssize_t readlen = SSL_read(conn->client_ssl, data, length);
    if (readlen <= 0) {
        int err = SSL_get_error(conn->client_ssl, readlen);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        if (readlen == 0) {
            return NGHTTP2_ERR_EOF;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    metrics_add_http2_bytes(readlen);
    return readlen;
}

/* Get or create a stream for the given stream ID */
static http2_stream *http2_get_stream(http2_conn *conn, int32_t stream_id) {
    if (stream_id < 0) {
        return NULL;
    }

    pthread_mutex_lock(&conn->lock);
    /* If we need to grow the streams array */
    if ((size_t)stream_id >= conn->max_streams) {
        size_t old_max = conn->max_streams;
        size_t new_max = stream_id + 1;
        http2_stream **new_streams = (http2_stream **)realloc(conn->streams, new_max * sizeof(http2_stream *));
        if (!new_streams) {
            pthread_mutex_unlock(&conn->lock);
            return NULL;
        }
        /* Initialize new elements to NULL */
        for (size_t i = old_max; i < new_max; i++) {
            new_streams[i] = NULL;
        }
        conn->streams = new_streams;
        conn->max_streams = new_max;
    }

/* If stream doesn't exist, create it */
     if (!conn->streams[stream_id]) {
         conn->streams[stream_id] = (http2_stream *)calloc(1, sizeof(http2_stream));
         if (!conn->streams[stream_id]) {
             pthread_mutex_unlock(&conn->lock);
             return NULL;
         }
         conn->streams[stream_id]->stream_id = stream_id;
         /* Initialize upstream connection state */
         conn->streams[stream_id]->upstream_fd = -1;
         conn->streams[stream_id]->upstream_ssl = NULL;
         conn->streams[stream_id]->upstream_state = HTTP2_UPSTREAM_STATE_INIT;
         conn->streams[stream_id]->upstream_request = NULL;
         conn->streams[stream_id]->upstream_request_len = 0;
         conn->streams[stream_id]->upstream_request_sent = 0;
         conn->streams[stream_id]->upstream_headers_complete = 0;
         conn->streams[stream_id]->upstream_status_code = 0;
         /* Initialize upstream response buffer */
         conn->streams[stream_id]->upstream_response_buf = NULL;
         conn->streams[stream_id]->upstream_response_len = 0;
         conn->streams[stream_id]->upstream_response_size = 0;
         conn->num_streams++;
     }
    pthread_mutex_unlock(&conn->lock);
    return conn->streams[stream_id];
}

/* Free a stream */
static void http2_stream_free(http2_stream *stream) {
    if (!stream) return;
    /* Clean up upstream connection resources */
    if (stream->upstream_ssl) {
        SSL_free(stream->upstream_ssl);
        stream->upstream_ssl = NULL;
    }
    if (stream->upstream_fd != -1) {
        close(stream->upstream_fd);
        stream->upstream_fd = -1;
    }
    free(stream->upstream_request);
    free(stream->upstream_response_buf);
    /* Clean up existing resources */
    free(stream->method);
    free(stream->path);
    free(stream->authority);
    free(stream->request_buf);
    free(stream->headers);
    free(stream->body);
    free(stream->response_buf);
    free(stream);
}

/* Callback when header block starts */
static int on_begin_headers_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
    http2_conn *conn = (http2_conn *)user_data;
    int32_t stream_id = frame->hd.stream_id;
    http2_stream *stream = http2_get_stream(conn, stream_id);
    if (!stream) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    /* Reset request buffer for new request */
    if (stream->request_buf) {
        free(stream->request_buf);
    }
    stream->request_buf = NULL;
    stream->request_buf_len = 0;
    stream->request_buf_size = 0;
    stream->request_complete = 0;
    /* Reset pseudo-header fields */
    if (stream->method) { free(stream->method); stream->method = NULL; }
    if (stream->path) { free(stream->path); stream->path = NULL; }
    if (stream->authority) { free(stream->authority); stream->authority = NULL; }
    stream->have_method = 0;
    stream->have_path = 0;
    stream->have_authority = 0;
    /* Reset body */
    if (stream->body) {
        free(stream->body);
    }
    stream->body = NULL;
    stream->body_len = 0;
    stream->body_size = 0;
    /* Reset headers */
    if (stream->headers) {
        free(stream->headers);
    }
    stream->headers = NULL;
    stream->headers_len = 0;
    stream->headers_size = 0;
    /* Reset response */
    if (stream->response_buf) {
        free(stream->response_buf);
    }
    stream->response_buf = NULL;
    stream->response_buf_len = 0;
    stream->response_buf_size = 0;
    stream->response_complete = 0;
    return 0;
}

/* Callback for each header */
static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data) {
    http2_conn *conn = (http2_conn *)user_data;
    int32_t stream_id = frame->hd.stream_id;
    http2_stream *stream = http2_get_stream(conn, stream_id);
    if (!stream) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    /* Check if this is a pseudo-header (starts with ':') */
    if (namelen > 0 && name[0] == ':') {
        /* We only care about a few pseudo-headers for now */
        if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
            if (stream->method) free(stream->method);
            stream->method = (char *)malloc(valuelen + 1);
            memcpy(stream->method, value, valuelen);
            stream->method[valuelen] = '\0';
            stream->have_method = 1;
        } else if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
            if (stream->path) free(stream->path);
            stream->path = (char *)malloc(valuelen + 1);
            memcpy(stream->path, value, valuelen);
            stream->path[valuelen] = '\0';
            stream->have_path = 1;
        } else if (namelen == 10 && memcmp(name, ":authority", 10) == 0) {
            if (stream->authority) free(stream->authority);
            stream->authority = (char *)malloc(valuelen + 1);
            memcpy(stream->authority, value, valuelen);
            stream->authority[valuelen] = '\0';
            stream->have_authority = 1;
        }
        /* We do not add pseudo-headers to the request_buf */
    } else {
        /* Regular header: append to request_buf in the format "name: value\r\n" */
        size_t needed = namelen + 2 + valuelen + 2; /* name: value\r\n */
        if (stream->request_buf_size < stream->request_buf_len + needed) {
            stream->request_buf_size = stream->request_buf_len + needed + 256;
            stream->request_buf = (char *)realloc(stream->request_buf, stream->request_buf_size);
            if (!stream->request_buf) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
        }
        memcpy(stream->request_buf + stream->request_buf_len, name, namelen);
        stream->request_buf_len += namelen;
        stream->request_buf[stream->request_buf_len++] = ':';
        stream->request_buf[stream->request_buf_len++] = ' ';
        memcpy(stream->request_buf + stream->request_buf_len, value, valuelen);
        stream->request_buf_len += valuelen;
        stream->request_buf[stream->request_buf_len++] = '\r';
        stream->request_buf[stream->request_buf_len++] = '\n';
    }
    return 0;
}

/* Callback when a frame is received */
static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
    http2_conn *conn = (http2_conn *)user_data;
    int32_t stream_id = frame->hd.stream_id;
    http2_stream *stream = http2_get_stream(conn, stream_id);
    if (!stream) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if (frame->hd.type == NGHTTP2_DATA) {
        /* We have data - append to request body */
        size_t needed = stream->body_len + frame->hd.length;
        if (stream->body_size < stream->body_len + needed) {
            stream->body_size = stream->body_len + needed + 256;
            stream->body = (char *)realloc(stream->body, stream->body_size);
            if (!stream->body) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
        }
        /* Note: We don't have direct access to payload here; nghttp2 provides it via on_data_chunk_recv_callback.
         * For simplicity, we'll assume the data is available in the session's memory buffer and we'll copy it later.
         * However, the nghttp2 API does not give us direct access to the payload in this callback.
         * We'll need to use on_data_chunk_recv_callback instead. Let's implement that.
         * For now, we'll just note that we have data and set a flag.
         * We'll actually copy the data in on_data_chunk_recv_callback.
         */
        /* We'll set request_complete when we see END_STREAM */
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            stream->request_complete = 1;
            metrics_increment_http2_requests();
            pthread_mutex_lock(&conn->lock);
            conn->active_streams_count++;
            pthread_mutex_unlock(&conn->lock);
        }
    } else if (frame->hd.type == NGHTTP2_HEADERS) {
        /* Check if this is a response (from server) - but we are acting as server to client */
        /* Actually, we are receiving headers from client, so this is a request */
        if (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
            /* If no data follows, then request is complete */
            if (frame->hd.length == 0 || (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
                stream->request_complete = 1;
                metrics_increment_http2_requests();
                pthread_mutex_lock(&conn->lock);
                conn->active_streams_count++;
                pthread_mutex_unlock(&conn->lock);
            }
        }
    } else if (frame->hd.type == NGHTTP2_GOAWAY) {
        /* Handle GOAWAY frame - initiate graceful shutdown */
        LOG_INFO("Received GOAWAY frame, initiating graceful shutdown");
        /* We could set a flag to stop accepting new streams */
    }
    return 0;
}

/* Callback for data chunks */
static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data) {
    http2_conn *conn = (http2_conn *)user_data;
    http2_stream *stream = http2_get_stream(conn, stream_id);
    if (!stream) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    /* Append data to body */
    size_t needed = stream->body_len + len;
    if (stream->body_size < stream->body_len + needed) {
        stream->body_size = stream->body_len + needed + 256;
        stream->body = (char *)realloc(stream->body, stream->body_size);
        if (!stream->body) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }
    memcpy(stream->body + stream->body_len, data, len);
    stream->body_len += len;
    if (flags & NGHTTP2_DATA_FLAG_EOF) {
        /* This is the last data chunk */
        stream->request_complete = 1;
        metrics_increment_http2_requests();
        /* Increment active streams count: request received, response not yet sent */
        pthread_mutex_lock(&conn->lock);
        conn->active_streams_count++;
        pthread_mutex_unlock(&conn->lock);
    }
    return 0;
}

/* Settings callback - currently just acknowledge */
int on_settings_callback(nghttp2_session *session, const nghttp2_settings_entry *iv, size_t niv, void *user_data) {
    (void)user_data;
    (void)session;
    for (size_t i = 0; i < niv; ++i) {
        if (iv[i].settings_id == NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS) {
            LOG_DEBUG("Client updated SETTINGS_MAX_CONCURRENT_STREAMS to %u", iv[i].value);
        } else if (iv[i].settings_id == NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE) {
            LOG_DEBUG("Client updated SETTINGS_INITIAL_WINDOW_SIZE to %u", iv[i].value);
        }
    }
    /* Acknowledge receipt of settings */
    return 0;
}

int on_priority_callback(nghttp2_session *session, int32_t stream_id, int32_t parent_stream_id, int32_t weight, int exclusive, void *user_data) {
    (void)user_data;
    (void)session;
    (void)stream_id;
    (void)parent_stream_id;
    (void)weight;
    (void)exclusive;
    /* We could implement priority handling here */
    return 0;
}

int on_ping_callback(nghttp2_session *session, const uint8_t *opaque_data, void *user_data) {
    (void)user_data;
    /* Respond to ping */
    nghttp2_submit_ping(session, NGHTTP2_FLAG_NONE, opaque_data);
    return 0;
}

static void http2_conn_init(http2_conn *conn, int client_fd, int server_fd, SSL *client_ssl, SSL *server_ssl, const char *target_host, int target_port) {
    conn->client_fd = client_fd;
    conn->server_fd = server_fd;
    conn->client_ssl = client_ssl;
    conn->server_ssl = server_ssl;
    strncpy(conn->target_host, target_host, sizeof(conn->target_host)-1);
    conn->target_host[sizeof(conn->target_host)-1] = '\0';
    conn->target_port = target_port;
    conn->session = NULL;
    conn->streams = NULL;
    conn->num_streams = 0;
    conn->max_streams = 0;
    pthread_mutex_init(&conn->lock, NULL);
    conn->active_streams_count = 0;
}

static void http2_conn_free(http2_conn *conn) {
    if (conn->session) {
        nghttp2_session_del(conn->session);
    }
    if (conn->streams) {
        for (size_t i = 0; i < conn->max_streams; i++) {
            if (conn->streams[i]) {
                http2_stream_free(conn->streams[i]);
            }
        }
        free(conn->streams);
    }
    pthread_mutex_destroy(&conn->lock);
}

/* Initialize HTTP/2 session */
int http2_init_context(http2_conn **conn_ptr, int client_fd, int server_fd, SSL *client_ssl, SSL *server_ssl, const char *target_host, int target_port) {
    http2_conn *conn = (http2_conn *)malloc(sizeof(http2_conn));
    if (!conn) {
        return -1;
    }
    http2_conn_init(conn, client_fd, server_fd, client_ssl, server_ssl, target_host, target_port);
    
nghttp2_session_callbacks *callbacks;
     nghttp2_session_callbacks_new(&callbacks);
     nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
     nghttp2_session_callbacks_set_recv_callback(callbacks, recv_callback);
     nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
     nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
     nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
            nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
     nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    // Not directly mapped but nghttp2 provides these equivalent setters:
     /* nghttp2_session_callbacks_set_on_settings_callback(callbacks, on_settings_callback);
     nghttp2_session_callbacks_set_on_priority_callback(callbacks, on_priority_callback);
     nghttp2_session_callbacks_set_on_ping_callback(callbacks, on_ping_callback); */
    
    int ret = nghttp2_session_server_new(&conn->session, callbacks, conn);
    nghttp2_session_callbacks_del(callbacks);
    if (ret != 0) {
        LOG_ERROR("Failed to create nghttp2 session: %s", nghttp2_strerror(ret));
        http2_conn_free(conn);
        free(conn);
        return -1;
    }

    nghttp2_settings_entry iv[2] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535 * 4}
    };
    
    ret = nghttp2_submit_settings(conn->session, NGHTTP2_FLAG_NONE, iv, 2);
    if (ret != 0) {
        LOG_ERROR("Failed to submit settings: %s", nghttp2_strerror(ret));
        return -1;
    }
    
    /* Store the connection in the session's user data for use in callbacks */
    nghttp2_session_set_user_data(conn->session, conn);
    
    *conn_ptr = conn;
    return 0;
}

/* Process incoming data */
int http2_process_input(http2_conn *conn, const uint8_t *data, size_t len) {
    if (!conn) {
        LOG_ERROR("HTTP2 connection not initialized");
        return -1;
    }
    
    int ret = nghttp2_session_mem_recv(conn->session, data, len);
    if (ret < 0) {
        LOG_ERROR("Failed to process nghttp2 input: %s", nghttp2_strerror(ret));
        return -1;
    }
    metrics_add_http2_bytes(len);
    return 0;
}

/* Send pending data */
int http2_send_pending(http2_conn *conn) {
    if (!conn) {
        LOG_ERROR("HTTP2 connection not initialized");
        return -1;
    }
    
    const uint8_t *data;
    ssize_t datalen;
    const uint8_t *data_ptr;
    while (1) {
        datalen = nghttp2_session_mem_send(conn->session, &data_ptr);
        data = data_ptr;
        if (datalen < 0) {
            LOG_ERROR("Failed to get nghttp2 send data: %s", nghttp2_strerror(datalen));
            return -1;
        }
        if (datalen == 0) {
            break;
        }
        ssize_t sent = SSL_write(conn->client_ssl, data, datalen);
        if (sent < 0) {
            int err = SSL_get_error(conn->client_ssl, sent);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                continue;
            }
            LOG_ERROR("Failed to write nghttp2 data: %s", strerror(errno));
            return -1;
        }
        /* Note: We should call nghttp2_session_mem_send again to tell nghttp2 we sent the data */
        nghttp2_session_mem_send(conn->session, &data_ptr);
    }
    return 0;
}

/* Process a single stream (make upstream HTTP/1.1 request and send HTTP/2 response) */
static int http2_process_stream(http2_conn *conn, http2_stream *stream) {
    if (!stream->request_complete) {
        return 0; /* Not ready yet */
    }
    if (stream->response_complete) {
        return 0; /* Already processed */
    }

    switch (stream->upstream_state) {
        case HTTP2_UPSTREAM_STATE_INIT: {
            /* Initialize upstream connection */
            stream->upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (stream->upstream_fd == -1) {
                LOG_ERROR("Failed to create upstream socket: %s", strerror(errno));
                return -1;
            }
            
            /* Set socket to non-blocking */
            int flags = fcntl(stream->upstream_fd, F_GETFL, 0);
            if (flags == -1) {
                LOG_ERROR("Failed to get socket flags: %s", strerror(errno));
                close(stream->upstream_fd);
                stream->upstream_fd = -1;
                return -1;
            }
            if (fcntl(stream->upstream_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                LOG_ERROR("Failed to set socket non-blocking: %s", strerror(errno));
                close(stream->upstream_fd);
                stream->upstream_fd = -1;
                return -1;
            }
            
            /* Create SSL object for upstream connection */
            stream->upstream_ssl = SSL_new(conn->server_ssl ? SSL_get_SSL_CTX(conn->server_ssl) : NULL);
            if (!stream->upstream_ssl) {
                LOG_ERROR("Failed to create upstream SSL object");
                close(stream->upstream_fd);
                stream->upstream_fd = -1;
                return -1;
            }
            
            /* Connect to upstream server */
            struct sockaddr_in server_addr;
            memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(conn->target_port);
            
            /* Resolve target host */
            struct hostent *host = gethostbyname(conn->target_host);
            if (!host) {
                LOG_ERROR("Failed to resolve host %s: %s", conn->target_host, hstrerror(h_errno));
                SSL_free(stream->upstream_ssl);
                stream->upstream_ssl = NULL;
                close(stream->upstream_fd);
                stream->upstream_fd = -1;
                return -1;
            }
            
            memcpy(&server_addr.sin_addr, host->h_addr_list[0], host->h_length);
            
            int connect_result = connect(stream->upstream_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
            if (connect_result == -1) {
                if (errno != EINPROGRESS) {
                    LOG_ERROR("Failed to initiate upstream connection: %s", strerror(errno));
                    SSL_free(stream->upstream_ssl);
                    stream->upstream_ssl = NULL;
                    close(stream->upstream_fd);
                    stream->upstream_fd = -1;
                    return -1;
                }
                /* Connection in progress */
                stream->upstream_state = HTTP2_UPSTREAM_STATE_CONNECTING;
                return 0; /* Wait for connection to complete */
            }
            
            /* Connection established immediately */
            SSL_set_fd(stream->upstream_ssl, stream->upstream_fd);
            stream->upstream_state = HTTP2_UPSTREAM_STATE_SENDING_REQUEST;
            /* Fall through to send request */
        }
        case HTTP2_UPSTREAM_STATE_CONNECTING: {
            /* Check if connection completed */
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(stream->upstream_fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1) {
                LOG_ERROR("Failed to get socket error: %s", strerror(errno));
                return -1;
            }
            
            if (error != 0) {
                LOG_ERROR("Upstream connection failed: %s", strerror(error));
                SSL_free(stream->upstream_ssl);
                stream->upstream_ssl = NULL;
                close(stream->upstream_fd);
                stream->upstream_fd = -1;
                return -1;
            }
            
            /* Connection successful */
            SSL_set_fd(stream->upstream_ssl, stream->upstream_fd);
            stream->upstream_state = HTTP2_UPSTREAM_STATE_SENDING_REQUEST;
            return 0;
        }
            
        case HTTP2_UPSTREAM_STATE_SENDING_REQUEST: {
            /* Build HTTP/1.1 request if not already built */
            if (!stream->upstream_request) {
                /* Request line: "<method> <path> HTTP/1.1\r\n" */
                size_t request_line_len = strlen(stream->method) + 1 + strlen(stream->path) + strlen(" HTTP/1.1\r\n");
                /* Host header: "Host: <authority>\r\n" */
                size_t host_header_len = strlen("Host: ") + strlen(stream->authority) + strlen("\r\n");
                /* Headers: we already have them in stream->headers (in the format "name: value\r\n") */
                size_t headers_len = stream->headers_len;
                /* Body */
                size_t body_len = stream->body_len;
                /* Final CRLF */
                size_t total_len = request_line_len + host_header_len + headers_len + body_len + strlen("\r\n");

                stream->upstream_request = (char*)malloc(total_len + 1);
                if (!stream->upstream_request) {
                    LOG_ERROR("Failed to allocate upstream request buffer");
                    return -1;
                }
                stream->upstream_request_len = total_len;
                char *p = stream->upstream_request;

                /* Request line */
                memcpy(p, stream->method, strlen(stream->method));
                p += strlen(stream->method);
                *p++ = ' ';
                memcpy(p, stream->path, strlen(stream->path));
                p += strlen(stream->path);
                memcpy(p, " HTTP/1.1\r\n", strlen(" HTTP/1.1\r\n"));
                p += strlen(" HTTP/1.1\r\n");

                /* Host header */
                memcpy(p, "Host: ", strlen("Host: "));
                p += strlen("Host: ");
                memcpy(p, stream->authority, strlen(stream->authority));
                p += strlen(stream->authority);
                memcpy(p, "\r\n", strlen("\r\n"));
                p += strlen("\r\n");

                /* Headers */
                if (stream->headers && stream->headers_len > 0) {
                    memcpy(p, stream->headers, stream->headers_len);
                    p += stream->headers_len;
                }

                /* Empty line */
                memcpy(p, "\r\n", strlen("\r\n"));
                p += strlen("\r\n");

                /* Body */
                if (stream->body_len > 0) {
                    memcpy(p, stream->body, stream->body_len);
                    p += stream->body_len;
                }
            }
            
            /* Send request data */
            while (stream->upstream_request_sent < stream->upstream_request_len) {
                int bytes = SSL_write(stream->upstream_ssl, 
                                    stream->upstream_request + stream->upstream_request_sent,
                                    stream->upstream_request_len - stream->upstream_request_sent);
                if (bytes > 0) {
                    stream->upstream_request_sent += bytes;
                } else {
                    int err = SSL_get_error(stream->upstream_ssl, bytes);
                    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                        return 0; /* Try again later */
                    }
                    LOG_ERROR("SSL_write error to upstream: %d", err);
                    return -1;
                }
            }
            
            /* Request sent completely, move to reading response */
            stream->upstream_state = HTTP2_UPSTREAM_STATE_READING_RESPONSE;
            return 0;
        }
            
        case HTTP2_UPSTREAM_STATE_READING_RESPONSE: {
            /* Read HTTP/1.1 response from upstream server */
            char *read_buf = (char *)malloc(16384);
            if (!read_buf) {
                LOG_ERROR("Failed to allocate read buffer");
                return -1;
            }

            int bytes = SSL_read(stream->upstream_ssl, (unsigned char*)read_buf, 16384);
            if (bytes > 0) {
                /* Append to response buffer */
                if (stream->upstream_response_size < stream->upstream_response_len + bytes) {
                    stream->upstream_response_size = stream->upstream_response_len + bytes + 256;
                    stream->upstream_response_buf = (char *)realloc(stream->upstream_response_buf, stream->upstream_response_size);
                    if (!stream->upstream_response_buf) {
                        LOG_ERROR("Failed to allocate upstream response buffer");
                        free(read_buf);
                        return -1;
                    }
                }
                memcpy(stream->upstream_response_buf + stream->upstream_response_len, read_buf, bytes);
                stream->upstream_response_len += bytes;
                
                /* Try to parse headers if we haven't already */
                if (!stream->upstream_headers_complete && stream->upstream_response_len >= 4) {
                    /* Look for \r\n\r\n which marks end of headers */
                    char *pos = (char*)custom_memmem(stream->upstream_response_buf, stream->upstream_response_len, "\r\n\r\n", 4);
                    if (pos) {
                        stream->upstream_headers_complete = 1;
                        /* Parse status line (first line) */
                        char *line_end = (char*)custom_memmem(stream->upstream_response_buf, pos - stream->upstream_response_buf, "\r\n", 2);
                        if (line_end) {
                            *line_end = '\0'; /* Temporarily null-terminate */
                            /* Parse HTTP/1.1 STATUS_CODE STATUS_MSG */
                            char *http_ver = stream->upstream_response_buf;
                            char *space1 = strchr(http_ver, ' ');
                            if (space1) {
                                *space1 = '\0';
                                char *status_code_str = space1 + 1;
                                char *space2 = strchr(status_code_str, ' ');
                                if (space2) {
                                    *space2 = '\0';
                                    stream->upstream_status_code = atoi(status_code_str);

                                    *space1 = ' ';
                                    *space2 = ' ';
                                }
                            }
                        }
                        
                        /* Now we have headers complete, we can send HTTP/2 response headers */
                        nghttp2_nv nva[2]; /* :status and maybe content-length or other headers */
                        size_t nva_count = 0;
                        
                        /* :status header */
                        char status_str[16];
                        snprintf(status_str, sizeof(status_str), "%d", stream->upstream_status_code);
                        nva[nva_count].name = (uint8_t *)":status";
                        nva[nva_count].namelen = strlen(":status");
                        nva[nva_count].value = (uint8_t *)status_str;
                        nva[nva_count].valuelen = strlen(status_str);
                        nva_count++;
                        
/* Submit response headers */
                         int ret = nghttp2_submit_response(conn->session, stream->stream_id, nva, nva_count, NULL);
                         if (ret != 0) {
                             LOG_ERROR("Failed to submit response headers: %s", nghttp2_strerror(ret));
                             free(read_buf);
                             return -1;
                         }
                         
                         /* Send any body data that came with the headers */
                         size_t body_received = stream->upstream_response_len - (pos - stream->upstream_response_buf + 4); /* skip \r\n\r\n */
                         if (body_received > 0) {
                             nghttp2_data_provider data_prd;
                             body_payload *payload = (body_payload *)malloc(sizeof(body_payload));
                             payload->data = (uint8_t *)(pos + 4);
                             payload->len = body_received;
                             data_prd.source.ptr = payload;
                             data_prd.read_callback = data_read_callback;
                             int ret = nghttp2_submit_data(conn->session, NGHTTP2_FLAG_NONE, stream->stream_id, &data_prd);
                             if (ret != 0) {
                                 LOG_ERROR("Failed to submit data frame: %s", nghttp2_strerror(ret));
                                 free(read_buf);
                                 free(payload);
                                 return -1;
                             }
                             /* Update window size for stream and connection */
                             nghttp2_submit_window_update(conn->session, NGHTTP2_FLAG_NONE, stream->stream_id, body_received);
                             nghttp2_submit_window_update(conn->session, NGHTTP2_FLAG_NONE, 0, body_received);
                         }
                        
                        /* Reset buffer for more body data */
                        stream->upstream_response_len = 0;
                    }
                }
                
                /* If we have headers complete, send body data as we receive it */
                if (stream->upstream_headers_complete) {
                    nghttp2_data_provider data_prd;
                    body_payload *payload = (body_payload *)malloc(sizeof(body_payload));
                    payload->data = (uint8_t *)read_buf;
                    payload->len = bytes;
                    data_prd.source.ptr = payload;
                    data_prd.read_callback = data_read_callback;
                    int ret = nghttp2_submit_data(conn->session, NGHTTP2_FLAG_NONE, stream->stream_id, &data_prd);
                    if (ret != 0) {
                        LOG_ERROR("Failed to submit data frame: %s", nghttp2_strerror(ret));
                        free(read_buf);
                        free(payload);
                        return -1;
                    }
                }
                
                free(read_buf);
                return 0; /* Continue reading */
            } else {
                int err = SSL_get_error(stream->upstream_ssl, bytes);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    free(read_buf);
                    return 0; /* Try again later */
                }
                if (bytes == 0) {
                    /* Server closed connection */
                    free(read_buf);
                    /* Send any remaining buffered data */
                    if (stream->upstream_response_len > 0 && stream->upstream_headers_complete) {
                        nghttp2_data_provider data_prd;
                        body_payload payload = { (uint8_t *)stream->upstream_response_buf, stream->upstream_response_len };
                        data_prd.source.ptr = &payload;
                        data_prd.read_callback = data_read_callback;
                        int ret = nghttp2_submit_data(conn->session, NGHTTP2_FLAG_NONE, stream->stream_id, &data_prd);
                        if (ret != 0) {
                            LOG_ERROR("Failed to submit data frame: %s", nghttp2_strerror(ret));
                            return -1;
                        }
                    }
                    
                    /* Send END_STREAM flag for the response */
                    nghttp2_data_provider empty_data_prd;
                    body_payload empty_payload = { NULL, 0 };
                    empty_data_prd.source.ptr = &empty_payload;
                    empty_data_prd.read_callback = data_read_callback;
                    int ret = nghttp2_submit_data(conn->session, NGHTTP2_FLAG_END_STREAM, stream->stream_id, &empty_data_prd);
                    if (ret != 0) {
                        LOG_ERROR("Failed to submit end stream: %s", nghttp2_strerror(ret));
                        return -1;
                    }

                    /* Send pending data (headers and data frames) */
                    ret = http2_send_pending(conn);
                    if (ret != 0) {
                        LOG_ERROR("Failed to send pending HTTP/2 data");
                        return -1;
                    }

                    /* Mark stream as response complete */
                    stream->response_complete = 1;
                    stream->upstream_state = HTTP2_UPSTREAM_STATE_DONE;
                    return 0;
                }
                free(read_buf);
                LOG_ERROR("SSL_read error from upstream: %d", err);
                return -1;
            }
        }
            
        case HTTP2_UPSTREAM_STATE_DONE: {
            /* Cleanup upstream connection */
            if (stream->upstream_ssl) {
                SSL_free(stream->upstream_ssl);
                stream->upstream_ssl = NULL;
            }
            if (stream->upstream_fd != -1) {
                close(stream->upstream_fd);
                stream->upstream_fd = -1;
            }
            free(stream->upstream_request);
            stream->upstream_request = NULL;
            stream->upstream_request_len = 0;
            stream->upstream_request_sent = 0;
            /* Note: We intentionally leave the response buffer as it might be needed for debugging */
            return 0;
        }
    }
    
    return 0;
}

/* Clean up HTTP/2 context */
void http2_cleanup_context(http2_conn *conn) {
    if (!conn) {
        return;
    }
    /* We must set the user data of the session to NULL to avoid use-after-free in callbacks */
    if (conn->session) {
        nghttp2_session_set_user_data(conn->session, NULL);
    }
    http2_conn_free(conn);
    free(conn);
}

/* Handle HTTP/2 connection - processes multiple streams */
void handle_http2_connection(SSL *client_ssl, SSL *server_ssl, const char* target_host, int target_port, SSL_CTX *upstream_ctx) {
    LOG_INFO("Handling HTTP/2 connection to %s:%d", target_host, target_port);
    metrics_increment_http2_active_connections();
    http2_conn *conn = NULL;
    int client_fd = SSL_get_fd(client_ssl);
    int server_fd = SSL_get_fd(server_ssl);
    int ret;
    uint8_t inbuf[16384];
    
    // Initialize HTTP/2 session
    if (http2_init_context(&conn, client_fd, server_fd, client_ssl, server_ssl, target_host, target_port) != 0) {
        LOG_ERROR("Failed to initialize HTTP/2 context");
        return;
    }

    int active = 1;
    while (active) {
        /* Read from client */
        int bytes = SSL_read(client_ssl, inbuf, sizeof(inbuf));
        if (bytes > 0) {
            ret = http2_process_input(conn, inbuf, bytes);
            if (ret != 0) {
                LOG_ERROR("Failed to process HTTP/2 input");
                break;
            }
        } else {
            int err = SSL_get_error(client_ssl, bytes);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                /* Try again later */
                continue;
            }
            if (bytes == 0) {
                /* Client closed connection */
                break;
            }
            LOG_ERROR("SSL_read error: %d", err);
            break;
        }

        /* Process any completed streams */
        pthread_mutex_lock(&conn->lock);
        if (conn->active_streams_count > 0) {
            for (size_t i = 0; i < conn->max_streams; i++) {
                if (conn->streams[i] && conn->streams[i]->request_complete && !conn->streams[i]->response_complete) {
                    int ret = http2_process_stream(conn, conn->streams[i]);
                    if (ret == 0 && conn->streams[i]->response_complete) {
                        conn->active_streams_count--;
                    }
                }
            }
        }
        pthread_mutex_unlock(&conn->lock);

        /* Send pending HTTP/2 data */
        ret = http2_send_pending(conn);
        if (ret != 0) {
            LOG_ERROR("Failed to send pending HTTP/2 data");
            break;
        }

        /* Check if we should exit: no active streams and connection is closed */
        /* We'll exit when the client has closed the connection and there are no more active streams */
        /* For simplicity, we'll break after a while if no data */
        /* In a real implementation, we'd check for GOAWAY etc. */
        /* We'll just break if we've done a few iterations without reading */
        /* This is a simplification for the exercise */
        static int idle_count = 0;
        if (bytes <= 0) {
            idle_count++;
            if (idle_count > 10) {
                // Check if any streams are active. If they are, handle them. If not, break.
                int active_streams = 0;
                pthread_mutex_lock(&conn->lock);
                for (size_t i = 0; i < conn->max_streams; i++) {
                    if (conn->streams[i] && (!conn->streams[i]->request_complete || !conn->streams[i]->response_complete)) {
                        active_streams++;
                    }
                }
                pthread_mutex_unlock(&conn->lock);
                if (active_streams == 0) {
                    active = 0;
                } else {
                    idle_count = 0;
                }
            }
        } else {
            idle_count = 0;
        }
    }

    metrics_decrement_http2_active_connections();
    if (conn) {
        http2_cleanup_context(conn);
    }
    SSL_shutdown(client_ssl);
    SSL_shutdown(server_ssl);
    SSL_free(client_ssl);
    SSL_free(server_ssl);
}
