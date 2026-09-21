#include "src/config.h"
ProxyConfig g_config;


#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"
#include "src/proxy_parse.h"

void test_parsed_request_create() {
    TEST_SUITE(test_parsed_request_create);
    struct ParsedRequest *req = ParsedRequest_create();
    TEST_ASSERT(req != NULL);
    ParsedRequest_destroy(req);
}

void test_parsed_request_parse() {
    TEST_SUITE(test_parsed_request_parse);
    
    // Normal GET request
    const char *c = "GET http://www.google.com:80/index.html/ HTTP/1.0\r\nContent-Length: 80\r\nIf-Modified-Since: Sat, 29 Oct 1994 19:43:31 GMT\r\n\r\n";
    int len = strlen(c);
    struct ParsedRequest *req = ParsedRequest_create();
    int result = ParsedRequest_parse(req, c, len);
    
    TEST_ASSERT_EQ(0, result);
    TEST_ASSERT_STR_EQ("GET", req->method);
    TEST_ASSERT_STR_EQ("www.google.com", req->host);
    TEST_ASSERT_STR_EQ("80", req->port);
    TEST_ASSERT_STR_EQ("/index.html/", req->path);
    TEST_ASSERT_STR_EQ("HTTP/1.0", req->version);
    
    struct ParsedHeader *h = ParsedHeader_get(req, "Content-Length");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT_STR_EQ("80", h->value);
    
    ParsedRequest_destroy(req);
}

// Additional unhappy path parses 
void test_parsed_request_parse_failures() {
    TEST_SUITE(test_parsed_request_parse_failures);
    struct ParsedRequest *req = ParsedRequest_create();
    
    // Missing end headers \r\n\r\n (invalid buflen < MIN_REQ_LEN)
    // Actually MIN_REQ_LEN is something.. but maybe we just pass an empty string
    TEST_ASSERT_EQ(-1, ParsedRequest_parse(req, "G", 1));
    
    // Missing end headers \r\n\r\n and enough length
    TEST_ASSERT_EQ(-1, ParsedRequest_parse(req, "GET http://google.com/ HTTP/1.0\r\n", strlen("GET http://google.com/ HTTP/1.0\r\n")));

    // Method not GET
    req = ParsedRequest_create();
    TEST_ASSERT_EQ(-1, ParsedRequest_parse(req, "POST http://google.com/ HTTP/1.0\r\n\r\n", strlen("POST http://google.com/ HTTP/1.0\r\n\r\n")));
    ParsedRequest_destroy(req);

    // Unsupported version
    req = ParsedRequest_create();
    TEST_ASSERT_EQ(-1, ParsedRequest_parse(req, "GET http://google.com/ FOO/1.0\r\n\r\n", strlen("GET http://google.com/ FOO/1.0\r\n\r\n")));
    ParsedRequest_destroy(req);

    // Absolute URI missing slash
    req = ParsedRequest_create();
    TEST_ASSERT_EQ(-1, ParsedRequest_parse(req, "GET http://google.com HTTP/1.0\r\n\r\n", strlen("GET http://google.com HTTP/1.0\r\n\r\n")));
    ParsedRequest_destroy(req);

    // Missing protocol
    req = ParsedRequest_create();
    TEST_ASSERT_EQ(-1, ParsedRequest_parse(req, "GET / HTTP/1.0\r\n\r\n", strlen("GET / HTTP/1.0\r\n\r\n")));
    ParsedRequest_destroy(req);
}

void test_parsed_request_headers() {
    TEST_SUITE(test_parsed_request_headers);
    
    const char *c = "GET http://www.google.com/ HTTP/1.0\r\n\r\n";
    int len = strlen(c);
    struct ParsedRequest *req = ParsedRequest_create();
    ParsedRequest_parse(req, c, len);
    
    // Set Header
    int res = ParsedHeader_set(req, "Connection", "close");
    TEST_ASSERT_EQ(0, res);
    
    struct ParsedHeader *h = ParsedHeader_get(req, "Connection");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT_STR_EQ("close", h->value);
    
    // Modify existing header
    res = ParsedHeader_set(req, "Connection", "keep-alive");
    TEST_ASSERT_EQ(0, res);
    h = ParsedHeader_get(req, "Connection");
    TEST_ASSERT_STR_EQ("keep-alive", h->value);
    
    // Remove Header
    res = ParsedHeader_remove(req, "Connection");
    TEST_ASSERT_EQ(0, res);
    h = ParsedHeader_get(req, "Connection");
    TEST_ASSERT(h == NULL);
    
    // Try to remove missing header
    res = ParsedHeader_remove(req, "X-Missing");
    TEST_ASSERT_EQ(-1, res);

    ParsedRequest_destroy(req);
}

void test_parsed_request_unparse() {
    TEST_SUITE(test_parsed_request_unparse);
    const char *c = "GET http://www.google.com:8080/path?query HTTP/1.0\r\nConnection: close\r\nHost: www.google.com\r\n\r\n";
    struct ParsedRequest *req = ParsedRequest_create();
    ParsedRequest_parse(req, c, strlen(c));

    int len = ParsedRequest_totalLen(req);
    TEST_ASSERT(len > 0);
    
    char* buf = (char*)malloc(len + 1);
    int rc = ParsedRequest_unparse(req, buf, len + 1);
    TEST_ASSERT_EQ(0, rc);

    // proxy_parse unparses as: "GET http://www.google.com:8080/path?query HTTP/1.0\r\n"
    TEST_ASSERT(strstr(buf, "GET http://www.google.com:8080/path?query HTTP/1.0") != NULL);
    TEST_ASSERT(strstr(buf, "Connection: close\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "Host: www.google.com\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "\r\n\r\n") != NULL);

    free(buf);
    
    // Unparse just headers
    len = ParsedHeader_headersLen(req);
    buf = (char*)malloc(len + 1);
    rc = ParsedRequest_unparse_headers(req, buf, len + 1);
    TEST_ASSERT_EQ(0, rc);
    TEST_ASSERT(strstr(buf, "Connection: close\r\n") != NULL);
    TEST_ASSERT(strstr(buf, "Host: www.google.com\r\n") != NULL);
    free(buf);
    
    ParsedRequest_destroy(req);
}

void test_parsed_request_edge_cases() {
    TEST_SUITE(test_parsed_request_edge_cases);
    
    // Exhaustive header set to trigger realloc
    struct ParsedRequest *req = ParsedRequest_create();
    ParsedRequest_parse(req, "GET http://a.com/ HTTP/1.0\r\n\r\n", strlen("GET http://a.com/ HTTP/1.0\r\n\r\n"));
    for (int i=0; i<40; i++) {
        char key[20]; sprintf(key, "X-Custom-%d", i);
        ParsedHeader_set(req, key, "val");
    }
    struct ParsedHeader *h = ParsedHeader_get(req, "X-Custom-39");
    TEST_ASSERT(h!=NULL);
    TEST_ASSERT_STR_EQ("val", h->value);
    ParsedRequest_destroy(req);
}

int main() {
    test_parsed_request_create();
    test_parsed_request_parse();
    test_parsed_request_parse_failures();
    test_parsed_request_headers();
    test_parsed_request_unparse();
    test_parsed_request_edge_cases();
    
    TEST_REPORT();
    return 0;
}
