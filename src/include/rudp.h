#ifndef RUDP_H
#define RUDP_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>

#define DAT 0
#define SYN 1
#define ACK 2
#define FIN 4

#define MAX_SOCKETS 10
#define PKT_LEN 1400

typedef struct {
  uint8_t type;
  uint32_t seqnum;
  uint8_t payload[PKT_LEN];
} rudp_packet_t;

/* forward-declare connection storage (defined in sans_socket.c) */
extern struct rudp_conn {
    int sockfd;
    struct sockaddr_storage addr;
    socklen_t addrlen;
} rudp_conns[MAX_SOCKETS];

/* send-window entry */
typedef struct {
    int socket;
    rudp_packet_t* packet;
    size_t packetlen;
    unsigned long last_sent_ms;
    unsigned char sent_once;
} swnd_entry_t;

/* sequence counters placed together so test harness can find them */
extern uint32_t seq_counters[2];
#define send_seq (seq_counters[0])
#define recv_seq (seq_counters[1])

/* Backend / transport API */
void enqueue_packet(int sock, const uint8_t* buf, size_t len);
void* rudp_backend(void* unused);
void init_rudp_backend(void);

#endif
