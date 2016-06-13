
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

typedef struct {
  Header header;
  const char *payload;
} Message;

typedef struct {
  size_t len, i;
  struct pollfd *fds;
} Server;

// dropConn drops the current socket from the buffer
static void server_drop(Server *s) {
  // remove from list
  s->len--;
  if (s->i < s->len) {
    memmove(&s->fds[s->i], &s->fds[s->i + 1], s->len - s->i);
  }
  s->fds = realloc(s->fds, s->len * sizeof(struct pollfd));
  s->i--;
}

// addConn adds a socket to the end of the buffer
static void server_add(Server *s, int sock) {
  // add connection socket
  s->len++;
  s->fds = realloc(s->fds, s->len * sizeof(struct pollfd));
  s->fds[s->len - 1] = (struct pollfd){
      .fd = sock, .events = POLLIN,
  };
}

// acceptConn accepts a new connection and adds it to the buffer
static void server_accept(Server *s) {
  const size_t lsocki = 0;  // listening socket index
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  socklen_t len = 0;
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
    server_add(s, csock);
  }
}

// TODO: Refactor so that partial reads and reads of any amount
// succeed and the server can call this again when more data is avaliable.
static int handlem(Server *s, Message *m) {
  const int sock = s->fds[s->i].fd;

  // read in header
  Header h;
  ssize_t len = read(sock, &h, sizeof(Header));
  if (len == -1 || len == 0) {
    if (len == -1) {
      warn("%s: read(sock, ...)", __func__);
    }
    server_drop(s);  // empty read, socket was closed
    return -1;
  } else if (len != sizeof(Header)) {
    warnx("%s: insufficient read %zd < %zd\n", __func__, len, sizeof(Header));
    return -1;
  }
  h.size = ntohs(h.size);
  // malloc appropriate buffer size
  uint8_t *bbuf = calloc(1, h.size);
  // read into buffer
  ssize_t blen = read(sock, bbuf, h.size);
  if (blen == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // improperly formatted message, garbage
      warn("%s: dropping socket, improperly formatted", __func__);
      server_drop(s);
    } else {
      // some sort of legitimate error, close & drop socket
      warn("%s: read(sock, ...)", __func__);
      server_drop(s);
    }
  } else if (blen < h.size) {
    warnx("insufficient read length: %zd < %d\n", blen, h.size);
  }
  *m = (Message){
      .header = h, .payload = (const char *)bbuf,
  };
  return 0;
}

static int rcvm(Server *s, Message *m) {
  const size_t lsocki = 0;  // listening socket index
  for (;;) {
    for (; s->i < s->len; s->i++) {
      if (s->fds[s->i].revents & POLLHUP) {
        printf("hup: dropping conn #%zd\n", s->i);
        server_drop(s);
      } else if (s->fds[s->i].revents & POLLIN) {
        if (s->i == lsocki) {
          // listening socket
          server_accept(s);
        } else {
          if (handlem(s, m) != -1) {
            return 0;
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
static int server(Server *s) {
  int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (sock == -1) {
    warn("%s: socket(...)", __func__);
    return -1;
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
    return -1;
  }

  // mark socket for accept
  int backlog = INT_MAX;
  if (listen(sock, backlog) == -1) {
    warn("%s: listen(sock, ...)", __func__);
    if (close(sock) == -1) {
      warn("%s: close(sock)", __func__);
    }  // try to close socket
    return -1;
  }

  // construct server
  *s = (Server){
      .fds = calloc(1, sizeof(struct pollfd)),
  };
  s->fds[s->len] = (struct pollfd){
      .fd = sock, .events = POLLIN,
  };
  s->len++;

  return 0;
}

typedef struct {
  int sock;  // connection
} Client;

// newClient connects to a server
static int client(Client *c) {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == -1) {
    warn("%s: socket(sock, ...)", __func__);
    return -1;
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
    return -1;
  }

  *c = (Client){
      .sock = sock,
  };
  return 0;
}

static int sendm(Client *c, Message *m) {
  // write message header
  size_t psize = m->header.size;
  m->header.size = htons(m->header.size);
  if (write(c->sock, &m->header, sizeof(m->header)) == -1) {
    warn("%s: write(sock, ...)", __func__);
    return -1;
  }
  // write message body
  if (write(c->sock, m->payload, psize) == -1) {
    warn("%s: write(sock, ...)", __func__);
    return -1;
  }
  return 0;
}

static int fclient(Client *c) {
  if (close(c->sock) == -1) {
    warn("%s: close(sock)", __func__);
    return -1;
  }
  return 0;
}

static void test_client(int i) {
  Client c;
  if (client(&c) == -1) {
    warnx("%s: client(&client) failed", __func__);
    return;
  }
  // send message
  char buf[10];
  int len = snprintf(buf, sizeof(buf), "hey: %d", i);
  if (len == -1) {
    warn("%s: snprintf(...)", __func__);
    fclient(&c);
    return;
  }
  Message m = (Message){
      .header = (Header){.size = (uint16_t)(len + 1)}, .payload = buf,
  };
  sendm(&c, &m);
  fclient(&c);
}

int main() {
  Server svr;
  if (server(&svr) == -1) {
    errx(EXIT_FAILURE, "server failed to allocate");
  }

  int schild = fork();
  if (schild == -1) {
    err(EXIT_FAILURE, "%s: fork()", __func__);
  } else if (schild == 0) {
    for (int i = 0; i < 100; i++) {
      Message msg;
      if (rcvm(&svr, &msg) == -1) {
        errx(EXIT_FAILURE, "server failed to recvm(svr, msg)");
      }
      printf("%s\n", msg.payload);
    }
  }

  for (int i = 0; i < 100; i++) {
    test_client(i);
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
