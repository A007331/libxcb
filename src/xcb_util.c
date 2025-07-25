/* Copyright (C) 2001-2004 Bart Massey and Jamey Sharp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 * Except as contained in this notice, the names of the authors or their
 * institutions shall not be used in advertising or otherwise to promote the
 * sale, use or other dealings in this Software without prior written
 * authorization from the authors.
 */

/* Utility functions implementable using only public APIs. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#include "xcb_windefs.h"
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <netdb.h>
#endif /* _WIN32 */

#include "xcb.h"
#include "xcbext.h"
#include "xcbint.h"

#if defined(HAVE_TSOL_LABEL_H) && defined(HAVE_IS_SYSTEM_LABELED)
# include <tsol/label.h>
# include <sys/stat.h>
#endif

#include <sys/stat.h>

#ifndef __has_builtin
# define  __has_builtin(x) 0
#endif

int xcb_popcount(uint32_t mask)
{
#if __has_builtin(__builtin_popcount)
    return __builtin_popcount(mask);
#else
    /*
     * Count the number of bits set to 1 in a 32-bit word.
     * Algorithm from MIT AI Lab Memo 239: "HAKMEM", ITEM 169.
     * https://dspace.mit.edu/handle/1721.1/6086
     */
    uint32_t y;
    y = (mask >> 1) & 033333333333;
    y = mask - y - ((y >> 1) & 033333333333);
    return ((y + (y >> 3)) & 030707070707) % 077;
#endif
}

int xcb_sumof(uint8_t *list, int len)
{
  int i, s = 0;
  for(i=0; i<len; i++) {
    s += *list;
    list++;
  }
  return s;
}

/* Return true and parse if name matches <path to socket>[.<screen>]
 * Upon success:
 *     host = <path to socket>
 *     protocol = "unix"
 *     display = 0
 *     screen = <screen>
 */
static int _xcb_parse_display_path_to_socket(const char *name, char **host, char **protocol,
                                             int *displayp, int *screenp)
{
    struct stat sbuf;
    /* In addition to the AF_UNIX path, there may be a screen number.
     * The trailing \0 is already accounted in the size of sun_path. */
    char path[sizeof(((struct sockaddr_un*)0)->sun_path) + 1 + 10];
    size_t len;
    int _screen = 0, res;

    len = strlen(name);
    if (len >= sizeof(path))
        return 0;
    memcpy(path, name, len + 1);
    res = stat(path, &sbuf);
    if (0 != res) {
        unsigned long lscreen;
	char *dot, *endptr;
        if (res != -1 || (errno != ENOENT && errno != ENOTDIR))
            return 0;
        dot = strrchr(path, '.');
        if (!dot || dot[1] < '1' || dot[1] > '9')
            return 0;
        *dot = '\0';
        errno = 0;
        lscreen = strtoul(dot + 1, &endptr, 10);
        if (lscreen > INT_MAX || !endptr || *endptr || errno)
            return 0;
        if (0 != stat(path, &sbuf))
            return 0;
        _screen = (int)lscreen;
    }

    if (host) {
        *host = strdup(path);
        if (!*host)
            return 0;
    }

    if (protocol) {
        *protocol = strdup("unix");
        if (!*protocol) {
            if (host)
                free(*host);
            return 0;
        }
    }

    if (displayp)
        *displayp = 0;

    if (screenp)
        *screenp = _screen;

    return 1;
}

static int _xcb_parse_display(const char *name, char **host, char **protocol,
                      int *displayp, int *screenp)
{
    int len, display, screen;
    char *slash, *colon, *dot, *end;

    if(!name || !*name)
        name = getenv("DISPLAY");
    if(!name)
        return 0;

    /* First check for <path to socket>[.<screen>] */
    if (name[0] == '/')
        return _xcb_parse_display_path_to_socket(name, host, protocol, displayp, screenp);

    if (strncmp(name, "unix:", 5) == 0)
        return _xcb_parse_display_path_to_socket(name + 5, host, protocol, displayp, screenp);

    slash = strrchr(name, '/');

    if (slash) {
        len = slash - name;
        if (protocol) {
            *protocol = malloc(len + 1);
            if(!*protocol)
                return 0;
            memcpy(*protocol, name, len);
            (*protocol)[len] = '\0';
        }
        name = slash + 1;
    } else
        if (protocol)
            *protocol = NULL;

    colon = strrchr(name, ':');
    if(!colon)
        goto error_out;
    len = colon - name;
    ++colon;
    display = strtoul(colon, &dot, 10);
    if(dot == colon)
        goto error_out;
    if(*dot == '\0')
        screen = 0;
    else
    {
        if(*dot != '.')
            goto error_out;
        ++dot;
        screen = strtoul(dot, &end, 10);
        if(end == dot || *end != '\0')
            goto error_out;
    }
    /* At this point, the display string is fully parsed and valid, but
     * the caller's memory is untouched. */

    *host = malloc(len + 1);
    if(!*host)
        goto error_out;
    memcpy(*host, name, len);
    (*host)[len] = '\0';
    *displayp = display;
    if(screenp)
        *screenp = screen;
    return 1;

error_out:
    if (protocol) {
        free(*protocol);
        *protocol = NULL;
    }

    return 0;
}

int xcb_parse_display(const char *name, char **host, int *displayp,
                             int *screenp)
{
    return _xcb_parse_display(name, host, NULL, displayp, screenp);
}

#ifndef XCB_HAVE_LOCAL_NO_TCP
static int _xcb_open_tcp(const char *host, char *protocol, const unsigned short port);
#endif
#ifndef _WIN32
static int _xcb_open_unix(char *protocol, const char *file);
#endif /* !WIN32 */
#ifdef HAVE_ABSTRACT_SOCKETS
static int _xcb_open_abstract(char *protocol, const char *file, size_t filelen);
#endif

static int _xcb_open(const char *host, char *protocol, const int display)
{
    int fd;
#ifdef __hpux
    static const char unix_base[] = "/usr/spool/sockets/X11/";
#else
    static const char unix_base[] = "/tmp/.X11-unix/X";
#endif
    const char *base = unix_base;
    size_t filelen;
    char *file = NULL;
    int actual_filelen;

#ifndef _WIN32
    if (protocol && strcmp("unix", protocol) == 0 && host && host[0] == '/') {
        /* Full path to socket provided, ignore everything else */
        filelen = strlen(host) + 1;
        if (filelen > INT_MAX)
            return -1;
        file = malloc(filelen);
        if (file == NULL)
            return -1;
        memcpy(file, host, filelen);
        actual_filelen = (int)(filelen - 1);
    } else {
#endif
        /* If protocol or host is "unix", fall through to Unix socket code below */
        if ((!protocol || (strcmp("unix",protocol) != 0)) &&
            (*host != '\0') && (strcmp("unix",host) != 0))
        {
            /* display specifies TCP */
#ifndef XCB_HAVE_LOCAL_NO_TCP
            unsigned short port = X_TCP_PORT + display;
            return _xcb_open_tcp(host, protocol, port);
#else
            return -1;
#endif
        }

#ifndef _WIN32
#if defined(HAVE_TSOL_LABEL_H) && defined(HAVE_IS_SYSTEM_LABELED)
        /* Check special path for Unix sockets under Solaris Trusted Extensions */
        if (is_system_labeled())
        {
            const char *tsol_base = "/var/tsol/doors/.X11-unix/X";
            char tsol_socket[PATH_MAX];
            struct stat sbuf;

            snprintf(tsol_socket, sizeof(tsol_socket), "%s%d", tsol_base, display);

            if (stat(tsol_socket, &sbuf) == 0)
                base = tsol_base;
            else if (errno != ENOENT)
                return 0;
        }
#endif

        filelen = strlen(base) + 1 + sizeof(display) * 3 + 1;
        file = malloc(filelen);
        if(file == NULL)
            return -1;

        /* display specifies Unix socket */
        actual_filelen = snprintf(file, filelen, "%s%d", base, display);

        if(actual_filelen < 0)
        {
            free(file);
            return -1;
        }
        /* snprintf may truncate the file */
        filelen = MIN(actual_filelen, filelen - 1);
#ifdef HAVE_ABSTRACT_SOCKETS
        fd = _xcb_open_abstract(protocol, file, filelen);
        if (fd >= 0 || (errno != ENOENT && errno != ECONNREFUSED))
        {
            free(file);
            return fd;
        }
#endif
    }
    fd = _xcb_open_unix(protocol, file);
    free(file);

    if (fd < 0 && !protocol && *host == '\0') {
#ifndef XCB_HAVE_LOCAL_NO_TCP
            unsigned short port = X_TCP_PORT + display;
            fd = _xcb_open_tcp(host, protocol, port);
#else
            return -1;
#endif
    }

    return fd;
#endif /* !_WIN32 */
    return -1; /* if control reaches here then something has gone wrong */
}

static int _xcb_socket(int family, int type, int proto)
{
    int fd;

#ifdef SOCK_CLOEXEC
    fd = socket(family, type | SOCK_CLOEXEC, proto);
    if (fd == -1 && errno == EINVAL)
#endif
    {
        fd = socket(family, type, proto);
#ifndef _WIN32
        if (fd >= 0)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    }
    return fd;
}


static int _xcb_do_connect(int fd, const struct sockaddr* addr, int addrlen) {
    int on = 1;

    if(fd < 0)
        return -1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

    return connect(fd, addr, addrlen);
}

#ifndef XCB_HAVE_LOCAL_NO_TCP
static int _xcb_open_tcp(const char *host, char *protocol, const unsigned short port)
{
    int fd = -1;
#if HAVE_GETADDRINFO
    struct addrinfo hints;
    char service[6]; /* "65535" with the trailing '\0' */
    struct addrinfo *results, *addr;
    char *bracket;
#endif

    if (protocol && strcmp("tcp",protocol) && strcmp("inet",protocol)
#ifdef AF_INET6
                 && strcmp("inet6",protocol)
#endif
        )
        return -1;

    if (*host == '\0')
        host = "localhost";

#if HAVE_GETADDRINFO
    memset(&hints, 0, sizeof(hints));
#ifdef AI_NUMERICSERV
    hints.ai_flags |= AI_NUMERICSERV;
#endif
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

#ifdef AF_INET6
    /* Allow IPv6 addresses enclosed in brackets. */
    if(host[0] == '[' && (bracket = strrchr(host, ']')) && bracket[1] == '\0')
    {
        *bracket = '\0';
        ++host;
        hints.ai_flags |= AI_NUMERICHOST;
        hints.ai_family = AF_INET6;
    }
#endif

    snprintf(service, sizeof(service), "%hu", port);
    if(getaddrinfo(host, service, &hints, &results))
        /* FIXME: use gai_strerror, and fill in error connection */
        return -1;

    for(addr = results; addr; addr = addr->ai_next)
    {
        fd = _xcb_socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (_xcb_do_connect(fd, addr->ai_addr, addr->ai_addrlen) >= 0)
            break;
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        fd = -1;
    }
    freeaddrinfo(results);
    return fd;
#else
    {
        struct hostent* _h;
        struct sockaddr_in _s;
        struct in_addr ** _c;

        if((_h = gethostbyname(host)) == NULL)
            return -1;

        _c = (struct in_addr**)_h->h_addr_list;
        fd = -1;

        while(*_c) {
            _s.sin_family = AF_INET;
            _s.sin_port = htons(port);
            _s.sin_addr = *(*_c);

            fd = _xcb_socket(_s.sin_family, SOCK_STREAM, 0);
            if(_xcb_do_connect(fd, (struct sockaddr*)&_s, sizeof(_s)) >= 0)
                break;

#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            fd = -1;
            ++_c;
        }

        return fd;
    }
#endif
}
#endif // #ifndef XCB_HAVE_LOCAL_NO_TCP

#ifndef _WIN32
static int _xcb_open_unix(char *protocol, const char *file)
{
    int fd;
    struct sockaddr_un addr;
    socklen_t len = sizeof(int);
    int val;

    if (protocol && strcmp("unix",protocol))
        return -1;

    strcpy(addr.sun_path, file);
    addr.sun_family = AF_UNIX;
#ifdef HAVE_SOCKADDR_SUN_LEN
    addr.sun_len = SUN_LEN(&addr);
#endif
    fd = _xcb_socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd == -1)
        return -1;
    if(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, &len) == 0 && val < 64 * 1024)
    {
        val = 64 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(int));
    }
    if(connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif /* !_WIN32 */

#ifdef HAVE_ABSTRACT_SOCKETS
static int _xcb_open_abstract(char *protocol, const char *file, size_t filelen)
{
    int fd;
    struct sockaddr_un addr = {0};
    socklen_t namelen;

    if (protocol && strcmp("unix",protocol))
        return -1;

    strcpy(addr.sun_path + 1, file);
    addr.sun_family = AF_UNIX;
    namelen = offsetof(struct sockaddr_un, sun_path) + 1 + filelen;
#ifdef HAVE_SOCKADDR_SUN_LEN
    addr.sun_len = 1 + filelen;
#endif
    fd = _xcb_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;
    if (connect(fd, (struct sockaddr *) &addr, namelen) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

xcb_connection_t *xcb_connect(const char *displayname, int *screenp)
{
    return xcb_connect_to_display_with_auth_info(displayname, NULL, screenp);
}

xcb_connection_t *xcb_connect_to_display_with_auth_info(const char *displayname, xcb_auth_info_t *auth, int *screenp)
{
    int fd, display = 0;
    char *host = NULL;
    char *protocol = NULL;
    xcb_auth_info_t ourauth;
    xcb_connection_t *c;

    int parsed = _xcb_parse_display(displayname, &host, &protocol, &display, screenp);

    if(!parsed) {
        c = _xcb_conn_ret_error(XCB_CONN_CLOSED_PARSE_ERR);
        goto out;
    }

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        c = _xcb_conn_ret_error(XCB_CONN_ERROR);
        goto out;
    }
#endif

    fd = _xcb_open(host, protocol, display);

    if(fd == -1) {
        c = _xcb_conn_ret_error(XCB_CONN_ERROR);
#ifdef _WIN32
        WSACleanup();
#endif
        goto out;
    }

    if(auth) {
        c = xcb_connect_to_fd(fd, auth);
    }
    else if(_xcb_get_auth_info(fd, &ourauth, display))
    {
        c = xcb_connect_to_fd(fd, &ourauth);
        free(ourauth.name);
        free(ourauth.data);
    }
    else
        c = xcb_connect_to_fd(fd, 0);

    if(c->has_error)
        goto out;

    /* Make sure requested screen number is in bounds for this server */
    if((screenp != NULL) && (*screenp >= (int) c->setup->roots_len)) {
        xcb_disconnect(c);
        c = _xcb_conn_ret_error(XCB_CONN_CLOSED_INVALID_SCREEN);
        goto out;
    }

out:
    free(host);
    free(protocol);
    return c;
}
