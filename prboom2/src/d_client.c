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

#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "m_random.h"
#include "hu_stuff.h"
#include "p_mobj.h"
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

static dboolean net_out_of_sync;
static dboolean net_desync_diag;
static dboolean net_waiting_for_peer;
static unsigned int net_local_checksum_tic[BACKUPTICS];
static unsigned int net_local_checksum_value[BACKUPTICS];
static dboolean net_local_checksum_valid[BACKUPTICS];
static unsigned int net_remote_checksum_tic[BACKUPTICS];
static unsigned int net_remote_checksum_value[BACKUPTICS];
static dboolean net_remote_checksum_valid[BACKUPTICS];

typedef struct {
  unsigned int tic;
  unsigned int full;
  unsigned int rng_full;
  unsigned int rng_index;
  unsigned int player0;
  unsigned int player1;
  unsigned int monsters;
  unsigned int alive_monsters;
  int p0_x;
  int p0_y;
  int p0_z;
  int p0_health;
  int p1_x;
  int p1_y;
  int p1_z;
  int p1_health;
  int rndindex;
  int prndindex;
  dboolean valid;
} net_desync_diag_snapshot_t;

static net_desync_diag_snapshot_t net_diag_local;
static unsigned int net_diag_last_remote_tic;
static unsigned int net_diag_last_remote_checksum;

static void NetResetChecksumState(void)
{
  memset(net_local_checksum_valid, 0, sizeof(net_local_checksum_valid));
  memset(net_remote_checksum_valid, 0, sizeof(net_remote_checksum_valid));
  net_out_of_sync = false;
  net_waiting_for_peer = false;
  memset(&net_diag_local, 0, sizeof(net_diag_local));
  net_diag_last_remote_tic = 0;
  net_diag_last_remote_checksum = 0;
}

static unsigned int NetChecksumBytes(unsigned int hash, const void *data, size_t size)
{
  const unsigned char *p;
  size_t i;

  p = (const unsigned char *)data;

  for (i = 0; i < size; i++) {
    hash ^= p[i];
    hash *= 16777619u;
  }

  return hash;
}

static unsigned int NetBuildChecksum(unsigned int tic)
{
  unsigned int hash;
  int alive_monsters;
  fixed_t p0_x, p0_y, p0_z;
  fixed_t p1_x, p1_y, p1_z;
  int p0_health, p1_health;
  unsigned int rng_full_hash;
  unsigned int rng_index_hash;
  unsigned int player0_hash;
  unsigned int player1_hash;
  unsigned int monsters_hash;

  hash = 2166136261u;

  p0_x = p0_y = p0_z = 0;
  p1_x = p1_y = p1_z = 0;

  if (players[0].mo) {
    p0_x = players[0].mo->x;
    p0_y = players[0].mo->y;
    p0_z = players[0].mo->z;
  }

  if (players[1].mo) {
    p1_x = players[1].mo->x;
    p1_y = players[1].mo->y;
    p1_z = players[1].mo->z;
  }

  p0_health = players[0].health;
  p1_health = players[1].health;

  rng_full_hash = NetChecksumBytes(2166136261u, &rng, sizeof(rng));
  rng_index_hash = NetChecksumBytes(2166136261u, &rng.rndindex, sizeof(rng.rndindex));

  player0_hash = 2166136261u;
  player0_hash = NetChecksumBytes(player0_hash, &p0_x, sizeof(p0_x));
  player0_hash = NetChecksumBytes(player0_hash, &p0_y, sizeof(p0_y));
  player0_hash = NetChecksumBytes(player0_hash, &p0_z, sizeof(p0_z));
  player0_hash = NetChecksumBytes(player0_hash, &p0_health, sizeof(p0_health));

  player1_hash = 2166136261u;
  player1_hash = NetChecksumBytes(player1_hash, &p1_x, sizeof(p1_x));
  player1_hash = NetChecksumBytes(player1_hash, &p1_y, sizeof(p1_y));
  player1_hash = NetChecksumBytes(player1_hash, &p1_z, sizeof(p1_z));
  player1_hash = NetChecksumBytes(player1_hash, &p1_health, sizeof(p1_health));

  alive_monsters = P_CountLivingMonsters();
  monsters_hash = NetChecksumBytes(2166136261u, &alive_monsters, sizeof(alive_monsters));

  hash = NetChecksumBytes(hash, &rng.rndindex, sizeof(rng.rndindex));
  hash = NetChecksumBytes(hash, &p0_x, sizeof(p0_x));
  hash = NetChecksumBytes(hash, &p0_y, sizeof(p0_y));
  hash = NetChecksumBytes(hash, &p0_z, sizeof(p0_z));
  hash = NetChecksumBytes(hash, &p0_health, sizeof(p0_health));
  hash = NetChecksumBytes(hash, &p1_x, sizeof(p1_x));
  hash = NetChecksumBytes(hash, &p1_y, sizeof(p1_y));
  hash = NetChecksumBytes(hash, &p1_z, sizeof(p1_z));
  hash = NetChecksumBytes(hash, &p1_health, sizeof(p1_health));
  hash = NetChecksumBytes(hash, &alive_monsters, sizeof(alive_monsters));

  net_diag_local.tic = tic;
  net_diag_local.full = hash;
  net_diag_local.rng_full = rng_full_hash;
  net_diag_local.rng_index = rng_index_hash;
  net_diag_local.player0 = player0_hash;
  net_diag_local.player1 = player1_hash;
  net_diag_local.monsters = monsters_hash;
  net_diag_local.alive_monsters = (unsigned int)alive_monsters;
  net_diag_local.p0_x = (int)p0_x;
  net_diag_local.p0_y = (int)p0_y;
  net_diag_local.p0_z = (int)p0_z;
  net_diag_local.p0_health = p0_health;
  net_diag_local.p1_x = (int)p1_x;
  net_diag_local.p1_y = (int)p1_y;
  net_diag_local.p1_z = (int)p1_z;
  net_diag_local.p1_health = p1_health;
  net_diag_local.rndindex = rng.rndindex;
  net_diag_local.prndindex = rng.prndindex;
  net_diag_local.valid = true;

  return hash;
}

static void NetCompareChecksumForTic(unsigned int tic)
{
  int idx;

  idx = tic % BACKUPTICS;

  if (!net_local_checksum_valid[idx] || !net_remote_checksum_valid[idx])
    return;

  if (net_local_checksum_tic[idx] != tic || net_remote_checksum_tic[idx] != tic)
    return;

  if (net_local_checksum_value[idx] != net_remote_checksum_value[idx]) {
      lprintf(LO_ERROR, "Desync detected at tic %u (local=%08x remote=%08x)\n",
        tic, net_local_checksum_value[idx], net_remote_checksum_value[idx]);

      if (net_diag_local.valid && net_diag_local.tic == tic) {
        lprintf(LO_ERROR,
          "Desync diag local tic=%u full=%08x rng_full=%08x rng_index=%08x p0=%08x p1=%08x monsters=%08x alive=%u rnd=%d prnd=%d\n",
          net_diag_local.tic,
          net_diag_local.full,
          net_diag_local.rng_full,
          net_diag_local.rng_index,
          net_diag_local.player0,
          net_diag_local.player1,
          net_diag_local.monsters,
          net_diag_local.alive_monsters,
          net_diag_local.rndindex,
          net_diag_local.prndindex);

        lprintf(LO_ERROR,
          "Desync diag local p0_xyz=(%d,%d,%d) p0_health=%d p1_xyz=(%d,%d,%d) p1_health=%d\n",
          net_diag_local.p0_x,
          net_diag_local.p0_y,
          net_diag_local.p0_z,
          net_diag_local.p0_health,
          net_diag_local.p1_x,
          net_diag_local.p1_y,
          net_diag_local.p1_z,
          net_diag_local.p1_health);
      }

      lprintf(LO_ERROR, "Desync diag remote tic=%u full=%08x\n",
        net_diag_last_remote_tic,
        net_diag_last_remote_checksum);

    net_out_of_sync = true;
  }

  net_local_checksum_valid[idx] = false;
  net_remote_checksum_valid[idx] = false;
}

static void NetMaybeSendChecksum(void)
{
  unsigned int tic;
  unsigned int checksum;
  int idx;
  unsigned char buf[NET_CHECKSUM_SIZE];
  net_checksum_msg_t msg;

  tic = (unsigned int)gametic;
  if ((tic % TICRATE) != 0)
    return;

  checksum = NetBuildChecksum(tic);
  idx = tic % BACKUPTICS;

  net_local_checksum_tic[idx] = tic;
  net_local_checksum_value[idx] = checksum;
  net_local_checksum_valid[idx] = true;

  msg.gametic = tic;
  msg.checksum = checksum;
  net_write_checksum(buf, &msg);

  if (net_send_packet(net_session.socket, NET_MSG_CHECKSUM, buf, NET_CHECKSUM_SIZE) != 0) {
    lprintf(LO_ERROR, "NetMaybeSendChecksum: failed to send checksum\n");
    net_session_disconnect();
    return;
  }

  if (net_desync_diag) {
    lprintf(LO_INFO,
            "Net checksum tic=%u local=%08x rng_full=%08x rng_index=%08x p0=%08x p1=%08x monsters=%08x alive=%u\n",
            tic,
            checksum,
            net_diag_local.rng_full,
            net_diag_local.rng_index,
            net_diag_local.player0,
            net_diag_local.player1,
            net_diag_local.monsters,
            net_diag_local.alive_monsters);
  }

  NetCompareChecksumForTic(tic);
}

static void NetUpdateOutOfSyncMessage(void)
{
  if (net_waiting_for_peer)
    SetCustomMessage(displayplayer, "waiting for peer", 2 * TICRATE, 0);
  else if (net_out_of_sync)
    SetCustomMessage(displayplayer, "out of sync", 2 * TICRATE, 0);
}

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
  dsda_arg_t *arg_host, *arg_join, *arg_port, *arg_latency;
  int port;

  NetResetChecksumState();
  remote_maketic = 0;

  arg_host = dsda_Arg(dsda_arg_host);
  arg_join = dsda_Arg(dsda_arg_join);
  arg_port = dsda_Arg(dsda_arg_port);
  arg_latency = dsda_Arg(dsda_arg_netlatency);
  net_desync_diag = dsda_Flag(dsda_arg_netdesyncdiag);
  port = NET_DEFAULT_PORT;

  if (arg_port->found)
    port = arg_port->value.v_int;

  // Initialize network latency simulation if requested
  if (arg_latency->found) {
    int latency_avg = 138;  // Default: US East Coast ping
    int latency_jitter = 45; // Default: reasonable jitter range
    
    if (arg_latency->count >= 1)
      latency_avg = arg_latency->value.v_int_array[0];
    if (arg_latency->count >= 2)
      latency_jitter = arg_latency->value.v_int_array[1];
    
    net_set_latency(latency_avg, latency_jitter);
    lprintf(LO_INFO, "Network latency simulation enabled: %d ms +/- %d ms\n",
            latency_avg, latency_jitter);
  }

  if (arg_host->found && arg_join->found) {
    I_Error("Cannot use -host and -join at the same time");
  }

  if (arg_host->found) {
    if (arg_host->value.v_int != 2) {
      I_Error("Only 2-player multiplayer is currently supported (use -host 2)");
    }

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

    if (!addr_str || !addr_str[0]) {
      I_Error("-join requires an address (e.g., 127.0.0.1)");
    }

    if (strchr(addr_str, ':')) {
      I_Error("Use -port to set the network port (e.g., -join 127.0.0.1 -port 26101)");
    }

    strncpy(address, addr_str, sizeof(address) - 1);
    address[sizeof(address) - 1] = '\0';

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
    int wait_result = net_wait_for_packet(net_session.socket, 1000);

    if (wait_result == 0) {
      if (!net_waiting_for_peer) {
        lprintf(LO_INFO, "NetRecvRemoteTic: waiting for remote tic %d\n", gametic);
      }

      net_waiting_for_peer = true;
      NetUpdateOutOfSyncMessage();
      return 1;
    }

    if (wait_result < 0) {
      lprintf(LO_ERROR, "NetRecvRemoteTic: socket wait failed\n");
      net_session_disconnect();
      return -1;
    }

    msg_type = net_recv_packet(net_session.socket, buf, &len, sizeof(buf));

    if (msg_type == NET_MSG_TICCMD) {
      net_read_ticcmd(buf, &local_cmds[remote][remote_maketic % BACKUPTICS]);
      remote_maketic++;
      net_waiting_for_peer = false;
    }
    else if (msg_type == NET_MSG_CHECKSUM) {
      net_checksum_msg_t msg;
      int idx;

      if (len != NET_CHECKSUM_SIZE)
        continue;

      net_read_checksum(buf, &msg);
      idx = msg.gametic % BACKUPTICS;
      net_remote_checksum_tic[idx] = msg.gametic;
      net_remote_checksum_value[idx] = msg.checksum;
      net_remote_checksum_valid[idx] = true;
      net_diag_last_remote_tic = msg.gametic;
      net_diag_last_remote_checksum = msg.checksum;

      if (net_desync_diag) {
        lprintf(LO_INFO, "Net checksum recv tic=%u remote=%08x\n",
                msg.gametic, msg.checksum);
      }

      NetCompareChecksumForTic(msg.gametic);
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
    {
      int recv_result = NetRecvRemoteTic();

      if (recv_result != 0)
        return;  // disconnected or still waiting for remote tic
    }

    // Both players have ticcmds for gametic — run one tic
    if (advancedemo)
      D_DoAdvanceDemo();
    M_Ticker();
    G_Ticker();
    NetMaybeSendChecksum();
    if (!net_session_active())
      return;
    gametic++;
    NetUpdateOutOfSyncMessage();
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
    NetUpdateOutOfSyncMessage();
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

  {
    int recv_result = NetRecvRemoteTic();

    if (recv_result != 0)
      return;
  }

  if (advancedemo)
    D_DoAdvanceDemo();
  M_Ticker();
  G_Ticker();
  NetMaybeSendChecksum();
  if (!net_session_active())
    return;
  gametic++;
  NetUpdateOutOfSyncMessage();
}
