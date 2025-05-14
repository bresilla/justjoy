// src/warpout/server.hpp
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

//---------------------------------------------------------------------------
// Function pointers used to implement the event-handlers for socket events
//---------------------------------------------------------------------------
typedef void *(*client_connect_handler_t)(int clientFd_);
typedef void (*client_disconnect_handler_t)(void *clientContext_);
typedef bool (*client_read_data_t)(int clientFd_, void *clientContext_);

//---------------------------------------------------------------------------
// Struct containing the handler functions for client events
typedef struct {
    client_connect_handler_t onConnect;       //!< Action called when socket is connected
    client_disconnect_handler_t onDisconnect; //!< Action called when the socket is disconnected
    client_read_data_t onReadData;            //!< Action called when there is data to read on the socket
} client_handlers_t;

//---------------------------------------------------------------------------
// Per-client context
typedef struct {
    bool inUse;        //!< Whether or not the context object is idle or active
    int clientFd;      //!< FD corresponding to the socket
    void *contextData; //!< Connection-specific pointer to app-specific data
} client_context_t;

//---------------------------------------------------------------------------
// Server master context
typedef struct {
    uint16_t port;                    //!< Port we're listening on
    int serverFd;                     //!< Listening socket FD
    int maxClients;                   //!< Max concurrent clients
    client_handlers_t handlers;       //!< Your callbacks
    client_context_t **clientContext; //!< Array [maxClients] of per-client slots
} server_context_t;

//---------------------------------------------------------------------------/
/**
 * @brief Create & bind a new server socket.
 *
 * @param bind_addr_  Either an IPv4 literal (e.g. "192.168.1.5") or an
 *                    interface name (e.g. "eth0"). If it parses as IPv4,
 *                    we bind() to that address. Otherwise we attempt
 *                    a SO_BINDTODEVICE.
 * @param port_       TCP port to bind/listen on.
 * @param maxClients_ Max simultaneous clients (also used as backlog).
 * @param clientHandlers_ Your client callbacks.
 * @return server_context_t* on success, NULL on error.
 */
server_context_t *server_create(const char *bind_addr_, uint16_t port_, int maxClients_,
                                client_handlers_t *clientHandlers_);

/**
 * @brief Run the server loop.  Never returns unless fatal error.
 * @param context_ Context from server_create().
 */
void server_run(server_context_t *context_);

#if defined(__cplusplus)
}
#endif
