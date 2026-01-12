#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <netinet/in.h>
#include "include/sans.h"

#define BUF_SIZE 1024

// Case-insensitive substring search (portable replacement for strcasestr)
static char *strcasestr_portable(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h = haystack + 1;
            const char *n = needle + 1;
            while (*n && *h && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                h++;
                n++;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}

int http_client(const char* host, int port) {
    char method[16];
    char path[256];
    char request[BUF_SIZE];
    char buffer[BUF_SIZE];
    char header_buf[BUF_SIZE * 10];   // enough for headers
    int sockfd;
    int bytes_received;
    int content_length = -1;   // -1 = unknown
    int header_done = 0;
    int body_received = 0;
    int header_len = 0;

    // Prompt user for method
    printf("Enter HTTP method (only GET supported): ");
    if (scanf("%15s", method) != 1) {
        fprintf(stderr, "Failed to read method.\n");
        return -1;
    }

    if (strcmp(method, "GET") != 0) {
        fprintf(stderr, "Only GET is supported.\n");
        return -1;
    }

    // Prompt user for path (no leading /)
    printf("Enter path (without leading /): ");
    if (scanf("%255s", path) != 1) {
        fprintf(stderr, "Failed to read path.\n");
        return -1;
    }

    // Connect to host:port using TCP
    sockfd = sans_connect(host, port, IPPROTO_TCP);
    if (sockfd < 0) {
        fprintf(stderr, "Connection failed.\n");
        return -1;
    }

    // Build HTTP request
    snprintf(request, sizeof(request),
             "%s /%s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "User-Agent: sans/1.0\r\n"
             "Cache-Control: no-cache\r\n"
             "Connection: close\r\n"
             "Accept: */*\r\n"
             "\r\n",
             method, path, host, port);

    // Send request
    if (sans_send_pkt(sockfd, request, strlen(request)) < 0) {
        fprintf(stderr, "Failed to send request.\n");
        sans_disconnect(sockfd);
        return -1;
    }

    
    while (!header_done && (bytes_received = sans_recv_pkt(sockfd, buffer, sizeof(buffer))) > 0) {
        if (header_len + bytes_received >= (int)sizeof(header_buf)) {
            fprintf(stderr, "Headers too large.\n");
            sans_disconnect(sockfd);
            return -1;
        }
        memcpy(header_buf + header_len, buffer, bytes_received);
        header_len += bytes_received;
        header_buf[header_len] = '\0';

        char *headers_end = strstr(header_buf, "\r\n\r\n");
        if (headers_end) {
            header_done = 1;
            headers_end += 4; // skip \r\n\r\n

            // Parse Content-Length
            char *cl = strcasestr_portable(header_buf, "Content-Length:");
            if (cl) {
                cl += 15;
                while (*cl == ' ' || *cl == '\t') cl++;
                content_length = (int)strtol(cl, NULL, 10);
            }

            // Print any body bytes already received
            int header_size = headers_end - header_buf;
            int body_len = header_len - header_size;
            if (body_len > 0) {
                fwrite(headers_end, 1, body_len, stdout);
                fflush(stdout);
                body_received += body_len;
            }
        }
    }

    
    while ((bytes_received = sans_recv_pkt(sockfd, buffer, sizeof(buffer))) > 0) {
        fwrite(buffer, 1, bytes_received, stdout);
        fflush(stdout);
        body_received += bytes_received;

        if (content_length > -1 && body_received >= content_length) {
            break;
        }
    }

    sans_disconnect(sockfd);
    return 0;
}
