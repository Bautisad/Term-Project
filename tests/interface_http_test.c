/* interface_http_test: the request parser, the response builder and the private-peer check,
 * with no sockets.  Every parse is made on an exactly-sized heap copy, so AddressSanitizer
 * (make asan) and Valgrind (make memcheck) catch a read past the end.  Exits 1 at the first
 * failed check. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "http.h"

#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) {                                                                    \
            (void)fprintf(stderr, "interface_http_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/* Parse a heap copy of exactly n bytes. */
static int parse_exact(const char* s, size_t n, http_req* out)
{
    char* copy = malloc(n > 0 ? n : 1);
    int   r;

    CHECK(copy != NULL);
    if (n > 0) {
        (void)memcpy(copy, s, n);
    }
    r = http_parse(copy, n, out);
    free(copy);
    return r;
}

/* The answer for the whole request, and for every prefix of it: a prefix either needs more, or
 * already gives the final answer (so how the bytes were split never changes the result). */
static void expect_n(const char* req, size_t n, int status, http_route route)
{
    http_req r = {HTTP_ROUTE_NONE, 0};
    http_req p = {HTTP_ROUTE_NONE, 0};

    CHECK(parse_exact(req, n, &r) == status);
    if (status == HTTP_OK) {
        CHECK(r.route == route && r.head_len == n);
    }
    for (size_t k = 0; k < n; k++) {
        int pr = parse_exact(req, k, &p);

        if (status == HTTP_OK) {
            CHECK(pr == HTTP_NEED_MORE);
        } else {
            CHECK(pr == HTTP_NEED_MORE || pr == status);
        }
    }
}

#define EXPECT(lit, status, route) expect_n((lit), sizeof(lit) - 1, (status), (route))
#define OK_INDEX(lit)  EXPECT(lit, HTTP_OK, HTTP_ROUTE_INDEX)
#define OK_STATUS(lit) EXPECT(lit, HTTP_OK, HTTP_ROUTE_STATUS)
#define OK_NONE(lit)   EXPECT(lit, HTTP_OK, HTTP_ROUTE_NONE)
#define ERR(lit, status) EXPECT(lit, (status), HTTP_ROUTE_NONE)

static void valid_requests(void)
{
    http_req r;
    char     tail[] = "GET / HTTP/1.1\r\n\r\nGARBAGE";

    OK_INDEX("GET / HTTP/1.1\r\nHost: greenthumb.local:8080\r\nUser-Agent: x\r\n\r\n");
    OK_INDEX("GET / HTTP/1.0\r\n\r\n");
    OK_INDEX("GET /index.html HTTP/1.1\r\n\r\n");
    OK_INDEX("GET /?refresh=1 HTTP/1.1\r\n\r\n");
    OK_INDEX("GET /index.html?x=/status.json HTTP/1.1\r\n\r\n");
    OK_STATUS("GET /status.json HTTP/1.1\r\n\r\n");
    OK_STATUS("GET /status.json?x=1 HTTP/1.0\r\n\r\n");
    OK_NONE("GET /nope HTTP/1.1\r\n\r\n");
    OK_NONE("GET /status.jsonx HTTP/1.1\r\n\r\n");
    OK_NONE("GET //status.json HTTP/1.1\r\n\r\n");
    OK_NONE("GET /index.html/ HTTP/1.1\r\n\r\n");
    OK_NONE("GET /%73tatus.json HTTP/1.1\r\n\r\n"); /* never percent-decoded */
    OK_NONE("GET /#frag HTTP/1.1\r\n\r\n");
    OK_INDEX("GET / HTTP/1.1\r\nX-A:\tv\xC3\xA9\r\n\r\n");             /* tab and non-ASCII in a value */
    OK_INDEX("GET / HTTP/1.1\r\nContent-Length-X: 1\r\nX-Transfer: 2\r\n\r\n"); /* only exact names */
    OK_INDEX("GET / HTTP/1.1\r\nHost:\r\n\r\n");                       /* an empty value is fine */

    /* Bytes after the blank line are not the parser's business. */
    CHECK(parse_exact(tail, sizeof tail - 1, &r) == HTTP_OK);
    CHECK(r.route == HTTP_ROUTE_INDEX && r.head_len == strlen("GET / HTTP/1.1\r\n\r\n"));
}

static void need_more(void)
{
    http_req r;

    CHECK(parse_exact("", 0, &r) == HTTP_NEED_MORE);
    CHECK(parse_exact("GET / HTTP/1.1\r\n", 16, &r) == HTTP_NEED_MORE);
    CHECK(parse_exact("GET / HTTP/1.1\r\nHost: x\r\n", 25, &r) == HTTP_NEED_MORE);
    CHECK(parse_exact("GET / HTTP/1.1\r", 15, &r) == HTTP_NEED_MORE);
}

static void refused_methods_and_versions(void)
{
    ERR("POST / HTTP/1.1\r\n\r\n", 405);
    ERR("PUT / HTTP/1.1\r\n\r\n", 405);
    ERR("DELETE / HTTP/1.1\r\n\r\n", 405);
    ERR("HEAD / HTTP/1.1\r\n\r\n", 405);
    ERR("OPTIONS * HTTP/1.1\r\n\r\n", 405);
    ERR("get / HTTP/1.1\r\n\r\n", 405);
    ERR("GETS / HTTP/1.1\r\n\r\n", 405);
    ERR("GE / HTTP/1.1\r\n\r\n", 405);
    ERR("G@T / HTTP/1.1\r\n\r\n", 400); /* not even a method */

    ERR("GET / HTTP/2.0\r\n\r\n", 505);
    ERR("GET / HTTP/0.9\r\n\r\n", 505);
    ERR("GET / HTTP/1.2\r\n\r\n", 505);
    ERR("GET / HTTP/1.9\r\n\r\n", 505);
    ERR("GET / HTTP/9.9\r\n\r\n", 505);
}

static void refused_request_lines(void)
{
    ERR(" / HTTP/1.1\r\n\r\n", 400);
    ERR("GET\r\n\r\n", 400);
    ERR("GET /\r\n\r\n", 400);
    ERR("GET  / HTTP/1.1\r\n\r\n", 400);
    ERR("GET / HTTP/1.1 \r\n\r\n", 400);
    ERR("GET / x HTTP/1.1\r\n\r\n", 400);
    ERR("GET * HTTP/1.1\r\n\r\n", 400);
    ERR("GET http://example.com/ HTTP/1.1\r\n\r\n", 400);
    ERR("GET index.html HTTP/1.1\r\n\r\n", 400);
    ERR("GET / http/1.1\r\n\r\n", 400);
    ERR("GET / HTTP/1.1x\r\n\r\n", 400);
    ERR("GET / HTTP/1\r\n\r\n", 400);
    ERR("GET / HTTP/11.1\r\n\r\n", 400);
    ERR("GET / HTTP/1.x\r\n\r\n", 400);
    ERR("\r\nGET / HTTP/1.1\r\n\r\n", 400); /* leading blank line */

    /* Line endings. */
    ERR("GET / HTTP/1.1\n\n", 400);
    ERR("GET / HTTP/1.1\r\n\n", 400);
    ERR("GET / HTTP/1.1\r\nHost: x\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nHost: a\rb\r\n\r\n", 400);

    /* Bytes that do not belong in a request line. */
    ERR("GET /\x01 HTTP/1.1\r\n\r\n", 400);
    ERR("GET /\x7F HTTP/1.1\r\n\r\n", 400);
    ERR("GET /\xC3\xA9 HTTP/1.1\r\n\r\n", 400);
    ERR("GET /\t HTTP/1.1\r\n\r\n", 400);
    ERR("GET /\0 HTTP/1.1\r\n\r\n", 400);
}

static void refused_headers(void)
{
    ERR("GET / HTTP/1.1\r\nHost x\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\n: v\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nHost: x\r\n continued\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nHost: x\r\n\tcontinued\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\ncontent-LENGTH: 5\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nTRANSFER-encoding: x\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nX: a\x01" "b\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nX: a\x7F" "b\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nX: a\0b\r\n\r\n", 400);
    ERR("GET / HTTP/1.1\r\nX\0Y: a\r\n\r\n", 400);
}

/* A request line, then n headers "H<i>: v", then the blank line. */
static size_t with_headers(char* out, size_t cap, unsigned n)
{
    int len = snprintf(out, cap, "GET / HTTP/1.1\r\n");

    CHECK(len > 0 && (size_t)len < cap);
    for (unsigned i = 0; i < n; i++) {
        int w = snprintf(out + len, cap - (size_t)len, "H%u: v\r\n", i);

        CHECK(w > 0 && (size_t)w < cap - (size_t)len);
        len += w;
    }
    CHECK((size_t)len + 2 < cap);
    out[len++] = '\r';
    out[len++] = '\n';
    return (size_t)len;
}

/* "GET /" plus fill 'a' bytes, then tail. */
static size_t target_request(char* out, size_t cap, size_t fill, const char* tail)
{
    size_t tail_len = strlen(tail);
    size_t n = 5 + fill + tail_len;

    CHECK(n < cap);
    (void)memcpy(out, "GET /", 5);
    (void)memset(out + 5, 'a', fill);
    (void)memcpy(out + 5 + fill, tail, tail_len + 1);
    return n;
}

static void limits(void)
{
    static char big[4096];
    size_t      n;
    http_req    r;

    /* Headers: 32 pass, 33 do not. */
    n = with_headers(big, sizeof big, HTTP_MAX_HEADERS);
    expect_n(big, n, HTTP_OK, HTTP_ROUTE_INDEX);
    n = with_headers(big, sizeof big, HTTP_MAX_HEADERS + 1);
    expect_n(big, n, 431, HTTP_ROUTE_NONE);

    /* Target: 512 bytes (the '/' and 511 more) pass, 513 do not. */
    n = target_request(big, sizeof big, HTTP_MAX_TARGET - 1, " HTTP/1.1\r\n\r\n");
    expect_n(big, n, HTTP_OK, HTTP_ROUTE_NONE);
    n = target_request(big, sizeof big, HTTP_MAX_TARGET, " HTTP/1.1\r\n\r\n");
    expect_n(big, n, 414, HTTP_ROUTE_NONE);

    /* A request line that never ends: waits until 600 bytes, then 414. */
    n = target_request(big, sizeof big, 599 - 5, "");
    CHECK(n == 599 && parse_exact(big, n, &r) == HTTP_NEED_MORE);
    n = target_request(big, sizeof big, 600 - 5, "");
    CHECK(n == 600 && parse_exact(big, n, &r) == 414);

    /* Whole head: exactly 2048 bytes passes; one byte more does not. */
    for (unsigned extra = 0; extra < 2; extra++) {
        int len = snprintf(big, sizeof big, "GET / HTTP/1.1\r\nX: ");
        size_t pad = 2048 - 16 - 3 - 2 - 2 + extra;

        CHECK(len == 19);
        (void)memset(big + len, 'v', pad);
        len += (int)pad;
        big[len++] = '\r';
        big[len++] = '\n';
        big[len++] = '\r';
        big[len++] = '\n';
        CHECK((size_t)len == 2048 + extra);
        if (extra == 0) {
            expect_n(big, (size_t)len, HTTP_OK, HTTP_ROUTE_INDEX);
        } else {
            expect_n(big, (size_t)len, 431, HTTP_ROUTE_NONE);
        }
    }

    /* One header line that never ends. */
    (void)memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
    (void)memset(big + 19, 'v', 3000);
    CHECK(parse_exact(big, 19 + 3000, &r) == 431);
    CHECK(parse_exact(big, 2047, &r) == HTTP_NEED_MORE);
    CHECK(parse_exact(big, 2048, &r) == 431);
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

/* Whatever the input, the answer is one of the defined ones, a valid result is sane, and the
 * heap copy is exactly as long as the input (so any overread is caught by the sanitizers). */
static void fuzz(void)
{
    static const char SEED[] = "GET /status.json?x=1 HTTP/1.1\r\nHost: greenthumb.local\r\n"
                               "User-Agent: test\r\nAccept: */*\r\n\r\n";
    static char       buf[3100];
    unsigned          prefix_checks = 0;

    for (unsigned i = 0; i < 20000; i++) {
        size_t   n;
        http_req r = {HTTP_ROUTE_NONE, 0};
        int      st;
        bool     mutated = (i % 3) != 0;

        if (!mutated) { /* random bytes */
            n = (size_t)(rnd() % 3000);
            for (size_t k = 0; k < n; k++) {
                buf[k] = (char)(rnd() & 0xFF);
            }
        } else { /* the valid request with a few bytes changed, some cut, some appended */
            n = sizeof SEED - 1;
            (void)memcpy(buf, SEED, n);
            for (unsigned m = (unsigned)(rnd() % 5); m > 0; m--) {
                size_t at = (size_t)(rnd() % n);
                char   specials[] = {'\r', '\n', ' ', ':', '\0', '/', '?', '\x01', '\x7F', '\xFF'};

                buf[at] = (rnd() & 1) ? specials[rnd() % sizeof specials] : (char)(rnd() & 0xFF);
            }
            if ((rnd() & 3) == 0) {
                n = (size_t)(rnd() % n);
            }
            if ((rnd() & 7) == 0) {
                size_t extra = (size_t)(rnd() % 200);

                for (size_t k = 0; k < extra && n + k < sizeof buf; k++) {
                    buf[n + k] = (char)(rnd() & 0xFF);
                }
                n += extra;
            }
        }
        st = parse_exact(buf, n, &r);
        CHECK(st == HTTP_NEED_MORE || st == HTTP_OK || st == 400 || st == 405 || st == 414 ||
              st == 431 || st == 505);
        if (st == HTTP_OK) {
            CHECK(r.route == HTTP_ROUTE_NONE || r.route == HTTP_ROUTE_INDEX ||
                  r.route == HTTP_ROUTE_STATUS);
            CHECK(r.head_len >= 18 && r.head_len <= n);
        }
        if (mutated && n <= 200 && prefix_checks < 400) { /* splitting never changes the answer */
            prefix_checks++;
            for (size_t k = 0; k < n; k++) {
                http_req p;
                int      pr = parse_exact(buf, k, &p);

                CHECK(pr == HTTP_NEED_MORE || pr == st);
            }
        }
    }
}

static bool has(const char* text, const char* needle)
{
    return strstr(text, needle) != NULL;
}

static void responses(void)
{
    static const int CODES[] = {400, 404, 405, 408, 414, 431, 500, 503, 505};
    static const char* const LINES[] = {
        "HTTP/1.1 400 Bad Request\r\n",
        "HTTP/1.1 404 Not Found\r\n",
        "HTTP/1.1 405 Method Not Allowed\r\n",
        "HTTP/1.1 408 Request Timeout\r\n",
        "HTTP/1.1 414 URI Too Long\r\n",
        "HTTP/1.1 431 Request Header Fields Too Large\r\n",
        "HTTP/1.1 500 Internal Server Error\r\n",
        "HTTP/1.1 503 Service Unavailable\r\n",
        "HTTP/1.1 505 HTTP Version Not Supported\r\n",
    };
    char buf[1024];
    int  n;

    n = http_build(buf, sizeof buf, 200, "text/html; charset=utf-8", "<p>hi</p>", 9);
    CHECK(n > 0 && (size_t)n == strlen(buf));
    CHECK(strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(has(buf, "Content-Type: text/html; charset=utf-8\r\n"));
    CHECK(has(buf, "Content-Length: 9\r\n"));
    CHECK(has(buf, "Cache-Control: no-store\r\n"));
    CHECK(has(buf, "X-Content-Type-Options: nosniff\r\n"));
    CHECK(has(buf, "Connection: close\r\n"));
    CHECK(!has(buf, "Allow:"));
    CHECK(strcmp(buf + n - 13, "\r\n\r\n<p>hi</p>") == 0);

    for (size_t i = 0; i < sizeof CODES / sizeof CODES[0]; i++) {
        const char* body = http_error_body(CODES[i]);
        char        want[32];

        CHECK(body != NULL && body[0] != '\0');
        n = http_build(buf, sizeof buf, CODES[i], "text/plain; charset=utf-8", body, strlen(body));
        CHECK(n > 0);
        CHECK(strncmp(buf, LINES[i], strlen(LINES[i])) == 0);
        CHECK(snprintf(want, sizeof want, "Content-Length: %zu\r\n", strlen(body)) > 0);
        CHECK(has(buf, want));
        CHECK(has(buf, "\r\n\r\n") && strcmp(strstr(buf, "\r\n\r\n") + 4, body) == 0);
        CHECK(has(buf, "Allow: GET\r\n") == (CODES[i] == 405));
    }

    /* Statuses we do not send. */
    for (int code = 0; code < 1000; code++) {
        bool known = code == 200;

        for (size_t i = 0; i < sizeof CODES / sizeof CODES[0]; i++) {
            known = known || code == CODES[i];
        }
        n = http_build(buf, sizeof buf, code, "text/plain", "x", 1);
        CHECK(known ? n > 0 : (n == -1 && buf[0] == '\0'));
        CHECK(known || http_error_body(code) == NULL);
    }
    CHECK(http_error_body(200) == NULL);

    /* Arguments. */
    CHECK(http_build(buf, sizeof buf, 200, NULL, "x", 1) == -1 && buf[0] == '\0');
    CHECK(http_build(buf, sizeof buf, 200, "text/plain\r\nX-Evil: 1", "x", 1) == -1);
    CHECK(http_build(buf, sizeof buf, 200, "text/plain\x01", "x", 1) == -1);
    CHECK(http_build(buf, sizeof buf, 200, "text/plain", NULL, 5) == -1);
    n = http_build(buf, sizeof buf, 200, "text/plain", NULL, 0);
    CHECK(n > 0 && has(buf, "Content-Length: 0\r\n") && strcmp(buf + n - 4, "\r\n\r\n") == 0);
    CHECK(http_build(NULL, 0, 200, "text/plain", "x", 1) == -1);

    /* A body is copied as bytes, a NUL included. */
    n = http_build(buf, sizeof buf, 200, "application/octet-stream", "a\0b", 3);
    CHECK(n > 3 && memcmp(buf + n - 3, "a\0b", 3) == 0);
}

/* Every too-small buffer is refused, left empty, and nothing is written past it. */
static void response_buffer_limits(void)
{
    char  full[1024];
    int   len = http_build(full, sizeof full, 405, "text/plain", "Method Not Allowed\n", 19);

    CHECK(len > 0);
    for (size_t cap = 0; cap <= (size_t)len + 1; cap++) {
        char buf[1100];
        int  n;

        (void)memset(buf, 0x5A, sizeof buf);
        n = http_build(buf, cap, 405, "text/plain", "Method Not Allowed\n", 19);
        for (size_t i = cap; i < sizeof buf; i++) {
            CHECK(buf[i] == 0x5A);
        }
        if (cap < (size_t)len + 1) {
            CHECK(n == -1);
            CHECK(cap == 0 || buf[0] == '\0');
        } else {
            CHECK(n == len && memcmp(buf, full, (size_t)len + 1) == 0);
        }
    }
}

static struct sockaddr_in v4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    struct sockaddr_in sin;

    (void)memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(40000);
    sin.sin_addr.s_addr = htonl((uint32_t)(a << 24 | b << 16 | c << 8 | d));
    return sin;
}

static bool peer(unsigned a, unsigned b, unsigned c, unsigned d)
{
    struct sockaddr_in sin = v4(a, b, c, d);

    return http_peer_allowed((const struct sockaddr*)&sin, sizeof sin);
}

static void peers(void)
{
    struct sockaddr_in         sin = v4(192, 168, 1, 50);
    struct sockaddr_in6        sin6;
    struct sockaddr_storage    ss;

    /* Allowed: both ends of every private range. */
    CHECK(peer(127, 0, 0, 0) && peer(127, 0, 0, 1) && peer(127, 255, 255, 255));
    CHECK(peer(10, 0, 0, 0) && peer(10, 1, 2, 3) && peer(10, 255, 255, 255));
    CHECK(peer(172, 16, 0, 0) && peer(172, 16, 0, 1) && peer(172, 31, 255, 255));
    CHECK(peer(192, 168, 0, 0) && peer(192, 168, 1, 50) && peer(192, 168, 255, 255));
    CHECK(peer(169, 254, 0, 0) && peer(169, 254, 255, 255));

    /* Refused: just outside each range, and everything else. */
    CHECK(!peer(0, 0, 0, 0) && !peer(0, 1, 2, 3));
    CHECK(!peer(1, 1, 1, 1) && !peer(8, 8, 8, 8));
    CHECK(!peer(9, 255, 255, 255) && !peer(11, 0, 0, 0));
    CHECK(!peer(126, 255, 255, 255) && !peer(128, 0, 0, 0));
    CHECK(!peer(100, 64, 0, 1) && !peer(100, 127, 255, 255)); /* carrier-grade NAT, Tailscale */
    CHECK(!peer(172, 0, 0, 1) && !peer(172, 15, 255, 255) && !peer(172, 32, 0, 0));
    CHECK(!peer(192, 167, 255, 255) && !peer(192, 169, 0, 0) && !peer(192, 0, 2, 1));
    CHECK(!peer(169, 253, 255, 255) && !peer(169, 255, 0, 0));
    CHECK(!peer(224, 0, 0, 1) && !peer(239, 255, 255, 255) && !peer(240, 0, 0, 1));
    CHECK(!peer(255, 255, 255, 255));

    /* Not an IPv4 address, or not a usable one. */
    CHECK(http_peer_allowed((const struct sockaddr*)&sin, sizeof sin));
    CHECK(!http_peer_allowed(NULL, sizeof sin));
    CHECK(!http_peer_allowed((const struct sockaddr*)&sin, 0));
    CHECK(!http_peer_allowed((const struct sockaddr*)&sin, sizeof sin - 1));
    (void)memset(&sin6, 0, sizeof sin6);
    sin6.sin6_family = AF_INET6;
    sin6.sin6_addr = in6addr_loopback;
    CHECK(!http_peer_allowed((const struct sockaddr*)&sin6, sizeof sin6));
    (void)memset(&ss, 0, sizeof ss);
    ss.ss_family = AF_UNIX;
    CHECK(!http_peer_allowed((const struct sockaddr*)&ss, sizeof ss));
    ss.ss_family = AF_UNSPEC;
    CHECK(!http_peer_allowed((const struct sockaddr*)&ss, sizeof ss));
    sin = v4(10, 0, 0, 1); /* a longer buffer holding an IPv4 address is fine */
    (void)memcpy(&ss, &sin, sizeof sin);
    CHECK(http_peer_allowed((const struct sockaddr*)&ss, sizeof ss));
}

int main(void)
{
    valid_requests();
    need_more();
    refused_methods_and_versions();
    refused_request_lines();
    refused_headers();
    limits();
    fuzz();
    responses();
    response_buffer_limits();
    peers();
    (void)puts("PASS");
    return 0;
}
