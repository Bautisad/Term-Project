#include "http.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_REQUEST_LINE 600u /* "GET " + a 512 byte target + " HTTP/1.1" is 525 */

/* Where a line ends. */
typedef enum eol_result {
    EOL_FOUND,    /* *cr is the index of the CR of the CRLF */
    EOL_BARE_LF,  /* a LF with no CR before it */
    EOL_NONE      /* no line end in the first `limit` bytes */
} eol_result;

/* Looks for CRLF in buf[from, limit), limit <= len.  A line is only ever judged when its CRLF is
 * there (or the limit is reached), so the result does not depend on how the bytes arrived. */
static eol_result find_eol(const char* buf, size_t from, size_t limit, size_t* cr)
{
    for (size_t i = from; i < limit; i++) {
        if (buf[i] == '\n') {
            if (i > from && buf[i - 1] == '\r') {
                *cr = i - 1;
                return EOL_FOUND;
            }
            return EOL_BARE_LF;
        }
    }
    return EOL_NONE;
}

static size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

static bool is_letter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_tchar(char c)
{
    /* strchr would match the terminating NUL, so rule it out first */
    return is_letter(c) || (c >= '0' && c <= '9') || (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* name[0, n) equals the lower-case string `want`, ignoring case. */
static bool name_is(const char* name, size_t n, const char* want)
{
    if (strlen(want) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (lower(name[i]) != want[i]) {
            return false;
        }
    }
    return true;
}

/* The route for the part of the target before any '?'. */
static http_route route_of(const char* target, size_t n)
{
    size_t path = 0;

    while (path < n && target[path] != '?') {
        path++;
    }
    if ((path == 1 && target[0] == '/') ||
        (path == strlen("/index.html") && memcmp(target, "/index.html", path) == 0)) {
        return HTTP_ROUTE_INDEX;
    }
    if (path == strlen("/status.json") && memcmp(target, "/status.json", path) == 0) {
        return HTTP_ROUTE_STATUS;
    }
    return HTTP_ROUTE_NONE;
}

/* Judge the request line line[0, n).  Returns HTTP_OK (with *route) or an error status. */
static int judge_request_line(const char* line, size_t n, http_route* route)
{
    size_t sp1 = 0;
    size_t sp2;
    size_t tlen;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)line[i];

        if (c < 0x20 || c > 0x7E) { /* NUL, control bytes, bare CR or LF, DEL, non-ASCII */
            return 400;
        }
    }
    while (sp1 < n && line[sp1] != ' ') {
        sp1++;
    }
    if (sp1 == 0 || sp1 >= n) {
        return 400;
    }
    sp2 = sp1 + 1;
    while (sp2 < n && line[sp2] != ' ') {
        sp2++;
    }
    if (sp2 == sp1 + 1 || sp2 >= n || sp2 + 1 >= n) { /* empty target, no second space, no version */
        return 400;
    }
    if (memchr(line + sp2 + 1, ' ', n - sp2 - 1) != NULL) {
        return 400;
    }
    for (size_t i = 0; i < sp1; i++) {
        if (!is_letter(line[i])) {
            return 400;
        }
    }
    if (sp1 != 3 || memcmp(line, "GET", 3) != 0) {
        return 405;
    }
    tlen = sp2 - sp1 - 1;
    if (line[sp1 + 1] != '/') {
        return 400;
    }
    if (tlen > HTTP_MAX_TARGET) {
        return 414;
    }
    {
        const char* v = line + sp2 + 1;
        size_t      vlen = n - sp2 - 1;

        if (vlen != 8 || memcmp(v, "HTTP/", 5) != 0 || v[5] < '0' || v[5] > '9' || v[6] != '.' ||
            v[7] < '0' || v[7] > '9') {
            return 400;
        }
        if (v[5] != '1' || (v[7] != '0' && v[7] != '1')) {
            return 505;
        }
    }
    *route = route_of(line + sp1 + 1, tlen);
    return HTTP_OK;
}

/* Judge one header line[0, n).  Returns HTTP_OK or 400. */
static int judge_header(const char* line, size_t n)
{
    size_t colon = 0;

    if (n == 0 || line[0] == ' ' || line[0] == '\t') { /* folded continuation line */
        return 400;
    }
    while (colon < n && line[colon] != ':') {
        if (!is_tchar(line[colon])) {
            return 400;
        }
        colon++;
    }
    if (colon == 0 || colon >= n) {
        return 400;
    }
    for (size_t i = colon + 1; i < n; i++) {
        unsigned char c = (unsigned char)line[i];

        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            return 400;
        }
    }
    if (name_is(line, colon, "content-length") || name_is(line, colon, "transfer-encoding")) {
        return 400; /* a GET page takes no body */
    }
    return HTTP_OK;
}

int http_parse(const char* buf, size_t len, http_req* out)
{
    size_t     limit = min_size(len, MAX_REQUEST_LINE);
    size_t     cr = 0;
    size_t     pos;
    http_route route = HTTP_ROUTE_NONE;
    unsigned   headers = 0;
    int        r;

    switch (find_eol(buf, 0, limit, &cr)) {
    case EOL_BARE_LF:
        return 400;
    case EOL_NONE:
        return len >= MAX_REQUEST_LINE ? 414 : HTTP_NEED_MORE;
    case EOL_FOUND:
        break;
    }
    r = judge_request_line(buf, cr, &route);
    if (r != HTTP_OK) {
        return r;
    }
    pos = cr + 2;

    for (;;) {
        limit = min_size(len, HTTP_MAX_REQUEST_BYTES);
        switch (find_eol(buf, pos, limit, &cr)) {
        case EOL_BARE_LF:
            return 400;
        case EOL_NONE:
            return len >= HTTP_MAX_REQUEST_BYTES ? 431 : HTTP_NEED_MORE;
        case EOL_FOUND:
            break;
        }
        if (cr == pos) { /* the blank line */
            out->route = route;
            out->head_len = cr + 2;
            return HTTP_OK;
        }
        if (++headers > HTTP_MAX_HEADERS) {
            return 431;
        }
        r = judge_header(buf + pos, cr - pos);
        if (r != HTTP_OK) {
            return r;
        }
        pos = cr + 2;
    }
}

typedef struct status_text {
    int         code;
    const char* reason;
    const char* body;
} status_text;

static const status_text STATUSES[] = {
    {200, "OK", NULL},
    {400, "Bad Request", "Bad Request\n"},
    {404, "Not Found", "Not Found\n"},
    {405, "Method Not Allowed", "Method Not Allowed\n"},
    {408, "Request Timeout", "Request Timeout\n"},
    {414, "URI Too Long", "URI Too Long\n"},
    {431, "Request Header Fields Too Large", "Request Header Fields Too Large\n"},
    {500, "Internal Server Error", "Internal Server Error\n"},
    {503, "Service Unavailable", "Service Unavailable\n"},
    {505, "HTTP Version Not Supported", "HTTP Version Not Supported\n"},
};
#define N_STATUSES (sizeof STATUSES / sizeof STATUSES[0])

static const status_text* status_of(int code)
{
    for (size_t i = 0; i < N_STATUSES; i++) {
        if (STATUSES[i].code == code) {
            return &STATUSES[i];
        }
    }
    return NULL;
}

const char* http_error_body(int status)
{
    const status_text* s = status_of(status);

    return s != NULL ? s->body : NULL;
}

int http_build(char* buf, size_t cap, int status, const char* content_type, const char* body,
               size_t body_len)
{
    const status_text* s = status_of(status);
    int                n;

    if (cap == 0) {
        return -1;
    }
    if (cap > (size_t)INT_MAX) { /* the length is returned as an int */
        cap = (size_t)INT_MAX;
    }
    buf[0] = '\0';
    if (s == NULL || content_type == NULL || (body == NULL && body_len > 0)) {
        return -1;
    }
    for (const char* p = content_type; *p != '\0'; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7F) {
            return -1;
        }
    }
    n = snprintf(buf, cap,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Cache-Control: no-store\r\n"
                 "X-Content-Type-Options: nosniff\r\n"
                 "Connection: close\r\n"
                 "%s"
                 "\r\n",
                 s->code, s->reason, content_type, body_len, status == 405 ? "Allow: GET\r\n" : "");
    if (n < 0 || (size_t)n >= cap || body_len >= cap - (size_t)n) { /* room for the body and a NUL */
        buf[0] = '\0';
        return -1;
    }
    if (body_len > 0) {
        (void)memcpy(buf + n, body, body_len);
    }
    buf[(size_t)n + body_len] = '\0';
    return n + (int)body_len;
}

bool http_peer_allowed(const struct sockaddr* sa, socklen_t len)
{
    struct sockaddr_in in;
    uint32_t           a;

    if (sa == NULL || len < (socklen_t)sizeof in) {
        return false;
    }
    (void)memcpy(&in, sa, sizeof in);
    if (in.sin_family != AF_INET) {
        return false;
    }
    a = ntohl(in.sin_addr.s_addr);
    return (a >> 24) == 127u ||               /* 127.0.0.0/8 */
           (a >> 24) == 10u ||                /* 10.0.0.0/8 */
           (a >> 20) == 0xAC1u ||             /* 172.16.0.0/12 */
           (a >> 16) == 0xC0A8u ||            /* 192.168.0.0/16 */
           (a >> 16) == 0xA9FEu;              /* 169.254.0.0/16 */
}
