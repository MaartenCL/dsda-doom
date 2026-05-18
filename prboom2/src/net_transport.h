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

// Initialize / shutdown platform socket library
void net_transport_init(void);
void net_transport_shutdown(void);

// Server: listen on a port, returns server socket or -1
int net_listen(int port);

// Server: accept a pending connection (non-blocking), returns client socket or -1
int net_accept(int server_socket);

// Client: connect to host:port, returns socket or -1
int net_connect(const char *address, int port);

// Send a complete message: [u16 type][u16 length][payload]
// Returns 0 on success, -1 on error
int net_send_packet(int socket, int type, const void *data, int length);

// Receive a complete message (blocks until available or timeout)
// Returns message type, fills data and length. Returns -1 on error/disconnect.
int net_recv_packet(int socket, void *data, int *length, int max_length);

// Wait for the socket to become readable.
// Returns 1 if data is ready, 0 on timeout, -1 on error.
int net_wait_for_packet(int socket, int timeout_ms);

// Close a socket
void net_close(int socket);

// Set receive timeout in milliseconds (0 = blocking forever)
void net_set_timeout(int socket, int milliseconds);

#endif
