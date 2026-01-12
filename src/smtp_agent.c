/* src/smtp_agent.c */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "include/sans.h"

/* Maximum buffer sizes */
#define SMALL_BUF   1024   /* for addresses, file paths, etc. */
#define CMDBUF      2048   /* for SMTP command strings */
#define RECV_BUF    1024   /* smaller buffer for responses */
#define SEND_CHUNK  4096

/* Trim trailing whitespace/newline */
static void trim_trailing(char *str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r' || str[len - 1] == ' '))
        str[--len] = '\0';
}

int smtp_agent(const char* host, int port) {
    char sender[SMALL_BUF];
    char filepath[SMALL_BUF];

    /* Read sender and filepath from stdin */
    if (scanf("%1023s %1023s", sender, filepath) != 2) {
        fprintf(stderr, "Failed to read sender and filepath\n");
        return 1;
    }
    trim_trailing(sender);

    /* Connect to server */
    int conn = sans_connect(host, port, IPPROTO_RUDP);
    if (conn < 0) {
        fprintf(stderr, "Connection failed\n");
        return 1;
    }

    char sendbuf[CMDBUF];
    char recvbuf[RECV_BUF];
    int rc;

    /* Receive initial banner */
    rc = sans_recv_pkt(conn, recvbuf, sizeof(recvbuf) - 1);
    if (rc <= 0) goto fail;
    recvbuf[rc] = '\0';

    /* 1) HELO */
    snprintf(sendbuf, sizeof(sendbuf), "HELO localhost\r\n");
    if (sans_send_pkt(conn, sendbuf, strlen(sendbuf)) < 0) goto fail;
    rc = sans_recv_pkt(conn, recvbuf, sizeof(recvbuf) - 1);
    if (rc <= 0) goto fail;
    recvbuf[rc] = '\0';
    if (strncmp(recvbuf, "250", 3) != 0) goto fail;

    /* 2) MAIL FROM */
    snprintf(sendbuf, sizeof(sendbuf), "MAIL FROM:<%s>\r\n", sender);
    if (sans_send_pkt(conn, sendbuf, strlen(sendbuf)) < 0) goto fail;
    rc = sans_recv_pkt(conn, recvbuf, sizeof(recvbuf) - 1);
    if (rc <= 0) goto fail;
    recvbuf[rc] = '\0';

    /* 3) RCPT TO */
    snprintf(sendbuf, sizeof(sendbuf), "RCPT TO:<%s>\r\n", sender);
    if (sans_send_pkt(conn, sendbuf, strlen(sendbuf)) < 0) goto fail;
    rc = sans_recv_pkt(conn, recvbuf, sizeof(recvbuf) - 1);
    if (rc <= 0) goto fail;
    recvbuf[rc] = '\0';

    /* 4) DATA */
    snprintf(sendbuf, sizeof(sendbuf), "DATA\r\n");
    if (sans_send_pkt(conn, sendbuf, strlen(sendbuf)) < 0) goto fail;

    /* Wait for 354 before sending body */
    rc = sans_recv_pkt(conn, recvbuf, sizeof(recvbuf) - 1);
    if (rc <= 0) goto fail;
    recvbuf[rc] = '\0';
    if (strncmp(recvbuf, "354", 3) != 0) goto fail;

    /* 5) Send email body */
    FILE *f = fopen(filepath, "rb");
    if (!f) goto fail;

    char filebuf[SEND_CHUNK];
    size_t nread;
    int last_byte = -1;

    while ((nread = fread(filebuf, 1, sizeof(filebuf), f)) > 0) {
        if (sans_send_pkt(conn, filebuf, nread) < 0) {
            fclose(f);
            goto fail;
        }
        last_byte = (unsigned char)filebuf[nread - 1];
    }
    fclose(f);

    /* Ensure exactly one CRLF before termination */
    if (last_byte != '\n') {
        const char *crlf = "\r\n";
        if (sans_send_pkt(conn, crlf, strlen(crlf)) < 0) goto fail;
    }

    /* 6) DATA termination string */
    const char *term = ".\r\n";
    if (sans_send_pkt(conn, term, strlen(term)) < 0) goto fail;

    /* Wait for 250 OK after DATA */
    rc = sans_recv_pkt(conn, recvbuf, sizeof(recvbuf) - 1);
    if (rc <= 0) goto fail;
    recvbuf[rc] = '\0';
    printf("%s\n", recvbuf);

    /* 7) QUIT */
    snprintf(sendbuf, sizeof(sendbuf), "QUIT\r\n");
    if (sans_send_pkt(conn, sendbuf, strlen(sendbuf)) < 0) goto fail;

    /* Wait for final 221 response */
    rc = sans_recv_pkt(conn, recvbuf, sizeof(recvbuf) - 1);
    if (rc > 0) {
        recvbuf[rc] = '\0';
    }

    sans_disconnect(conn);
    return 0;

fail:
    sans_disconnect(conn);
    return 1;
}
