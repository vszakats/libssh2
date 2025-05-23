/* Copyright (C) The libssh2 project and its contributors.
 *
 * Sample showing how to do SFTP non-blocking write transfers.
 *
 * The sample code has default values for host name, user name, password
 * and path to copy, but you can specify them on the command line like:
 *
 * $ ./sftp_write_sliding 192.168.0.1 user password thisfile /tmp/storehere
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "libssh2_setup.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <stdio.h>
#include <time.h>  /* for time() */
#include <string.h>

static const char *pubkey = "/home/username/.ssh/id_rsa.pub";
static const char *privkey = "/home/username/.ssh/id_rsa";
static const char *username = "username";
static const char *password = "password";
static const char *loclfile = "sftp_write_nonblock.c";
static const char *sftppath = "/tmp/sftp_write_nonblock.c";

static int waitsocket(libssh2_socket_t socket_fd, LIBSSH2_SESSION *session)
{
    struct timeval timeout;
    int rc;
    fd_set fd;
    fd_set *writefd = NULL;
    fd_set *readfd = NULL;
    int dir;

    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    FD_ZERO(&fd);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
    FD_SET(socket_fd, &fd);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    /* now make sure we wait in the correct direction */
    dir = libssh2_session_block_directions(session);

    if(dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        readfd = &fd;

    if(dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        writefd = &fd;

    rc = select((int)(socket_fd + 1), readfd, writefd, NULL, &timeout);

    return rc;
}

int main(int argc, char *argv[])
{
    uint32_t hostaddr;
    libssh2_socket_t sock;
    int i, auth_pw = 1;
    struct sockaddr_in sin;
    const char *fingerprint;
    int rc;
    LIBSSH2_SESSION *session = NULL;
    LIBSSH2_SFTP *sftp_session;
    LIBSSH2_SFTP_HANDLE *sftp_handle;
    FILE *local;
    char mem[1024 * 1000];
    size_t nread;
    ssize_t nwritten;
    size_t memuse;
    time_t start;
    libssh2_struct_stat_size total = 0;
    int duration;

#ifdef _WIN32
    WSADATA wsadata;

    rc = WSAStartup(MAKEWORD(2, 0), &wsadata);
    if(rc) {
        fprintf(stderr, "WSAStartup failed with error: %d\n", rc);
        return 1;
    }
#endif

    if(argc > 1) {
        hostaddr = inet_addr(argv[1]);
    }
    else {
        hostaddr = htonl(0x7F000001);
    }
    if(argc > 2) {
        username = argv[2];
    }
    if(argc > 3) {
        password = argv[3];
    }
    if(argc > 4) {
        loclfile = argv[4];
    }
    if(argc > 5) {
        sftppath = argv[5];
    }

    rc = libssh2_init(0);
    if(rc) {
        fprintf(stderr, "libssh2 initialization failed (%d)\n", rc);
        return 1;
    }

    local = fopen(loclfile, "rb");
    if(!local) {
        fprintf(stderr, "Cannot open local file %s\n", loclfile);
        return 1;
    }

    /*
     * The application code is responsible for creating the socket
     * and establishing the connection
     */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock == LIBSSH2_INVALID_SOCKET) {
        fprintf(stderr, "failed to create socket.\n");
        goto shutdown;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(22);
    sin.sin_addr.s_addr = hostaddr;
    if(connect(sock, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in))) {
        fprintf(stderr, "failed to connect.\n");
        goto shutdown;
    }

    /* Create a session instance */
    session = libssh2_session_init();
    if(!session) {
        fprintf(stderr, "Could not initialize SSH session.\n");
        goto shutdown;
    }

    /* Since we have set non-blocking, tell libssh2 we are non-blocking */
    libssh2_session_set_blocking(session, 0);

    /* ... start it up. This will trade welcome banners, exchange keys,
     * and setup crypto, compression, and MAC layers
     */
    while((rc = libssh2_session_handshake(session, sock)) ==
          LIBSSH2_ERROR_EAGAIN);
    if(rc) {
        fprintf(stderr, "Failure establishing SSH session: %d\n", rc);
        goto shutdown;
    }

    /* At this point we have not yet authenticated.  The first thing to do
     * is check the hostkey's fingerprint against our known hosts Your app
     * may have it hard coded, may go to a file, may present it to the
     * user, that's your call
     */
    fingerprint = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1);
    fprintf(stderr, "Fingerprint: ");
    for(i = 0; i < 20; i++) {
        fprintf(stderr, "%02X ", (unsigned char)fingerprint[i]);
    }
    fprintf(stderr, "\n");

    if(auth_pw) {
        /* We could authenticate via password */
        while((rc = libssh2_userauth_password(session, username, password)) ==
              LIBSSH2_ERROR_EAGAIN);
        if(rc) {
            fprintf(stderr, "Authentication by password failed.\n");
            goto shutdown;
        }
    }
    else {
        /* Or by public key */
        while((rc = libssh2_userauth_publickey_fromfile(session, username,
                                                        pubkey, privkey,
                                                        password)) ==
              LIBSSH2_ERROR_EAGAIN);
        if(rc) {
            fprintf(stderr, "Authentication by public key failed.\n");
            goto shutdown;
        }
    }

    fprintf(stderr, "libssh2_sftp_init().\n");
    do {
        sftp_session = libssh2_sftp_init(session);

        if(!sftp_session &&
           libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
            fprintf(stderr, "Unable to init SFTP session\n");
            goto shutdown;
        }
    } while(!sftp_session);

    fprintf(stderr, "libssh2_sftp_open().\n");
    /* Request a file via SFTP */
    do {
        sftp_handle = libssh2_sftp_open(sftp_session, sftppath,
                                        LIBSSH2_FXF_WRITE |
                                        LIBSSH2_FXF_CREAT |
                                        LIBSSH2_FXF_TRUNC,
                                        LIBSSH2_SFTP_S_IRUSR |
                                        LIBSSH2_SFTP_S_IWUSR |
                                        LIBSSH2_SFTP_S_IRGRP |
                                        LIBSSH2_SFTP_S_IROTH);
        if(!sftp_handle &&
           libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
            fprintf(stderr, "Unable to open file with SFTP: %ld\n",
                    libssh2_sftp_last_error(sftp_session));
            goto shutdown;
        }
    } while(!sftp_handle);

    fprintf(stderr, "libssh2_sftp_open() is done, now send data.\n");
    start = time(NULL);
    memuse = 0;  /* it starts blank */
    do {
        nread = fread(&mem[memuse], 1, sizeof(mem) - memuse, local);
        if(nread <= 0) {
            /* end of file */
            if(memuse > 0)
                /* the previous sending is not finished */
                nread = 0;
            else
                break;
        }
        memuse += nread;
        total += (libssh2_struct_stat_size)nread;

        /* write data in a loop until we block */
        while((nwritten = libssh2_sftp_write(sftp_handle, mem, memuse)) ==
              LIBSSH2_ERROR_EAGAIN) {
            waitsocket(sock, session);
        }
        if(nwritten < 0)
            break;

        if(memuse - (size_t)nwritten) {
            /* make room for more data at the end of the buffer */
            memmove(&mem[0], &mem[nwritten], memuse - (size_t)nwritten);
            memuse -= (size_t)nwritten;
        }
        else
            /* 'mem' was consumed fully */
            memuse = 0;

    } while(nwritten > 0);

    duration = (int)(time(NULL) - start);

    fprintf(stderr, "%ld bytes in %d seconds makes %.1f bytes/sec\n",
            (long)total, duration, (double)total / duration);

    libssh2_sftp_close(sftp_handle);
    libssh2_sftp_shutdown(sftp_session);

shutdown:

    fclose(local);

    if(session) {
        while(libssh2_session_disconnect(session, "Normal Shutdown") ==
              LIBSSH2_ERROR_EAGAIN);
        libssh2_session_free(session);
    }

    if(sock != LIBSSH2_INVALID_SOCKET) {
        shutdown(sock, 2);
        LIBSSH2_SOCKET_CLOSE(sock);
    }

    fprintf(stderr, "all done\n");

    libssh2_exit();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
