/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1991 University of Maryland
 * All Rights Reserved.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of U.M. not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  U.M. makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * U.M. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL U.M.
 * BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author: James da Silva, Systems Design and Analysis Group
 *			   Computer Science Department
 *			   University of Maryland at College Park
 */
/*
 * $Id: stream.c,v 1.4 1997/12/31 01:56:24 jrj Exp $
 *
 * functions for managing stream sockets
 */
#include "amanda.h"
#include "dgram.h"

#include "stream.h"

/* local functions */
static void try_socksize P((int sock, int which, int size));

int stream_server(portp)
int *portp;
{
    int server_socket, len;
    struct sockaddr_in server;

    if((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	return -1;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;

    if(geteuid() == 0) {
	if(bind_reserved(server_socket, &server) == -1) {
	    aclose(server_socket);
	    return -1;
	}
    }
    else {
	/* pick any available non-reserved port */
	server.sin_port = INADDR_ANY;

	if(bind(server_socket, (struct sockaddr *)&server, sizeof(server)) 
	   == -1) {
	    aclose(server_socket);
	    return -1;
	}
    }

    listen(server_socket, 1);

    /* find out what port was actually used */

    len = sizeof(server);
    if(getsockname(server_socket, (struct sockaddr *)&server, &len) == -1) {
	aclose(server_socket);
	return -1;
    }

    *portp = (int) ntohs(server.sin_port);
    return server_socket;
}

int stream_client(hostname, port, sendsize, recvsize)
char *hostname;
int port, sendsize, recvsize;
{
    int client_socket;
    struct sockaddr_in svaddr, claddr;
    struct hostent *hostp;

    if((hostp = gethostbyname(hostname)) == NULL)
	return -1;

    memset(&svaddr, 0, sizeof(svaddr));
    svaddr.sin_family = AF_INET;
    svaddr.sin_port = htons(port);
    memcpy(&svaddr.sin_addr, hostp->h_addr, hostp->h_length);

    if((client_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	return -1;

    if(geteuid() == 0) {	
	/* bind client side to reserved port before connect */
	memset(&claddr, 0, sizeof(claddr));
	claddr.sin_family = AF_INET;
	claddr.sin_addr.s_addr = INADDR_ANY;

	if(bind_reserved(client_socket, &claddr) == -1) {
	    aclose(client_socket);
	    return -1;
	}
    }

    if(connect(client_socket, (struct sockaddr *)&svaddr, sizeof(svaddr))
       == -1) {
	aclose(client_socket);
	return -1;
    }

    if(sendsize != DEFAULT_SIZE) 
	try_socksize(client_socket, SO_SNDBUF, sendsize);
    if(recvsize != DEFAULT_SIZE) 
	try_socksize(client_socket, SO_RCVBUF, recvsize);

    return client_socket;
}

/* don't care about these values */
static struct sockaddr_in addr;
static int addrlen;

int stream_accept(server_socket, timeout, sendsize, recvsize)
int server_socket, timeout, sendsize, recvsize;
{
    fd_set readset;
    struct timeval tv;
    int nfound, connected_socket;

    assert(server_socket >= 0);

    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    FD_ZERO(&readset);
    FD_SET(server_socket, &readset);
    nfound = select(server_socket+1, (SELECT_ARG_TYPE *)&readset, NULL, NULL, &tv);
    if(nfound <= 0 || !FD_ISSET(server_socket, &readset))
	return -1;

    while(1) {
	addrlen = sizeof(struct sockaddr);
	connected_socket = accept(server_socket,
				  (struct sockaddr *)&addr,
				  &addrlen);
	/*
	 * Make certain we got an inet connection and that it is not
	 * from port 20 (a favorite unauthorized entry tool).
	 */
	if(addr.sin_family == AF_INET && htons(addr.sin_port) != 20) {
	    break;
	}
	aclose(connected_socket);
    }

    if(sendsize != DEFAULT_SIZE) 
	try_socksize(connected_socket, SO_SNDBUF, sendsize);
    if(recvsize != DEFAULT_SIZE) 
	try_socksize(connected_socket, SO_RCVBUF, recvsize);

    return connected_socket;
}

static void try_socksize(sock, which, size)
int sock, which, size;
{
    int origsize;

    origsize = size;
    /* keep trying, get as big a buffer as possible */
    while(size > 1024 &&
	  setsockopt(sock, SOL_SOCKET, which, (void *) &size, sizeof(int)) < 0)
	size -= 1024;
}
