#include "rudp.h"

/* Define MAX_SOCKETS locally to avoid include dependency issues */
#ifndef MAX_SOCKETS
#define MAX_SOCKETS 10
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "include/sans.h"

/* Export send_window and swnd_size for test harness */
swnd_entry_t* send_window = NULL;
const unsigned int swnd_size = 20; /* sliding window with 20 slots */

/* Internal state */
static pthread_mutex_t swnd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static unsigned int swnd_head = 0; /* next slot to write to */
static unsigned int swnd_tail = 0; /* oldest unacked packet */
static unsigned int swnd_count = 0; /* number of packets in window */

static void initialize_window(void) {
  send_window = calloc(swnd_size, sizeof(swnd_entry_t));
  if (!send_window) { perror("calloc"); return; }
  for (unsigned i = 0; i < swnd_size; i++) send_window[i].socket = -1;
  swnd_head = 0;
  swnd_tail = 0;
  swnd_count = 0;
}

void enqueue_packet(int sock, const uint8_t* buf, size_t len) {
  /* ensure window is allocated (thread-safe) */
  pthread_once(&init_once, initialize_window);
  
  pthread_mutex_lock(&swnd_mutex);
  if (!send_window) {
    pthread_mutex_unlock(&swnd_mutex);
    return; /* initialization failed */
  }

  /* block until a slot is free (window full) */
  while (swnd_count >= swnd_size) {
    pthread_mutex_unlock(&swnd_mutex);
    usleep(1000);
    pthread_mutex_lock(&swnd_mutex);
  }

  /* insert at head, using ring buffer */
  swnd_entry_t* entry = &send_window[swnd_head];
  entry->socket = sock;
  entry->packet = malloc(sizeof(rudp_packet_t));
  if (!entry->packet) { perror("malloc"); entry->socket = -1; pthread_mutex_unlock(&swnd_mutex); return; }
  /* zero initialize full packet buffer */
  memset(entry->packet, 0, sizeof(rudp_packet_t));
  entry->packet->type = DAT;
  entry->packet->seqnum = send_seq++;
  size_t copy_len = len;
  if (copy_len > PKT_LEN) copy_len = PKT_LEN;
  memcpy(entry->packet->payload, buf, copy_len);
  entry->packetlen = copy_len;
  entry->last_sent_ms = 0;
  entry->sent_once = 0;

  swnd_head = (swnd_head + 1) % swnd_size;
  swnd_count++;

  pthread_mutex_unlock(&swnd_mutex);
}

void dequeue_packet(unsigned int seqnum) {
  pthread_mutex_lock(&swnd_mutex);
  if (!send_window || swnd_count == 0) {
    pthread_mutex_unlock(&swnd_mutex);
    return;
  }

  /* Remove all packets from tail up to and including seqnum */
  while (swnd_count > 0) {
    swnd_entry_t* entry = &send_window[swnd_tail];
    if (entry->packet && entry->packet->seqnum <= seqnum) {
      free(entry->packet);
      entry->packet = NULL;
      entry->socket = -1;
      entry->packetlen = 0;
      entry->last_sent_ms = 0;
      entry->sent_once = 0;
      swnd_tail = (swnd_tail + 1) % swnd_size;
      swnd_count--;
    } else {
      break;
    }
  }
  pthread_mutex_unlock(&swnd_mutex);
}

void* rudp_backend(void* unused) {
  (void)unused;
  /* ensure window is allocated (thread-safe) */
  pthread_once(&init_once, initialize_window);
  if (!send_window) return NULL;

  while (1) {
    pthread_mutex_lock(&swnd_mutex);

    if (swnd_count == 0) {
      pthread_mutex_unlock(&swnd_mutex);
      usleep(1000);
      continue;
    }

    /* Get current time */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned long now_ms = (unsigned long)tv.tv_sec * 1000UL + (unsigned long)(tv.tv_usec / 1000UL);

    /* Send all pending, unsent packets in the send_window */
    for (unsigned int i = 0; i < swnd_count; i++) {
      unsigned int idx = (swnd_tail + i) % swnd_size;
      swnd_entry_t* entry = &send_window[idx];

      if (!entry->packet) continue;

      /* find connection info for this socket */
      struct rudp_conn* conn = NULL;
      for (int j = 0; j < MAX_SOCKETS; j++) {
        if (rudp_conns[j].sockfd == entry->socket) { conn = &rudp_conns[j]; break; }
      }

      if (conn == NULL || conn->addrlen == 0) continue;

      /* rate-limit retransmits: only resend if >100ms since last send */
      if (entry->sent_once && entry->last_sent_ms != 0 && now_ms - entry->last_sent_ms < 100) {
        continue;
      }

      /* header size: use offsetof to allow flexible struct layout */
      const size_t hdr_size = offsetof(rudp_packet_t, payload);
      size_t send_len = hdr_size + entry->packetlen;

      /* send packet (as raw bytes matching rudp_packet_t layout) */
      sendto(entry->socket, entry->packet, send_len, 0,
             (struct sockaddr*)&conn->addr, conn->addrlen);
      entry->last_sent_ms = now_ms;
      entry->sent_once = 1;
    }

    pthread_mutex_unlock(&swnd_mutex);

    /* attempt to receive acknowledgements */
    pthread_mutex_lock(&swnd_mutex);
    if (swnd_count > 0) {
      swnd_entry_t* first_entry = &send_window[swnd_tail];
      if (first_entry->packet) {
        /* find connection info for this socket */
        struct rudp_conn* conn = NULL;
        for (int j = 0; j < MAX_SOCKETS; j++) {
          if (rudp_conns[j].sockfd == first_entry->socket) { conn = &rudp_conns[j]; break; }
        }

        if (conn && conn->addrlen > 0) {
          const size_t hdr_size = offsetof(rudp_packet_t, payload);
          char ackbuf[sizeof(rudp_packet_t)];
          struct sockaddr_storage from;
          socklen_t fromlen = sizeof(from);
          
          pthread_mutex_unlock(&swnd_mutex);
          ssize_t r = recvfrom(first_entry->socket, ackbuf, hdr_size, 0, (struct sockaddr*)&from, &fromlen);
          pthread_mutex_lock(&swnd_mutex);

          if (r > 0) {
            uint8_t ack_type = (uint8_t)ackbuf[offsetof(rudp_packet_t, type)];
            uint32_t ack_seq = 0;
            memcpy(&ack_seq, ackbuf + offsetof(rudp_packet_t, seqnum), sizeof(uint32_t));
            
            if (ack_type == ACK) {
              /* Remove all packets up to and including ack_seq */
              while (swnd_count > 0) {
                swnd_entry_t* entry = &send_window[swnd_tail];
                if (entry->packet && entry->packet->seqnum <= ack_seq) {
                  free(entry->packet);
                  entry->packet = NULL;
                  entry->socket = -1;
                  entry->packetlen = 0;
                  entry->last_sent_ms = 0;
                  entry->sent_once = 0;
                  swnd_tail = (swnd_tail + 1) % swnd_size;
                  swnd_count--;
                } else {
                  break;
                }
              }
            }
          } else {
            /* Timeout - mark all packets in window as needing retransmit */
            for (unsigned int i = 0; i < swnd_count; i++) {
              unsigned int idx = (swnd_tail + i) % swnd_size;
              swnd_entry_t* entry = &send_window[idx];
              if (entry->packet) {
                entry->sent_once = 0;
                entry->last_sent_ms = 0;
              }
            }
          }
        }
      }
    }

    pthread_mutex_unlock(&swnd_mutex);
    usleep(1000);
  }

  return NULL;
}

void init_rudp_backend(void) {
  /* Initialize send_window early so test harness can access it */
  pthread_once(&init_once, initialize_window);
}
