#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <semaphore.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include "testing.h"

#define IPPROTO_RUDP 63

#define DAT 0
#define SYN 1
#define ACK 2
#define FIN 4

#define SEND 0
#define RECV 1

typedef struct {
  char type;
  int seqnum;
  char payload[];
} rudp_packet_t;

typedef struct {
  int socket;
  int packetlen;
  rudp_packet_t* packet;
} swnd_entry_t;

int sans_connect(const char*, int, int);
int sans_accept(const char*, int, int);

int sans_send_pkt(int, char*, int);
int sans_recv_pkt(int, char*, int);

extern char* s__testdir;
extern swnd_entry_t* send_window;

static sem_t lock, rev_lock;

static tests_t tests[] = {
  {
    .category = "General",
    .prompts = {
      "Program Compiles",
    }
  },
  {
    .category = "Send Window",
    .prompts = {
      "Window queues at least one packet",
      "Window queues multiple packets",
      "Window retransmitted on timeout",
      "Window NOT modified on out-of-order acknowledgement"
    }
  },
  {
    .category = "Backend Functionality",
    .prompts = {
      "Correct data sent from queue",
      "Send called on single packet window",
      "Send called on multiple packet window",
      "Recv called on single packet window",
      "Recv called on multiple packet window",
    }
  }
};

static jmp_buf env;

static int call_counts[2] = {0};
static int current_packet = -1;
static int current_offset = 0;
static swnd_entry_t test_window[10];
void (*send_worker)(rudp_packet_t*, int) = NULL;
int  (*recv_worker)(rudp_packet_t*) = NULL;

static void sigsegv_handler(int sig) {
  siglongjmp(env, 1);
}

static void _pause(sem_t* sem) {
  struct timespec ts = {0};
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    printf("[ERROR] Failed to get current clock time\n");
  }
  ts.tv_sec += 1;
  //ts.tv_nsec += 3200000;
  
  /* Ensures implementations that do not call recv when queue is empty still recover */
  if (sem_timedwait(sem, &ts) == -1 && errno == ETIMEDOUT) {
    return;
  }
}

static void build_test_window(void) {
  for (int i=0; i<10; i++) {
    rudp_packet_t* pkt = malloc(sizeof(rudp_packet_t) + 1024);
    strcpy(pkt->payload, "0 packet");
    pkt->payload[0] = '0' + i;
    test_window[i] = (swnd_entry_t) {
      .packet = pkt,
      .packetlen = 8
    };
  }
}

static void test_reset(void (*sender)(rudp_packet_t*, int), int (*receiver)(rudp_packet_t*)) {
  call_counts[SEND] = call_counts[RECV] = 0;
  send_worker = sender;
  recv_worker = receiver;
}

static char* h_match_window(int offset, int len) {
  for (int i=offset; i<len+offset; i++) {
    for (int j=i+1; j<len+offset; j++) {
      if (send_window[i].packet == send_window[j].packet)
	return "FAIL - Same packet appears multiple times in the send window";
      else if (send_window[i].packet->seqnum == send_window[j].packet->seqnum)
	return "FAIL - Sequence number appears multiple times in the send window";
    }

    for (int j=0; j<test_window[i].packetlen; j++) {
      if (send_window[i].packet->payload[j] != test_window[i].packet->payload[j])
	return "FAIL - Ring buffer entry did not match expected packet";
    }
  }
  return NULL;
}

static char* match_window(int offset, int len) {
  char* result = NULL;

  if (sigsetjmp(env, 1) == 0) {
    signal(SIGSEGV, sigsegv_handler);
    result = h_match_window(offset, len);
  }
  else {
    result = "FAIL - Segmentation fault while attempting to access send_window";
  }
  signal(SIGSEGV, SIG_DFL);
  return result;
}


static int pre_send_pkt(int* result, void* args) {
  current_packet += 1;
  return 0;
}


static int pre_sendto_base(int* result, arg6_t* args) {
  rudp_packet_t* pkt = (rudp_packet_t*)args->buf;
  test_window[pkt->payload[0] - '0'].packet->seqnum = pkt->seqnum;
  *result = args->len;
  printf("Sendto: [%s]\n", pkt->payload);
  if (send_worker != NULL) {
    call_counts[SEND] += 1;
    send_worker(pkt, 1 + current_packet - current_offset);
  }
  return 1;
}

static int pre_recvfrom_base(int* result, arg6_t* args) {
  rudp_packet_t pkt = {
    .seqnum = test_window[current_offset].packet->seqnum,
    .type = ACK
  };

  printf("Recvfrom: [%d]\n", pkt.seqnum);
  if (recv_worker != NULL) {
    call_counts[RECV] += 1;
    *result = recv_worker(&pkt);
  }
  memcpy((char*)args->buf, &pkt, sizeof(pkt));
  return 1;
}


/* -------------------------  Phase 1 --------------------------------- */
static int burst = 0;
static void send_phase1(rudp_packet_t* pkt, int windowsize) {
  int test = (windowsize == 1 ? 0 : 1);
  int idx = current_offset + burst;

  char* error_msg = match_window(current_offset, burst + 1);
  assert(error_msg == NULL, tests[1].results[test], error_msg);

  
  assert(strcmp(pkt->payload, test_window[idx].packet->payload) == 0, tests[2].results[0],
	 "FAIL - Packet does not contain expected payload");
  assert(1, tests[2].results[1], "FAIL - Send not called");
  burst += 1;
}

static int recv_phase1(rudp_packet_t*);
static int recv_phase1_final(rudp_packet_t* pkt) {
  recv_worker = recv_phase1;
  errno = EAGAIN;
  burst = 0;
  sem_post(&lock);
  return -1;
}

static int recv_phase1(rudp_packet_t* pkt) {
  if (current_offset >= current_packet)
    recv_worker = recv_phase1_final;
  current_offset += 1;
  burst = 0;
  return sizeof(rudp_packet_t);
}


/* -------------------------  Phase 2 --------------------------------- */
static int recv_phase2(rudp_packet_t* pkt) {
  recv_worker = recv_phase1;
  errno = EAGAIN;
  burst = 0;
  return -1;
}

/* -------------------------  Phase 3 --------------------------------- */
static int recv_phase3(rudp_packet_t* pkt) {
  recv_worker = recv_phase1;
  pkt->seqnum = test_window[current_offset - 1].packet->seqnum;
  burst = 0;
  return sizeof(rudp_packet_t);
}

/* -------------------------  Phase 4 --------------------------------- */
static int recv_phase4_dup(rudp_packet_t* pkt) {
  recv_worker = recv_phase1;
  pkt->seqnum = test_window[current_offset - 1].packet->seqnum;
  return sizeof(rudp_packet_t);
}
static int recv_phase4_timeout(rudp_packet_t* pkt) {
  recv_worker = recv_phase4_dup;
  errno = EAGAIN;
  burst = 0;
  return -1;
}
static int recv_phase4(rudp_packet_t* pkt) {
  recv_worker = recv_phase4_timeout;
  _pause(&rev_lock);
  current_offset += 1;
  return sizeof(rudp_packet_t);
}


/* ----------------------------------------------------------------------------------------- */
typedef struct sockaddr_in sa_in;
static char* addr;
static int addrlen;
static int pre_sendto_handshake(int* result, arg6_t* args) {
  addrlen = *args->addrlen;
  addr = malloc(sizeof(char) * addrlen);
  memcpy(addr, (char*)args->dst, addrlen);
  *result = args->len;
  return 1;
}

static int pre_recvfrom_handshake(int* result, arg6_t* args) {
  if (args->dst != NULL)
    memcpy((char*)args->dst, addr, addrlen);
  if (args->addrlen != NULL)
    *args->addrlen = addrlen;
  *result = 1;
  ((char*)args->buf)[0] = SYN | ACK;
  return 1;
}

void t__p7_tests(void) {
  char out[128], err[128];
  
  s__initialize_tests(tests, 3);

  build_test_window();
  if (sem_init(&lock, 0, 0) == -1 || sem_init(&rev_lock, 0, 0)) {
    fprintf(stderr, "[Error] Failed to initialize semaphore for test thread\n");
    exit(-1);
  }

  { /*  Transport Driver thread  */
    pthread_t backend_thread;
    void* rudp_backend(void*);
    int result = pthread_create(&backend_thread, NULL, rudp_backend, NULL);
    if (result != 0) {
      fprintf(stderr, "Failed to create background worker thread\n");
      exit(-1);
    }
  }

  if (s__testdir == NULL) {
#ifdef HEADLESS
    s__dump_stdout("test.out", "test.err");
#else
    s__dump_stdout();
#endif
  }
  else {
    snprintf(out, 1024, "%s/stdout", s__testdir);
    snprintf(err, 1024, "%s/stderr", s__testdir);
#ifdef HEADLESS
    s__dump_stdout(out, err);
#else
    s__dump_stdout();
#endif
  }

  
  assert(1, tests[0].results[0], "");
  
  s__analytics[SENDTO_REF].precall  = (int (*)(int*, void*))pre_sendto_handshake;
  s__analytics[RECVFROM_REF].precall  = (int (*)(int*, void*))pre_recvfrom_handshake;

  int sock = sans_connect("192.168.1.1", 80, IPPROTO_RUDP);

  s__analytics[S_SEND_PKT_REF].precall = (int (*)(int*, void*))pre_send_pkt;
  s__analytics[SENDTO_REF].precall   = (int (*)(int*, void*))pre_sendto_base;
  s__analytics[RECVFROM_REF].precall = (int (*)(int*, void*))pre_recvfrom_base;


  /* ---------------------  Test 1 ---------------------- */
  test_reset(send_phase1, recv_phase1);
  sans_send_pkt(sock, test_window[0].packet->payload, 8);
  _pause(&lock);
  assert(call_counts[SEND] > 0, tests[2].results[1], "FAIL - Send not called during single packet tests");
  assert(call_counts[SEND] < 2, tests[2].results[1], "FAIL - Send called too many times during single packet tests");
  assert(call_counts[RECV] > 0, tests[2].results[3], "FAIL - Recv not called during single packet tests");
  
  test_reset(send_phase1, recv_phase1);
  sans_send_pkt(sock, test_window[1].packet->payload, 8);
  _pause(&lock);
  assert(call_counts[SEND] > 0, tests[2].results[1], "FAIL - Send not called during single packet tests");
  assert(call_counts[SEND] < 2, tests[2].results[1], "FAIL - Send called too many times during single packet tests");
  assert(call_counts[RECV] > 0, tests[2].results[3], "FAIL - Recv not called during single packet tests");

  test_reset(send_phase1, recv_phase1);
  sans_send_pkt(sock, test_window[2].packet->payload, 8);
  _pause(&lock);
  assert(call_counts[SEND] > 0, tests[2].results[1], "FAIL - Send not called during single packet tests");
  assert(call_counts[SEND] < 2, tests[2].results[1], "FAIL - Send called too many times during single packet tests");
  assert(call_counts[RECV] > 0, tests[2].results[3], "FAIL - Recv not called during single packet tests");

  test_reset(send_phase1, recv_phase1);
  sans_send_pkt(sock, test_window[3].packet->payload, 8);
  _pause(&lock);
  assert(call_counts[SEND] > 0, tests[2].results[1], "FAIL - Send not called during single packet tests");
  assert(call_counts[SEND] < 2, tests[2].results[1], "FAIL - Send called too many times during single packet tests");
  assert(call_counts[RECV] > 0, tests[2].results[3], "FAIL - Recv not called during single packet tests");

  /* ---------------------  Test 2 ---------------------- */
  test_reset(send_phase1, recv_phase2);
  sans_send_pkt(sock, test_window[4].packet->payload, 8);
  _pause(&lock);
  assert(call_counts[SEND] > 0, tests[2].results[1], "FAIL - Send not called during timeout tests");
  assert(call_counts[RECV] > 0, tests[2].results[3], "FAIL - Recv not called during timeout tests");
  assert(call_counts[SEND] > 1, tests[1].results[2],
  	 "FAIL - Window did not retransmit single packet on timeout");
  assert(call_counts[SEND] < 3, tests[1].results[2],
  	 "FAIL - Window transmitted single packe too many times on timeout");
  assert(call_counts[RECV] == 2, tests[1].results[2],
  	 "FAIL - Receive was called too few times on timeout");

  /* ---------------------  Test 3 ---------------------- */
  test_reset(send_phase1, recv_phase3);
  sans_send_pkt(sock, test_window[5].packet->payload, 8);
  _pause(&lock);
  assert(call_counts[SEND] > 0, tests[2].results[1], "FAIL - Send not called during out-of-order tests");
  assert(call_counts[RECV] > 0, tests[2].results[3], "FAIL - Recv not called during out-of-order tests");
  assert(call_counts[SEND] < 2, tests[1].results[3],
     	 "FAIL - Window was retransmitted unnecessarily on an out-of-order acknowledgement");
  assert(call_counts[RECV] == 2, tests[1].results[2],
  	 "FAIL - Receive was called too few times after out-of-order acknowledgement");

  /* ---------------------  Test 4 ---------------------- */
  test_reset(send_phase1, recv_phase4);
  sans_send_pkt(sock, test_window[6].packet->payload, 8);
  sans_send_pkt(sock, test_window[7].packet->payload, 8);
  sans_send_pkt(sock, test_window[8].packet->payload, 8);
  sans_send_pkt(sock, test_window[9].packet->payload, 8);
  sem_post(&rev_lock);
  _pause(&lock);

  assert(call_counts[SEND] > 3, tests[2].results[2], "FAIL - Send called too few times during queue tests");
  assert(call_counts[RECV] > 5, tests[2].results[4], "FAIL - Recv called too few times during queue tests");
  
  assert(call_counts[SEND] > 6, tests[1].results[2],
  	 "FAIL - Send not called enough times for retransmission on timeout");
  assert(call_counts[SEND] < 8, tests[1].results[2],
  	 "FAIL - Send called too many times for retransmission on timeout");
}
