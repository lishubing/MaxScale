#pragma once
#ifndef _MAXSCALE_GW_HG
#define _MAXSCALE_GW_HG
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/cdefs.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <maxscale/gwdirs.h>

MXS_BEGIN_DECLS

// network buffer is 32K
#define MAX_BUFFER_SIZE 32768

/**
 * Configuration for send and receive socket buffer sizes for
 * backend and cleint connections.
 */
#define GW_BACKEND_SO_SNDBUF (128 * 1024)
#define GW_BACKEND_SO_RCVBUF (128 * 1024)
#define GW_CLIENT_SO_SNDBUF  (128 * 1024)
#define GW_CLIENT_SO_RCVBUF  (128 * 1024)

#define GW_NOINTR_CALL(A)       do { errno = 0; A; } while (errno == EINTR)

bool gw_daemonize(void);

MXS_END_DECLS

#endif
