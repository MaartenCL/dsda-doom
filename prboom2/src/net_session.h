//
// Copyright(C) 2026 dsda-doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//  Multiplayer session management (connection protocol)
//

#ifndef __NET_SESSION__
#define __NET_SESSION__

#include "net_defs.h"

// Start a host session: listen, wait for client, exchange settings.
// Returns 0 on success, -1 on failure.
int net_session_host_start(int port);

// Start a client session: connect, receive settings.
// Returns 0 on success, -1 on failure.
int net_session_client_start(const char *address, int port);

// Disconnect gracefully
void net_session_disconnect(void);

// Check if a multiplayer session is active
int net_session_active(void);

#endif
