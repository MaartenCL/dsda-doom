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
#include "net_transport.h"

void net_transport_init(void)
{
#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    lprintf(LO_ERROR, "net_transport_init: WSAStartup failed\n");
  }
#endif
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
  struct sockaddr_in addr;
  struct hostent *host;

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == SOCKET_INVALID) {
    lprintf(LO_ERROR, "net_connect: socket() failed\n");
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);

  // Try numeric address first
  addr.sin_addr.s_addr = inet_addr(address);
  if (addr.sin_addr.s_addr == INADDR_NONE) {
    host = gethostbyname(address);
    if (!host) {
      lprintf(LO_ERROR, "net_connect: cannot resolve '%s'\n", address);
      NET_CLOSESOCKET(sock);
      return -1;
    }
    memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
  }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR_VAL) {
    lprintf(LO_ERROR, "net_connect: connect() to %s:%d failed\n", address, port);
    NET_CLOSESOCKET(sock);
    return -1;
  }

  net_set_nodelay(sock);

  lprintf(LO_INFO, "net_connect: connected to %s:%d\n", address, port);
  return (int)sock;
}

// Send exactly 'length' bytes. Returns 0 on success, -1 on error.
static int net_send_all(socket_t sock, const void *data, int length)
{
  const char *ptr = (const char *)data;
  int remaining = length;

  while (remaining > 0) {
    int sent = send(sock, ptr, remaining, 0);
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
    lprintf(LO_ERROR, "net_recv_packet: payload too large (%d > %d)\n",
            payload_len, max_length);
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
