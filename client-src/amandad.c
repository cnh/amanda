/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1991-1999 University of Maryland at College Park
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
 * Authors: the Amanda Development Team.  Its members are listed in a
 * file named AUTHORS, in the root directory of this distribution.
 */

/*
 * $Id: amandad.c,v 1.42 1999/09/15 00:31:23 jrj Exp $
 *
 * handle client-host side of Amanda network communications, including
 * security checks, execution of the proper service, and acking the
 * master side
 */

/*#define	AMANDAD_DEBUG*/

#include "amanda.h"
#include "amandad.h"
#include "event.h"
#include "packet.h"
#include "version.h"
#include "queue.h"
#include "security.h"

#define	REP_TIMEOUT	(6*60*60)	/* secs for service to reply */
#define	ACK_TIMEOUT  	10		/* XXX should be configurable */
#define	MAX_REP_RETRIES	5

/*
 * These are the actions for entering the state machine
 */
typedef enum { A_START, A_RECVPKT, A_RECVREP, A_PENDING, A_FINISH, A_CONTINUE,
    A_SENDNAK, A_TIMEOUT } action_t;

/*
 * This is a state in the state machine.  It is a function pointer to
 * the function that actually implements the state.
 */
struct active_service;
typedef action_t (*state_t) P((struct active_service *, action_t, pkt_t *));

/*
 * This structure describes an active running service.
 *
 * An active service is something running that we have received
 * a request for.  This structure holds info on that service, including
 * file descriptors for data, etc, as well as the security handle
 * for communications with the amanda server.
 */
struct active_service {
    char *cmd;				/* name of command we ran */
    char *arguments;			/* arguments we sent it */
    security_handle_t *security_handle;	/* remote server */
    state_t state;			/* how far this has progressed */
    pid_t pid;				/* pid of subprocess */
    int reqfd;				/* pipe to write requests */
    int repfd;				/* pipe to read replies */
    event_handle_t *ev_repfd;		/* read event handle for repfd */
    event_handle_t *ev_reptimeout;	/* timeout for rep data */
    pkt_t rep_pkt;			/* rep packet we're sending out */
    char repbuf[MAX_PACKET];		/* buffer to read the rep into */
    int repbufsize;			/* length of repbuf */
    int repretry;			/* times we'll retry sending the rep */
    /*
     * General user streams to the process, and their equivalent
     * network streams.
     */
    struct datafd_handle {
	int fd;				/* pipe to child process */
	event_handle_t *ev_handle;	/* it's read event handle */
	security_stream_t *netfd;	/* stream to amanda server */
	struct active_service *as;	/* pointer back to our enclosure */
    } data[DATA_FD_COUNT];
    char databuf[TAPE_BLOCK_BYTES];	/* buffer to relay netfd data in */
    TAILQ_ENTRY(active_service) tq;	/* queue handle */
};

/* 
 * Here are the services that we allow.
 */
static const char *services[] = {
    "sendsize",
    "sendbackup",
    "selfcheck",
};
#define	NSERVICES	(sizeof(services) / sizeof(services[0]))

/*
 * Queue of outstanding requests that we are running.
 */
static struct {
    TAILQ_HEAD(, active_service) tailq;
    int qlength;
} serviceq = {
    TAILQ_HEAD_INITIALIZER(serviceq.tailq), 0
};

/*
 * Data for dbmalloc to check for memory leaks
 */
#ifdef USE_DBMALLOC
static struct {
    struct {
	unsigned long size, hist;
    } start, end;
} dbmalloc_info;
#endif

int ack_timeout     = ACK_TIMEOUT;

int main P((int argc, char **argv));

static int allocstream P((struct active_service *, int));
static void exit_check P((void *));
static void protocol_accept P((security_handle_t *, pkt_t *));
static void state_machine P((struct active_service *, action_t, pkt_t *));

static action_t s_sendack P((struct active_service *, action_t, pkt_t *));
static action_t s_repwait P((struct active_service *, action_t, pkt_t *));
static action_t s_processrep P((struct active_service *, action_t, pkt_t *));
static action_t s_sendrep P((struct active_service *, action_t, pkt_t *));
static action_t s_ackwait P((struct active_service *, action_t, pkt_t *));

static void repfd_recv P((void *));
static void timeout_repfd P((void *));
static void protocol_recv P((void *, pkt_t *, security_status_t));
static void process_netfd P((void *));
static struct active_service *service_new P((security_handle_t *,
    const char *, const char *));
static void service_delete P((struct active_service *));
static int writebuf P((int, const void *, size_t));

#ifdef AMANDAD_DEBUG
static const char *state2str P((state_t));
static const char *action2str P((action_t));
#endif

int
main(argc, argv)
    int argc;
    char **argv;
{
    int i, in, out;
    const security_driver_t *secdrv;
    int no_exit = 0;

    /*
     * Make sure nobody spoofs us with a lot of extra open files
     * that would cause an open we do to get a very high file
     * descriptor, which in turn might be used as an index into
     * an array (e.g. an fd_set).
     */
    for (i = 3; i < FD_SETSIZE; i++)
	close(i);

    safe_cd();

    set_pname("amandad");

#ifdef USE_DBMALLOC
    dbmalloc_info.start.size = malloc_inuse(&dbmalloc_info.start.hist);
#endif

    erroutput_type = (ERR_INTERACTIVE|ERR_SYSLOG);

#ifdef FORCE_USERID
    /* we'd rather not run as root */
    if (geteuid() == 0) {
	if(client_uid == (uid_t) -1) {
	    error("error [cannot find user %s in passwd file]\n", CLIENT_LOGIN);
	}
	initgroups(CLIENT_LOGIN, client_gid);
	setgid(client_gid);
	setegid(client_gid);
	seteuid(client_uid);
    }
#endif	/* FORCE_USERID */

    /*
     * ad-hoc argument parsing
     *
     * We accept	-auth=[authentication type]
     *			-no-exit
#ifdef AMANDAD_DEBUG
     *			-tcp=[port]
     *			-udp=[port]
#endif
     */
    secdrv = NULL;
    in = 0; out = 1;		/* default to stdin/stdout */
    for (i = 1; i < argc; i++) {
	/*
	 * accept -krb4 as an alias for -auth=krb4 (for compatibility)
	 */
	if (strcmp(argv[i], "-krb4") == 0) {
	    argv[i] = "-auth=krb4";
	    /* FALLTHROUGH */
	}

	/*
	 * Get a driver for a security type specified after -auth=
	 */
	if (strncmp(argv[i], "-auth=", strlen("-auth=")) == 0) {
	    argv[i] += strlen("-auth=");
	    secdrv = security_getdriver(argv[i]);
	    if (secdrv == NULL)
		error("no driver for security type '%s'", argv[i]);
	    continue;
	}

	/*
	 * If -no-exit is specified, always run even after requests have
	 * been satisfied.
	 */
	if (strcmp(argv[i], "-no-exit") == 0) {
	    no_exit = 1;
	    continue;
	}

#ifdef AMANDAD_DEBUG
	/*
	 * Allow us to directly bind to a udp port for debugging.
	 * This may only apply to some security types.
	 */
	if (strncmp(argv[i], "-udp=", strlen("-udp=")) == 0) {
	    struct sockaddr_in sin;

	    argv[i] += strlen("-udp=");
	    in = out = socket(AF_INET, SOCK_DGRAM, 0);
	    if (in < 0)
		error("can't create dgram socket: %s\n", strerror(errno));
	    sin.sin_family = AF_INET;
	    sin.sin_addr.s_addr = INADDR_ANY;
	    sin.sin_port = htons(atoi(argv[i]));
	    if (bind(in, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		error("can't bind to port %d: %s\n", atoi(argv[i]),
		    strerror(errno));
	}
	/*
	 * Ditto for tcp ports.
	 */
	if (strncmp(argv[i], "-tcp=", strlen("-tcp=")) == 0) {
	    struct sockaddr_in sin;
	    int sock, n;

	    argv[i] += strlen("-tcp=");
	    sock = socket(AF_INET, SOCK_STREAM, 0);
	    if (sock < 0)
		error("can't create tcp socket: %s\n", strerror(errno));
	    n = 1;
	    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&n, sizeof(n));
	    sin.sin_family = AF_INET;
	    sin.sin_addr.s_addr = INADDR_ANY;
	    sin.sin_port = htons(atoi(argv[i]));
	    if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		error("can't bind to port %d: %s\n", atoi(argv[i]),
		    strerror(errno));
	    listen(sock, 10);
	    n = sizeof(sin);
	    in = out = accept(sock, (struct sockaddr *)&sin, &n);
	}
#endif
    }

    /*
     * If no security type specified, use BSD
     */
    if (secdrv == NULL) {
	secdrv = security_getdriver("BSD");
	if (secdrv == NULL)
	    error("no driver for default security type 'BSD'");
    }

    /* initialize */

    dbopen();
    {
	/* this lameness is for error() */
	extern int db_fd;
	dup2(db_fd, 2);
    }
    dbprintf(("%s: version %s\n", argv[0], version()));
    for (i = 0; version_info[i] != NULL; i++)
	dbprintf(("%s: %s", argv[0], version_info[i]));

    /*
     * Schedule to call protocol_accept() when new security handles
     * are created on stdin.
     */
    security_accept(secdrv, in, out, protocol_accept);

    /*
     * Schedule an event that will try to exit every 30 seconds if there
     * are no requests outstanding.
     */
    (void)event_register(30, EV_TIME, exit_check, &no_exit);

    /*
     * Call event_loop() with an arg of 0, telling it to block until all
     * events are completed.
     */
    event_loop(0);

    /* NOTREACHED */
    exit(1);	/* appease gcc/lint */
}

/*
 * This runs periodically and checks to see if we have any active services
 * still running.  If we don't, then we quit.
 */
static void
exit_check(cookie)
    void *cookie;
{
    int no_exit;

    assert(cookie != NULL);
    no_exit = *(int *)cookie;

    /*
     * If things are still running, then don't exit.
     */
    if (serviceq.qlength > 0)
	return;

    /*
     * If the caller asked us to never exit, then we're done
     */
    if (no_exit)
	return;

#ifdef USE_DBMALLOC
    dbmalloc_info.end.size = malloc_inuse(&dbmalloc_info.end.hist);

    if (dbmalloc_info.start.size != dbmalloc_info.end.size) {
	extern int db_fd;

	malloc_list(db_fd, dbmalloc_info.start.hist,
	    dbmalloc_info.end.hist);
    }
#endif

    dbclose();
    exit(0);
}

/*
 * Handles new incoming protocol handles.  This is a callback for
 * security_accept(), which gets called when new handles are detected.
 */
static void
protocol_accept(handle, pkt)
    security_handle_t *handle;
    pkt_t *pkt;
{
    pkt_t nak;
    struct active_service *as;
    char *pktbody, *tok, *service, *arguments;
    int i;

    /*
     * If pkt is NULL, then there was a problem with the new connection.
     */
    if (pkt == NULL) {
	dbprintf(("amandad: accept error: %s\n",
	    security_geterror(handle)));
	pkt_init(&nak, P_NAK, "ERROR %s\n", security_geterror(handle));
	security_sendpkt(handle, &nak);
	security_close(handle);
	return;
    }

#ifdef AMANDAD_DEBUG
    dbprintf(("accept recv %s pkt:\n----\n%s-----\n", pkt_type2str(pkt->type),
	pkt->body));
#endif

    /*
     * If this is not a REQ packet, just forget about it.
     */
    if (pkt->type != P_REQ) {
	dbprintf(("amandad: received unexpected %s packet:\n-----\n%s-----\n\n",
	    pkt_type2str(pkt->type), pkt->body));
	security_close(handle);
	return;
    }

    pktbody = service = arguments = NULL;
    as = NULL;

    /*
     * Parse out the service and arguments
     */

    pktbody = stralloc(pkt->body);

    tok = strtok(pktbody, " ");
    if (tok == NULL)
	goto badreq;
    if (strcmp(tok, "SERVICE") != 0)
	goto badreq;

    tok = strtok(NULL, " \n");
    if (tok == NULL)
	goto badreq;
    service = stralloc(tok);

    /* we call everything else 'arguments' */
    tok = strtok(NULL, "");
    if (tok == NULL)
	goto badreq;
    arguments = stralloc(tok);

    /* see if it's one we allow */
    for (i = 0; i < NSERVICES; i++)
	if (strcmp(services[i], service) == 0)
	    break;
    if (i == NSERVICES) {
	dbprintf(("amandad: %s: invalid service\n", service));
	pkt_init(&nak, P_NAK, "ERROR %s: invalid service\n", service);
	goto sendnak;
    }

    /* see if the binary exists */
    tok = vstralloc(libexecdir, "/", service,
	versionsuffix(), NULL);
    amfree(service);
    service = tok;

    if (access(service, X_OK) < 0) {
	dbprintf(("amandad: can't execute %s: %s\n", service, strerror(errno)));
	pkt_init(&nak, P_NAK, "ERROR execute access to \"%s\" denied\n",
	    service);
	goto sendnak;
    }

    /* see if its already running */
    for (as = TAILQ_FIRST(&serviceq.tailq); as != NULL;
	as = TAILQ_NEXT(as, tq)) {
	    if (strcmp(as->cmd, service) == 0 &&
		strcmp(as->arguments, arguments) == 0) {
		    dbprintf(("amandad: %s %s: already running, acking req\n",
			service, arguments));
		    pkt_init(&nak, P_ACK, "");
		    goto sendnak;
	    }
    }

    /*
     * create a new service instance, and send the arguments down
     * the request pipe.
     */
    dbprintf(("amandad: creating new service: %s %s\n", service, arguments));
    as = service_new(handle, service, arguments);
    if (writebuf(as->reqfd, arguments, strlen(arguments)) < 0) {
	const char *errmsg = strerror(errno);
	dbprintf(("amandad: error sening arguments to %s: %s\n", service,
	    errmsg));
	pkt_init(&nak, P_NAK, "ERROR error writing arguments to %s: %s\n",
	    service, errmsg);
	goto sendnak;
    }
    aclose(as->reqfd);

    amfree(pktbody);
    amfree(service);
    amfree(arguments);

    /*
     * Move to the sendack state, and start up the state
     * machine.
     */
    as->state = s_sendack;
    state_machine(as, A_START, NULL);
    return;

badreq:
    pkt_init(&nak, P_NAK, "ERROR invalid REQ\n");
    dbprintf(("amandad: received invalid %s packet:\n-----\n%s-----\n\n",
	pkt_type2str(pkt->type), pkt->body));

sendnak:
    if (pktbody != NULL)
	amfree(pktbody);
    if (service != NULL)
	amfree(service);
    if (arguments != NULL)
	amfree(arguments);
    if (as != NULL)
	service_delete(as);
    security_sendpkt(handle, &nak);
    security_close(handle);
}

/*
 * Handles incoming protocol packets.  Routes responses to the proper
 * running service.
 */
static void
state_machine(as, action, pkt)
    struct active_service *as;
    action_t action;
    pkt_t *pkt;
{
    action_t retaction;
    state_t curstate;
    pkt_t nak;

#ifdef AMANDAD_DEBUG
    dbprintf(("state_machine: %X entering\n", (unsigned int)as));
#endif
    for (;;) {
	curstate = as->state;
#ifdef AMANDAD_DEBUG
	dbprintf(("state_machine: %X curstate=%s action=%s\n", (unsigned int)as,
	    state2str(curstate), action2str(action)));
#endif
	retaction = (*curstate)(as, action, pkt);
#ifdef AMANDAD_DEBUG
	dbprintf(("state_machine: %X curstate=%s returned %s (nextstate=%s)\n",
	    (unsigned int)as, state2str(curstate), action2str(retaction),
	    state2str(as->state)));
#endif

	switch (retaction) {
	/*
	 * State has queued up and is now blocking on input.
	 */
	case A_PENDING:
#ifdef AMANDAD_DEBUG
	    dbprintf(("state_machine: %X leaving\n", (unsigned int)as));
#endif
	    return;

	/*
	 * service has switched states.  Loop.
	 */
	case A_CONTINUE:
	    break;

	/*
	 * state has determined that the packet it received was bogus.
	 * Send a nak, and return.
	 */
	case A_SENDNAK:
	    dbprintf(("amandad: received unexpected %s packet\n",
		pkt_type2str(pkt->type)));
	    dbprintf(("-----\n%s----\n\n", pkt->body));
	    pkt_init(&nak, P_NAK, "ERROR unexpected packet type %s\n",
		pkt_type2str(pkt->type));
	    security_sendpkt(as->security_handle, &nak);
#ifdef AMANDAD_DEBUG
	    dbprintf(("state_machine: %X leaving\n", (unsigned int)as));
#endif
	    return;

	/*
	 * Service is done.  Remove it and finish.
	 */
	case A_FINISH:
	    service_delete(as);
#ifdef AMANDAD_DEBUG
	    dbprintf(("state_machine: %X leaving\n", (unsigned int)as));
#endif
	    return;

	default:
	    assert(0);
	    break;
	}
    }
    /* NOTREACHED */
}

/*
 * This state just sends an ack.  After that, we move to the repwait
 * state to wait for REP data to arrive from the subprocess.
 */
static action_t
s_sendack(as, action, pkt)
    struct active_service *as;
    action_t action;
    pkt_t *pkt;
{
    pkt_t ack;

    pkt_init(&ack, P_ACK, "");
    if (security_sendpkt(as->security_handle, &ack) < 0) {
	dbprintf(("error sending ACK: %s\n",
	    security_geterror(as->security_handle)));
	return (A_FINISH);
    }

    /*
     * move to the repwait state
     * Setup a listener for data on the reply fd, but also
     * listen for packets over the wire, as the server may
     * poll us if we take a long time.
     * Setup a timeout that will fire if it takes too long to
     * receive rep data.
     */
    as->state = s_repwait;
    as->ev_repfd = event_register(as->repfd, EV_READFD, repfd_recv, as);
    as->ev_reptimeout = event_register(REP_TIMEOUT, EV_TIME,
	timeout_repfd, as);
    security_recvpkt(as->security_handle, protocol_recv, as, -1);
    return (A_PENDING);
}

/*
 * This is the repwait state.  We have responded to the initial REQ with
 * an ACK, and we are now waiting for the process we spawned to pass us 
 * data to send in a REP.
 */
static action_t
s_repwait(as, action, pkt)
    struct active_service *as;
    action_t action;
    pkt_t *pkt;
{
    int n;

    /*
     * We normally shouldn't receive any packets while waiting
     * for our REP data, but in some cases we do.
     */
    if (action == A_RECVPKT) {
	assert(pkt != NULL);
	/*
	 * Another req for something that's running.  Just send an ACK
	 * and go back and wait for more data.
	 */
	if (pkt->type == P_REQ) {
	    dbprintf(("amandad: received dup P_REQ packet, ACKing it\n"));
	    pkt_init(&as->rep_pkt, P_ACK, "");
	    security_sendpkt(as->security_handle, &as->rep_pkt);
	    return (A_PENDING);
	}
	/* something unexpected.  Nak it */
	return (A_SENDNAK);
    }

    if (action == A_TIMEOUT) {
	pkt_init(&as->rep_pkt, P_NAK, "ERROR timeout on reply pipe\n");
	dbprintf(("%s timed out waiting for REP data\n", as->cmd));
	security_sendpkt(as->security_handle, &as->rep_pkt);
	return (A_FINISH);
    }

    assert(action == A_RECVREP);

    /*
     * If the read fails, consider the process dead, and remove it.
     * Always save room for nul termination.
     */
    n = read(as->repfd, as->repbuf + as->repbufsize,
	sizeof(as->repbuf) - as->repbufsize - 1);
    if (n < 0) {
	const char *errstr = strerror(errno);
	dbprintf(("read error on reply pipe: %s\n", errstr));
	pkt_init(&as->rep_pkt, P_NAK, "ERROR read error on reply pipe: %s\n",
	    errstr);
	security_sendpkt(as->security_handle, &as->rep_pkt);
	return (A_FINISH);
    }
    /*
     * If we got some data, go back and wait for more, or EOF.
     */
    if (n > 0) {
	as->repbufsize += n;
	return (A_PENDING);
    }

    /*
     * If we got 0, then we hit EOF.  Process the data and release
     * the timeout.  Nul terminate the buffer first.
     */
    assert(n == 0);
    as->repbuf[as->repbufsize] = '\0';


    assert(as->ev_repfd != NULL);
    event_release(as->ev_repfd);
    as->ev_repfd = NULL;

    assert(as->ev_reptimeout != NULL);
    event_release(as->ev_reptimeout);
    as->ev_reptimeout = NULL;

    as->state = s_processrep;
    aclose(as->repfd);
    return (A_CONTINUE);
}

/*
 * After we have read in all of the rep data, we process it and send
 * it out as a REP packet.
 */
static action_t
s_processrep(as, action, pkt)
    struct active_service *as;
    action_t action;
    pkt_t *pkt;
{
    char *tok, *repbuf;

    /*
     * Copy the rep lines into the outgoing packet.
     *
     * If this line is a CONNECT, translate it
     * Format is "CONNECT <tag> <handle> <tag> <handle> etc...
     * Example:
     *
     *  CONNECT DATA 4 MESG 5 INDEX 6
     *
     * The tags are arbitrary.  The handles are in the DATA_FD pool.
     * We need to map these to security streams and pass them back
     * to the amanda server.  If the handle is -1, then we don't map.
     */
    repbuf = stralloc(as->repbuf);
    pkt_init(&as->rep_pkt, P_REP, "");
    tok = strtok(repbuf, " ");
    if (tok == NULL)
	goto error;
    if (strcmp(tok, "CONNECT") == 0) {
	char *line, *nextbuf;

	/* Save the entire line */
	line = strtok(NULL, "\n");
	/* Save the buf following the line */
	nextbuf = strtok(NULL, "");

	if (line == NULL || nextbuf == NULL)
	    goto error;

	pkt_cat(&as->rep_pkt, "CONNECT");

	/* loop over the id/handle pairs */
	for (;;) {
	    /* id */
	    tok = strtok(line, " ");
	    line = NULL;	/* keep working from line */
	    if (tok == NULL)
		break;
	    pkt_cat(&as->rep_pkt, " %s", tok);

	    /* handle */
	    tok = strtok(NULL, " \n");
	    if (tok == NULL)
		goto error;
	    /* convert the handle into something the server can process */
	    pkt_cat(&as->rep_pkt, " %d", allocstream(as, atoi(tok)));
	}
	pkt_cat(&as->rep_pkt, "\n%s", nextbuf);
    } else {
error:
	pkt_cat(&as->rep_pkt, as->repbuf);
    }

    /*
     * We've setup our REP packet in as->rep_pkt.  Now move to the transmission
     * state.
     */
    as->state = s_sendrep;
    as->repretry = MAX_REP_RETRIES;
    amfree(repbuf);
    return (A_CONTINUE);
}

/*
 * This is the state where we send the REP we just collected from our child.
 */
static action_t
s_sendrep(as, action, pkt)
    struct active_service *as;
    action_t action;
    pkt_t *pkt;
{

    /*
     * Transmit it and move to the ack state.
     */
#ifdef AMANDAD_DEBUG
    dbprintf(("sending REP pkt:\n----\n%s-----\n", as->rep_pkt.body));
#endif
    security_sendpkt(as->security_handle, &as->rep_pkt);
    security_recvpkt(as->security_handle, protocol_recv, as, ACK_TIMEOUT);
    as->state = s_ackwait;
    return (A_PENDING);
}

/*
 * This is the state in which we wait for the server to ACK the REP
 * we just sent it.
 */
static action_t
s_ackwait(as, action, pkt)
    struct active_service *as;
    action_t action;
    pkt_t *pkt;
{
    struct datafd_handle *dh;
    int npipes;

    /*
     * If we got a timeout, try again, but eventually give up.
     */
    if (action == A_TIMEOUT) {
	if (--as->repretry > 0) {
	    as->state = s_sendrep;
	    return (A_CONTINUE);
	}
	dbprintf(("timeout waiting for ACK for our REP\n"));
	return (A_FINISH);
    }
#ifdef AMANDAD_DEBUG
    dbprintf(("received ACK, now opening streams\n"));
#endif

    assert(action == A_RECVPKT);
    if (pkt->type != P_ACK)
	return (A_SENDNAK);

    /*
     * Got the ack, now open the pipes
     */
    for (dh = &as->data[0]; dh < &as->data[DATA_FD_COUNT]; dh++) {
	if (dh->netfd == NULL)
	    continue;
	if (security_stream_accept(dh->netfd) < 0) {
	    dbprintf(("stream %d accept failed: %s\n", dh - &as->data[0],
		security_geterror(as->security_handle)));
	    security_stream_close(dh->netfd);
	    dh->netfd = NULL;
	}
	/* setup an event for reads from it */
	dh->ev_handle = event_register(dh->fd, EV_READFD, process_netfd, dh);
    }

    /*
     * Pipes are open, so auth them.  Count them at the same time.
     */
    for (npipes = 0, dh = &as->data[0]; dh < &as->data[DATA_FD_COUNT]; dh++) {
	if (dh->netfd == NULL)
	    continue;
	if (security_stream_auth(dh->netfd) < 0) {
	    security_stream_close(dh->netfd);
	    dh->netfd = NULL;
	    event_release(dh->ev_handle);
	    dh->ev_handle = NULL;
	} else {
	    npipes++;
	}
    }

    /*
     * If no pipes are open, then we're done.  Otherwise, just start running.
     * The event handlers on all of the pipes will take it from here.
     */
    if (npipes == 0)
	return (A_FINISH);
    else {
	security_close(as->security_handle);
	as->security_handle = NULL;
	return (A_PENDING);
    }
}

/*
 * Called when a repfd has received data
 */
static void
repfd_recv(cookie)
    void *cookie;
{
    struct active_service *as = cookie;

    assert(as != NULL);
    assert(as->ev_repfd != NULL);

    state_machine(as, A_RECVREP, NULL);
}

/*
 * Called when a repfd has timed out
 */
static void
timeout_repfd(cookie)
    void *cookie;
{
    struct active_service *as = cookie;

    assert(as != NULL);
    assert(as->ev_reptimeout != NULL);

    state_machine(as, A_TIMEOUT, NULL);
}

/*
 * Called when a handle has received data
 */
static void
protocol_recv(cookie, pkt, status)
    void *cookie;
    pkt_t *pkt;
    security_status_t status;
{
    struct active_service *as = cookie;

    assert(as != NULL);

    switch (status) {
    case S_OK:
#ifdef AMANDAD_DEBUG
	dbprintf(("received %s pkt:\n----\n%s-----\n", pkt_type2str(pkt->type),
	    pkt->body));
#endif
	state_machine(as, A_RECVPKT, pkt);
	break;
    case S_TIMEOUT:
	state_machine(as, A_TIMEOUT, NULL);
	break;
    case S_ERROR:
	dbprintf(("receive error: %s\n",
	    security_geterror(as->security_handle)));
	break;
    }
}

/*
 * This is a generic relay function that just reads data from one of
 * the process's pipes and passes it up the equivalent security_stream_t
 */
static void
process_netfd(cookie)
    void *cookie;
{
    pkt_t nak;
    struct datafd_handle *dh = cookie;
    struct active_service *as = dh->as;
    int n;

    n = read(dh->fd, as->databuf, sizeof(as->databuf));

    /*
     * Process has died.
     */
    if (n < 0) {
	pkt_init(&nak, P_NAK, "ERROR data descriptor %d broken: %s\n",
	    dh->fd, strerror(errno));
	goto sendnak;
    }
    /*
     * Process has closed the pipe.  Just remove this event handler.
     * If all pipes are closed, shut down this service.
     */
    if (n == 0) {
	event_release(dh->ev_handle);
	dh->ev_handle = NULL;
	security_stream_close(dh->netfd);
	dh->netfd = NULL;
	for (dh = &as->data[0]; dh < &as->data[DATA_FD_COUNT]; dh++) {
	    if (dh->netfd != NULL)
		return;
	}
	service_delete(as);
	return;
    }
    if (security_stream_write(dh->netfd, as->databuf, n) < 0) {
	/* stream has croaked */
	pkt_init(&nak, P_NAK, "ERROR write error on stream %d: %s\n",
	    security_stream_id(dh->netfd),
	    security_stream_geterror(dh->netfd));
	goto sendnak;
    }
    return;

sendnak:
    security_sendpkt(as->security_handle, &nak);
    service_delete(as);
}


/*
 * Convert a local stream handle (DATA_FD...) into something that
 * can be sent to the amanda server.
 *
 * Returns a number that should be sent to the server in the REP packet.
 */
static int
allocstream(as, handle)
    struct active_service *as;
    int handle;
{
    struct datafd_handle *dh;

    /* if the handle is -1, then we don't bother */
    if (handle < 0)
	return (-1);

    /* make sure the handle's kosher */
    if (handle < DATA_FD_OFFSET || handle >= DATA_FD_OFFSET + DATA_FD_COUNT)
	return (-1);

    /* get a pointer into our handle array */
    dh = &as->data[handle - DATA_FD_OFFSET];

    /* make sure we're not already using the net handle */
    if (dh->netfd != NULL)
	return (-1);

    /* allocate a stream from the security layer and return */
    dh->netfd = security_stream_server(as->security_handle);
    if (dh->netfd == NULL) {
	dbprintf(("couldn't open stream to server: %s\n",
	    security_geterror(as->security_handle)));
	return (-1);
    }

    /*
     * convert the stream into a numeric id that can be sent to the
     * remote end.
     */
    return (security_stream_id(dh->netfd));
}

/*
 * Create a new service instance
 */
static struct active_service *
service_new(security_handle, cmd, arguments)
    security_handle_t *security_handle;
    const char *cmd, *arguments;
{
    int data[DATA_FD_COUNT + 2][2], i;
    struct active_service *as;
    pid_t pid;

    assert(security_handle != NULL);
    assert(cmd != NULL);
    assert(arguments != NULL);

    /* a plethora of pipes */
    for (i = 0; i < DATA_FD_COUNT + 2; i++)
	if (pipe(data[i]) < 0)
	    error("pipe: %s", strerror(errno));

    switch(pid = fork()) {
    case -1:
	error("could not fork service %s: %s", cmd, strerror(errno));
    default:
	/*
	 * The parent.  Close the far ends of our pipes and return.
	 */
	as = alloc(sizeof(*as));
	as->cmd = stralloc(cmd);
	as->arguments = stralloc(arguments);
	as->security_handle = security_handle;
	as->state = NULL;
	as->pid = pid;

	/* write to the request pipe */
	aclose(data[0][0]);
	as->reqfd = data[0][1];

	/*
	 * read from the reply pipe
	 */
	as->repfd = data[1][0];
	aclose(data[1][1]);
	as->ev_repfd = NULL;
	as->repbufsize = 0;
	as->repretry = 0;

	/*
	 * read from the rest of the general-use pipes
	 * (netfds are opened as the client requests them)
	 */
	for (i = 0; i < DATA_FD_COUNT; i++) {
	    aclose(data[i + 2][1]);
	    as->data[i].fd = data[i + 2][0];
	    as->data[i].ev_handle = NULL;
	    as->data[i].netfd = NULL;
	    as->data[i].as = as;
	}

	/* add it to the service queue */
	/* increment the active service count */
	TAILQ_INSERT_TAIL(&serviceq.tailq, as, tq);
	serviceq.qlength++;

	return (as);
    case 0:
	/*
	 * The child.  Put our pipes in their advertised locations
	 * and start up.
	 */

	/*
	 * The data stream is stdin in the new process
	 */
        if (dup2(data[0][0], 0) < 0) {
	    error("dup %d to %d failed: %s\n", data[0][0], 0,
		strerror(errno));
	}
	aclose(data[0][0]);
	aclose(data[0][1]);

	/*
	 * The reply stream is stdout
	 */
        if (dup2(data[1][1], 1) < 0) {
	    error("dup %d to %d failed: %s\n", data[1][1], 1,
		strerror(errno));
	}
        aclose(data[1][0]);
        aclose(data[1][1]);

	/*
	 * The rest start at the offset defined in amandad.h, and continue
	 * through the internal defined.
	 */
	for (i = 0; i < DATA_FD_COUNT; i++) {
	    if (dup2(data[i + 2][1], i + DATA_FD_OFFSET) < 0) {
		error("dup %d to %d failed: %s\n", data[i + 2][1],
		    i + DATA_FD_OFFSET, strerror(errno));
	    }
	    aclose(data[i + 2][0]);
	    aclose(data[i + 2][1]);
	}

	/* run service */
	execle(cmd, cmd, NULL, safe_env());
	error("could not exec service %s: %s", cmd, strerror(errno));
    }
    /* NOTREACHED */
}

/*
 * Unallocate a service instance
 */
static void
service_delete(as)
    struct active_service *as;
{
    int i;
    struct datafd_handle *dh;

    assert(as != NULL);

    assert(as->cmd != NULL);
    amfree(as->cmd);

    assert(as->arguments != NULL);
    amfree(as->arguments);

    if (as->reqfd != -1)
	aclose(as->reqfd);
    if (as->repfd != -1)
	aclose(as->repfd);

    if (as->ev_repfd != NULL)
	event_release(as->ev_repfd);
    if (as->ev_reptimeout != NULL)
	event_release(as->ev_reptimeout);

    for (i = 0; i < DATA_FD_COUNT; i++) {
	dh = &as->data[i];

	aclose(dh->fd);

	if (dh->netfd != NULL)
	    security_stream_close(dh->netfd);

	if (dh->ev_handle != NULL)
	    event_release(dh->ev_handle);
    }

    if (as->security_handle != NULL)
	security_close(as->security_handle);

    assert(as->pid > 0);
    kill(as->pid, SIGTERM);
    sleep(1);
    waitpid(as->pid, NULL, WNOHANG);

    TAILQ_REMOVE(&serviceq.tailq, as, tq);
    assert(serviceq.qlength > 0);
    serviceq.qlength--;

    amfree(as);
}

/*
 * Like 'write', but always writes everything
 */
static int
writebuf(fd, bufp, size)
    int fd;
    const void *bufp;
    size_t size;
{
    const char *buf = bufp;		/* cast to char * so we can increment */
    const size_t origsize = size;	/* save orig size so we can return it */
    int n;

    while (size > 0) {
	n = write(fd, buf, size);
	if (n < 0)
	    return (n);
	buf += n;
	size -= n;
    }
    return (origsize);
}

#ifdef AMANDAD_DEBUG
/*
 * Convert a state into a string
 */
static const char *
state2str(state)
    state_t state;
{
    static const struct {
	state_t state;
	const char str[13];
    } states[] = {
#define	X(state)	{ state, stringize(state) }
	X(s_sendack),
	X(s_repwait),
	X(s_processrep),
	X(s_sendrep),
	X(s_ackwait),
#undef X
    };
    int i;

    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++)
	if (state == states[i].state)
	    return (states[i].str);
    return ("INVALID STATE");
}

/*
 * Convert an action into a string
 */
static const char *
action2str(action)
    action_t action;
{
    static const struct {
	action_t action;
	const char str[12];
    } actions[] = {
#define	X(action)	{ action, stringize(action) }
	X(A_START),
	X(A_RECVPKT),
	X(A_RECVREP),
	X(A_PENDING),
	X(A_FINISH),
	X(A_CONTINUE),
	X(A_SENDNAK),
	X(A_TIMEOUT),
#undef X
    };
    int i;

    for (i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
	if (action == actions[i].action)
	    return (actions[i].str);
    return ("UNKNOWN ACTION");
}
#endif	/* AMANDAD_DEBUG */
