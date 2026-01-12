#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/time.h>
#include "rudp.h"
#include "include/sans.h"

/* Define MAX_SOCKETS locally to avoid include dependency issues */
#ifndef MAX_SOCKETS
#define MAX_SOCKETS 10
#endif

/* `struct rudp_conn` and `rudp_conns` are declared in the header; define the
   actual array here (header provides the type). */
struct rudp_conn rudp_conns[MAX_SOCKETS];

int save_rudp_conn(int sockfd, struct sockaddr *addr, socklen_t addrlen);

int sans_connect(const char* host, int port, int protocol) {
    // --- TCP behavior (unchanged)
    if (protocol == IPPROTO_TCP) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        struct addrinfo hints, *res, *rp;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = protocol;

        if (getaddrinfo(host, port_str, &hints, &res) != 0)
            return -1;

        int sockfd = -1;
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sockfd == -1)
                continue;
            if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
                break;
            close(sockfd);
            sockfd = -1;
        }

        freeaddrinfo(res);
        return sockfd;
    }

    // --- RUDP behavior
    else if (protocol == IPPROTO_RUDP) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        if (getaddrinfo(host, port_str, &hints, &res) != 0)
            return -1;

        int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sockfd < 0) {
            freeaddrinfo(res);
            return -1;
        }

        struct timeval tv = {1, 0};
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Begin 3-way handshake
        rudp_packet_t syn = {SYN, 0};
        rudp_packet_t synack;
        rudp_packet_t ack = {ACK, 0};
        struct sockaddr_storage from;
        socklen_t fromlen = sizeof(from);

        // Send SYN
        sendto(sockfd, &syn, sizeof(rudp_packet_t), 0, res->ai_addr, res->ai_addrlen);

        // Retransmit on failure
        int retries = 3;
        for (int i = 0; i < retries; i++) {
            ssize_t n = recvfrom(sockfd, &synack, sizeof(rudp_packet_t), 0, (struct sockaddr *)&from, &fromlen);
            if (n > 0 && synack.type == (SYN | ACK)) {
                // Send final ACK
                sendto(sockfd, &ack, sizeof(rudp_packet_t), 0, (struct sockaddr *)&from, fromlen);
                save_rudp_conn(sockfd, (struct sockaddr *)&from, fromlen);
                return sockfd;
            }
            if (i < retries - 1) {
                sendto(sockfd, &syn, sizeof(rudp_packet_t), 0, res->ai_addr, res->ai_addrlen);
            }
        }

        close(sockfd);
        freeaddrinfo(res);
        return -1;
    }

    errno = EPROTONOSUPPORT;
    return -1;
}

int sans_accept(const char* iface, int port, int protocol) {
    if (protocol == IPPROTO_TCP) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        struct addrinfo hints, *res, *rp;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        hints.ai_protocol = protocol;

        if (getaddrinfo(iface, port_str, &hints, &res) != 0)
            return -1;

        int sockfd = -1;
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sockfd == -1)
                continue;

            int optval = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

            if (bind(sockfd, rp->ai_addr, rp->ai_addrlen) == 0 &&
                listen(sockfd, SOMAXCONN) == 0)
                break;

            close(sockfd);
            sockfd = -1;
        }

        freeaddrinfo(res);
        if (sockfd == -1)
            return -1;

        int client_fd = accept(sockfd, NULL, NULL);
        return client_fd;
    }

    /* -------------------------- RUDP BEHAVIOR -------------------------- */
    else if (protocol == IPPROTO_RUDP) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;
        hints.ai_protocol = IPPROTO_UDP;

        if (getaddrinfo(iface, port_str, &hints, &res) != 0)
            return -1;

        int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sockfd < 0) {
            freeaddrinfo(res);
            return -1;
        }

        if (bind(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
            close(sockfd);
            freeaddrinfo(res);
            return -1;
        }

        freeaddrinfo(res);

        struct timeval tv = {1, 0};
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        rudp_packet_t syn, synack = {SYN | ACK, 0}, ack;
        struct sockaddr_storage client_addr;
        socklen_t addrlen = sizeof(client_addr);

        for (;;) {
            ssize_t n = recvfrom(sockfd, &syn, sizeof(syn), 0,
                                 (struct sockaddr *)&client_addr, &addrlen);
            if (n < 0 || syn.type != SYN)
                continue; // ignore bad packets

            while (1) {
                sendto(sockfd, &synack, sizeof(synack), 0,
                       (struct sockaddr *)&client_addr, addrlen);

                n = recvfrom(sockfd, &ack, sizeof(ack), 0,
                             (struct sockaddr *)&client_addr, &addrlen);
                if (n > 0 && ack.type == ACK) {
                    save_rudp_conn(sockfd, (struct sockaddr *)&client_addr, addrlen);
                    return sockfd;
                }
            }
        }

        close(sockfd);
        return -1;
    }

    errno = EPROTONOSUPPORT;
    return -1;
}


int sans_disconnect(int socket) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (rudp_conns[i].sockfd == socket) {
            rudp_conns[i].sockfd = -1;
            memset(&rudp_conns[i].addr, 0, sizeof(struct sockaddr_storage));
            rudp_conns[i].addrlen = 0;
        }
    }
    return close(socket);
}

int save_rudp_conn(int sockfd, struct sockaddr *addr, socklen_t addrlen) {
    static int initialized = 0;
    if (!initialized) {
        for (int i = 0; i < MAX_SOCKETS; i++) rudp_conns[i].sockfd = -1;
        initialized = 1;
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (rudp_conns[i].sockfd == -1) {
            rudp_conns[i].sockfd = sockfd;
            memcpy(&rudp_conns[i].addr, addr, addrlen);
            rudp_conns[i].addrlen = addrlen;
            return 0;
        }
    }
    return -1;
}
