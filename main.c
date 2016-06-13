
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { uint16_t size; } Header;

typedef struct { const char *payload; } Message;

typedef struct {
  size_t len, i;
  struct pollfd *fds;
} Server;

// dropConn drops the current socket from the buffer
static void dropConn(Server *s) {
  // remove from list
  s->len--;
  if (s->i < s->len) {
    memmove(&s->fds[s->i], &s->fds[s->i + 1], s->len - s->i);
  }
  s->fds = realloc(s->fds, s->len * sizeof(struct pollfd));
  s->i--;
}

// addConn adds a socket to the end of the buffer
static void addConn(Server *s, int sock) {
  // add connection socket
  s->len++;
  s->fds = realloc(s->fds, s->len * sizeof(struct pollfd));
  s->fds[s->len - 1] = (struct pollfd){
      .fd = sock, .events = POLLIN,
  };
}

// acceptConn accepts a new connection and adds it to the buffer
static void acceptConn(Server *s) {
  const size_t lsocki = 0;  // listening socket index
  struct sockaddr_in addr;
  socklen_t len;
  int csock = accept(s->fds[lsocki].fd, (struct sockaddr *)&addr, &len);
  if (csock == -1) {
    if (errno == ENETDOWN || errno == EPROTO || errno == ENOPROTOOPT ||
        errno == EHOSTDOWN || errno == ENONET || errno == EHOSTUNREACH ||
        errno == EOPNOTSUPP || errno == ENETUNREACH || errno == EAGAIN) {
      // treat these errors like EAGAIN
    }
    // TODO: if socket is toast, bind new socket
    warn("%s: accept(sock, ...)", __func__);
  } else {
    // mark socket as nonblocking
    if (fcntl(csock, F_SETFL, O_NONBLOCK) == -1) {
      // unrecoverable error, return
      warn("%s: fcntl(sock, ...)", __func__);
      return;
    }
    addConn(s, csock);
  }
}

static Message *handleMessage(Server *s) {
  const int sock = s->fds[s->i].fd;

  // read in header
  Header h;
  ssize_t len = read(sock, &h, sizeof(Header));
  if (len == -1 || len == 0) {
    if (len == -1) {
      warn("%s: read(sock, ...)", __func__);
    }
    dropConn(s);  // empty read, socket was closed
    return NULL;
  } else if (len != sizeof(Header)) {
    warnx("%s: insufficient read %zd < %zd\n", __func__, len, sizeof(Header));
    return NULL;
  }
  h.size = ntohs(h.size);
  printf("%d\n", h.size);

  // malloc appropriate buffer size
  uint8_t *bbuf = calloc(1, h.size);
  // read into buffer
  ssize_t blen = read(sock, bbuf, h.size);
  if (blen == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // improperly formatted message, garbage
      warn("%s: dropping message, improperly formatted", __func__);
    } else {
      // some sort of legitimate error, close & drop socket
      warn("%s: read(sock, ...)", __func__);
      dropConn(s);
    }
  } else if (blen < h.size) {
    warnx("insufficient read length: %zd < %d\n", blen, h.size);
  }
  Message *m = calloc(sizeof(Message), 1);
  m->payload = (const char *)bbuf;
  return m;
}

static Message *rcvMessage(Server *s) {
  const size_t lsocki = 0;  // listening socket index
  for (;;) {
    for (; s->i < s->len; s->i++) {
      if (s->fds[s->i].revents & POLLHUP) {
        printf("hup: dropping conn #%zd\n", s->i);
        dropConn(s);
      } else if (s->fds[s->i].revents & POLLIN) {
        printf("in: reading conn #%zd\n", s->i);
        if (s->i == lsocki) {
          // listening socket
          acceptConn(s);
        } else {
          Message *m = handleMessage(s);
          if (m != NULL) {
            return m;
          }
        }
      } else if (s->fds[s->i].revents & POLLERR) {
        printf("conn #%zd is in an error'd state\n", s->i);
      } else if (s->fds[s->i].revents & POLLNVAL) {
        printf("conn #%zd\n is invalid", s->i);
      }
    }
    s->i = 0;
    // select fds that can be read
    if (poll(s->fds, s->len, -1) == -1) {
      warn("%s: poll(...)", __func__);
    }
  }
}

// sets up server and forks to accept-loop
static Server *newServer() {
  int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (sock == -1) {
    warn("%s: socket(...)", __func__);
    return NULL;
  }

  // bind to socket
  if (bind(sock,
           (const struct sockaddr *)&(const struct sockaddr_in){
               .sin_family = AF_INET, .sin_port = htons(0xbad0),
           },
           sizeof(struct sockaddr_in)) == -1) {
    warn("%s: bind(sock, ...)", __func__);
    if (close(sock) == -1) {
      warn("%s: close(sock)", __func__);
    }  // try to close socket
    return NULL;
  }

  // mark socket for accept
  int backlog = INT_MAX;
  if (listen(sock, backlog) == -1) {
    warn("%s: listen(sock, ...)", __func__);
    if (close(sock) == -1) {
      warn("%s: close(sock)", __func__);
    }  // try to close socket
    return NULL;
  }

  // construct server
  Server *s = calloc(sizeof(Server), 1);
  *s = (Server){
      .fds = calloc(1, sizeof(struct pollfd)),
  };
  s->fds[s->len] = (struct pollfd){
      .fd = sock, .events = POLLIN,
  };
  s->len++;

  return s;
}

typedef struct {
  int sock;  // connection
} Client;

// newClient connects to a server
static Client *newClient() {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == -1) {
    warn("%s: socket(sock, ...)", __func__);
    return NULL;
  }

  if (connect(sock,
              (const struct sockaddr *)&(const struct sockaddr_in){
                  .sin_family = AF_INET,
                  .sin_port = htons(0xbad0),
                  .sin_addr = {.s_addr = htonl(0x7f000001)},  // 127.0.0.1
              },
              sizeof(struct sockaddr_in)) == -1) {
    warn("%s: connect(sock, ...)", __func__);
    if (close(sock) == -1) {
      warn("%s: close(sock)", __func__);
    }  // try to close socket
    return NULL;
  }

  Client *c = calloc(sizeof(Client), 1);
  *c = (Client){
      .sock = sock,
  };
  return c;
}

static int sendMessage(Client *c, Header h, const uint8_t *b) {
  // write message header
  h.size = htons(h.size);
  if (write(c->sock, &h, sizeof(Header)) == -1) {
    warn("%s: write(sock, ...)", __func__);
    return -1;
  }
  // write message body
  if (write(c->sock, b, h.size) == -1) {
    warn("%s: write(sock, ...)", __func__);
    return -1;
  }
  return 0;
}

static int destroyClient(Client *c) {
  if (close(c->sock) == -1) {
    warn("%s: close(sock)", __func__);
    return -1;
  }
  free(c);
  return 0;
}

static void test_client(int i) {
  Client *c = newClient();
  if (c) {
    // send message
    size_t blen = 128;
    uint8_t *buf = calloc(sizeof(char), blen);
    int plen = snprintf((char *)buf, blen, "hey: %d", i);
    if (plen == -1) {
      warn("%s: snprintf(...)", __func__);
    } else {
      sendMessage(c, (Header){.size = (uint16_t)(plen + 1)}, buf);
    }

    // cleanup
    destroyClient(c);
  }
}

int main() {
  Server *s = newServer();

  int schild = fork();
  if (schild == -1) {
    err(EXIT_FAILURE, "%s: fork()", __func__);
  } else if (schild == 0) {
    printf("forked server\n");
    for (int i = 0; i < 100; i++) {
      Message *m = rcvMessage(s);
      printf("%s\n", m->payload);
    }
    exit(EXIT_SUCCESS);
  }

  for (int i = 0; i < 100; i++) {
    if (!fork()) {
      test_client(i);
      exit(EXIT_SUCCESS);
    }
  }

  // wait for the server to exit properly
  for (;;) {
    pid_t child = wait(NULL);
    if (child == -1) {
      if (errno == ECHILD) {
        printf("All children have terminated\n");
        exit(EXIT_SUCCESS);
      } else {
        err(EXIT_FAILURE, "%s: wait(NULL)", __func__);
      }
    }
  }
}
