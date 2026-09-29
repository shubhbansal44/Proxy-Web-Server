#include "proxy_parse.h"
#include "config.h"
#include "tls_tunnel.h"
#include "auth.h"
#include "filter.h"
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <bits/time.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "cache.h"
#include "logger.h"
#include "metrics.h"
#include "admin.h"
#include "http3.h"
#include <signal.h>

// Global configuration structure
ProxyConfig g_config;

// Server's socket id
int PROXY_SOCKET_ID;

// Buffer to store Thread IDs associated with each client's request.
pthread_t *THREAD_ID;

// semaphore lock for handling multiple (MAX CLIENTS) users.
sem_t SEMAPHORE;


int ConnectEndServer(void *hostname, int port)
{
  int END_SERVER_SOCKET = -1;
  struct addrinfo hints, *res, *rp;
  char port_str[16];
  int ret;

  snprintf(port_str, sizeof(port_str), "%d", port);

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
  hints.ai_socktype = SOCK_STREAM; /* TCP socket */
  hints.ai_flags = 0;
  hints.ai_protocol = 0;          /* Any protocol */

  ret = getaddrinfo((const char *)hostname, port_str, &hints, &res);
  if (ret != 0)
  {
    LOG_ERROR("ConnectEndServer: getaddrinfo failed: %s\n", gai_strerror(ret));
    return -1;
  }

  /* Try each address until we successfully connect */
  for (rp = res; rp != NULL; rp = rp->ai_next)
  {
    // Check if resolved IP is blocked
    if (filter_is_ip_blocked(rp->ai_addr)) {
        LOG_WARN("Resolved IP is blocked for %s", (const char*)hostname);
        continue;
    }

    END_SERVER_SOCKET = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (END_SERVER_SOCKET == -1)
      continue;

    if (connect(END_SERVER_SOCKET, rp->ai_addr, rp->ai_addrlen) != -1)
      break; /* Success */

    close(END_SERVER_SOCKET);
    END_SERVER_SOCKET = -1;
  }

  freeaddrinfo(res);

  if (rp == NULL) /* No address succeeded */
  {
    LOG_ERROR("ConnectEndServer: Could not connect to %s:%d\n", (const char *)hostname, port);
    return -1;
  }

  return END_SERVER_SOCKET;
}

/* Parse Content-Type from HTTP response headers */
static int parse_content_type_header(const char *buf, size_t len, char *out, size_t out_size) {
    if (!buf || len == 0 || !out || out_size == 0) return -1;
    const char *header_start = (const char *)memmem((void *)buf, len, "Content-Type:", 13);
    if (!header_start) return -1;
    const char *val = header_start + 13;
    while (val < buf + len && (*val == ' ' || *val == '\t')) val++;
    const char *end = (const char *)memmem((void *)val, len - (val - buf), "\r\n", 2);
    if (!end) end = (const char *)memmem((void *)val, len - (val - buf), "\n", 1);
    if (!end) end = buf + len;
    size_t val_len = end - val;
    if (val_len >= out_size) val_len = out_size - 1;
    memcpy(out, val, val_len);
    out[val_len] = '\0';
    char *trim = out + val_len - 1;
    while (trim >= out && (*trim == ' ' || *trim == '\t' || *trim == '\r' || *trim == '\n')) *trim-- = '\0';
    return 0;
}
int HandleRequest(int CLIENT_SOCKET_ID, struct ParsedRequest *CLIENT_PARSED_REQUEST, char *CLIENT_REQUEST)
{
  char *BUFFER = (char *)calloc(g_config.max_bytes, sizeof(char));
  strcpy(BUFFER, "GET ");
  strcat(BUFFER, CLIENT_PARSED_REQUEST->path);
  strcat(BUFFER, " ");
  strcat(BUFFER, CLIENT_PARSED_REQUEST->version);
  strcat(BUFFER, "\r\n");
  size_t LENGTH = strlen(BUFFER);

  if (ParsedHeader_set(CLIENT_PARSED_REQUEST, "Connection", "close") < 0)
  {
    LOG_ERROR("HandleRequest: Error occured While Establising Parsed request connection!\n");
  }
  ParsedHeader_remove(CLIENT_PARSED_REQUEST, "Proxy-Connection");

  struct ParsedHeader *h_hdr = ParsedHeader_get(CLIENT_PARSED_REQUEST, "Host");
  if (CLIENT_PARSED_REQUEST->host != NULL)
  {
    if (h_hdr == NULL || strstr(h_hdr->value, "localhost") != NULL || strstr(h_hdr->value, "127.0.0.1") != NULL)
    {
      if (ParsedHeader_set(CLIENT_PARSED_REQUEST, "Host", CLIENT_PARSED_REQUEST->host) < 0)
      {
        LOG_ERROR("HandleRequest: Error occured while setting host in Parsed Request header!\n");
      }
    }
  }

  if (ParsedRequest_unparse_headers(CLIENT_PARSED_REQUEST, BUFFER + LENGTH, (size_t)g_config.max_bytes - LENGTH) < 0)
  {
    LOG_ERROR("HandleRequest: Error occured while unparsing headers!\n");
  }

  int END_SERVER_PORT = 80;
  if (CLIENT_PARSED_REQUEST->port != NULL)
  {
    END_SERVER_PORT = atoi(CLIENT_PARSED_REQUEST->port);
  }

  int END_SERVER_SOCKET_ID = ConnectEndServer(CLIENT_PARSED_REQUEST->host, END_SERVER_PORT);
  if (END_SERVER_SOCKET_ID < 0)
  {
    LOG_ERROR("HandleRequest: Error occured while creating end server socket ID!\n");
    return -1;
  }

  /* send request to end server */
  ssize_t BYTES_SEND = send(END_SERVER_SOCKET_ID, BUFFER, strlen(BUFFER), 0);
  if (BYTES_SEND < 0)
  {
    LOG_ERROR("HandleRequest: sending request to end server failed");
    close(END_SERVER_SOCKET_ID);
    return -1;
  }

  /* Receive from end server and stream to client; also build RESPONSE for caching */
  char *RESPONSE = (char *)malloc(g_config.max_bytes);
  if (!RESPONSE)
  {
    close(END_SERVER_SOCKET_ID);
    return -1;
  }
  size_t RESPONSE_CAPACITY = g_config.max_bytes;
  size_t RESPONSE_LENGTH = 0;

  ssize_t BYTES_RECEIVED;
  int ct_blocked = 0;
  char ct_buf[256] = {0};
  while ((BYTES_RECEIVED = recv(END_SERVER_SOCKET_ID, BUFFER, g_config.max_bytes, 0)) > 0)
  {
    /* Safe bounded append (length-tracked, no strlen) */
    if (RESPONSE_LENGTH + (size_t)BYTES_RECEIVED + 1 > RESPONSE_CAPACITY)
    {
      RESPONSE_CAPACITY *= 2;
      char *TEMP = (char *)realloc(RESPONSE, RESPONSE_CAPACITY);
      if (!TEMP)
      {
        LOG_ERROR("HandleRequest: realloc failed");
        break;
      }
      RESPONSE = TEMP;
    }
    memcpy(RESPONSE + RESPONSE_LENGTH, BUFFER, BYTES_RECEIVED);
    RESPONSE_LENGTH += BYTES_RECEIVED;

    /* Header boundary via memmem (safe, bounded) */
    char *hdr_end = (char *)memmem(RESPONSE, RESPONSE_LENGTH, "\r\n\r\n", 4);
    if (!hdr_end) hdr_end = (char *)memmem(RESPONSE, RESPONSE_LENGTH, "\n\n", 2);

    if (hdr_end && !ct_blocked && parse_content_type_header(RESPONSE, RESPONSE_LENGTH, ct_buf, sizeof(ct_buf)) == 0) {
      if (filter_is_content_type_blocked(ct_buf)) {
        ct_blocked = 1;
        break;  /* Block: do not relay any bytes of blocked response */
      }
    }

    /* Only stream permitted content to client */
    if (!ct_blocked) {
      ssize_t BYTES_SEND_CLIENT = send(CLIENT_SOCKET_ID, BUFFER, BYTES_RECEIVED, 0);
      if (BYTES_SEND_CLIENT > 0) {
        metrics_add_bytes(BYTES_SEND_CLIENT);
        struct sockaddr_storage addr_check;
        socklen_t addr_check_len = sizeof(addr_check);
        if (getpeername(CLIENT_SOCKET_ID, (struct sockaddr*)&addr_check, &addr_check_len) == 0) {
          if (addr_check.ss_family == AF_INET6) metrics_add_ipv6_bytes(BYTES_SEND_CLIENT);
        }
      }
      if (BYTES_SEND_CLIENT < 0) {
        perror("Error parsing from server to client");
        break;
      }
    }
  }

  if (BYTES_RECEIVED < 0)
    LOG_ERROR("HandleRequest: recv from end server failed");

  /* null-terminate for safety */
  if (RESPONSE_LENGTH + 1 > RESPONSE_CAPACITY)
  {
    char *TEMP = (char *)realloc(RESPONSE, RESPONSE_LENGTH + 1);
    if (TEMP)
      RESPONSE = TEMP;
  }
  RESPONSE[RESPONSE_LENGTH] = '\0';

  /* Structural: block after loop if MIME type disallowed */
  if (ct_blocked) {
      const char *custom = filter_get_block_page();
      if (!custom) custom = "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD><BODY><H1>403 Forbidden</H1></BODY></HTML>";
      char block_resp[1024];
      snprintf(block_resp, sizeof(block_resp),
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        strlen(custom), custom);
      send(CLIENT_SOCKET_ID, block_resp, strlen(block_resp), 0);
    send(CLIENT_SOCKET_ID, block_resp, strlen(block_resp), 0);
    metrics_increment_errors();
    free(BUFFER);
    free(RESPONSE);
    close(END_SERVER_SOCKET_ID);
    return 0;
  }

  /* Post-loop safeguard for non-blocked paths */
  char ct_buf_final[256] = {0};
  if (parse_content_type_header(RESPONSE, RESPONSE_LENGTH, ct_buf_final, sizeof(ct_buf_final)) == 0) {
    if (filter_is_content_type_blocked(ct_buf_final)) {
      LOG_INFO("Blocked content type: %s", ct_buf_final);
      const char *custom = filter_get_block_page();
      if (!custom) custom = "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD><BODY><H1>403 Forbidden</H1></BODY></HTML>";
      char block_resp[1024];
      snprintf(block_resp, sizeof(block_resp),
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        strlen(custom), custom);
      send(CLIENT_SOCKET_ID, block_resp, strlen(block_resp), 0);
      metrics_increment_errors();
      free(BUFFER);
      free(RESPONSE);
      close(END_SERVER_SOCKET_ID);
      return 0;
    }
  }

  /* Attempt caching (use the original request string as key) */
  AddCache(RESPONSE, RESPONSE_LENGTH, CLIENT_REQUEST);

  free(BUFFER);
  free(RESPONSE);
  close(END_SERVER_SOCKET_ID);

  return 0;
}

int checkHTTPversion(char *clientVersion)
{
  int version = -1;

  if (strncmp(clientVersion, "HTTP/1.1", 8) == 0)
  {
    version = 1;
  }
  else if (strncmp(clientVersion, "HTTP/1.0", 8) == 0)
  {
    version = 1; // Handling this similar to version 1.1
  }
  else
    version = -1;

  return version;
}

int ThrowError(int socket, int status_code)
{
  char str[1024];
  char currentTime[50];
  time_t now = time(0);

  struct tm data = *gmtime(&now);
  strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

  switch (status_code)
  {
  case 400:
    snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("400 Bad Request");
    send(socket, str, strlen(str), 0);
    break;

  case 403:
    {
      const char *html = filter_get_block_page();
      snprintf(str, sizeof(str), 
               "HTTP/1.1 403 Forbidden\r\n"
               "Content-Length: %zu\r\n"
               "Content-Type: text/html\r\n"
               "Connection: keep-alive\r\n"
               "Date: %s\r\n"
               "Server: VaibhavN/14785\r\n\r\n"
               "%s", 
               strlen(html), currentTime, html);
      LOG_INFO("403 Forbidden");
      send(socket, str, strlen(str), 0);
    }
    break;

  case 404:
    snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("404 Not Found");
    send(socket, str, strlen(str), 0);
    break;

  case 407:
    snprintf(str, sizeof(str), "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"Proxy\"\r\nContent-Length: 122\r\nContent-Type: text/html\r\nConnection: close\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>407 Proxy Authentication Required</TITLE></HEAD>\n<BODY><H1>407 Authentication Required</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("407 Proxy Authentication Required");
    send(socket, str, strlen(str), 0);
    break;

  case 500:
    snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("500 Internal Server Error");
    send(socket, str, strlen(str), 0);
    break;

  case 501:
    snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("501 Not Implemented");
    send(socket, str, strlen(str), 0);
    break;

  case 505:
    snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("505 HTTP Version Not Supported");
    send(socket, str, strlen(str), 0);
    break;

  default:
    return -1;
  }
  return 1;
}

void *THREAD_ROUTINE(void *NEW_SOCKET)
{
  struct timespec start_time, end_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  
  metrics_increment_requests();
  metrics_increment_active_connections();
  int *NEW_SOCKET_PTR = (int *)NEW_SOCKET;
  int SOCKET = *NEW_SOCKET_PTR;
  struct sockaddr_storage addr_check_start;
  socklen_t addr_check_start_len = sizeof(addr_check_start);
  if (getpeername(SOCKET, (struct sockaddr*)&addr_check_start, &addr_check_start_len) == 0) {
      if (addr_check_start.ss_family == AF_INET6) {
          metrics_increment_ipv6_active_connections();
          metrics_increment_ipv6_requests();
      }
  }

  sem_wait(&SEMAPHORE);
  int CURRENT_SEMAPHORE_VALUE;
  sem_getvalue(&SEMAPHORE, &CURRENT_SEMAPHORE_VALUE);
  LOG_INFO("Currently available Sockets: %d", CURRENT_SEMAPHORE_VALUE);

  int BYTES_RECIEVED, LENGTH;

  char *BUFFER = (char *)calloc(g_config.max_bytes, sizeof(char));
  memset(BUFFER, 0, g_config.max_bytes);
size_t buf_len = 0;
   ssize_t r = recv(SOCKET, BUFFER, g_config.max_bytes, 0);
   if (r <= 0) {
       // Handle error or disconnect
       close(SOCKET);
       free(BUFFER);
       sem_post(&SEMAPHORE);
       return NULL;
   }
   buf_len = r;

   // Check if it's an HTTP/2 or WebSockets or generic TLS tunnel request (for port 443 proxy)
   // Or if it's h2c preface: "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
   const char *H2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
   
   if (buf_len > 0 && 
       (BUFFER[0] == 0x16 || // TLS Handshake
        (buf_len >= 24 && memcmp(BUFFER, H2_PREFACE, 24) == 0))) 
   {
       // Explicit CONNECT is mandatory. Raw TLS/h2c detected directly on a 
       // fresh socket instead of an HTTP/1.1 proxy payload must be rejected.
       LOG_ERROR("Raw TLS or h2c detected without prior CONNECT. Rejecting.");
       
       // Get client IP for access log reporting before exit
       struct sockaddr_storage client_addr_fail;
       socklen_t addr_len_fail = sizeof(client_addr_fail);
       char client_ip_fail[INET6_ADDRSTRLEN] = "UNKNOWN";
       if (getpeername(SOCKET, (struct sockaddr*)&client_addr_fail, &addr_len_fail) == 0) {
           if (client_addr_fail.ss_family == AF_INET) {
               struct sockaddr_in *s = (struct sockaddr_in *)&client_addr_fail;
               inet_ntop(AF_INET, &(s->sin_addr), client_ip_fail, sizeof(client_ip_fail));
           } else if (client_addr_fail.ss_family == AF_INET6) {
               struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr_fail;
               inet_ntop(AF_INET6, &(s->sin6_addr), client_ip_fail, sizeof(client_ip_fail));
           }
       }
       
       clock_gettime(CLOCK_MONOTONIC, &end_time);
       double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
       logger_access_log(client_ip_fail, 400, 0, "UNKNOWN", "UNKNOWN", elapsed_ms, "text/html", "UNKNOWN");
       
       close(SOCKET);
       free(BUFFER);
       sem_post(&SEMAPHORE);
       
       return NULL;
   }

   // Now we have HTTP data. We need to read until we get the full headers.
   while (buf_len < g_config.max_bytes) {
       // Check if we have received the end of headers
       char *ptr = (char *)memmem(BUFFER, buf_len, "\r\n\r\n", 4);
       if (ptr != NULL) {
           break;
       }
       // Otherwise, read more
       ssize_t r = recv(SOCKET, BUFFER + buf_len, g_config.max_bytes - buf_len, 0);
       if (r <= 0) {
           break;
       }
       buf_len += r;
   }
   BYTES_RECIEVED = buf_len;

   char *REQUEST = (char *)malloc(BYTES_RECIEVED + 1);
   if (REQUEST) {
       memcpy(REQUEST, BUFFER, BYTES_RECIEVED);
       REQUEST[BYTES_RECIEVED] = '\0';
   } else {
       fprintf(stderr, "Failed to allocate memory for REQUEST\n");
       close(SOCKET);
       free(BUFFER);
       sem_post(&SEMAPHORE);
       return NULL;
   }

   // Get client IP for access log
  struct sockaddr_storage client_addr;
  socklen_t addr_len = sizeof(client_addr);
  char client_ip[INET6_ADDRSTRLEN] = "UNKNOWN";
  if (getpeername(SOCKET, (struct sockaddr*)&client_addr, &addr_len) == 0) {
      if (client_addr.ss_family == AF_INET) {
          struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
          inet_ntop(AF_INET, &(s->sin_addr), client_ip, sizeof(client_ip));
      } else if (client_addr.ss_family == AF_INET6) {
          struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
          inet_ntop(AF_INET6, &(s->sin6_addr), client_ip, sizeof(client_ip));
      }
  }

  int response_status = 200;
  int response_size = 0;
  const char* method = "UNKNOWN";
  const char* url = "UNKNOWN";
  char trace_id_str[64];
  snprintf(trace_id_str, sizeof(trace_id_str), "%08x%08x", (unsigned int)time(NULL), (unsigned int)rand());

  struct ParsedRequest *PARSED_REQUEST = ParsedRequest_create();
  int parse_result = -1;
  if (BYTES_RECIEVED > 0) {
      parse_result = ParsedRequest_parse(PARSED_REQUEST, BUFFER, strlen(BUFFER));
      if (parse_result >= 0) {
          struct ParsedHeader *trace_hdr = ParsedHeader_get(PARSED_REQUEST, "X-Trace-Id");
          if (trace_hdr && trace_hdr->value) {
              snprintf(trace_id_str, sizeof(trace_id_str), "%s", trace_hdr->value);
          } else {
              ParsedHeader_set(PARSED_REQUEST, "X-Trace-Id", trace_id_str);
          }
          if (PARSED_REQUEST->method) method = PARSED_REQUEST->method;
          if (PARSED_REQUEST->path) url = PARSED_REQUEST->path;
      }
  }

  if (parse_result >= 0) {
      // Check auth if enabled
      if (g_config.enable_auth) {
        struct ParsedHeader *auth_hdr = ParsedHeader_get(PARSED_REQUEST, "Proxy-Authorization");
        const char *auth_val = auth_hdr ? auth_hdr->value : NULL;
        
        if (!check_basic_auth(auth_val)) {
            ThrowError(SOCKET, 407);
            response_status = 407;
            
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
            logger_access_log(client_ip, response_status, 0, method, url, elapsed_ms, "text/html", trace_id_str);
            
            ParsedRequest_destroy(PARSED_REQUEST);
            free(BUFFER);
            free(REQUEST);
            close(SOCKET);
            sem_post(&SEMAPHORE);
            return NULL;
        }
      }

      // Check domain filtering
      if (PARSED_REQUEST->host && filter_is_domain_blocked(PARSED_REQUEST->host)) {
          LOG_WARN("Domain blocked: %s", PARSED_REQUEST->host);
          ThrowError(SOCKET, 403);
          response_status = 403;
          
          clock_gettime(CLOCK_MONOTONIC, &end_time);
          double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
          logger_access_log(client_ip, response_status, 0, method, url, elapsed_ms, "text/html", trace_id_str);
          
          ParsedRequest_destroy(PARSED_REQUEST);
          free(BUFFER);
          free(REQUEST);
          close(SOCKET);
          sem_post(&SEMAPHORE);
          return NULL;
      }

      // Check URL filtering
      if (PARSED_REQUEST->path && filter_is_url_blocked(PARSED_REQUEST->path)) {
          LOG_WARN("URL blocked: %s", PARSED_REQUEST->path);
          ThrowError(SOCKET, 403);
          response_status = 403;
          
          clock_gettime(CLOCK_MONOTONIC, &end_time);
          double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
          logger_access_log(client_ip, response_status, 0, method, url, elapsed_ms, "text/html", trace_id_str);
          
          ParsedRequest_destroy(PARSED_REQUEST);
          free(BUFFER);
          free(REQUEST);
          close(SOCKET);
          sem_post(&SEMAPHORE);
          return NULL;
      }
  }

  CacheModule *CACHE = FindCache(REQUEST);
  if (CACHE != NULL)
  {
    metrics_increment_cache_hits();
    int SIZE = CACHE->LENGTH / sizeof(char);
    int POS = 0;
    char RESPONSE[g_config.max_bytes];
    while (POS < SIZE)
    {
      bzero(RESPONSE, g_config.max_bytes);
      int response_size = 0;
      for (int i = 0; i < g_config.max_bytes && POS < SIZE; i++, POS++)
      {
        RESPONSE[i] = CACHE->DATA[POS];
        response_size++;
      }
      ssize_t sent = send(SOCKET, RESPONSE, response_size, 0);
      if (sent > 0) {
          metrics_add_bytes(sent);
          struct sockaddr_storage addr_check;
          socklen_t addr_check_len = sizeof(addr_check);
          if (getpeername(SOCKET, (struct sockaddr*)&addr_check, &addr_check_len) == 0) {
              if (addr_check.ss_family == AF_INET6) {
                  metrics_add_ipv6_bytes(sent);
              }
          }
      }
    }
    LOG_INFO("Data retrived from cache");
    LOG_INFO("%s\n", RESPONSE);
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
    logger_access_log(client_ip, 200, response_size, method, url, elapsed_ms, "text/html", trace_id_str);
    
    ParsedRequest_destroy(PARSED_REQUEST);
  }
  else if (BYTES_RECIEVED > 0)
  {
    LENGTH = strlen(BUFFER);

    if (parse_result < 0)
    {
      if (strncmp(BUFFER, "GET /", 5) == 0 && strncmp(BUFFER, "GET /http", 9) != 0) {
        // Deliberate drop for incomplete relative paths (like /favicon.ico) so they do not emit 400 error logs or crash.
        close(SOCKET);
        free(BUFFER);
        free(REQUEST);
        if (PARSED_REQUEST) ParsedRequest_destroy(PARSED_REQUEST);
        sem_post(&SEMAPHORE);
        return NULL;
      }
      LOG_INFO("Parsing failed!");
      ThrowError(SOCKET, 400);
      response_status = 400;
    }
    else
    {

      // Filters and Auth moved above FindCache

      memset(BUFFER, 0, g_config.max_bytes);
if (!strcmp(PARSED_REQUEST->method, "GET"))
      {
        if (PARSED_REQUEST->host && PARSED_REQUEST->path && checkHTTPversion(PARSED_REQUEST->version) == 1)
        {
          int handle_res = HandleRequest(SOCKET, PARSED_REQUEST, REQUEST);
          if (handle_res == -1)
          {
            ThrowError(SOCKET, 500);
            response_status = 500;
            metrics_increment_errors();
          }
        }
        else
        {
          ThrowError(SOCKET, 500);
          response_status = 500;
          metrics_increment_errors();
        }
      }
      else if (!strcmp(PARSED_REQUEST->method, "CONNECT"))
      {
        int port = PARSED_REQUEST->port ? atoi(PARSED_REQUEST->port) : 443;
        int handle_res = HandleConnect(SOCKET, PARSED_REQUEST->host, port);
        if (handle_res == -1)
        {
          ThrowError(SOCKET, 500); // Bad Gateway or internal error
          response_status = 502;
          metrics_increment_errors();
        }
      }
      else
      {
        LOG_INFO("Can't handle request other than \'GET\'");
        ThrowError(SOCKET, 501);
        response_status = 501;
        metrics_increment_errors();
      }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
    logger_access_log(client_ip, response_status, 0, method, url, elapsed_ms, "text/html", trace_id_str); // Need actual sizes if possible
    ParsedRequest_destroy(PARSED_REQUEST);
  }
  else if (BYTES_RECIEVED == 0)
  {
    LOG_INFO("Request didn't received, user may be disconnected");
  }

  struct sockaddr_storage addr_check;
  socklen_t addr_check_len = sizeof(addr_check);
  if (getpeername(SOCKET, (struct sockaddr*)&addr_check, &addr_check_len) == 0) {
      if (addr_check.ss_family == AF_INET6) {
          metrics_decrement_ipv6_active_connections();
      }
  }
  metrics_decrement_active_connections();

  shutdown(SOCKET, SHUT_RDWR);
  close(SOCKET);
  free(BUFFER);

  sem_post(&SEMAPHORE);
  sem_getvalue(&SEMAPHORE, &CURRENT_SEMAPHORE_VALUE);
  LOG_INFO("Currently available Sockets: %d", CURRENT_SEMAPHORE_VALUE);

  free(REQUEST);
  
  return NULL;
}


void handle_signal(int sig) {
    LOG_INFO("Received signal %d, shutting down...", sig);
    admin_server_stop();
    CleanupOpenSSL();
    logger_shutdown();
    exit(0);
}

int main(int argc, char *argv[])
{
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  const char *config_file = getenv("PROXY_CONFIG_FILE");
  
  // Scan argv for --config=
  for (int i = 1; i < argc; i++) {
      if (strncmp(argv[i], "--config=", 9) == 0) {
          config_file = argv[i] + 9;
          break;
      }
  }

  if (!config_file) {
      if (access("/etc/proxy/proxy.conf", F_OK) == 0) {
          config_file = "/etc/proxy/proxy.conf";
      } else {
          config_file = "proxy.conf";
      }
  }

  // Initialize defaults
  InitOpenSSL();
  config_init_defaults(&g_config);
  config_load_file(&g_config, config_file);
  config_apply_env(&g_config);
  
  // Start hot reload
  config_start_hot_reload(config_file);
  
int CLIENT_SOCKET_ID, CLIENT_LENGTH;
struct sockaddr_storage SERVER_ADDR;   // supports IPv4 and IPv6
struct sockaddr_storage CLIENT_ADDR;
  sem_init(&SEMAPHORE, 0, g_config.max_clients);
  Cache_init();

  if (argc == 2 && isdigit(argv[1][0]))
  {
    g_config.port = atoi(argv[1]); // positional
  }
  
  // Apply command-line overrides via official parser (handles --port=N etc.)
  // Skip parser for pure positional numeric to preserve backward compatibility
  int args_ok = 1;
  if (argc == 2 && isdigit(argv[1][0])) {
    args_ok = 1; // allow positional
  } else {
    args_ok = (config_apply_args(&g_config, argc, argv) == 0);
  }
  
  if (args_ok == 0) {
    LOG_ERROR("Invalid command-line arguments\n");
    exit(1);
  }
  
  if (config_validate(&g_config) < 0) {
    LOG_ERROR("Configuration validation failed\n");
    exit(1);
  }
  
  // Initialize Auth & ACL subsystem
  auth_init(&g_config);
  filter_init(&g_config);
  
  // Initialize Observability subsystem
  LoggerConfig log_cfg;
  strncpy(log_cfg.log_file, g_config.log_file, sizeof(log_cfg.log_file)-1);
  log_cfg.log_file[sizeof(log_cfg.log_file)-1] = '\0';
  
  LogLevelEnum l_enum = LOG_LEVEL_INFO;
  if (!strcmp(g_config.log_level, "DEBUG")) l_enum = LOG_LEVEL_DEBUG;
  else if (!strcmp(g_config.log_level, "WARN")) l_enum = LOG_LEVEL_WARN;
  else if (!strcmp(g_config.log_level, "ERROR")) l_enum = LOG_LEVEL_ERROR;
  else if (!strcmp(g_config.log_level, "CRITICAL")) l_enum = LOG_LEVEL_CRITICAL;
  
  log_cfg.level = l_enum;
  log_cfg.max_file_size_mb = g_config.log_max_size_mb;
  log_cfg.rotation_enabled = g_config.log_rotation;
  log_cfg.use_syslog = false;
  if (strcmp(g_config.log_format, "JSON") == 0) log_cfg.format = LOG_FORMAT_JSON;
  else if (strcmp(g_config.log_format, "CLF") == 0) log_cfg.format = LOG_FORMAT_CLF;
  else log_cfg.format = LOG_FORMAT_SQUID;
  
  if (!logger_init(&log_cfg)) {
      LOG_ERROR("Failed to initialize logger!\n");
  }

  metrics_init();
  
  AdminConfig admin_cfg;
  admin_cfg.enabled = g_config.enable_admin;
  admin_cfg.port = g_config.admin_port;
  admin_server_start(&admin_cfg);
  
  // Dynamic allocations based on loaded limits
  THREAD_ID = (pthread_t *)malloc(sizeof(pthread_t) * g_config.max_clients);

  LOG_INFO("Starting Proxy Server at Port: %d...", g_config.port);

  start_http3_server(g_config.port);

   if (g_config.enable_ipv6) {
       PROXY_SOCKET_ID = socket(AF_INET6, SOCK_STREAM, 0);
       int v6only = 0;
       setsockopt(PROXY_SOCKET_ID, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&v6only, sizeof(v6only));
   } else {
       PROXY_SOCKET_ID = socket(AF_INET, SOCK_STREAM, 0);
   }
  if (PROXY_SOCKET_ID < 0)
  {
    LOG_INFO("Failed to create Proxy Socket ID!");
    exit(1);
  }

  int REUSE = 1;
  if (setsockopt(PROXY_SOCKET_ID, SOL_SOCKET, SO_REUSEADDR,
                 (const char *)&REUSE, sizeof(REUSE)) < 0)
  {
    LOG_INFO("Execution failed while setting Socket option(setsockopt)!");
  }

memset(&SERVER_ADDR, 0, sizeof(SERVER_ADDR));
   socklen_t bind_len;
   if (g_config.enable_ipv6) {
       struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&SERVER_ADDR;
       addr6->sin6_family = AF_INET6;
       addr6->sin6_port = htons(g_config.port);
       addr6->sin6_addr = in6addr_any;
       bind_len = sizeof(struct sockaddr_in6);
   } else {
       struct sockaddr_in *addr4 = (struct sockaddr_in *)&SERVER_ADDR;
       addr4->sin_family = AF_INET;
       addr4->sin_port = htons(g_config.port);
       addr4->sin_addr.s_addr = INADDR_ANY;
       bind_len = sizeof(struct sockaddr_in);
   }

   if (bind(PROXY_SOCKET_ID, (struct sockaddr *)&SERVER_ADDR, bind_len) < 0)
   {
     LOG_INFO("Port is not available!");
     exit(0);
   }
  LOG_INFO("Binding on Port: %d", g_config.port);
  int LISTEN_STATUS = listen(PROXY_SOCKET_ID, g_config.max_clients);

  if (LISTEN_STATUS < 0)
  {
    LOG_INFO("Error occured while listening!");
    exit(1);
  }

  int ITERATOR = 0;
  int *CONNECTED_SOCKET_ID = (int *)malloc(sizeof(int) * g_config.max_clients);

while (1)
   {
     memset((char *)&CLIENT_ADDR, 0, sizeof(CLIENT_ADDR));
     CLIENT_LENGTH = sizeof(CLIENT_ADDR);
     CLIENT_SOCKET_ID = accept(PROXY_SOCKET_ID, (struct sockaddr *)&CLIENT_ADDR,
                               (socklen_t *)&CLIENT_LENGTH);

     if (CLIENT_SOCKET_ID < 0)
     {
       LOG_INFO("Unable to connect new user!");
       exit(1);
     }
     else
     {
       CONNECTED_SOCKET_ID[ITERATOR] = CLIENT_SOCKET_ID;
     }

     // Extract client IP and port from CLIENT_ADDR (sockaddr_storage)
     char ipstr[INET6_ADDRSTRLEN] = "UNKNOWN";
     uint16_t port = 0;
     if (CLIENT_ADDR.ss_family == AF_INET) {
         struct sockaddr_in *s = (struct sockaddr_in *)&CLIENT_ADDR;
         inet_ntop(AF_INET, &(s->sin_addr), ipstr, sizeof(ipstr));
         port = ntohs(s->sin_port);
     } else if (CLIENT_ADDR.ss_family == AF_INET6) {
         struct sockaddr_in6 *s = (struct sockaddr_in6 *)&CLIENT_ADDR;
         inet_ntop(AF_INET6, &(s->sin6_addr), ipstr, sizeof(ipstr));
         port = ntohs(s->sin6_port);
     }
     

     if (!check_ip_allowed(ipstr)) {
         LOG_INFO("Connection denied for IP: %s (IP ACL block)", ipstr);
         close(CLIENT_SOCKET_ID);
         CONNECTED_SOCKET_ID[ITERATOR] = 0;
         continue;
     }
     
     printf("Client is connected via Port number: %d and IP address: %s\n",
            port, ipstr);

     pthread_create(&THREAD_ID[ITERATOR], NULL, THREAD_ROUTINE,
                    (void *)&CONNECTED_SOCKET_ID[ITERATOR]);
     ITERATOR += 1;
   }
  close(PROXY_SOCKET_ID);
  CleanupOpenSSL();
  free(THREAD_ID);
  free(CONNECTED_SOCKET_ID);
  filter_cleanup();
  return 0;
}
