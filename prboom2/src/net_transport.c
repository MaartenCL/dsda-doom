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

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;
  #define SOCKET_INVALID INVALID_SOCKET
  #define SOCKET_ERROR_VAL SOCKET_ERROR
  #define NET_CLOSESOCKET closesocket
  #define NET_WOULDBLOCK WSAEWOULDBLOCK
  #define NET_LAST_ERROR WSAGetLastError()
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int socket_t;
  #define SOCKET_INVALID (-1)
  #define SOCKET_ERROR_VAL (-1)
  #define NET_CLOSESOCKET close
  #define NET_WOULDBLOCK EWOULDBLOCK
  #define NET_LAST_ERROR errno
#endif

#include <string.h>
#include <stdio.h>

#include "lprintf.h"
#include "i_system.h"
#include "net_transport.h"

void net_transport_init(void)
{
  static int initialized = 0;
  if (initialized)
    return;
  initialized = 1;

#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    lprintf(LO_ERROR, "net_transport_init: WSAStartup failed\n");
  }
#endif

  I_AtExit(net_transport_shutdown, true, "net_transport_shutdown", exit_priority_normal);
}

void net_transport_shutdown(void)
{
#ifdef _WIN32
  WSACleanup();
#endif
}

static void net_set_nodelay(socket_t sock)
{
  int flag = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
}

int net_listen(int port)
{
  socket_t sock;
  struct sockaddr_in addr;
  int opt = 1;

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == SOCKET_INVALID) {
    lprintf(LO_ERROR, "net_listen: socket() failed\n");
    return -1;
  }

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((unsigned short)port);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR_VAL) {
    lprintf(LO_ERROR, "net_listen: bind() failed on port %d\n", port);
    NET_CLOSESOCKET(sock);
    return -1;
  }

  if (listen(sock, 1) == SOCKET_ERROR_VAL) {
    lprintf(LO_ERROR, "net_listen: listen() failed\n");
    NET_CLOSESOCKET(sock);
    return -1;
  }

  // Set non-blocking for accept polling
#ifdef _WIN32
  {
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
  }
#else
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif

  lprintf(LO_INFO, "net_listen: listening on port %d\n", port);
  return (int)sock;
}

int net_accept(int server_socket)
{
  socket_t client;
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);

  client = accept((socket_t)server_socket, (struct sockaddr *)&addr, &addr_len);
  if (client == SOCKET_INVALID) {
    return -1;  // No pending connection (non-blocking)
  }

  net_set_nodelay(client);

  // Set the accepted socket to blocking mode
#ifdef _WIN32
  {
    u_long mode = 0;
    ioctlsocket(client, FIONBIO, &mode);
  }
#else
  fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) & ~O_NONBLOCK);
#endif

  lprintf(LO_INFO, "net_accept: client connected from %s:%d\n",
          inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
  return (int)client;
}

int net_connect(const char *address, int port)
{
  socket_t sock;
  struct addrinfo hints, *res, *rp;
  char port_str[16];

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port_str, sizeof(port_str), "%d", port);

  if (getaddrinfo(address, port_str, &hints, &res) != 0) {
    lprintf(LO_ERROR, "net_connect: cannot resolve '%s'\n", address);
    return -1;
  }

  sock = SOCKET_INVALID;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == SOCKET_INVALID)
      continue;
    if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) != SOCKET_ERROR_VAL)
      break;  // connected
    NET_CLOSESOCKET(sock);
    sock = SOCKET_INVALID;
  }
  freeaddrinfo(res);

  if (sock == SOCKET_INVALID) {
    lprintf(LO_ERROR, "net_connect: connect() to %s:%d failed\n", address, port);
    return -1;
  }

  net_set_nodelay(sock);

  lprintf(LO_INFO, "net_connect: connected to %s:%d\n", address, port);
  return (int)sock;
}

// Drain and discard 'count' bytes from the socket to re-sync the TCP stream
// after a payload-too-large error, so the next header read sees valid data.
static void net_drain_bytes(socket_t sock, int count)
{
  unsigned char discard[256];
  while (count > 0) {
    int want = count < (int)sizeof(discard) ? count : (int)sizeof(discard);
    int got = recv(sock, (char *)discard, want, 0);
    if (got <= 0)
      break;
    count -= got;
  }
}

// Send exactly 'length' bytes. Returns 0 on success, -1 on error.
static int net_send_all(socket_t sock, const void *data, int length)
{
  const char *ptr = (const char *)data;
  int remaining = length;

  while (remaining > 0) {
    int sent;
#ifdef _WIN32
    sent = send(sock, ptr, remaining, 0);
#else
  #ifdef MSG_NOSIGNAL
    sent = send(sock, ptr, remaining, MSG_NOSIGNAL);
  #else
    sent = send(sock, ptr, remaining, 0);
  #endif
#endif
    if (sent <= 0) {
      return -1;
    }
    ptr += sent;
    remaining -= sent;
  }
  return 0;
}

// Receive exactly 'length' bytes. Returns 0 on success, -1 on error/disconnect.
static int net_recv_all(socket_t sock, void *data, int length)
{
  char *ptr = (char *)data;
  int remaining = length;

  while (remaining > 0) {
    int received = recv(sock, ptr, remaining, 0);
    if (received <= 0) {
      return -1;
    }
    ptr += received;
    remaining -= received;
  }
  return 0;
}

int net_send_packet(int socket, int type, const void *data, int length)
{
  unsigned char header[NET_HEADER_SIZE];

  // Write type (big-endian u16)
  header[0] = (unsigned char)((type >> 8) & 0xff);
  header[1] = (unsigned char)(type & 0xff);
  // Write payload length (big-endian u16)
  header[2] = (unsigned char)((length >> 8) & 0xff);
  header[3] = (unsigned char)(length & 0xff);

  if (net_send_all((socket_t)socket, header, NET_HEADER_SIZE) != 0)
    return -1;

  if (length > 0 && data) {
    if (net_send_all((socket_t)socket, data, length) != 0)
      return -1;
  }

  return 0;
}

int net_recv_packet(int socket, void *data, int *length, int max_length)
{
  unsigned char header[NET_HEADER_SIZE];
  int type, payload_len;

  if (net_recv_all((socket_t)socket, header, NET_HEADER_SIZE) != 0)
    return -1;

  type = (header[0] << 8) | header[1];
  payload_len = (header[2] << 8) | header[3];

  if (payload_len > max_length) {
    lprintf(LO_ERROR, "net_recv_packet: payload too large (%d > %d), draining\n",
            payload_len, max_length);
    net_drain_bytes((socket_t)socket, payload_len);
    return -1;
  }

  if (payload_len > 0) {
    if (net_recv_all((socket_t)socket, data, payload_len) != 0)
      return -1;
  }

  if (length)
    *length = payload_len;

  return type;
}

// Like net_recv_packet but gates the initial read with a select() timeout.
// Returns the message type on success, -1 on connection error/disconnect,
// or NET_RECV_TIMEOUT (-2) if no data arrives within timeout_ms.
int net_recv_packet_timeout(int socket, void *data, int *length, int max_length,
                            int timeout_ms)
{
  int wait_result = net_wait_for_packet(socket, timeout_ms);
  if (wait_result == 0)
    return NET_RECV_TIMEOUT;
  if (wait_result < 0)
    return -1;
  return net_recv_packet(socket, data, length, max_length);
}

int net_wait_for_packet(int socket, int timeout_ms)
{
  fd_set readfds;
  struct timeval tv;
  int result;

  if (socket < 0)
    return -1;

  FD_ZERO(&readfds);
  FD_SET((socket_t)socket, &readfds);

  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  result = select(socket + 1, &readfds, NULL, NULL, &tv);
  if (result > 0)
    return 1;

  if (result == 0)
    return 0;

  return -1;
}

void net_close(int socket)
{
  if (socket >= 0) {
    NET_CLOSESOCKET((socket_t)socket);
  }
}

void net_set_timeout(int socket, int milliseconds)
{
#ifdef _WIN32
  DWORD tv = milliseconds;
  setsockopt((socket_t)socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
  struct timeval tv;
  tv.tv_sec = milliseconds / 1000;
  tv.tv_usec = (milliseconds % 1000) * 1000;
  setsockopt((socket_t)socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}
