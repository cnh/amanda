/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 2008, 2009, 2010 Zmanda, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 * Contact information: Zmanda Inc., 465 S. Mathilda Ave., Suite 300
 * Sunnyvale, CA 94085, USA, or: http://www.zmanda.com
 */

#include "amanda.h"
#include "amxfer.h"
#include "element-glue.h"
#include "directtcp.h"
#include "util.h"
#include "sockaddr-util.h"

/*
 * Instance definition
 */

typedef struct XferElementGlue_ {
    XferElement __parent__;

    /* instructions to push_buffer_impl */
    enum {
	PUSH_TO_RING_BUFFER,
	PUSH_TO_FD, /* write to *write_fdp */
	PUSH_INVALID,

	PUSH_ACCEPT_FIRST = (1 << 16),
	PUSH_CONNECT_FIRST = (2 << 16),
    } on_push;

    /* instructions to pull_buffer_impl */
    enum {
	PULL_FROM_RING_BUFFER,
	PULL_FROM_FD, /* read from *read_fdp */
	PULL_INVALID,

	PULL_ACCEPT_FIRST = (1 << 16),
	PULL_CONNECT_FIRST = (2 << 16),
    } on_pull;

    int *write_fdp;
    int *read_fdp;

    gboolean need_thread;

    /* the stuff we might use, depending on what flavor of glue we're
     * providing.. */
    int pipe[2];
    int input_listen_socket, output_listen_socket;
    int input_data_socket, output_data_socket;

    /* a ring buffer of ptr/size pairs with semaphores */
    struct { gpointer buf; size_t size; } *ring;
    semaphore_t *ring_used_sem, *ring_free_sem;
    gint ring_head, ring_tail;

    GThread *thread;
    GThreadFunc threadfunc;
} XferElementGlue;

/*
 * Class definition
 */

typedef struct XferElementGlueClass_ {
    XferElementClass __parent__;
} XferElementGlueClass;

static GObjectClass *parent_class = NULL;

/*
 * Utility functions, etc.
 */

static void
make_pipe(
    XferElementGlue *self)
{
    if (pipe(self->pipe) < 0)
	g_critical(_("Could not create pipe: %s"), strerror(errno));
}

static void
send_xfer_done(
    XferElementGlue *self)
{
    xfer_queue_message(XFER_ELEMENT(self)->xfer,
	    xmsg_new((XferElement *)self, XMSG_DONE, 0));
}

static gboolean
do_directtcp_listen(
    XferElement *elt,
    int *sockp,
    DirectTCPAddr **addrsp)
{
    int sock;
    sockaddr_union addr;
    DirectTCPAddr *addrs;
    socklen_t len;

    sock = *sockp = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
	xfer_cancel_with_error(elt, "socket(): %s", strerror(errno));
	return FALSE;
    }

    if (listen(sock, 1) < 0) {
	xfer_cancel_with_error(elt, "listen(): %s", strerror(errno));
	return FALSE;
    }

    /* TODO: which addresses should this display? all ifaces? localhost? */
    len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr *)&addr, &len) < 0)
	error("getsockname(): %s", strerror(errno));
    g_assert(SU_GET_FAMILY(&addr) == AF_INET);

    addrs = g_new0(DirectTCPAddr, 2);
    addrs[0].ipv4 = ntohl(inet_addr("127.0.0.1")); /* TODO: be smarter! */
    addrs[0].port = SU_GET_PORT(&addr);
    *addrsp = addrs;

    return TRUE;
}

static gboolean
prolong_accept(
    gpointer data)
{
    return !XFER_ELEMENT(data)->cancelled;
}

static int
do_directtcp_accept(
    XferElementGlue *self,
    int *socketp)
{
    int sock;
    g_assert(*socketp != -1);

    if ((sock = interruptible_accept(*socketp, NULL, NULL,
				     prolong_accept, self)) == -1) {
	/* if the accept was interrupted due to a cancellation, then do not
	 * add a further error message */
	if (errno == 0 && XFER_ELEMENT(self)->cancelled)
	    return -1;

	xfer_cancel_with_error(XFER_ELEMENT(self),
	    _("Error accepting incoming connection: %s"), strerror(errno));
	wait_until_xfer_cancelled(XFER_ELEMENT(self)->xfer);
	return -1;
    }

    /* close the listening socket now, for good measure */
    close(*socketp);
    *socketp = -1;

    return sock;
}

static int
do_directtcp_connect(
    XferElementGlue *self,
    DirectTCPAddr *addrs)
{
    XferElement *elt = XFER_ELEMENT(self);
    sockaddr_union addr;
    int sock;

    if (!addrs) {
	g_debug("element-glue got no directtcp addresses to connect to!");
	if (!elt->cancelled) {
	    xfer_cancel_with_error(elt,
		"%s got no directtcp addresses to connect to",
		xfer_element_repr(elt));
	}
	goto cancel_wait;
    }

    /* set up the sockaddr -- IPv4 only */
    SU_INIT(&addr, AF_INET);
    SU_SET_PORT(&addr, addrs->port);
    ((struct sockaddr_in *)&addr)->sin_addr.s_addr = htonl(addrs->ipv4);

    g_debug("making data connection to %s", str_sockaddr(&addr));
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
	xfer_cancel_with_error(elt,
	    "socket(): %s", strerror(errno));
	goto cancel_wait;
    }
    if (connect(sock, (struct sockaddr *)&addr, SS_LEN(&addr)) < 0) {
	xfer_cancel_with_error(elt,
	    "connect(): %s", strerror(errno));
	goto cancel_wait;
    }

    g_debug("connected to %s", str_sockaddr(&addr));

    return sock;

cancel_wait:
    wait_until_xfer_cancelled(elt->xfer);
    return -1;
}

#define GLUE_BUFFER_SIZE 32768
#define GLUE_RING_BUFFER_SIZE 32

#define mech_pair(IN,OUT) ((IN)*XFER_MECH_MAX+(OUT))

/* if self->read_fdp or self->write_fdp are pointing to this integer, then
 * they should be redirected to point to the upstream's output_fd or
 * downstream's input_fd, respectively, at start() */
static int neighboring_element_fd = -1;

/*
 * Worker thread utility functions
 */

static void
pull_and_write(XferElementGlue *self)
{
    XferElement *elt = XFER_ELEMENT(self);
    int fd = *self->write_fdp;

    while (!elt->cancelled) {
	size_t len;
	char *buf;

	/* get a buffer from upstream */
	buf = xfer_element_pull_buffer(elt->upstream, &len);
	if (!buf)
	    break;

	/* write it */
	if (full_write(fd, buf, len) < len) {
	    if (!elt->cancelled) {
		xfer_cancel_with_error(elt,
		    _("Error writing to fd %d: %s"), fd, strerror(errno));
		wait_until_xfer_cancelled(elt->xfer);
	    }
	    amfree(buf);
	    break;
	}

	amfree(buf);
    }

    if (elt->cancelled && elt->expect_eof)
	xfer_element_drain_by_pulling(elt->upstream);

    /* close the fd we've been writing, as an EOF signal to downstream, and
     * set it to -1 to avoid accidental re-use */
    close(fd);
    *self->write_fdp = -1;
}

static void
read_and_write(XferElementGlue *self)
{
    XferElement *elt = XFER_ELEMENT(self);
    int rfd = *self->read_fdp;
    int wfd = *self->write_fdp;

    /* dynamically allocate a buffer, in case this thread has
     * a limited amount of stack allocated */
    char *buf = g_malloc(GLUE_BUFFER_SIZE);

    while (!elt->cancelled) {
	size_t len;

	/* read from upstream */
	len = full_read(rfd, buf, GLUE_BUFFER_SIZE);
	if (len < GLUE_BUFFER_SIZE) {
	    if (errno) {
		if (!elt->cancelled) {
		    xfer_cancel_with_error(elt,
			_("Error reading from fd %d: %s"), rfd, strerror(errno));
		    wait_until_xfer_cancelled(elt->xfer);
		}
		break;
	    } else if (len == 0) { /* we only count a zero-length read as EOF */
		break;
	    }
	}

	/* write the buffer fully */
	if (full_write(wfd, buf, len) < len) {
	    if (!elt->cancelled) {
		xfer_cancel_with_error(elt,
		    _("Could not write to fd %d: %s"), wfd, strerror(errno));
		wait_until_xfer_cancelled(elt->xfer);
	    }
	    break;
	}
    }

    if (elt->cancelled && elt->expect_eof)
	xfer_element_drain_by_pulling(elt->upstream);

    /* close the read fd, if it's at EOF, and set it to -1 to avoid accidental
     * re-use */
    if (!elt->cancelled || elt->expect_eof) {
	close(rfd);
	*self->read_fdp = -1;
    }

    /* close the fd we've been writing, as an EOF signal to downstream, and
     * set it to -1 to avoid accidental re-use */
    close(wfd);
    *self->write_fdp = -1;

    amfree(buf);
}

static void
read_and_push(
    XferElementGlue *self)
{
    XferElement *elt = XFER_ELEMENT(self);
    int fd = *self->read_fdp;

    while (!elt->cancelled) {
	char *buf = g_malloc(GLUE_BUFFER_SIZE);
	size_t len;

	/* read a buffer from upstream */
	len = full_read(fd, buf, GLUE_BUFFER_SIZE);
	if (len < GLUE_BUFFER_SIZE) {
	    if (errno) {
		if (!elt->cancelled) {
		    int saved_errno = errno;
		    xfer_cancel_with_error(elt,
			_("Error reading from fd %d: %s"), fd, strerror(saved_errno));
		    g_debug("element-glue: error reading from fd %d: %s",
			    fd, strerror(saved_errno));
		    wait_until_xfer_cancelled(elt->xfer);
		}
		break;
	    } else if (len == 0) { /* we only count a zero-length read as EOF */
		amfree(buf);
		break;
	    }
	}

	xfer_element_push_buffer(elt->downstream, buf, len);
    }

    if (elt->cancelled && elt->expect_eof)
	xfer_element_drain_by_reading(fd);

    /* send an EOF indication downstream */
    xfer_element_push_buffer(elt->downstream, NULL, 0);

    /* close the read fd, since it's at EOF, and set it to -1 to avoid accidental
     * re-use */
    close(fd);
    *self->read_fdp = -1;
}

static void
pull_and_push(XferElementGlue *self)
{
    XferElement *elt = XFER_ELEMENT(self);
    gboolean eof_sent = FALSE;

    while (!elt->cancelled) {
	char *buf;
	size_t len;

	/* get a buffer from upstream */
	buf = xfer_element_pull_buffer(elt->upstream, &len);

	/* and push it downstream */
	xfer_element_push_buffer(elt->downstream, buf, len);

	if (!buf) {
	    eof_sent = TRUE;
	    break;
	}
    }

    if (elt->cancelled && elt->expect_eof)
	xfer_element_drain_by_pulling(elt->upstream);

    if (!eof_sent)
	xfer_element_push_buffer(elt->downstream, NULL, 0);
}

static gpointer
worker_thread(
    gpointer data)
{
    XferElement *elt = XFER_ELEMENT(data);
    XferElementGlue *self = XFER_ELEMENT_GLUE(data);

    switch (mech_pair(elt->input_mech, elt->output_mech)) {
    case mech_pair(XFER_MECH_READFD, XFER_MECH_WRITEFD):
	read_and_write(self);
	break;

    case mech_pair(XFER_MECH_READFD, XFER_MECH_PUSH_BUFFER):
    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_PUSH_BUFFER):
	read_and_push(self);
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_READFD):
    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_WRITEFD):
	pull_and_write(self);
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_PUSH_BUFFER):
	pull_and_push(self);
	break;

    case mech_pair(XFER_MECH_READFD, XFER_MECH_DIRECTTCP_LISTEN):
    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_DIRECTTCP_LISTEN):
	if ((self->output_data_socket = do_directtcp_connect(self,
				    elt->downstream->input_listen_addrs)) == -1)
	    break;
	self->write_fdp = &self->output_data_socket;
	read_and_write(self);
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_DIRECTTCP_LISTEN):
	if ((self->output_data_socket = do_directtcp_connect(self,
				    elt->downstream->input_listen_addrs)) == -1)
	    break;
	self->write_fdp = &self->output_data_socket;
	pull_and_write(self);
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_READFD):
    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_WRITEFD):
	if ((self->input_data_socket = do_directtcp_accept(self, &self->input_listen_socket)) == -1)
	    break;
	self->read_fdp = &self->input_data_socket;
	read_and_write(self);
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_PUSH_BUFFER):
	if ((self->input_data_socket = do_directtcp_accept(self,
					    &self->input_listen_socket)) == -1)
	    break;
	self->read_fdp = &self->input_data_socket;
	read_and_push(self);
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_PULL_BUFFER):
    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_PULL_BUFFER):
    case mech_pair(XFER_MECH_READFD, XFER_MECH_PULL_BUFFER):
    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_READFD):
    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_PULL_BUFFER):
    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_READFD):
    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_WRITEFD):
    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_PULL_BUFFER):
    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_DIRECTTCP_LISTEN):
    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_DIRECTTCP_CONNECT):
    default:
	g_assert_not_reached();
	break;

    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_DIRECTTCP_CONNECT):
    case mech_pair(XFER_MECH_READFD, XFER_MECH_DIRECTTCP_CONNECT):
	if ((self->output_data_socket = do_directtcp_accept(self,
					    &self->output_listen_socket)) == -1)
	    break;
	self->write_fdp = &self->output_data_socket;
	read_and_write(self);
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_WRITEFD):
    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_READFD):
	if ((self->input_data_socket = do_directtcp_connect(self,
				    elt->upstream->output_listen_addrs)) == -1)
	    break;
	self->read_fdp = &self->input_data_socket;
	read_and_write(self);
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_PUSH_BUFFER):
	if ((self->input_data_socket = do_directtcp_connect(self,
				    elt->upstream->output_listen_addrs)) == -1)
	    break;
	self->read_fdp = &self->input_data_socket;
	read_and_push(self);
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_DIRECTTCP_CONNECT):
	if ((self->output_data_socket = do_directtcp_accept(self,
					    &self->output_listen_socket)) == -1)
	    break;
	self->write_fdp = &self->output_data_socket;
	pull_and_write(self);
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_DIRECTTCP_CONNECT):
	/* TODO: use async accept's here to avoid order dependency */
	if ((self->output_data_socket = do_directtcp_accept(self,
					    &self->output_listen_socket)) == -1)
	    break;
	self->write_fdp = &self->output_data_socket;
	if ((self->input_data_socket = do_directtcp_accept(self,
					    &self->input_listen_socket)) == -1)
	    break;
	self->read_fdp = &self->input_data_socket;
	read_and_write(self);
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_DIRECTTCP_LISTEN):
	/* TODO: use async connects and select() to avoid order dependency here */
	if ((self->input_data_socket = do_directtcp_connect(self,
				    elt->upstream->output_listen_addrs)) == -1)
	    break;
	self->read_fdp = &self->input_data_socket;
	if ((self->output_data_socket = do_directtcp_connect(self,
				    elt->downstream->input_listen_addrs)) == -1)
	    break;
	self->write_fdp = &self->output_data_socket;
	read_and_write(self);
	break;
    }

    send_xfer_done(self);

    return NULL;
}

/*
 * Implementation
 */

static gboolean
setup_impl(
    XferElement *elt)
{
    XferElementGlue *self = (XferElementGlue *)elt;
    gboolean need_ring = FALSE;
    gboolean need_listen_input = FALSE;
    gboolean need_listen_output = FALSE;

    g_assert(elt->input_mech != XFER_MECH_NONE);
    g_assert(elt->output_mech != XFER_MECH_NONE);
    g_assert(elt->input_mech != elt->output_mech);

    self->read_fdp = NULL;
    self->write_fdp = NULL;
    self->on_push = PUSH_INVALID;
    self->on_pull = PULL_INVALID;
    self->need_thread = FALSE;

    switch (mech_pair(elt->input_mech, elt->output_mech)) {
    case mech_pair(XFER_MECH_READFD, XFER_MECH_WRITEFD):
	/* thread will read from one fd and write to the other */
	self->read_fdp = &neighboring_element_fd;
	self->write_fdp = &neighboring_element_fd;
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_READFD, XFER_MECH_PUSH_BUFFER):
	/* thread will read from one fd and call push_buffer downstream */
	self->read_fdp = &neighboring_element_fd;
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_READFD, XFER_MECH_PULL_BUFFER):
	self->read_fdp = &neighboring_element_fd;
	self->on_pull = PULL_FROM_FD;
	break;

    case mech_pair(XFER_MECH_READFD, XFER_MECH_DIRECTTCP_LISTEN):
	/* thread will connect for output, then read from fd and write to the
	 * socket. */
	self->read_fdp = &neighboring_element_fd;
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_READFD, XFER_MECH_DIRECTTCP_CONNECT):
	/* thread will accept output conn, then read from upstream and write to socket */
	self->read_fdp = &neighboring_element_fd;
	self->need_thread = TRUE;
	need_listen_output = TRUE;
	break;

    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_READFD):
	make_pipe(self);
	elt->input_fd = self->pipe[1];
	self->pipe[1] = -1; /* upstream will close this for us */
	elt->output_fd = self->pipe[0];
	self->pipe[0] = -1; /* downstream will close this for us */
	break;

    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_PUSH_BUFFER):
	/* thread will read from pipe and call downstream's push_buffer */
	make_pipe(self);
	elt->input_fd = self->pipe[1];
	self->pipe[1] = -1; /* upstream will close this for us */
	self->read_fdp = &self->pipe[0];
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_PULL_BUFFER):
	make_pipe(self);
	elt->input_fd = self->pipe[1];
	self->pipe[1] = -1; /* upstream will close this for us */
	self->on_pull = PULL_FROM_FD;
	self->read_fdp = &self->pipe[0];
	break;

    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_DIRECTTCP_LISTEN):
	/* thread will connect for output, then read from pipe and write to socket */
	make_pipe(self);
	elt->input_fd = self->pipe[1];
	self->pipe[1] = -1; /* upstream will close this for us */
	self->read_fdp = &self->pipe[0];
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_WRITEFD, XFER_MECH_DIRECTTCP_CONNECT):
	/* thread will accept output conn, then read from pipe and write to socket */
	make_pipe(self);
	elt->input_fd = self->pipe[1];
	self->pipe[1] = -1; /* upstream will close this for us */
	self->read_fdp = &self->pipe[0];
	self->need_thread = TRUE;
	need_listen_output = TRUE;
	break;

    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_READFD):
	make_pipe(self);
	elt->output_fd = self->pipe[0];
	self->pipe[0] = -1; /* downstream will close this for us */
	self->on_push = PUSH_TO_FD;
	self->write_fdp = &self->pipe[1];
	break;

    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_WRITEFD):
	self->on_push = PUSH_TO_FD;
	self->write_fdp = &neighboring_element_fd;
	break;

    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_PULL_BUFFER):
	self->on_push = PUSH_TO_RING_BUFFER;
	self->on_pull = PULL_FROM_RING_BUFFER;
	need_ring = TRUE;
	break;

    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_DIRECTTCP_LISTEN):
	/* push will connect for output first */
	self->on_push = PUSH_TO_FD | PUSH_CONNECT_FIRST;
	break;

    case mech_pair(XFER_MECH_PUSH_BUFFER, XFER_MECH_DIRECTTCP_CONNECT):
	/* push will accept for output first */
	self->on_push = PUSH_TO_FD | PUSH_ACCEPT_FIRST;
	need_listen_output = TRUE;
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_READFD):
	/* thread will pull from upstream and write to pipe */
	make_pipe(self);
	elt->output_fd = self->pipe[0];
	self->pipe[0] = -1; /* downstream will close this for us */
	self->write_fdp = &self->pipe[1];
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_WRITEFD):
	/* thread will pull from upstream and write to downstream */
	self->write_fdp = &neighboring_element_fd;
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_PUSH_BUFFER):
	/* thread will pull from upstream and push to downstream */
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_DIRECTTCP_LISTEN):
	/* thread will connect for output, then pull from upstream and write to socket */
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_PULL_BUFFER, XFER_MECH_DIRECTTCP_CONNECT):
	/* thread will accept for output, then pull from upstream and write to socket */
	self->need_thread = TRUE;
	need_listen_output = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_READFD):
	/* thread will accept for input, then read from socket and write to pipe */
	make_pipe(self);
	elt->output_fd = self->pipe[0];
	self->pipe[0] = -1; /* downstream will close this for us */
	self->write_fdp = &self->pipe[1];
	self->need_thread = TRUE;
	need_listen_input = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_WRITEFD):
	/* thread will accept for input, then read from socket and write to downstream */
	self->write_fdp = &neighboring_element_fd;
	self->need_thread = TRUE;
	need_listen_input = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_PUSH_BUFFER):
	/* thread will accept for input, then read from socket and push downstream */
	self->need_thread = TRUE;
	need_listen_input = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_PULL_BUFFER):
	/* first pull will accept for input, then read from socket */
	self->on_pull = PULL_FROM_FD | PULL_ACCEPT_FIRST;
	need_listen_input = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_DIRECTTCP_CONNECT):
	/* thread will accept on both sides, then copy from socket to socket */
	self->need_thread = TRUE;
	need_listen_input = TRUE;
	need_listen_output = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_READFD):
	/* thread will connect for input, then read from socket and write to pipe */
	make_pipe(self);
	elt->output_fd = self->pipe[0];
	self->pipe[0] = -1; /* downstream will close this for us */
	self->write_fdp = &self->pipe[1];
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_WRITEFD):
	/* thread will connect for input, then read from socket and write to downstream */
	self->write_fdp = &neighboring_element_fd;
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_PUSH_BUFFER):
	/* thread will connect for input, then read from socket and push downstream */
	self->need_thread = TRUE;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_PULL_BUFFER):
	/* first pull will connect for input, then read from socket */
	self->on_pull = PULL_FROM_FD | PULL_CONNECT_FIRST;
	break;

    case mech_pair(XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_DIRECTTCP_LISTEN):
	/* thread will connect on both sides, then copy from socket to socket */
	self->on_pull = PULL_FROM_FD | PULL_ACCEPT_FIRST;
	self->need_thread = TRUE;
	break;

    default:
	g_assert_not_reached();
	break;
    }

    /* set up ring if desired */
    if (need_ring) {
	self->ring = g_malloc(sizeof(*self->ring) * GLUE_RING_BUFFER_SIZE);
	self->ring_used_sem = semaphore_new_with_value(0);
	self->ring_free_sem = semaphore_new_with_value(GLUE_RING_BUFFER_SIZE);
    }

    if (need_listen_input) {
	if (!do_directtcp_listen(elt,
		    &self->input_listen_socket, &elt->input_listen_addrs))
	    return FALSE;
    }
    if (need_listen_output) {
	if (!do_directtcp_listen(elt,
		    &self->output_listen_socket, &elt->output_listen_addrs))
	    return FALSE;
    }

    return TRUE;
}

static gboolean
start_impl(
    XferElement *elt)
{
    XferElementGlue *self = (XferElementGlue *)elt;

    /* upstream and downstream are now set, so we can point our fdp's to them */
    if (self->write_fdp == &neighboring_element_fd)
	self->write_fdp = &elt->downstream->input_fd;
    if (self->read_fdp == &neighboring_element_fd)
	self->read_fdp = &elt->upstream->output_fd;

    if (self->need_thread)
	self->thread = g_thread_create(worker_thread, (gpointer)self, FALSE, NULL);

    /* we're active if we have a thread that will eventually die */
    return self->need_thread;
}

static gpointer
pull_buffer_impl(
    XferElement *elt,
    size_t *size)
{
    XferElementGlue *self = XFER_ELEMENT_GLUE(elt);

    /* accept first, if required */
    if (self->on_pull & PULL_ACCEPT_FIRST) {
	/* don't accept the next time around */
	self->on_pull &= ~PULL_ACCEPT_FIRST;

	if (elt->cancelled) {
	    *size = 0;
	    return NULL;
	}

	if ((self->input_data_socket = do_directtcp_accept(self,
					    &self->input_listen_socket)) == -1) {
	    /* do_directtcp_accept already signalled an error; xfer
	     * is cancelled */
	    *size = 0;
	    return NULL;
	}

	/* read from this new socket */
	self->read_fdp = &self->input_data_socket;
    }

    /* or connect first, if required */
    if (self->on_pull & PULL_CONNECT_FIRST) {
	/* don't connect the next time around */
	self->on_pull &= ~PULL_CONNECT_FIRST;

	if (elt->cancelled) {
	    *size = 0;
	    return NULL;
	}

	if ((self->input_data_socket = do_directtcp_connect(self,
				    elt->upstream->output_listen_addrs)) == -1) {
	    /* do_directtcp_connect already signalled an error; xfer
	     * is cancelled */
	    *size = 0;
	    return NULL;
	}

	/* read from this new socket */
	self->read_fdp = &self->input_data_socket;
    }

    switch (self->on_pull) {
	case PULL_FROM_RING_BUFFER: {
	    gpointer buf;

	    if (elt->cancelled) {
		/* the finalize method will empty the ring buffer */
		*size = 0;
		return NULL;
	    }

	    /* make sure there's at least one element available */
	    semaphore_down(self->ring_used_sem);

	    /* get it */
	    buf = self->ring[self->ring_tail].buf;
	    *size = self->ring[self->ring_tail].size;
	    self->ring_tail = (self->ring_tail + 1) % GLUE_RING_BUFFER_SIZE;

	    /* and mark this element as free to be overwritten */
	    semaphore_up(self->ring_free_sem);

	    return buf;
	}

	case PULL_FROM_FD: {
	    int fd = *self->read_fdp;
	    char *buf = g_malloc(GLUE_BUFFER_SIZE);
	    ssize_t len;

	    if (elt->cancelled) {
		if (elt->expect_eof)
		    xfer_element_drain_by_reading(fd);

		close(fd);
		*self->read_fdp = -1;

		*size = 0;
		return NULL;
	    }

	    /* read from upstream */
	    len = full_read(fd, buf, GLUE_BUFFER_SIZE);
	    if (len < GLUE_BUFFER_SIZE) {
		if (errno) {
		    if (!elt->cancelled) {
			xfer_cancel_with_error(elt,
			    _("Error reading from fd %d: %s"), fd, strerror(errno));
			wait_until_xfer_cancelled(elt->xfer);
		    }

		    /* return an EOF */
		    amfree(buf);
		    len = 0;

		    /* and finish off the upstream */
		    if (elt->expect_eof) {
			xfer_element_drain_by_reading(fd);
		    }
		    close(fd);
		    *self->read_fdp = -1;
		} else if (len == 0) {
		    /* EOF */
		    g_free(buf);
		    buf = NULL;
		    *size = 0;

		    /* signal EOF to downstream */
		    close(fd);
		    *self->read_fdp = -1;
		}
	    }

	    *size = (size_t)len;

	    return buf;
	}

	default:
	case PULL_INVALID:
	    g_assert_not_reached();
	    return NULL;
    }
}

static void
push_buffer_impl(
    XferElement *elt,
    gpointer buf,
    size_t len)
{
    XferElementGlue *self = (XferElementGlue *)elt;

    /* accept first, if required */
    if (self->on_push & PUSH_ACCEPT_FIRST) {
	/* don't accept the next time around */
	self->on_push &= ~PUSH_ACCEPT_FIRST;

	if (elt->cancelled) {
	    return;
	}

	if ((self->output_data_socket = do_directtcp_accept(self,
					    &self->output_listen_socket)) == -1) {
	    /* do_directtcp_accept already signalled an error; xfer
	     * is cancelled */
	    return;
	}

	/* write to this new socket */
	self->write_fdp = &self->output_data_socket;
    }

    /* or connect first, if required */
    if (self->on_push & PUSH_CONNECT_FIRST) {
	/* don't accept the next time around */
	self->on_push &= ~PUSH_CONNECT_FIRST;

	if (elt->cancelled) {
	    return;
	}

	if ((self->output_data_socket = do_directtcp_connect(self,
				    elt->downstream->input_listen_addrs)) == -1) {
	    /* do_directtcp_connect already signalled an error; xfer
	     * is cancelled */
	    return;
	}

	/* read from this new socket */
	self->write_fdp = &self->output_data_socket;
    }

    switch (self->on_push) {
	case PUSH_TO_RING_BUFFER:
	    /* just drop packets if the transfer has been cancelled */
	    if (elt->cancelled) {
		amfree(buf);
		return;
	    }

	    /* make sure there's at least one element free */
	    semaphore_down(self->ring_free_sem);

	    /* set it */
	    self->ring[self->ring_head].buf = buf;
	    self->ring[self->ring_head].size = len;
	    self->ring_head = (self->ring_head + 1) % GLUE_RING_BUFFER_SIZE;

	    /* and mark this element as available for reading */
	    semaphore_up(self->ring_used_sem);

	    return;

	case PUSH_TO_FD: {
	    int fd = *self->write_fdp;

	    if (elt->cancelled) {
		if (!elt->expect_eof || !buf) {
		    close(fd);
		    *self->write_fdp = -1;

		    /* hack to ensure we won't close the fd again, if we get another push */
		    elt->expect_eof = TRUE;
		}

		amfree(buf);

		return;
	    }

	    /* write the full buffer to the fd, or close on EOF */
	    if (buf) {
		if (full_write(fd, buf, len) < len) {
		    if (!elt->cancelled) {
			xfer_cancel_with_error(elt,
			    _("Error writing to fd %d: %s"), fd, strerror(errno));
			wait_until_xfer_cancelled(elt->xfer);
		    }
		    /* nothing special to do to handle a cancellation */
		}
		amfree(buf);
	    } else {
		close(fd);
		*self->write_fdp = -1;
	    }

	    return;
	}

	default:
	case PUSH_INVALID:
	    g_assert_not_reached();
	    break;
    }
}

static void
instance_init(
    XferElementGlue *self)
{
    XferElement *elt = (XferElement *)self;
    elt->can_generate_eof = TRUE;
    self->pipe[0] = self->pipe[1] = -1;
    self->input_listen_socket = -1;
    self->output_listen_socket = -1;
    self->input_data_socket = -1;
    self->output_data_socket = -1;
}

static void
finalize_impl(
    GObject * obj_self)
{
    XferElementGlue *self = XFER_ELEMENT_GLUE(obj_self);

    /* close our pipes if they're still open (they shouldn't be!) */
    if (self->pipe[0] != -1) close(self->pipe[0]);
    if (self->pipe[1] != -1) close(self->pipe[1]);
    if (self->input_listen_socket != -1) close(self->input_listen_socket);
    if (self->output_listen_socket != -1) close(self->output_listen_socket);
    if (self->input_data_socket != -1) close(self->input_data_socket);
    if (self->output_data_socket != -1) close(self->output_data_socket);

    if (self->ring) {
	/* empty the ring buffer, ignoring syncronization issues */
	while (self->ring_used_sem->value) {
	    if (self->ring[self->ring_tail].buf)
		amfree(self->ring[self->ring_tail].buf);
	    self->ring_tail = (self->ring_tail + 1) % GLUE_RING_BUFFER_SIZE;
	}

	amfree(self->ring);
	semaphore_free(self->ring_used_sem);
	semaphore_free(self->ring_free_sem);
    }

    /* chain up */
    G_OBJECT_CLASS(parent_class)->finalize(obj_self);
}

static xfer_element_mech_pair_t _pairs[] = {
    { XFER_MECH_READFD, XFER_MECH_WRITEFD, 2, 1 }, /* splice or copy */
    { XFER_MECH_READFD, XFER_MECH_PUSH_BUFFER, 1, 1 }, /* read and call */
    { XFER_MECH_READFD, XFER_MECH_PULL_BUFFER, 1, 0 }, /* read on demand */
    { XFER_MECH_READFD, XFER_MECH_DIRECTTCP_LISTEN, 2, 1 }, /* splice or copy */
    { XFER_MECH_READFD, XFER_MECH_DIRECTTCP_CONNECT, 2, 1 }, /* splice or copy */

    { XFER_MECH_WRITEFD, XFER_MECH_READFD, 0, 0 }, /* pipe */
    { XFER_MECH_WRITEFD, XFER_MECH_PUSH_BUFFER, 1, 1 }, /* pipe + read and call*/
    { XFER_MECH_WRITEFD, XFER_MECH_PULL_BUFFER, 1, 0 }, /* pipe + read on demand */
    { XFER_MECH_WRITEFD, XFER_MECH_DIRECTTCP_LISTEN, 2, 1 }, /* pipe + splice or copy*/
    { XFER_MECH_WRITEFD, XFER_MECH_DIRECTTCP_CONNECT, 2, 1 }, /* splice or copy + pipe */

    { XFER_MECH_PUSH_BUFFER, XFER_MECH_READFD, 1, 0 }, /* write on demand + pipe */
    { XFER_MECH_PUSH_BUFFER, XFER_MECH_WRITEFD, 1, 0 }, /* write on demand */
    { XFER_MECH_PUSH_BUFFER, XFER_MECH_PULL_BUFFER, 0, 0 }, /* async queue */
    { XFER_MECH_PUSH_BUFFER, XFER_MECH_DIRECTTCP_LISTEN, 1, 0 }, /* write on demand */
    { XFER_MECH_PUSH_BUFFER, XFER_MECH_DIRECTTCP_CONNECT, 1, 0 }, /* write on demand */

    { XFER_MECH_PULL_BUFFER, XFER_MECH_READFD, 1, 1 }, /* call and write + pipe */
    { XFER_MECH_PULL_BUFFER, XFER_MECH_WRITEFD, 1, 1 }, /* call and write */
    { XFER_MECH_PULL_BUFFER, XFER_MECH_PUSH_BUFFER, 0, 1 }, /* call and call */
    { XFER_MECH_PULL_BUFFER, XFER_MECH_DIRECTTCP_LISTEN, 1, 1 }, /* call and write */
    { XFER_MECH_PULL_BUFFER, XFER_MECH_DIRECTTCP_CONNECT, 1, 1 }, /* call and write */

    { XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_READFD, 2, 1 }, /* splice or copy + pipe */
    { XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_WRITEFD, 2, 1 }, /* splice or copy */
    { XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_PUSH_BUFFER, 1, 1 }, /* read and call */
    { XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_PULL_BUFFER, 1, 0 }, /* read on demand */
    { XFER_MECH_DIRECTTCP_LISTEN, XFER_MECH_DIRECTTCP_CONNECT, 2, 1 }, /* splice or copy */

    { XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_READFD, 2, 1 }, /* splice or copy + pipe */
    { XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_WRITEFD, 2, 1 }, /* splice or copy + pipe */
    { XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_PUSH_BUFFER, 1, 1 }, /* read and call */
    { XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_PULL_BUFFER, 1, 0 }, /* read on demand */
    { XFER_MECH_DIRECTTCP_CONNECT, XFER_MECH_DIRECTTCP_LISTEN, 2, 1 }, /* splice or copy  */

    /* terminator */
    { XFER_MECH_NONE, XFER_MECH_NONE, 0, 0},
};
xfer_element_mech_pair_t *xfer_element_glue_mech_pairs = _pairs;

static void
class_init(
    XferElementGlueClass * selfc)
{
    XferElementClass *klass = XFER_ELEMENT_CLASS(selfc);
    GObjectClass *goc = G_OBJECT_CLASS(selfc);

    klass->setup = setup_impl;
    klass->start = start_impl;
    klass->push_buffer = push_buffer_impl;
    klass->pull_buffer = pull_buffer_impl;

    klass->perl_class = "Amanda::Xfer::Element::Glue";
    klass->mech_pairs = xfer_element_glue_mech_pairs;

    goc->finalize = finalize_impl;

    parent_class = g_type_class_peek_parent(selfc);
}

GType
xfer_element_glue_get_type (void)
{
    static GType type = 0;

    if G_UNLIKELY(type == 0) {
        static const GTypeInfo info = {
            sizeof (XferElementGlueClass),
            (GBaseInitFunc) NULL,
            (GBaseFinalizeFunc) NULL,
            (GClassInitFunc) class_init,
            (GClassFinalizeFunc) NULL,
            NULL /* class_data */,
            sizeof (XferElementGlue),
            0 /* n_preallocs */,
            (GInstanceInitFunc) instance_init,
            NULL
        };

        type = g_type_register_static (XFER_ELEMENT_TYPE, "XferElementGlue", &info, 0);
    }

    return type;
}

/* create an element of this class; prototype is in xfer-element.h */
XferElement *
xfer_element_glue(void)
{
    XferElementGlue *self = (XferElementGlue *)g_object_new(XFER_ELEMENT_GLUE_TYPE, NULL);
    XferElement *elt = XFER_ELEMENT(self);

    return elt;
}
