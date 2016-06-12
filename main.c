
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
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

// multi-message per connection protocol,
// each session terminates with a HangUp
typedef struct {
  enum {
    kMessagePayload,
  } type;
  union {
    int canary;
  };
} Message;

typedef struct {
  size_t len, mlen;
  struct pollfd *fds;
  Message *messages;
} Server;

static __attribute__((noreturn)) void listenAndServe(Server s) {
  for (;;) {
    // select fds that can be read
    if (poll(s.fds, s.len, -1) == -1) {
      err(EXIT_FAILURE, "%s: poll(...)", __func__);
    }
    for (size_t i = 0; i < s.len; i++) {
      if (s.fds[i].revents & POLLIN) {
        if (i == 0) {
          // listening socket
          struct sockaddr_in addr;
          socklen_t len;
          int csock = accept(s.fds[i].fd, (struct sockaddr *)&addr, &len);
          if (csock == -1) {
            if (errno == ENETDOWN || errno == EPROTO || errno == ENOPROTOOPT ||
                errno == EHOSTDOWN || errno == ENONET ||
                errno == EHOSTUNREACH || errno == EOPNOTSUPP ||
                errno == ENETUNREACH || errno == EAGAIN) {
              // treat these errors like EAGAIN
              warn("%s: accept(sock, ...)", __func__);
            } else {
              err(EXIT_FAILURE, "%s: accept(sock, ...)", __func__);
            }
          } else {
            // add connection socket
            s.len++;
            s.fds = realloc(s.fds, s.len * sizeof(struct pollfd));
            s.fds[s.len - 1] = (struct pollfd){
                .fd = csock, .events = POLLIN,
            };
          }
        } else {
          Message m;
          ssize_t len = read(s.fds[i].fd, &m, sizeof(m));
          if (len == -1) {
            err(EXIT_FAILURE, "%s: read(sock, ...)", __func__);
          } else if (len == 0) {
            // remove from list
            s.len--;
            if (i < s.len) {
              memmove(&s.fds[i], &s.fds[i + 1], s.len - i);
            }
            s.fds = realloc(s.fds, s.len * sizeof(struct pollfd));
            i--;  // hit this again on the next round
          } else {
            // have a message ready
            switch (m.type) {
              case kMessagePayload:
                printf("canary: %d\n", m.canary);
                break;
            }
          }
        }
      }
    }
  }
}

// sets up server and forks to accept-loop
static void newServer() {
  int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (sock == -1) {
    err(EXIT_FAILURE, "%s: socket(...)", __func__);
  }

  // bind to socket
  if (bind(sock,
           (const struct sockaddr *)&(const struct sockaddr_in){
               .sin_family = AF_INET, .sin_port = htons(0xbad0),
           },
           sizeof(struct sockaddr_in)) == -1) {
    err(EXIT_FAILURE, "%s: bind(sock, ...)", __func__);
  }

  // mark socket for accept
  int backlog = INT_MAX;
  if (listen(sock, backlog) == -1) {
    err(EXIT_FAILURE, "%s: listen(sock, ...)", __func__);
  }

  // established server, fork to loop
  pid_t child;
  if ((child = fork()) == -1) {
    err(1, "Server: fork()");
  } else if (child == 0) {
    // drop any priviledges
    if (setgid(getgid()) == -1) {
      err(EXIT_FAILURE, "%s: setgid(getgid())", __func__);
    }
    if (setuid(getuid()) == -1) {
      err(EXIT_FAILURE, "%s: setuid(getuid())", __func__);
    }

    Server s = {
        .fds = calloc(1, sizeof(struct pollfd)),
    };
    s.fds[s.len++] = (struct pollfd){
        .fd = sock, .events = POLLIN,
    };

    // run server-accepter
    listenAndServe(s);
  }
}

typedef struct {
  int sock;  // connection
} Client;

// newClient connects to a server
static Client newClient() {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == -1) {
    err(EXIT_FAILURE, "%s: socket(...)", __func__);
  }

  if (connect(sock,
              (const struct sockaddr *)&(const struct sockaddr_in){
                  .sin_family = AF_INET,
                  .sin_port = htons(0xbad0),
                  .sin_addr = {.s_addr = htonl(0x7f000001)},  // 127.0.0.1
              },
              sizeof(struct sockaddr_in)) == -1) {
    err(EXIT_FAILURE, "%s: connect(sock, ...)", __func__);
  }

  return (Client){
      .sock = sock,
  };
}

static void sendMessage(Client c, Message m) {
  // write a canary message
  if (write(c.sock, &m, sizeof(Message)) == -1) {
    err(EXIT_FAILURE, "%s: write(sock, ...)", __func__);
  }
}

static void destroyClient(Client c) {
  if (close(c.sock) == -1) {
    err(EXIT_FAILURE, "%s: close(sock)", __func__);
  }
}

int main() {
  newServer();

  for (int i = 0; i < 100; i++) {
    Client c = newClient();
    sendMessage(c, (Message){.canary = i});
    destroyClient(c);
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
