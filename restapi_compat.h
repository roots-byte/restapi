/* 
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */
/**
 * @file restapi_compat.h
 * @brief Cross-platform socket compatibility helpers.
 */

#ifndef RESTAPI_COMPAT_H
#define RESTAPI_COMPAT_H

#ifndef _WIN32
  /* Must be set before POSIX headers on strict C builds. */
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200112L
  #endif
#endif

#ifdef _WIN32
  /* Windows socket configuration */
  #define _WIN32_WINNT 0x0600
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>

  typedef SOCKET socket_t;
#else
  /* POSIX socket configuration */
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <sys/select.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>

  /* Type and constant compatibility */
  typedef int socket_t; 
  typedef int WSADATA;

  #ifndef INVALID_SOCKET
    #define INVALID_SOCKET (-1)
  #endif
  #ifndef SOCKET_ERROR
    #define SOCKET_ERROR (-1)
  #endif

  /* WinSock-like error codes mapped to POSIX errno */
  #define WSAEWOULDBLOCK   EWOULDBLOCK
  #define WSAEALREADY      EALREADY
  #define WSAEINPROGRESS   EINPROGRESS
  #define WSAEINVAL        EINVAL
  #define WSAETIMEDOUT     ETIMEDOUT
  #define WSAECONNRESET    ECONNRESET
  #define WSAENOTCONN      ENOTCONN
  #define WSAECONNABORTED  ECONNABORTED
  #define WSAENETRESET     ENETRESET
  #define WSAENETDOWN      ENETDOWN
  #define WSAESHUTDOWN     ESHUTDOWN


  /* WinSock-like functions/macros */
  #define closesocket      close
  #define WSACleanup()     ((void)0)
  #define WSAStartup(a,b)  (0)
  #define WSAGetLastError() (errno)
  #define WSASetLastError(e) (errno = (e))

  /* Windows gai_strerrorA equivalent */
  #define gai_strerrorA    gai_strerror

  /* FIONBIO + ioctlsocket compatibility using fcntl */
  #ifndef FIONBIO
    #define FIONBIO 0
  #endif

  static inline int ioctlsocket(socket_t s, long cmd, unsigned long *mode)
  {
      (void)cmd; /* Always treated as nonblocking toggle. */
      int flags = fcntl(s, F_GETFL, 0);
      if (flags < 0) return -1;

      if (*mode) flags |= O_NONBLOCK;
      else       flags &= ~O_NONBLOCK;

      return fcntl(s, F_SETFL, flags);
  }
#endif


#endif /* RESTAPI_COMPAT_H */