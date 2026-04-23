/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *    Contains the main wait loop, waiting for the next tic.
 *    Rewritten for LxDoom, but based around bits of the old code.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "z_zone.h"

#include "d_main.h"
#include "g_game.h"
#include "m_menu.h"

#include "i_system.h"
#include "i_main.h"
#include "i_video.h"
#include "r_fps.h"
#include "lprintf.h"
#include "e6y.h"

#include "dsda/args.h"
#include "dsda/settings.h"
#include "dsda/time.h"

#include "net_session.h"
#include "net_transport.h"
#include "net_serialize.h"

ticcmd_t local_cmds[MAX_MAXPLAYERS][BACKUPTICS];
int maketic;
int solo_net = 0;
static int remote_maketic;

void D_InitFakeNetGame (void)
{
  int i;

  consoleplayer = displayplayer = 0;
  solo_net = dsda_Flag(dsda_arg_solo_net);
  coop_spawns = dsda_Flag(dsda_arg_coop_spawns);
  netgame = solo_net;

  playeringame[0] = true;
  for (i = 1; i < g_maxplayers; i++)
    playeringame[i] = false;
}

void D_InitNetGame(void)
{
  dsda_arg_t *arg_host, *arg_join;

  arg_host = dsda_Arg(dsda_arg_host);
  arg_join = dsda_Arg(dsda_arg_join);

  if (arg_host->found && arg_join->found) {
    I_Error("Cannot use -host and -join at the same time");
  }

  if (arg_host->found) {
    int port = NET_DEFAULT_PORT;
    if (arg_host->value.v_string)
      port = atoi(arg_host->value.v_string);
    if (port <= 0 || port > 65535)
      port = NET_DEFAULT_PORT;

    if (net_session_host_start(port) != 0) {
      I_Error("Failed to start multiplayer host on port %d", port);
    }

    consoleplayer = displayplayer = 0;
    playeringame[0] = true;
    playeringame[1] = true;
    netgame = true;
    solo_net = 0;
    coop_spawns = 1;
  }
  else if (arg_join->found) {
    const char *addr_str = arg_join->value.v_string;
    char address[256];
    int port = NET_DEFAULT_PORT;
    const char *colon;

    if (!addr_str || !addr_str[0]) {
      I_Error("-join requires an address (e.g., 127.0.0.1 or 127.0.0.1:26101)");
    }

    // Parse address:port
    colon = strrchr(addr_str, ':');
    if (colon && colon != addr_str) {
      int addr_len = (int)(colon - addr_str);
      if (addr_len >= (int)sizeof(address))
        addr_len = (int)sizeof(address) - 1;
      memcpy(address, addr_str, addr_len);
      address[addr_len] = '\0';
      port = atoi(colon + 1);
      if (port <= 0 || port > 65535)
        port = NET_DEFAULT_PORT;
    }
    else {
      strncpy(address, addr_str, sizeof(address) - 1);
      address[sizeof(address) - 1] = '\0';
    }

    if (net_session_client_start(address, port) != 0) {
      I_Error("Failed to connect to %s:%d", address, port);
    }

    consoleplayer = displayplayer = 1;
    playeringame[0] = true;
    playeringame[1] = true;
    netgame = true;
    solo_net = 0;
    coop_spawns = 1;
  }
  else {
    D_InitFakeNetGame();
  }
}

void FakeNetUpdate(void)
{
  static int lastmadetic;

  if (isExtraDDisplay)
    return;

  if (net_session_active())
    return;

  { // Build new ticcmds
    int newtics = dsda_GetTick() - lastmadetic;
    lastmadetic += newtics;

    while (newtics--) {
      I_StartTic();
      if (maketic - gametic > BACKUPTICS/2) break;

      // e6y
      // Eliminating the sudden jump of six frames(BACKUPTICS/2)
      // after change of game_speed.
      if (maketic - gametic && gametic <= force_singletics_to && dsda_GameSpeed() < 200) break;

      G_BuildTiccmd(&local_cmds[consoleplayer][maketic%BACKUPTICS]);
      maketic++;
    }
  }
}

// Build local ticcmd, send it to remote, receive remote's ticcmd.
// For multiplayer: builds exactly one tic per call.
static void NetUpdate(void)
{
  unsigned char buf[64];
  int local = net_session.local_player;

  if (isExtraDDisplay)
    return;

  // Only build one tic ahead
  if (maketic > gametic)
    return;

  I_StartTic();

  // Build local ticcmd
  G_BuildTiccmd(&local_cmds[local][maketic % BACKUPTICS]);

  // Send to remote
  net_write_ticcmd(buf, &local_cmds[local][maketic % BACKUPTICS]);
  if (net_send_packet(net_session.socket, NET_MSG_TICCMD, buf, NET_TICCMD_SIZE) != 0) {
    lprintf(LO_ERROR, "NetUpdate: failed to send ticcmd\n");
    net_session_disconnect();
    return;
  }

  maketic++;
}

// Receive remote player's ticcmd for the given tic.
// Blocks until data arrives (hard stall). Returns 0 on success, -1 on error.
static int NetRecvRemoteTic(void)
{
  unsigned char buf[64];
  int len;
  int msg_type;
  int remote = net_session.remote_player;

  while (remote_maketic <= gametic) {
    msg_type = net_recv_packet(net_session.socket, buf, &len, sizeof(buf));

    if (msg_type == NET_MSG_TICCMD) {
      net_read_ticcmd(buf, &local_cmds[remote][remote_maketic % BACKUPTICS]);
      remote_maketic++;
    }
    else if (msg_type == NET_MSG_QUIT || msg_type < 0) {
      lprintf(LO_ERROR, "NetRecvRemoteTic: remote disconnected\n");
      net_session_disconnect();
      return -1;
    }
    // Ignore unknown message types
  }

  return 0;
}

// Implicitly tracked whenever we check the current tick
int ms_to_next_tick;

void TryRunTics (void)
{
  int runtics;
  int entertime = dsda_GetTick();

  if (net_session_active()) {
    // Multiplayer lockstep: build and send one local tic, then
    // block until we have the remote player's tic, then advance.
    NetUpdate();

    if (!net_session_active())
      return;  // disconnected during send

    // Hard stall: wait for remote ticcmd
    if (NetRecvRemoteTic() != 0)
      return;  // disconnected

    // Both players have ticcmds for gametic — run one tic
    if (advancedemo)
      D_DoAdvanceDemo();
    M_Ticker();
    G_Ticker();
    gametic++;
    return;
  }

  // Single-player path (unchanged)
  // Wait for tics to run
  while (1) {
    FakeNetUpdate();
    runtics = maketic - gametic;
    if (!runtics) {
      if (!movement_smooth) {
          I_uSleep(ms_to_next_tick*1000);
      }
      if (dsda_GetTick() - entertime > 10) {
        M_Ticker(); return;
      }

      if (gametic > 0)
      {
        WasRenderedInTryRunTics = true;
        if (movement_smooth && gamestate==wipegamestate)
        {
          isExtraDDisplay = true;
          D_Display(-1);
          isExtraDDisplay = false;
        }
      }
    } else break;
  }

  while (runtics--) {
    if (advancedemo)
      D_DoAdvanceDemo ();
    M_Ticker ();
    G_Ticker ();
    gametic++;
    FakeNetUpdate();
  }
}

// Multiplayer-aware singletics handler (called from D_DoomLoop)
void NetSingleTic(void)
{
  if (!net_session_active()) {
    // Original singletics path
    I_StartTic();
    G_BuildTiccmd(&local_cmds[consoleplayer][maketic % BACKUPTICS]);
    if (advancedemo)
      D_DoAdvanceDemo();
    M_Ticker();
    G_Ticker();
    gametic++;
    maketic++;
    return;
  }

  // Multiplayer singletics: same lockstep as TryRunTics
  NetUpdate();
  if (!net_session_active())
    return;

  if (NetRecvRemoteTic() != 0)
    return;

  if (advancedemo)
    D_DoAdvanceDemo();
  M_Ticker();
  G_Ticker();
  gametic++;
}
