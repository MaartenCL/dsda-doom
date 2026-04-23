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

#include <string.h>

#include "doomstat.h"
#include "lprintf.h"

#include "net_transport.h"
#include "net_serialize.h"
#include "net_session.h"

net_session_t net_session;

static int server_socket = -1;

static void net_session_reset(void)
{
  memset(&net_session, 0, sizeof(net_session));
  net_session.socket = -1;
  net_session.state = NET_STATE_DISCONNECTED;
}

static void net_session_build_setup(net_setup_t *setup)
{
  extern int startskill, startepisode, startmap;

  setup->skill      = startskill;
  setup->episode    = startepisode;
  setup->map        = startmap;
  setup->complevel  = compatibility_level;
  setup->deathmatch = deathmatch;
  setup->nomonsters = nomonsters;
  setup->fast       = fastparm;
  setup->respawn    = respawnparm;
  setup->longtics   = 0;
}

static void net_session_apply_setup(const net_setup_t *setup)
{
  extern int startskill, startepisode, startmap;

  startskill    = setup->skill;
  startepisode  = setup->episode;
  startmap      = setup->map;
  // Note: compatibility_level is set via -complevel, not overwritten here
  // to avoid breaking demo compatibility assumptions. The host should
  // ensure both players use the same complevel.
  deathmatch    = setup->deathmatch;
  nomonsters    = setup->nomonsters;
  fastparm      = setup->fast;
  respawnparm   = setup->respawn;
}

int net_session_host_start(int port)
{
  net_setup_t setup;
  unsigned char buf[256];
  int len;
  int client_sock;
  int msg_type;

  net_transport_init();
  net_session_reset();

  server_socket = net_listen(port);
  if (server_socket < 0) {
    lprintf(LO_ERROR, "net_session_host_start: failed to listen on port %d\n", port);
    return -1;
  }

  lprintf(LO_INFO, "Waiting for player to connect on port %d...\n", port);

  // Poll for client connection (blocking poll)
  client_sock = -1;
  while (client_sock < 0) {
    client_sock = net_accept(server_socket);
    if (client_sock < 0) {
      // Small sleep to avoid busy-waiting (platform-independent via I_uSleep)
      // For now, just keep polling — the accept is non-blocking
    }
  }

  // Close the server socket — we only need the one client
  net_close(server_socket);
  server_socket = -1;

  net_session.socket = client_sock;
  net_session.is_host = 1;
  net_session.local_player = 0;
  net_session.remote_player = 1;
  net_session.state = NET_STATE_CONNECTING;

  // Send game settings to client
  net_session_build_setup(&setup);
  len = net_write_setup(buf, &setup);
  if (net_send_packet(client_sock, NET_MSG_SETUP, buf, len) != 0) {
    lprintf(LO_ERROR, "net_session_host_start: failed to send setup\n");
    net_session_disconnect();
    return -1;
  }

  lprintf(LO_INFO, "Sent game settings, waiting for client ready...\n");

  // Wait for READY from client
  msg_type = net_recv_packet(client_sock, buf, &len, sizeof(buf));
  if (msg_type != NET_MSG_READY) {
    lprintf(LO_ERROR, "net_session_host_start: expected READY, got %d\n", msg_type);
    net_session_disconnect();
    return -1;
  }

  net_session.state = NET_STATE_PLAYING;
  lprintf(LO_INFO, "Client connected. Starting game.\n");
  return 0;
}

int net_session_client_start(const char *address, int port)
{
  net_setup_t setup;
  unsigned char buf[256];
  int len;
  int sock;
  int msg_type;

  net_transport_init();
  net_session_reset();

  lprintf(LO_INFO, "Connecting to %s:%d...\n", address, port);

  sock = net_connect(address, port);
  if (sock < 0) {
    lprintf(LO_ERROR, "net_session_client_start: failed to connect to %s:%d\n",
            address, port);
    return -1;
  }

  net_session.socket = sock;
  net_session.is_host = 0;
  net_session.local_player = 1;
  net_session.remote_player = 0;
  net_session.state = NET_STATE_CONNECTING;

  // Receive game settings from host
  msg_type = net_recv_packet(sock, buf, &len, sizeof(buf));
  if (msg_type != NET_MSG_SETUP) {
    lprintf(LO_ERROR, "net_session_client_start: expected SETUP, got %d\n", msg_type);
    net_session_disconnect();
    return -1;
  }

  net_read_setup(buf, &setup);
  net_session_apply_setup(&setup);

  lprintf(LO_INFO, "Received game settings (skill=%d, episode=%d, map=%d)\n",
          setup.skill, setup.episode, setup.map);

  // Send READY to host
  if (net_send_packet(sock, NET_MSG_READY, NULL, 0) != 0) {
    lprintf(LO_ERROR, "net_session_client_start: failed to send READY\n");
    net_session_disconnect();
    return -1;
  }

  net_session.state = NET_STATE_PLAYING;
  lprintf(LO_INFO, "Connected to host. Starting game.\n");
  return 0;
}

void net_session_disconnect(void)
{
  if (net_session.state == NET_STATE_DISCONNECTED)
    return;

  if (net_session.socket >= 0) {
    // Try to send QUIT (best effort, ignore errors)
    net_send_packet(net_session.socket, NET_MSG_QUIT, NULL, 0);
    net_close(net_session.socket);
  }

  if (server_socket >= 0) {
    net_close(server_socket);
    server_socket = -1;
  }

  // Reset game state so the engine doesn't try to tick a missing player
  playeringame[1] = false;
  netgame = false;

  net_session_reset();
  lprintf(LO_INFO, "Disconnected from multiplayer session.\n");
}

int net_session_active(void)
{
  return net_session.state == NET_STATE_PLAYING;
}
