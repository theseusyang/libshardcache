/*
 * \file
 *
 * \brief Connection setup
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>

#include <pthread.h>

#include "connections.h"

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001
#endif

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif


/**
 * \brief Convert "localhost" or "localhost:smtp" and store it in sockaddr.
 * \returns 0 if sockaddr is valid or -1 otherwise.
 */
int
string2sockaddr(const char *host, int port, struct sockaddr_in *sockaddr)
{
    u_int32_t ip = htonl(INADDR_LOOPBACK);
    errno = EINVAL;
    int is_nbo = 0;

    if (host) {
        char host2[512];
        char *p;

        strncpy(host2, host, sizeof(host2)-1);
        p = strchr(host2, ':');
        if (p) {
            *p = 0;
            p++;
        }

        struct addrinfo *info = NULL;
        struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
            .ai_addrlen = sizeof(struct sockaddr_in),
        };

        int rc = -1;

        // XXX - we need to serialize calls to getaddrinfo() here
        // to workaround a bug in some older glibc versions
        // triggered by many concurrent calls
        // https://sourceware.org/bugzilla/show_bug.cgi?id=15946
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

        pthread_mutex_lock(&lock);
        if (strcmp(host2, "*") == 0) {
            ip = INADDR_ANY;
            if (p)
                rc = getaddrinfo(NULL, p, &hints, &info);
        } else {
            rc = getaddrinfo(host2, p, &hints, &info);
        }
        pthread_mutex_unlock(&lock);

        if (rc == 0) {
            if (ip != INADDR_ANY)
                ip = ((struct sockaddr_in *)info->ai_addr)->sin_addr.s_addr;
            if (p) {
                port = ((struct sockaddr_in *)info->ai_addr)->sin_port;
                is_nbo = 1;
            }
            freeaddrinfo(info);
        } else {
            errno = ENOENT;
            return -1;
        }

    }

    if (port == 0)
        return -1;

    bzero(sockaddr, sizeof(struct sockaddr_in));
#ifndef __linux
    sockaddr->sin_len = sizeof(struct sockaddr_in);
#endif
    sockaddr->sin_family = AF_INET;
    sockaddr->sin_addr.s_addr = ip;
    sockaddr->sin_port = is_nbo ? port : htons(port);

    return 0;
}
/*!
 * \brief Open a listen socket.
 * \param host hostname to listen on
 * \param port port to listen on
 * \returns file handle for socket to call accept() on or -1 otherwise (errno is set).
 *
 * \note Examples of valid port combinations: ("*", 3456), ("localhost", 3456),
 * or ("10.0.0.9", 4546).
 */
int
open_socket(const char *host, int port)
{
    int val = 1;
    struct sockaddr_in sockaddr;
    int sock;
    struct linger ling = {0, 0};

    errno = EINVAL;
    if ((host == NULL || !*host) && port == 0)
        return -1;

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
        return -1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &val,  sizeof(val));
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    if (string2sockaddr(host, port, &sockaddr) == -1
        || bind(sock, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1)
    {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        return -1;
    }

    listen(sock, -1);
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    return sock;
}

/*!
 * \brief Writes to a socket
 * \param fd socket
 * \param buf buffer to write
 * \param len size of the buffer
 * \returns number of written bytes (or 0) on success, -1 on error (errno is se to the underlying error)
 */
int write_socket(int fd, char *buf, int len) {
    int wb = 0;
    int ofx = 0;
    do {
        len -= wb;
        ofx += wb;
        wb =  write(fd, buf+ofx, len);
        if (wb == -1) {
            if (errno != EINTR && errno != EAGAIN) {
                fprintf(stderr, "write on fd %d failed: %s", fd, strerror(errno));
                return -1;
            }
            wb = 0;
        } else if (wb == 0) {
            break;
        }
    } while (wb != len);
    return wb;
}

/*!
 * \brief Read from a socket
 * \param fd socket
 * \param buf buffer where to store the read data
 * \param len pointer to an integer indicating the size of the buffer in input
 *            and the actual size written on output
 * \param ignore_timeout if true the tcp timeout will be ignored
 * \returns the number of read bytes (or 0) on success, -1 on error (errno is set to the underlying error)
 */
int read_socket(int fd, char *buf, int len, int ignore_timeout) {
    int rb = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;

    do {
        rb =  read(fd, buf, len);
    } while(rb < 0 && (errno == EINTR || (!(flags&O_NONBLOCK) && ignore_timeout && errno == EAGAIN)));
    return rb;
}


/*!
 * \brief Open a TCP connection to a client.
 * \param host hostname
 * \param port port number
 * \param timeout timeout in milliseconds for connection (send and receive)
 *        0 to use the system default
 * \returns file handle on success, or -1 otherwise (errno is set).
 *
 * \note Examples for valid host and port combinations: ("test.com", 1099),
 * ("test.com:1099", 0), or ("10.0.0.10", 1099).
 */
int
open_connection(const char *host, int port, unsigned int timeout)
{
    int val = 1;
    struct sockaddr_in sockaddr;
    int sock;
    int secs = timeout/1000;
    int msecs = (timeout%1000) * 1000; // struct timeval wants microsecs
    struct timeval tv = { secs, msecs * 1000 };

    errno = EINVAL;
    if (host == NULL || !*host || port == 0)
        return -1;


    if (string2sockaddr(host, port, &sockaddr) == -1)
        return -1;

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
        return -1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &val,  sizeof(val));

    if (timeout > 0) {
        if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1
            || setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1)
        {
            fprintf(stderr, "%s:%d: Failed to set timeout to %d : %s\n", host, port, timeout, strerror(errno));
            shutdown(sock, SHUT_RDWR);
            close(sock);
            return -1;
        }
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        return -1;
    }

    if (timeout > 0) {
        flags |= O_NONBLOCK;
        fcntl(sock, F_SETFL, flags);

        int rc = connect(sock, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
        if (rc == 0 || errno == EISCONN) {
            flags &= ~O_NONBLOCK;
            fcntl(sock, F_SETFL, flags);
            fcntl(sock, F_SETFD, FD_CLOEXEC);
            return sock;
        } else if (rc == -1 && errno != EINPROGRESS) {
            fprintf(stderr, "Can't connect to %s:%d : %s\n", host, port, strerror(errno));
            shutdown(sock, SHUT_RDWR);
            close(sock);
            return -1;
        }

        // XXX - hack to overcome the 1024 FD_SETSIZE limit on some
        //       system's select() implementation (notably linux)
        struct rlimit rlim;
        int fdset_size = sizeof(fd_set) * 8; // defaults to at least 8192 fds
        if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
            int limit = sizeof(fd_set) * rlim.rlim_max;
            if (!limit)
                limit = sizeof(fd_set) * rlim.rlim_cur;
            if (limit > 0)
                fdset_size = limit/FD_SETSIZE;
        }

        void *fdset = calloc(1, fdset_size); // we want to fit at most 8192 filedescriptors
        FD_SET(sock, (fd_set *)fdset);

        struct timeval timeout = { tv.tv_sec, tv.tv_usec };
        struct timeval before;
        gettimeofday(&before, NULL);
        rc = select(sock + 1, NULL, (fd_set *)fdset, NULL, &timeout);
        while (rc >= 0)
        {
            int err;
            socklen_t len = sizeof err;
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);

            if (err == 0) {
                flags &= ~O_NONBLOCK;
                fcntl(sock, F_SETFL, flags);
                fcntl(sock, F_SETFD, FD_CLOEXEC);
                free(fdset);
                return sock;
            } else if (err != EINPROGRESS) {
                free(fdset);
                shutdown(sock, SHUT_RDWR);
                close(sock);
                errno = err;
                fprintf(stderr, "Can't connect (2) to %s:%d : %s\n", host, port, strerror(errno));
                return -1;
            }
            struct timeval now;
            struct timeval diff = { 0, 0 };
            gettimeofday(&now, NULL);
            timersub(&now, &before, &diff);
            if (timercmp(&diff, &tv, >)) {
                fprintf(stderr, "Can't connect to %s:%d : Timeout occurred\n", host, port);
                break;
            }
            memcpy(&timeout, &tv, sizeof(struct timeval));
            memset(fdset, 0, fdset_size);
            FD_SET(sock, (fd_set *)fdset);
            rc = select(sock + 1, NULL, (fd_set *)fdset, NULL, &timeout);
        }

        free(fdset);
        shutdown(sock, SHUT_RDWR);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        return -1;
    }

    fcntl(sock, F_SETFD, FD_CLOEXEC);
    return sock;
}

/*!
 * \brief Open a UNIX domain socket.
 * \param filename filename for socket
 * \returns file handle for socket to call accept() on or -1 otherwise (errno is set).
 */
int
open_lsocket(const char *filename)
{
    struct sockaddr_un sockaddr;
    int sock;

    errno = EINVAL;
    if (filename == NULL || !*filename)
        return -1;

    sock = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sock == -1)
        return -1;

    unlink(filename);

    sockaddr.sun_family = AF_UNIX;
    strncpy(sockaddr.sun_path, filename, sizeof(sockaddr.sun_path));

    if (bind(sock, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1 || listen(sock, -1) == -1)
    {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        return -1;
    }

    fcntl(sock, F_SETFD, FD_CLOEXEC);

    return sock;
}

/*!
 * \brief Open a FIFO.
 * \param filename FIFO file name
 * \returns file handle on success, or -1 otherwise (errno is set).
 */

int
open_fifo(const char *filename)
{
    struct stat sb;
    int fd;

    if (mkfifo(filename, S_IFIFO | 0600) != 0) {
    if (errno == EEXIST) {
        if (stat(filename, &sb) == -1) {
        errno = EEXIST;        // reset errno to the previous value
        return -1;
        } else if (!S_ISFIFO(sb.st_mode)) {
        return -1;
        }
    } else {
        return -1;
    }
    }

    fd = open(filename, O_RDWR|O_EXCL|O_NONBLOCK);
    if (fd == -1)
    return -1;

    fcntl(fd, F_SETFD, FD_CLOEXEC);

    return fd;
}

// vim: tabstop=4 shiftwidth=4 expandtab:
/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
