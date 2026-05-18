//
// Copyright(C) 2026 dsda-doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//  TCP transport layer for multiplayer
//

#ifndef __NET_TRANSPORT__
#define __NET_TRANSPORT__

#include "doomtype.h"
#include "net_defs.h"

// Return value from net_recv_packet_timeout when the select() deadline expires.
#define NET_RECV_TIMEOUT (-2)

// Initialize / shutdown platform socket library
void net_transport_init(void);
void net_transport_shutdown(void);

// Server: listen on a port, returns server socket or -1
int net_listen(int port);

// Server: accept a pending connection (non-blocking), returns client socket or -1
int net_accept(int server_socket);

// Client: connect to host:port, returns socket or -1
int net_connect(const char *address, int port);

// Set network latency simulation (avg_ms = 0 disables)
void net_set_latency(int avg_ms, int jitter_ms);

// Send a complete message: [u16 type][u16 length][payload]
// Returns 0 on success, -1 on error
int net_send_packet(int socket, int type, const void *data, int length);

// Receive a complete message (blocks until available or timeout)
// Returns message type, fills data and length. Returns -1 on error/disconnect.
int net_recv_packet(int socket, void *data, int *length, int max_length);

// Like net_recv_packet but returns NET_RECV_TIMEOUT (-2) if no data arrives
// within timeout_ms milliseconds instead of blocking indefinitely.
int net_recv_packet_timeout(int socket, void *data, int *length, int max_length,
                            int timeout_ms);

// Wait for the socket to become readable.
// Returns 1 if data is ready, 0 on timeout, -1 on error.
int net_wait_for_packet(int socket, int timeout_ms);

// Close a socket
void net_close(int socket);

// Set receive timeout in milliseconds (0 = blocking forever)
void net_set_timeout(int socket, int milliseconds);

#endif
