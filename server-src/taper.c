/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1991, 1994 University of Maryland
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
/* $Id: taper.c,v 1.21 1998/01/02 18:48:38 jrj Exp $
 *
 * moves files from holding disk to tape, or from a socket to tape
 */

#include "amanda.h"
#include "conffile.h"
#include "tapefile.h"
#include "clock.h"
#include "stream.h"
#include "logfile.h"
#include "tapeio.h"
#include "changer.h"
#include "version.h"
#include "arglist.h"
#include "token.h"

/*
 * XXX update stat collection/printing
 * XXX advance to next tape first in next_tape
 * XXX label is being read twice?
 */
#define MAX_ARGS	7
#define NBUFS		20
#define CONNECT_TIMEOUT 2*60

#define THRESHOLD	(NBUFS-1)


typedef enum { BOGUS, START_TAPER, FILE_WRITE, PORT_WRITE, QUIT } cmd_t;

#define EMPTY 1
#define FILLING 2
#define FULL 3

typedef struct buffer_s {
    long status;
    char buffer[TAPE_BLOCK_BYTES];
} buffer_t;

#define nextbuf(p)    ((p) == buftable+NBUFS-1? buftable : (p)+1)
#define prevbuf(p)    ((p) == buftable? buftable+NBUFS-1 : (p)-1)

/* major modules */
int main P((int main_argc, char **main_argv));
void file_reader_side P((int rdpipe, int wrpipe));
void tape_writer_side P((int rdpipe, int wrpipe));

/* misc helper functions */
cmd_t getcmd P((int *argcp, char ***argvp));
void putresult P((char *format, ...))
    __attribute__ ((format (printf, 1, 2)));

/* shared-memory routines */
buffer_t *attach_buffers P((void));
void detach_buffers P((buffer_t *bufp));
void destroy_buffers P((void));

/* synchronization pipe routines */
void syncpipe_init P((int rd, int wr));
char syncpipe_get P((void));
int  syncpipe_getint P((void));
char *syncpipe_getstr P((void));
void syncpipe_put P((int ch));
void syncpipe_putint P((int i));
void syncpipe_putstr P((char *str));

/* tape manipulation subsystem */
int first_tape P((char *new_datestamp));
int next_tape P((int writerr));
int end_tape P((int writerr));
int write_filemark P((void));

/*
 * ========================================================================
 * GLOBAL STATE
 *
 */
int interactive;
int writerpid;
times_t total_wait;
#ifdef TAPER_DEBUG
int bufdebug = 1;
#else
int bufdebug = 0;
#endif

buffer_t *buftable;

char *pname = "taper";
char *procname = "parent";

extern char *datestamp;
char *label = NULL;
int filenum;
char *errstr = NULL;
int tape_fd;

/*
 * ========================================================================
 * MAIN PROGRAM
 *
 */
int main(main_argc, main_argv)
int main_argc;
char **main_argv;
{
    int p2c[2], c2p[2];		/* parent-to-child, child-to-parent pipes */
    int fd;

    for(fd = 3; fd < FD_SETSIZE; fd++) {
	/*
	 * Make sure nobody spoofs us with a lot of extra open files
	 * that would cause an open we do to get a very high file
	 * descriptor, which in turn might be used as an index into
	 * an array (e.g. an fd_set).
	 */
	close(fd);
    }

    /* print prompts and debug messages if running interactive */

    interactive = (main_argc > 1 && strcmp(main_argv[1],"-t") == 0);
    if(interactive) erroutput_type = ERR_INTERACTIVE;
    else erroutput_type = ERR_AMANDALOG;

    fprintf(stderr, "%s: pid %ld executable %s version %s\n",
	    pname, (long) getpid(), main_argv[0], version());
    fflush(stderr);

    if(read_conffile(CONFFILE_NAME))
	error("parse error in %s", CONFFILE_NAME);

    if(interactive) {
	fprintf(stderr,"taper: running in interactive test mode\n");
	fflush(stderr);
    }

    /* create read/write syncronization pipes */

    if(pipe(p2c) || pipe(c2p))
	error("creating sync pipes: %s", strerror(errno));

    /* create shared memory segment */

    buftable = attach_buffers();

    /* fork off child writer process, parent becomes reader process */

    switch(writerpid = fork()) {
    case -1:
	error("fork: %s", strerror(errno));

    case 0:	/* child */
	aclose(p2c[1]);
	aclose(c2p[0]);

	tape_writer_side(p2c[0], c2p[1]);
	error("tape writer terminated unexpectedly");

    default:	/* parent */
	aclose(p2c[0]);
	aclose(c2p[1]);

	file_reader_side(c2p[0], p2c[1]);
	error("file reader terminated unexpectedly");
    }

    /* NOTREACHED */
    return 0;
}


/*
 * ========================================================================
 * FILE READER SIDE
 *
 */
void read_file P((int fd, char *handle,
		  char *host, char *disk, int level, int port_flag));
int fill_buffer P((int fd, char *buffer, int size));
void dumpbufs P((char *str1));
void dumpstatus P((buffer_t *bp));

void file_reader_side(rdpipe, wrpipe)
int rdpipe, wrpipe;
{
    cmd_t cmd;
    int argc;
    char **argv;
    char *handle = NULL, *hostname = NULL, *diskname = NULL, *result;
    char tok;
    int level, fd, data_port, data_socket, retstat, wpid;

    procname = "reader";
    syncpipe_init(rdpipe, wrpipe);

    /* must get START_TAPER before beginning */

    startclock();
    cmd = getcmd(&argc, &argv);
    total_wait = stopclock();

    if(cmd != START_TAPER || argc != 2) {
	error("error [file_reader_side cmd %d argc %d]", cmd, argc);
    }

    /* pass start command on to tape writer */

    datestamp = newstralloc(datestamp, argv[2]);

    syncpipe_put('S');
    syncpipe_putstr(datestamp);

    /* get result of start command */

    tok = syncpipe_get();
    switch(tok) {
    case 'S':
	putresult("TAPER-OK\n");
	/* start is logged in writer */
	break;
    case 'E':
	/* no tape, bail out */
	result = syncpipe_getstr();
	putresult("TAPE-ERROR %s\n",
		  squotef("[%s]", result ? result : "(null)"));
	log(L_ERROR,"no-tape [%s]", result);
	exit(1);
    default:
	error("expected 'S' or 'E' for START-TAPER, got '%c'", tok);
    }

    /* process further commands */

    while(1) {
	startclock();
	cmd = getcmd(&argc, &argv);
	total_wait = timesadd(total_wait, stopclock());

	switch(cmd) {
	case PORT_WRITE:
	    /*
	     * PORT-WRITE <handle> <host> <disk> <lev>
	     */
	    if(argc != 5) {
		error("error [file_reader_side PORT-WRITE argc != 5: %d]",
		      argc);
	    }

	    handle = stralloc(argv[2]);
	    hostname = stralloc(argv[3]);
	    diskname = stralloc(argv[4]);
	    level = atoi(argv[5]);

	    data_port = 0;
	    data_socket = stream_server(&data_port);
	    putresult("PORT %d\n", data_port);

	    if((fd = stream_accept(data_socket, CONNECT_TIMEOUT,
				   DEFAULT_SIZE, sizeof(buftable->buffer))) == -1) {
		putresult("TAPE-ERROR %s %s\n", handle,
			  squote("[port connect timeout]"));
		aclose(data_socket);
		afree(handle);
		afree(hostname);
		afree(diskname);
		break;
	    }
	    read_file(fd, handle, hostname, diskname, level, 1);
	    aclose(data_socket);
	    afree(handle);
	    afree(hostname);
	    afree(diskname);
	    break;

	case FILE_WRITE:
	    /*
	     * FILE-WRITE <sn> <fname> <hst> <dsk> <lev>
	     */
	    if(argc != 6) {
		error("error [file_reader_side FILE-WRITE argc != 6: %d]",
		      argc);
	    }

	    handle = stralloc(argv[2]);
	    hostname = stralloc(argv[4]);
	    diskname = stralloc(argv[5]);
	    level = atoi(argv[6]);

	    if((fd = open(argv[3], O_RDONLY)) == -1) {
		putresult("TAPE-ERROR %s %s\n", handle,
			  squotef("[%s]", strerror(errno)));
		afree(handle);
		afree(hostname);
		afree(diskname);
		break;
	    }
	    read_file(fd, handle, hostname, diskname, level, 0);
	    afree(handle);
	    afree(hostname);
	    afree(diskname);
	    break;

	case QUIT:
	    putresult("QUITTING\n");
	    fprintf(stderr,"taper: DONE [idle wait: %s secs]\n",
		    walltime_str(total_wait));
	    fflush(stderr);
	    syncpipe_put('Q');	/* tell writer we're exiting gracefully */
	    aclose(wrpipe);

	    if((wpid = wait(&retstat)) != writerpid) {
		fprintf(stderr,
			"taper: writer wait returned %d instead of %d: %s\n",
			wpid, writerpid, strerror(errno));
		fflush(stderr);
	    }

	    detach_buffers(buftable);
	    destroy_buffers();
	    exit(0);

	default:
	    handle = stralloc(argv[1]);
	    putresult("BAD-COMMAND %s\n", squote(handle));
	    afree(handle);
	    break;
	}
    }
}

void dumpbufs(str1)
char *str1;
{
    int i,j;
    long v;

    fprintf(stderr, "%s: state", str1);
    for(i = j = 0; i < NBUFS; i = j+1) {
	v = buftable[i].status;
	for(j = i; j < NBUFS && buftable[j].status == v; j++);
	j--;
	if(i == j) fprintf(stderr, " %d:", i);
	else fprintf(stderr, " %d-%d:", i, j);
	switch(v) {
	case FULL:	fputc('F', stderr); break;
	case FILLING:	fputc('f', stderr); break;
	case EMPTY:	fputc('E', stderr); break;
	default:
	    fprintf(stderr, "%ld", v);
	    break;
	}

    }
    fputc('\n', stderr);
    fflush(stderr);
}

void dumpstatus(bp)
buffer_t *bp;
{
    char pn[2];
    char bt[NUM_STR_SIZE];
    char status[NUM_STR_SIZE];
    char *str = NULL;

    pn[0] = procname[0];
    pn[1] = '\0';
    ap_snprintf(bt, sizeof(bt), "%d", (int)(bp-buftable));

    switch(bp->status) {
    case FULL:		status[0] = 'F'; status[1] = '\0'; break;
    case FILLING:	status[0] = 'f'; status[1] = '\0'; break;
    case EMPTY:		status[0] = 'E'; status[1] = '\0'; break;
    default:
	ap_snprintf(status, sizeof(status), "%ld", bp->status);
	break;
    }

    str = vstralloc("taper: ", pn, ": [buf ", bt, ":=", status, "]", NULL);
    dumpbufs(str);
    afree(str);
}


void read_file(fd, handle, hostname, diskname, level, port_flag)
int fd, level, port_flag;
char *handle, *hostname, *diskname;
{
    buffer_t *bp;
    char tok;
    int rc, err, opening, closing, bufnum;
    long filesize;
    times_t runtime;
    char *str;

    /* initialize */

    filesize = 0;
    closing = 0;
    err = 0;

    if(bufdebug) {
	fprintf(stderr, "taper: r: start file\n");
	fflush(stderr);
    }

    for(bp = buftable; bp < buftable + NBUFS; bp++) {
	bp->status = EMPTY;
	if(interactive || bufdebug) dumpstatus(bp);
    }

    bp = buftable;

    /* tell writer to open tape */

    opening = 1;
    syncpipe_put('O');

    startclock();

    /* read file in loop */

    while(1) {
	tok = syncpipe_get();
	switch(tok) {

	case 'O':
	    assert(opening);
	    opening = 0;
	    err = 0;
	    break;

	case 'R':
	    bufnum = syncpipe_getint();

	    if(bufdebug) {
		fprintf(stderr, "taper: r: got R%d\n", bufnum);
		fflush(stderr);
	    }

	    if(closing) break;	/* ignore extra read tokens */

	    assert(!opening);
	    if(bp->status != EMPTY || bufnum != bp-buftable) {
		/* XXX this SHOULD NOT HAPPEN.  Famous last words. */
		fprintf(stderr,"taper: panic: buffer mismatch at ofs %ld:\n",
			filesize);
		if(bufnum != bp-buftable) {
		    fprintf(stderr, "    my buf %d but writer buf %d\n",
			    (int)(bp-buftable), bufnum);
		}
		else {
		    fprintf(stderr,"buf %d state %s (%ld) instead of EMPTY\n",
			    (int)(bp-buftable),
			    bp->status == FILLING? "FILLING" :
			    bp->status == FULL? "FULL" : "EMPTY!?!?",
			    (long)bp->status);
		}
		dumpbufs("taper");
		sleep(1);
		dumpbufs("taper: after 1 sec");
		if(bp->status == EMPTY)
		    fprintf(stderr, "taper: result now correct!\n");
		fflush(stderr);

		errstr = newstralloc(errstr,
				     "[fatal buffer mismanagement bug]");
		putresult("TRY-AGAIN %s %s\n", handle, squote(errstr));
		log(L_INFO, "retrying %s:%s.%d on new tape: %s",
		    hostname, diskname, level, errstr);
		closing = 1;
		syncpipe_put('X');	/* X == buffer snafu, bail */
		do {
		    tok = syncpipe_get();
		    if(tok == 'R')
			bufnum = syncpipe_getint();
		} while(tok != 'x');
		aclose(fd);
		return;
	    }

	    bp->status = FILLING;
	    if(interactive || bufdebug) dumpstatus(bp);
	    if((rc = fill_buffer(fd, bp->buffer, sizeof(bp->buffer))) <= 0) {
		err = (rc < 0)? errno : 0;
		closing = 1;
		syncpipe_put('C');
	    }
	    else {
		bp->status = FULL;
		if(interactive || bufdebug) dumpstatus(bp);
		filesize += rc / 1024;
		if(bufdebug) {
		    fprintf(stderr,"taper: r: put W%d\n",(int)(bp-buftable));
		    fflush(stderr);
		}
		syncpipe_put('W'); syncpipe_putint(bp-buftable);

		bp = nextbuf(bp);
	    }
	    break;

	case 'T':
	case 'E':
	    syncpipe_put('e');	/* ACK error */

	    aclose(fd);
	    str = syncpipe_getstr();
	    errstr = newvstralloc(errstr, "[", str ? str : "(null)", "]", NULL);

	    if(tok == 'T') {
		putresult("TRY-AGAIN %s %s\n", handle, squote(errstr));
		log(L_INFO, "retrying %s:%s.%d on new tape: %s",
		    hostname, diskname, level, errstr);
	    } else {
		putresult("TAPE-ERROR %s %s\n", handle, squote(errstr));
		log(L_FAIL, "%s %s %d %s", hostname, diskname, level, errstr);
	    }
	    return;


	case 'C':
	    assert(!opening);
	    assert(closing);

	    str = syncpipe_getstr();
	    label = newstralloc(label, str ? str : "(null)");
	    str = syncpipe_getstr();
	    filenum = atoi(str ? str : "-9876");	/* ??? */
	    fprintf(stderr, "taper: reader-side: got label %s filenum %d\n",
		    label, filenum);
	    fflush(stderr);

	    aclose(fd);
	    runtime = stopclock();
	    if(err) {
		errstr = newvstralloc(errstr,
				      "[input: ", strerror(err), "]",
				      NULL);
		putresult("TAPE-ERROR %s %s\n", handle, squote(errstr));
		log(L_FAIL, "%s %s %d %s", hostname, diskname, level, errstr);
		str = syncpipe_getstr();	/* reap stats */
	    } else {
		char kb_str[NUM_STR_SIZE];
		char kps_str[NUM_STR_SIZE];
		double rt;

		rt = runtime.r.tv_sec+runtime.r.tv_usec/1000000.0;
		ap_snprintf(kb_str, sizeof(kb_str), "%ld", filesize);
		ap_snprintf(kps_str, sizeof(kps_str), "%3.1f",
				     rt ? filesize / rt : 0.0);
		str = syncpipe_getstr();
		errstr = newvstralloc(errstr,
				      "[sec ", walltime_str(runtime),
				      " kb ", kb_str,
				      " kps ", kps_str,
				      " ", str ? str : "(null)",
				      "]",
				      NULL);
		putresult("DONE %s %s %d %s\n",
			  handle, label, filenum, squote(errstr));
		log(L_SUCCESS, "%s %s %d %s",
		    hostname, diskname, level, errstr);
	    }
	    return;

	default:
	    assert(0);
	}
    }
}

int fill_buffer(fd, buffer, size)
int fd, size;
char *buffer;
{
    char *curptr;
    int spaceleft, cnt;

    curptr = buffer;
    spaceleft = size;

    do {
	cnt = read(fd, curptr, spaceleft);
	switch(cnt) {
	case 0:	/* eof */
	    if(spaceleft == size) {
		if(interactive) fputs("r0", stderr);
		return 0;
	    }
	    else {
		/* partial buffer, zero rest */
		memset(curptr, '\0', spaceleft);
		if(interactive) fputs("rP", stderr);
		return size;
	    }
	case -1:	/* error on read, punt */
	    if(interactive) fputs("rE", stderr);
	    return -1;
	default:
	    spaceleft -= cnt;
	    curptr += cnt;
	}

    } while(spaceleft > 0);

    if(interactive) fputs("R", stderr);
    return size;
}



/*
 * ========================================================================
 * TAPE WRITER SIDE
 *
 */
times_t idlewait, rdwait, wrwait, fmwait;
long total_writes;
double total_tape_used;
int total_tape_fm;

void write_file P((void));
int write_buffer P((buffer_t *bp));

void tape_writer_side(getp, putp)
int getp, putp;
{
    char tok;
    int tape_started, out_open;
    char *str;

    procname = "writer";
    syncpipe_init(getp, putp);

    tape_started = 0;
    out_open = 0;
    idlewait = times_zero;

    while(1) {
	startclock();
	tok = syncpipe_get();
	idlewait = timesadd(idlewait, stopclock());

	switch(tok) {
	case 'S':		/* start-tape */
	    assert(!tape_started);
	    tape_started = 1;
	    str = syncpipe_getstr();
	    if(!first_tape(str ? str : "bad-datestamp")) {
		syncpipe_put('E');
		syncpipe_putstr(errstr);
		exit(1);
	    }
	    else
		syncpipe_put('S');

	    break;

	case 'O':		/* open-output */
	    assert(tape_started);
	    write_file();
	    break;

	case 'Q':
	    end_tape(0);	/* XXX check results of end tape ?? */
	    exit(0);
	default:
	    assert(0);
	}
    }
}

void write_file()
{
    buffer_t *bp;
    int full_buffers, i, bufnum;
    char tok;
    char number[NUM_STR_SIZE];

    rdwait = wrwait = times_zero;
    total_writes = 0;

    bp = buftable;
    full_buffers = 0;
    tok = '?';

    if(bufdebug) {
	fprintf(stderr, "taper: w: start file\n");
	fflush(stderr);
    }

    /*
     * Tell the reader that the tape is open, and give it all the buffers.
     */
    syncpipe_put('O');
    for(i = 0; i < NBUFS; i++) {
	if(bufdebug) {
	    fprintf(stderr, "taper: w: put R%d\n", i);
	    fflush(stderr);
	}
	syncpipe_put('R'); syncpipe_putint(i);

    }

    /*
     * We write the filemark at the start of the file rather than at the end,
     * so that it can proceed in parallel with the reader's initial filling
     * up of the buffers.
     */

    startclock();
    if(!write_filemark())
	goto tape_error;
    fmwait = stopclock();

    filenum += 1;

    do {

	/*
	 * STOPPED MODE
	 *
	 * At the start of the file, or if the input can't keep up with the
	 * tape, we enter STOPPED mode, which waits for most of the buffers
	 * to fill up before writing to tape.  This maximizes the amount of
	 * data written in chunks to the tape drive, minimizing the number
	 * of starts/stops, which in turn saves tape and time.
	 */

	if(interactive) fputs("[WS]", stderr);
	startclock();
	while(full_buffers < THRESHOLD) {
	    tok = syncpipe_get();
	    if(tok != 'W') break;
	    bufnum = syncpipe_getint();
	    if(bufdebug) {
		fprintf(stderr,"taper: w: got W%d\n",bufnum);
		fflush(stderr);
	    }
	    full_buffers++;
	}
	rdwait = timesadd(rdwait, stopclock());

	/*
	 * STARTING MODE
	 *
	 * We start output when sufficient buffers have filled up, or at
	 * end-of-file, whichever comes first.  Here we drain all the buffers
	 * that were waited on in STOPPED mode.  If more full buffers come
	 * in, then we will be STREAMING.
	 */

	while(full_buffers) {
	    if(!write_buffer(bp)) goto tape_error;
	    full_buffers--;
	    bp = nextbuf(bp);
	}

	/*
	 * STREAMING MODE
	 *
	 * With any luck, the input source is faster than the tape drive.  In
	 * this case, full buffers will appear in the circular queue faster
	 * than we can write them, so the next buffer in the queue will always
	 * be marked FULL by the time we get to it.  If so, we'll stay in
	 * STREAMING mode.
	 *
	 * On the other hand, if we catch up to the input and thus would have
	 * to wait for buffers to fill, we are then STOPPED again.
	 */

	while(tok == 'W' && bp->status == FULL) {
	    tok = syncpipe_get();
	    if(tok == 'W') {
		bufnum = syncpipe_getint();
		if(bufdebug) {
		    fprintf(stderr,"taper: w: got W%d\n",bufnum);
		    fflush(stderr);
		}
		if(bufnum != bp-buftable) {
		    fprintf(stderr,
			    "taper: tape-writer: my buf %d reader buf %d\n",
			    (int)(bp-buftable), bufnum);
		    fflush(stderr);
		    syncpipe_put('E');
		    syncpipe_putstr("writer-side buffer mismatch");
		    goto error_ack;
		}
		if(!write_buffer(bp)) goto tape_error;
		bp = nextbuf(bp);
	    }
	    else if(tok == 'Q')
		return;
	    else if(tok == 'X')
		goto reader_buffer_snafu;
	    else
		error("writer-side not expecting token: %c", tok);
	}
    } while(tok == 'W');

    /* got close signal from reader, acknowledge it */

    if(tok == 'X')
	goto reader_buffer_snafu;

    assert(tok == 'C');
    syncpipe_put('C');

    /* tell reader the tape and file number */

    syncpipe_putstr(label);
    ap_snprintf(number, sizeof(number), "%d", filenum);
    syncpipe_putstr(number);

    ap_snprintf(number, sizeof(number), "%ld", total_writes);
    errstr = newvstralloc(errstr,
			  "{wr:",
			  " writers ", number,
			  " rdwait ", walltime_str(rdwait),
			  " wrwait ", walltime_str(wrwait),
			  " filemark ", walltime_str(fmwait),
			  "}",
			  NULL);
    syncpipe_putstr(errstr);

    /* XXX go to next tape if past tape size? */

    return;

 tape_error:
    /* got tape error */
    if(next_tape(1)) syncpipe_put('T');	/* next tape in place, try again */
    else syncpipe_put('E');		/* no more tapes, fail */
    syncpipe_putstr(errstr);

 error_ack:
    /* wait for reader to acknowledge error */
    do {
	tok = syncpipe_get();
	if(tok != 'W' && tok != 'C' && tok != 'e')
	    error("writer: got '%c' unexpectedly after error", tok);
	if(tok == 'W')
	    syncpipe_getint();	/* eat buffer number */
    } while(tok != 'e');
    return;

 reader_buffer_snafu:
    syncpipe_put('x');
    return;
}

int write_buffer(bp)
buffer_t *bp;
{
    int rc;

    if(bp->status != FULL) {
	/* XXX buffer management snafu */
	assert(0);
    }

    startclock();
    rc = tapefd_write(tape_fd, bp->buffer, sizeof(bp->buffer));
    if(rc == sizeof(bp->buffer)) {
	tapefd_resetofs(tape_fd);
	wrwait = timesadd(wrwait, stopclock());
	total_writes += 1;
	total_tape_used += (double)rc;
	bp->status = EMPTY;
	if(interactive || bufdebug) dumpstatus(bp);
	if(interactive) fputs("W", stderr);

	if(bufdebug) {
	    fprintf(stderr, "taper: w: put R%d\n", (int)(bp-buftable));
	    fflush(stderr);
	}
	syncpipe_put('R'); syncpipe_putint(bp-buftable);
	return 1;
    }
    else {
	errstr = newvstralloc(errstr,
			      "writing file: ",
			      (rc != -1) ? "short write" : strerror(errno),
			      NULL);
	wrwait = timesadd(wrwait, stopclock());
	if(interactive) fputs("[WE]", stderr);
	return 0;
    }
}


/*
 * ========================================================================
 * MISC HELPER FUNCTIONS
 *
 */
cmd_t getcmd(argcp, argvp)
int *argcp;
char ***argvp;
{
    /*
     * WARNING: this routine stores the decoded strings in a static buffer.
     * Subsequent calls will overwrite previous values, so if you want
     * them to hang around, the calling routine had better copy them to
     * storage it has allocated for that purpose.
     */
    static char *line = NULL;
    static char *argv[MAX_ARGS+1];
    int argc;

    if(interactive) {
	fprintf(stderr, "%s> ", pname);
	fflush(stderr);
    }

    afree(line);
    if((line = agets(stdin)) == NULL) return QUIT;

    argc = split(line, argv, sizeof(argv) / sizeof(argv[0]), " ");

#ifdef DEBUG
    {
      int arg;
      printf("argc = %d\n", argc);
      for(arg = 0; arg < sizeof(argv) / sizeof(argv[0]); arg++)
	printf("argv[%d] = \"%s\"\n", arg, argv[arg]);
    }
#endif

    *argvp = argv;
    *argcp = argc;

    /* not enough commands for a table lookup */

    if(argc < 1) return BOGUS;
    if(strcmp(argv[1],"START-TAPER") == 0) return START_TAPER;
    if(strcmp(argv[1],"FILE-WRITE") == 0) return FILE_WRITE;
    if(strcmp(argv[1],"PORT-WRITE") == 0) return PORT_WRITE;
    if(strcmp(argv[1],"QUIT") == 0) return QUIT;
    return BOGUS;
}


arglist_function(void putresult, char *, format)
{
    va_list argp;

    arglist_start(argp, format);
    vprintf(format, argp);
    fflush(stdout);
    arglist_end(argp);
}


/*
 * ========================================================================
 * SHARED-MEMORY BUFFER SUBSYSTEM
 *
 */

#ifdef HAVE_SYSVSHM

int shmid = -1;

buffer_t *attach_buffers()
{
    buffer_t *result;

    shmid = shmget(IPC_PRIVATE, sizeof(buffer_t)*NBUFS, IPC_CREAT|0700);
    if(shmid == -1)
	error("shmget: %s", strerror(errno));

    result = (buffer_t *)shmat(shmid, (SHM_ARG_TYPE *)NULL, 0);

    if(result == (buffer_t *)-1) {
	destroy_buffers();
	error("shmat: %s", strerror(errno));
    }

    return result;
}


void detach_buffers(bufp)
buffer_t *bufp;
{
    if(shmdt((SHM_ARG_TYPE *)bufp) == -1)
	error("shmdr: %s", strerror(errno));
}

void destroy_buffers()
{
    if(shmid == -1) return;	/* nothing to destroy */
    if(shmctl(shmid, IPC_RMID, NULL) == -1)
	error("shmctl: %s", strerror(errno));
}

#else
#ifdef HAVE_MMAP

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#ifndef MAP_ANON
#  ifdef MAP_ANONYMOUS			/* OSF/1-style */
#    define MAP_ANON MAP_ANONYMOUS
#  else					/* SunOS4-style */
#    define MAP_ANON 0
#    define ZERO_FILE "/dev/zero"
#  endif
#endif

int shmfd = -1;

buffer_t *attach_buffers()
{
    buffer_t *shmbuf;

#ifdef ZERO_FILE
    shmfd = open(ZERO_FILE, O_RDWR);
    if(shmfd == -1)
	error("attach_buffers: could not open %s: %s",
	      ZERO_FILE, strerror(errno));
#endif

    shmbuf = (buffer_t *) mmap((void *) 0,
			       sizeof(buffer_t)*NBUFS,
			       PROT_READ|PROT_WRITE,
			       MAP_ANON|MAP_SHARED,
			       shmfd, 0);
    if((long)shmbuf == -1)
	error("attach_buffer: mmap: %s", strerror(errno));

    return shmbuf;
}

void detach_buffers(bufp)
buffer_t *bufp;
{
    if(munmap((void *)bufp, sizeof(buffer_t)*NBUFS) == -1)
	error("detach_buffers: munmap: %s", strerror(errno));

    if(shmfd != -1) aclose(shmfd);
}

void destroy_buffers()
{
}

#else
error: must define either HAVE_SYSVSHM or HAVE_MMAP!
#endif
#endif



/*
 * ========================================================================
 * SYNC-PIPE SUBSYSTEM
 *
 */

int getpipe, putpipe;

void syncpipe_init(rd, wr)
int rd, wr;
{
    getpipe = rd;
    putpipe = wr;
}

char syncpipe_get()
{
    int rc;
    char buf[1];

    rc = read(getpipe, buf, sizeof(buf));
    if(rc == 0)		/* EOF */
	error("syncpipe_get: %c: unexpected EOF", *procname);
    else if(rc < 0)
	error("syncpipe_get: %c: %s", *procname, strerror(errno));

    if(bufdebug && *buf != 'R' && *buf != 'W') {
	fprintf(stderr,"taper: %c: getc %c\n",*procname,*buf);
	fflush(stderr);
    }

    return buf[0];
}

int syncpipe_getint()
{
    int rc;
    int i;
    int len = sizeof(i);
    char *p;

    for(p = (char *)&i; len > 0; len -= rc, p += rc) {
	if ((rc = read(getpipe, p, len)) <= 0) {
	    error("syncpipe_getint: %s",
		  rc < 0 ? strerror(errno) : "short read");
	}
    }

    return i;
}


char *syncpipe_getstr()
{
    int rc;
    int len;
    char *p;
    static char *str = NULL;
    static int strsize = 0;

    if((len = syncpipe_getint()) <= 0) {
	return NULL;
    }

    if(len > strsize) {
	strsize = ((len + 1024 - 1) / 1024) * 1024;
	str = newalloc(str, strsize);
    }

    for(p = str; len > 0; len -= rc, p += rc) {
	if ((rc = read(getpipe, p, len)) <= 0) {
	    error("syncpipe_getstr: %s",
		  rc < 0 ? strerror(errno) : "short read");
	}
    }

    return str;
}


void syncpipe_put(chi)
int chi;
{
    int l, n, s;
    char ch = chi;
    char *item = &ch;

    if(bufdebug && chi != 'R' && chi != 'W') {
	fprintf(stderr,"taper: %c: putc %c\n",*procname,chi);
	fflush(stderr);
    }

    for(l = 0, n = sizeof(ch); l < n; l += s) {
	if((s = write(putpipe, item + l, n - l)) < 0) {
	    error("syncpipe_put: %s", strerror(errno));
	}
    }
}

void syncpipe_putint(i)
int i;
{
    int l, n, s;
    char *item = (char *)&i;

    for(l = 0, n = sizeof(i); l < n; l += s) {
	if((s = write(putpipe, item + l, n - l)) < 0) {
	    error("syncpipe_putint: %s", strerror(errno));
	}
    }
}

void syncpipe_putstr(item)
char *item;
{
    int l, n, s;

    n = strlen(item)+1;				/* send '\0' as well */
    syncpipe_putint(n);
    for(l = 0, n = strlen(item)+1; l < n; l += s) {
	if((s = write(putpipe, item + l, n - l)) < 0) {
	    error("syncpipe_putstr: %s", strerror(errno));
	}
    }
}


/*
 * ========================================================================
 * TAPE MANIPULATION SUBSYSTEM
 *
 */

int runtapes, cur_tape, have_changer, tapedays;
char *tapedev, *labelstr, *tapefilename;

/* local functions */
int scan_init P((int rc, int ns, int bk));
int taperscan_slot P((int rc, char *slotstr, char *device));
char *taper_scan P((void));
int label_tape P((void));

int label_tape()
{
    char *oldtapefilename = NULL;
    char *olddatestamp = NULL;
    char *result;
    tape_t *tp;

    if(have_changer && (tapedev = taper_scan()) == NULL) {
	errstr = newstralloc(errstr, changer_resultstr);
	return 0;
    }

    if((result = tape_rdlabel(tapedev, &olddatestamp, &label)) != NULL) {
	afree(olddatestamp);
	errstr = newstralloc(errstr, result);
	return 0;
    }

    fprintf(stderr, "taper: read label `%s' date `%s'\n", label, olddatestamp);
    fflush(stderr);

    /* check against tape list */
    tp = lookup_tapelabel(label);
    if(tp != NULL && tp->position < tapedays) {
	errstr = newvstralloc(errstr,
			      "cannot overwrite active tape ", label,
			      NULL);
	afree(olddatestamp);
	return 0;
    }

    if(!match(labelstr, label)) {
	errstr = newvstralloc(errstr,
			      "label ", label,
			      " doesn\'t match labelstr \"", labelstr, "\"",
			      NULL);
	afree(olddatestamp);
	return 0;
    }

    if((tape_fd = tape_open(tapedev, O_WRONLY)) == -1) {
	if(errno == EACCES) {
	    errstr = newstralloc(errstr,
				 "writing label: tape is write protected");
	} else {
	    errstr = newstralloc2(errstr,
				  "writing label: ", strerror(errno));
	}
	afree(olddatestamp);
	return 0;
    }

    if((result = tapefd_wrlabel(tape_fd, datestamp, label)) != NULL) {
	errstr = newstralloc(errstr, result);
	afree(olddatestamp);
	return 0;
    }

    fprintf(stderr, "taper: wrote label `%s' date `%s'\n", label, datestamp);
    fflush(stderr);

    /* write tape list */

    /* XXX add cur_tape number to tape list structure */
    shift_tapelist(atoi(datestamp), label, tapedays);

    if(cur_tape == 0) {
	oldtapefilename = stralloc2(tapefilename, ".yesterday");
    } else {
	char cur_str[NUM_STR_SIZE];

	ap_snprintf(cur_str, sizeof(cur_str), "%d", cur_tape - 1);
	oldtapefilename = vstralloc(tapefilename, ".today.", cur_str, NULL);
    }
    rename(tapefilename, oldtapefilename);
    afree(oldtapefilename);
    if(write_tapelist(tapefilename))
	error("couldn't write tapelist: %s", strerror(errno));

    log(L_START, "datestamp %s label %s tape %d", datestamp, label, cur_tape);

    total_tape_used=0.0;
    total_tape_fm = 0;

    afree(olddatestamp);
    return 1;
}

int first_tape(new_datestamp)
char *new_datestamp;
{

    tapefilename = getconf_str(CNF_TAPELIST);

    tapedev	= getconf_str(CNF_TAPEDEV);
    tapedays	= getconf_int(CNF_TAPECYCLE);
    labelstr	= getconf_str(CNF_LABELSTR);

    runtapes	= getconf_int(CNF_RUNTAPES);
    cur_tape	= 0;

    if(read_tapelist(tapefilename))
	error("parse error in %s", tapefilename);

    have_changer = changer_init();

    datestamp = newstralloc(datestamp, new_datestamp);

    if(!label_tape())
	return 0;

    filenum = 0;
    return 1;
}

int next_tape(writerror)
int writerror;
{
    end_tape(writerror);

    if(++cur_tape >= runtapes)
	return 0;

    if(!label_tape()) {
	return 0;
    }

    filenum = 0;
    return 1;
}


int end_tape(writerror)
int writerror;
{
    char *result;

    fprintf(stderr, "taper: writing end marker. [%s %s kb %ld fm %d]\n",
	label,
	writerror? "ERR" : "OK",
	(long) ((total_tape_used+1023.0) / 1024.0),
	total_tape_fm);
    fflush(stderr);

    if(!writerror) {
	if(!write_filemark()) {
	    tapefd_close(tape_fd);
	    goto tape_error;
	}

	if((result = tapefd_wrendmark(tape_fd, datestamp)) != NULL) {
	    tapefd_close(tape_fd);
	    errstr = newstralloc(errstr, result);
	    goto tape_error;
	}
    }

    /* write the final filemarks */

    if(tapefd_close(tape_fd) == -1) {
	errstr = newstralloc2(errstr, "closing tape: ", strerror(errno));
	goto tape_error;
    }

    /* rewind the tape */

    if((result = tape_rewind(tapedev)) != NULL) {
	errstr = newstralloc(errstr, result);
	goto tape_error;
    }

    return 1;

tape_error:
    /* global "errstr" should get passed back to reader-side */
    return 0;
}


int write_filemark()
{
    if(tapefd_weof(tape_fd, 1) == -1) {
	errstr = newstralloc2(errstr, "writing filemark: ", strerror(errno));
	return 0;
    }
    total_tape_fm++;
    return 1;
}


/*
 * ========================================================================
 * TAPE CHANGER SCAN
 *
 */
int nslots, backwards, found, got_match, tapedays;
char *first_match_label = NULL, *first_match = NULL, *found_device = NULL;
char *scan_datestamp = NULL;
char *searchlabel, *labelstr;
tape_t *tp;

int scan_init(rc, ns, bk)
int rc, ns, bk;
{
    if(rc)
	error("could not get changer info: %s", changer_resultstr);

    nslots = ns;
    backwards = bk;

    return 0;
}

int taperscan_slot(rc, slotstr, device)
int rc;
char *slotstr;
char *device;
{
    char *t_errstr;

    if(rc == 2) {
	fprintf(stderr, "%s: fatal slot %s: %s\n",
		pname, slotstr, changer_resultstr);
	fflush(stderr);
	return 1;
    }
    else if(rc == 1) {
	fprintf(stderr, "%s: slot %s: %s\n", pname,
		slotstr, changer_resultstr);
	fflush(stderr);
	return 0;
    }
    else {
	afree(scan_datestamp);
	if((t_errstr = tape_rdlabel(device, &scan_datestamp, &label)) != NULL) {
	    fprintf(stderr, "%s: slot %s: %s\n", pname, slotstr, t_errstr);
	    fflush(stderr);
	}
	else {
	    /* got an amanda tape */
	    fprintf(stderr, "%s: slot %s: date %-8s label %s",
		    pname, slotstr, scan_datestamp, label);
	    fflush(stderr);
	    if(searchlabel != NULL && strcmp(label, searchlabel) == 0) {
		/* it's the one we are looking for, stop here */
		fprintf(stderr, " (exact label match)\n");
		fflush(stderr);
		found_device = newstralloc(found_device, device);
		found = 1;
		return 1;
	    }
	    else if(!match(labelstr, label)) {
		fprintf(stderr, " (no match)\n");
		fflush(stderr);
	    }
	    else {
		/* not an exact label match, but a labelstr match */
		/* check against tape list */
		tp = lookup_tapelabel(label);
		if(tp != NULL && tp->position < tapedays) {
		    fprintf(stderr, " (active tape)\n");
		    fflush(stderr);
		}
		else if(got_match) {
		    fprintf(stderr, " (labelstr match)\n");
		    fflush(stderr);
		}
		else {
		    got_match = 1;
		    first_match = newstralloc(first_match, slotstr);
		    first_match_label = newstralloc(first_match_label, label);
		    fprintf(stderr, " (first labelstr match)\n");
		    fflush(stderr);
		    if(!backwards || !searchlabel) {
			found = 2;
			found_device = newstralloc(found_device, device);
			return 1;
		    }
		}
	    }
	}
    }
    return 0;
}

char *taper_scan()
{
    char *outslot = NULL;

    if((tp = lookup_tapepos(getconf_int(CNF_TAPECYCLE))) == NULL)
	searchlabel = NULL;
    else
	searchlabel = tp->label;

    found = 0;
    got_match = 0;

    changer_scan(scan_init, taperscan_slot);

    if(found == 2)
	searchlabel = first_match_label;
    else if(!found && got_match) {
	searchlabel = first_match_label;
	afree(found_device);
	if(changer_loadslot(first_match, &outslot, &found_device) == 0) {
	    found = 1;
	}
	afree(outslot);
    }
    else if(!found) {
	if(searchlabel) {
	    changer_resultstr = newvstralloc(changer_resultstr,
					     "label ", searchlabel,
					     " or new tape not found in rack",
					     NULL);
	} else {
	    changer_resultstr = newstralloc(changer_resultstr,
					    "new tape not found in rack");
	}
    }

    return found ? found_device : NULL;
}
