#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Provided by the assignment framework
// (Signatures assumed from the spec; adjust if your headers differ.)
int sans_accept(const char *iface, int port);
int sans_recv_pkt(int conn, void *buf, int maxlen);
int sans_send_pkt(int conn, const void *buf, int len);
void sans_disconnect(int conn);

// --- Helpers limited to allowed libc calls ---
static int path_has_traversal(const char *p) {
    // Reject any ".." path segment or backslash usage
    const char *s = p;
    while (*s) {
        if (*s == '\\') return 1;
        if (*s == '.') {
            int prev_boundary = (s == p) || (*(s-1) == '/');
            if (prev_boundary && s[0] == '.' && s[1] == '.') {
                int next_boundary = (s[2] == '\0') || (s[2] == '/') || (s[2] == '?');
                if (next_boundary) return 1;
            }
        }
        s++;
    }
    return 0;
}

static void sanitize_path(const char *in, char *out, size_t outsz) {
    const char *p = in;
    if (*p == '/') p++;
    if (*p == '\0') {
        snprintf(out, outsz, "index.html");
        return;
    }
    size_t i = 0;
    while (p[i] && p[i] != '?' && i + 1 < outsz) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

static int send_text_response(int conn, const char *status, const char *body) {
    char header[256];
    int body_len = (int)strlen(body);
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n",
        status, body_len);
    if (hdr_len < 0) return -1;
    if (hdr_len >= (int)sizeof(header)) hdr_len = (int)sizeof(header) - 1;
    header[hdr_len] = '\0';

    if (sans_send_pkt(conn, header, hdr_len) < 0) return -1;
    if (body_len > 0) {
        if (sans_send_pkt(conn, body, body_len) < 0) return -1;
    }
    return 0;
}

int http_server(const char* iface, int port) {
    int conn = sans_accept(iface, port);
    if (conn < 0) {
        return -1;
    }

    char req[1024];
    memset(req, 0, sizeof(req));  // zero to avoid uninitialized read warnings

    int n = sans_recv_pkt(conn, req, (int)sizeof(req) - 1);
    if (n <= 0) {
        send_text_response(conn, "400 Bad Request", "Bad Request\n");
        sans_disconnect(conn);
        return -1;
    }
    if (n < (int)sizeof(req)) req[n] = '\0'; else req[sizeof(req)-1] = '\0';

    char method[8] = {0};
    char raw_path[512] = {0};
    char version[16] = {0};

    if (sscanf(req, "%7s %511s %15s", method, raw_path, version) != 3) {
        send_text_response(conn, "400 Bad Request", "Bad Request\n");
        sans_disconnect(conn);
        return -1;
    }

    if (strcmp(method, "GET") != 0) {
        send_text_response(conn, "405 Method Not Allowed", "Method Not Allowed\n");
        sans_disconnect(conn);
        return 0;
    }

    if (strncmp(version, "HTTP/1.1", 8) != 0) {
        send_text_response(conn, "400 Bad Request", "Bad Request\n");
        sans_disconnect(conn);
        return -1;
    }

    if (path_has_traversal(raw_path)) {
        send_text_response(conn, "403 Forbidden", "Forbidden\n");
        sans_disconnect(conn);
        return 0;
    }

    char safe_path[512];
    sanitize_path(raw_path, safe_path, sizeof(safe_path));

    struct stat st;
    if (stat(safe_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_text_response(conn, "404 Not Found", "Not Found\n");
        sans_disconnect(conn);
        return 0;
    }

    long content_len = (long)st.st_size;

    char header[256];
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %ld\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n",
        content_len);
    if (hdr_len < 0) {
        sans_disconnect(conn);
        return -1;
    }
    if (hdr_len >= (int)sizeof(header)) hdr_len = (int)sizeof(header) - 1;
    header[hdr_len] = '\0';

    if (sans_send_pkt(conn, header, hdr_len) < 0) {
        sans_disconnect(conn);
        return -1;
    }

    FILE *fp = fopen(safe_path, "rb");
    if (!fp) {
        send_text_response(conn, "404 Not Found", "Not Found\n");
        sans_disconnect(conn);
        return 0;
    }

    char buf[1024];
    long remaining = content_len;
    while (remaining > 0) {
        size_t to_read = (remaining > (long)sizeof(buf)) ? sizeof(buf) : (size_t)remaining;
        size_t got = fread(buf, 1, to_read, fp);
        if (got == 0) {
            break;
        }
        if (sans_send_pkt(conn, buf, (int)got) < 0) {
            break;
        }
        remaining -= (long)got;
    }

    fclose(fp);
    sans_disconnect(conn);
    return 0;
}
