#include "warpout/server.hpp"

#include <arpa/inet.h> // inet_pton
#include <errno.h>
#include <fcntl.h>
#include <linux/socket.h> // SO_BINDTODEVICE
#include <net/if.h>       // struct ifreq
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

//---------------------------------------------------------------------------
// Create + bind listening socket, set reuse, optional SO_BINDTODEVICE
//---------------------------------------------------------------------------

server_context_t *server_create(const char *bind_addr_, uint16_t port_, int maxClients_,
                                client_handlers_t *clientHandlers_) {
    // 1) socket()
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() error: %s\n", strerror(errno));
        return NULL;
    }
    // 2) SO_REUSEADDR + SO_REUSEPORT
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) < 0) {
        fprintf(stderr, "setsockopt(REUSE): %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    // 3) Prepare sockaddr_in
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    // 3a) Try IPv4 literal
    if (inet_pton(AF_INET, bind_addr_, &addr.sin_addr) == 1) {
        // OK, bind to that IP
    } else {
        // Not an IP; treat as interface name
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, bind_addr_, (socklen_t)strlen(bind_addr_)) < 0) {
            fprintf(stderr, "warning: SO_BINDTODEVICE(%s) failed: %s\n", bind_addr_, strerror(errno));
            // we'll still bind to INADDR_ANY below
        }
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    addr.sin_port = htons(port_);

    // 4) bind()
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind(%s:%u) error: %s\n", bind_addr_, port_, strerror(errno));
        close(fd);
        return NULL;
    }

    // 5) listen() using maxClients_ as backlog
    if (listen(fd, maxClients_) < 0) {
        fprintf(stderr, "listen() error: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    // 6) allocate context + per-client slots
    server_context_t *ctx = (server_context_t *)calloc(1, sizeof(*ctx));
    ctx->port = port_;
    ctx->serverFd = fd;
    ctx->maxClients = maxClients_;
    ctx->handlers = *clientHandlers_;
    ctx->clientContext = (client_context_t **)calloc(maxClients_, sizeof(*ctx->clientContext));
    for (int i = 0; i < maxClients_; ++i) {
        ctx->clientContext[i] = (client_context_t *)calloc(1, sizeof(**ctx->clientContext));
        ctx->clientContext[i]->inUse = false;
        ctx->clientContext[i]->clientFd = -1;
        ctx->clientContext[i]->contextData = NULL;
    }
    return ctx;
}

//---------------------------------------------------------------------------
// Epoll helper
//---------------------------------------------------------------------------

static void epoll_add(int efd, int fd) {
    struct epoll_event ev = {};
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl ADD %d: %s\n", fd, strerror(errno));
        exit(1);
    }
}

static void epoll_del(int efd, int fd) {
    if (epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        fprintf(stderr, "epoll_ctl DEL %d: %s\n", fd, strerror(errno));
        exit(1);
    }
}

//---------------------------------------------------------------------------
// On new client connect
//---------------------------------------------------------------------------

static void server_on_client_connect(server_context_t *S, int efd, int cfd) {
    for (int i = 0; i < S->maxClients; ++i) {
        if (!S->clientContext[i]->inUse) {
            S->clientContext[i]->inUse = true;
            S->clientContext[i]->clientFd = cfd;
            S->clientContext[i]->contextData = S->handlers.onConnect(cfd);

            // non-blocking + keepalive
            int flags = fcntl(cfd, F_GETFL, 0);
            fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

            int ena = 1;
            setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &ena, sizeof(ena));

            int idleTime = 10;
            setsockopt(cfd, SOL_TCP, TCP_KEEPIDLE, &idleTime, sizeof(idleTime));

            int keepCount = 5;
            setsockopt(cfd, SOL_TCP, TCP_KEEPCNT, &keepCount, sizeof(keepCount));

            int keepInterval = 5;
            setsockopt(cfd, SOL_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(keepInterval));

            epoll_add(efd, cfd);
            return;
        }
    }
    // no slot free
    close(cfd);
    fprintf(stderr, "refused connection: server full\n");
}

//---------------------------------------------------------------------------
// On client disconnect
//---------------------------------------------------------------------------

static void server_on_client_disconnect(server_context_t *S, int efd, int idx) {
    S->handlers.onDisconnect(S->clientContext[idx]->contextData);
    epoll_del(efd, S->clientContext[idx]->clientFd);
    close(S->clientContext[idx]->clientFd);
    S->clientContext[idx]->inUse = false;
    S->clientContext[idx]->clientFd = -1;
}

//---------------------------------------------------------------------------
// Main loop
//---------------------------------------------------------------------------

void server_run(server_context_t *S) {
    int efd = epoll_create1(0);
    epoll_add(efd, S->serverFd);

    while (true) {
        struct epoll_event ev;
        int n = epoll_wait(efd, &ev, 1, -1);
        if (n < 0) {
            fprintf(stderr, "epoll_wait: %s\n", strerror(errno));
            break;
        }

        if (ev.data.fd == S->serverFd) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int cfd = accept(S->serverFd, (struct sockaddr *)&peer, &plen);
            if (cfd < 0) {
                fprintf(stderr, "accept: %s\n", strerror(errno));
                break;
            }
            server_on_client_connect(S, efd, cfd);
        } else {
            for (int i = 0; i < S->maxClients; ++i) {
                if (S->clientContext[i]->clientFd == ev.data.fd) {
                    bool err = false;
                    if (ev.events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                        err = true;
                    } else if (ev.events & EPOLLIN) {
                        if (!S->handlers.onReadData(ev.data.fd, S->clientContext[i]->contextData)) {
                            err = true;
                        }
                    }
                    if (err) {
                        server_on_client_disconnect(S, efd, i);
                    }
                    break;
                }
            }
        }
    }
}
