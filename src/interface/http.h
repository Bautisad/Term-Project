/* http.h: what the LAN page accepts and says on the wire.  A strict request parser, a response
 * builder and the check that a connection comes from a private IPv4 address.  No sockets, no
 * allocation and no logging here: the daemon (main.c) reads and writes, and logs what these
 * return.
 *
 * Only GET of "/", "/index.html" and "/status.json" is served.  Anything odd is refused rather
 * than guessed at.  Every response closes the connection (no keep-alive). */
#ifndef GT_HTTP_H
#define GT_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

#define HTTP_MAX_REQUEST_BYTES 2048u /* the request line, headers and the blank line */
#define HTTP_MAX_TARGET        512u  /* bytes of request target, including the leading '/' */
#define HTTP_MAX_HEADERS       32u

/* http_parse results: these two, or the HTTP status the caller should send. */
#define HTTP_NEED_MORE 0
#define HTTP_OK        200

typedef enum http_route {
    HTTP_ROUTE_NONE = 0, /* a well-formed request for something we do not serve: answer 404 */
    HTTP_ROUTE_INDEX,    /* "/" or "/index.html": the HTML page */
    HTTP_ROUTE_STATUS    /* "/status.json" */
} http_route;

typedef struct http_req {
    http_route route;
    size_t     head_len; /* bytes up to and including the blank line */
} http_req;

/* Parse the start of a request.  buf is not NUL-terminated and may hold only part of the
 * request (call again with more bytes after HTTP_NEED_MORE; the answer for a given request does
 * not depend on how it was split).  Bytes after the blank line are ignored.
 *   HTTP_NEED_MORE  not enough bytes yet
 *   HTTP_OK         a valid GET; *out is filled in
 *   400             malformed: bare LF or CR, control or non-ASCII bytes in the request line,
 *                   a bad target, a bad header, obsolete line folding, a request body
 *                   (Content-Length or Transfer-Encoding)
 *   405             a method other than GET
 *   414             target longer than HTTP_MAX_TARGET, or a request line over 600 bytes
 *   431             more than HTTP_MAX_HEADERS headers, or no end of the headers within
 *                   HTTP_MAX_REQUEST_BYTES
 *   505             an HTTP version other than 1.0 and 1.1
 * *out is written only for HTTP_OK. */
int http_parse(const char* buf, size_t len, http_req* out);

/* Build a complete response in buf (cap bytes): status line, Content-Type, Content-Length,
 * Cache-Control: no-store, X-Content-Type-Options: nosniff, Connection: close (and Allow: GET
 * on 405), then body.  Returns the length, or -1 if cap is too small, the status is not one of
 * 200 400 404 405 408 414 431 500 503 505, content_type is NULL or holds a control byte, or
 * body is NULL with body_len > 0; then buf holds an empty string (if cap > 0). */
int http_build(char* buf, size_t cap, int status, const char* content_type, const char* body,
               size_t body_len);

/* A short text/plain body for an error status, or NULL if status is not an error we send. */
const char* http_error_body(int status);

/* True only for an AF_INET peer in 127/8, 10/8, 172.16/12, 192.168/16 or 169.254/16.  Refuses
 * everything else: other families, NULL, a length too short for sockaddr_in, 0/8, 100.64/10
 * (carrier-grade NAT, which includes Tailscale), multicast, broadcast and public addresses. */
bool http_peer_allowed(const struct sockaddr* sa, socklen_t len);

#endif /* GT_HTTP_H */
