/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1991-1998 University of Maryland at College Park
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
/* $Id: amidxtaped.c,v 1.59 2006/04/19 14:13:19 martinea Exp $
 *
 * This daemon extracts a dump image off a tape for amrecover and
 * returns it over the network. It basically, reads a number of
 * arguments from stdin (it is invoked via inet), one per line,
 * preceeded by the number of them, and forms them into an argv
 * structure, then execs amrestore
 */

#include "amanda.h"
#include "version.h"
#include "clock.h"
#include "restore.h"

#include "changer.h"
#include "tapeio.h"
#include "conffile.h"
#include "logfile.h"
#include "amfeatures.h"
#include "stream.h"

#define TIMEOUT 30

static char *pgm = "amidxtaped";	/* in case argv[0] is not set */

extern char *rst_conf_logdir;
extern char *rst_conf_logfile;
extern char *config_dir;

static int get_lock = 0;

static am_feature_t *our_features = NULL;
static am_feature_t *their_features = NULL;

static char *get_client_line P((void));
static char *get_client_line_fd P((int));
static void check_security_buffer P((char*));

/* exit routine */
static int parent_pid = -1;
static void cleanup P((void));

/* get a line from client - line terminated by \r\n */
static char *
get_client_line()
{
    static char *line = NULL;
    char *part = NULL;
    int len;

    amfree(line);
    while(1) {
	if((part = agets(stdin)) == NULL) {
	    if(errno != 0) {
		dbprintf(("%s: read error: %s\n",
			  debug_prefix_time(NULL), strerror(errno)));
	    } else {
		dbprintf(("%s: EOF reached\n", debug_prefix_time(NULL)));
	    }
	    if(line) {
		dbprintf(("%s: unprocessed input:\n", debug_prefix_time(NULL)));
		dbprintf(("-----\n"));
		dbprintf(("%s\n", line));
		dbprintf(("-----\n"));
	    }
	    amfree(line);
	    amfree(part);
	    dbclose();
	    exit(1);
	    /* NOTREACHED */
	}
	if(line) {
	    strappend(line, part);
	    amfree(part);
	} else {
	    line = part;
	    part = NULL;
	}
	if((len = strlen(line)) > 0 && line[len-1] == '\r') {
	    line[len-1] = '\0';		/* zap the '\r' */
	    break;
	}
	/*
	 * Hmmm.  We got a "line" from agets(), which means it saw
	 * a '\n' (or EOF, etc), but there was not a '\r' before it.
	 * Put a '\n' back in the buffer and loop for more.
	 */
	strappend(line, "\n");
    }
    dbprintf(("%s: > %s\n", debug_prefix_time(NULL), line));
    return line;
}

/* get a line from client - line terminated by \r\n */
static char *
get_client_line_fd(fd)
int fd;
{
    static char *line = NULL;
    static int line_size = 0;
    char *s = line;
    int len = 0;
    char c;
    int nb;

    if(line == NULL) { /* first time only, allocate initial buffer */
	s = line = malloc(128);
	line_size = 128;
    }
    while(1) {
	nb = read(fd, &c, 1);
	if (nb <= 0) {
	    /* EOF or error */
	    if ((nb <= 0) && ((errno == EINTR) || (errno == EAGAIN))) {
		/* Keep looping if failure is temporary */
		continue;
	    }
	    dbprintf(("%s: Control pipe read error - %s\n",
		      pgm, strerror(errno)));
	    break;
	}

	if(len >= line_size-1) { /* increase buffer size */
	    line_size *= 2;
	    line = realloc(line, line_size);
	    if (line == NULL) {
		error("Memory reallocation failure");
		/* NOTREACHED */
	    }
	    s = &line[len];
	}
	*s = c;
	if(c == '\n') {
	    if(len > 0 && *(s-1) == '\r') { /* remove '\r' */
		s--;
		len--;
	    }
	    *s = '\0';
	    return line;
	}
	s++;
	len++;
    }
    line[len] = '\0';
    return line;
}


void check_security_buffer(buffer)
     char *buffer;
{
    socklen_t i;
    struct sockaddr_in addr;
    char *s, *fp, ch;
    char *errstr = NULL;

    i = sizeof (addr);
    if (getpeername(0, (struct sockaddr *)&addr, &i) == -1)
	error("getpeername: %s", strerror(errno));
    if (addr.sin_family != AF_INET || ntohs(addr.sin_port) == 20) {
	error("connection rejected from %s family %d port %d",
	      inet_ntoa(addr.sin_addr), addr.sin_family, htons(addr.sin_port));
    }

    /* do the security thing */
    s = buffer;
    ch = *s++;

    skip_whitespace(s, ch);
    if (ch == '\0') {
	error("cannot parse SECURITY line");
    }
    fp = s-1;
    skip_non_whitespace(s, ch);
    s[-1] = '\0';
    if (strcmp(fp, "SECURITY") != 0) {
	error("cannot parse SECURITY line");
    }
    skip_whitespace(s, ch);
    if (!check_security(&addr, s-1, 0, &errstr)) {
	error("security check failed: %s", errstr);
    }
}

int main(argc, argv)
int argc;
char **argv;
{
    char *buf = NULL;
    int data_sock = -1, data_port = -1;
    socklen_t socklen;
    struct sockaddr_in addr;
    match_list_t *match_list;
    tapelist_t *tapes = NULL;
    char *their_feature_string = NULL;
    rst_flags_t *rst_flags;
    int use_changer = 0;
    FILE *prompt_stream = NULL;
    int re_end = 0;
    char *re_config = NULL;
    char *conf_tapetype;
    tapetype_t *tape;

    safe_fd(-1, 0);
    safe_cd();

    /* Don't die when child closes pipe */
    signal(SIGPIPE, SIG_IGN);

    rst_flags = new_rst_flags();
    rst_flags->mask_splits = 1; /* for older clients */
    rst_flags->amidxtaped = 1;
    our_features = am_init_feature_set();
    their_features = am_set_default_feature_set();

    /*
     * When called via inetd, it is not uncommon to forget to put the
     * argv[0] value on the config line.  On some systems (e.g. Solaris)
     * this causes argv and/or argv[0] to be NULL, so we have to be
     * careful getting our name.
     */
    if (argc >= 1 && argv != NULL && argv[0] != NULL) {
	if((pgm = strrchr(argv[0], '/')) != NULL) {
	    pgm++;
	} else {
	    pgm = argv[0];
	}
    }

    set_pname(pgm);

#ifdef FORCE_USERID

    /* we'd rather not run as root */

    if(geteuid() == 0) {
	if(client_uid == (uid_t) -1) {
	    error("error [cannot find user %s in passwd file]\n", CLIENT_LOGIN);
	}

	initgroups(CLIENT_LOGIN, client_gid);
	setgid(client_gid);
	setuid(client_uid);
    }

#endif	/* FORCE_USERID */

    /* initialize */
    /* close stderr first so that debug file becomes it - amrestore
       chats to stderr, which we don't want going to client */
    /* if no debug file, ship to bit bucket */
    (void)close(STDERR_FILENO);
    dbopen();
    startclock();
    dbprintf(("%s: version %s\n", pgm, version()));
#ifdef DEBUG_CODE
    if(dbfd() != -1 && dbfd() != STDERR_FILENO)
    {
	if(dup2(dbfd(),STDERR_FILENO) != STDERR_FILENO)
	{
	    perror("amidxtaped can't redirect stderr to the debug file");
	    dbprintf(("%s: can't redirect stderr to the debug file\n",
		      debug_prefix_time(NULL)));
	    return 1;
	}
    }
#else
    if ((i = open("/dev/null", O_WRONLY)) == -1 ||
	(i != STDERR_FILENO &&
	 (dup2(i, STDERR_FILENO) != STDERR_FILENO ||
	  close(i) != 0))) {
	perror("amidxtaped can't redirect stderr");
	return 1;
    }
#endif

    if (! (argc >= 1 && argv != NULL && argv[0] != NULL)) {
	dbprintf(("%s: WARNING: argv[0] not defined: check inetd.conf\n",
		  debug_prefix_time(NULL)));
    }

    socklen = sizeof (addr);
    if (getpeername(0, (struct sockaddr *)&addr, &socklen) == -1)
	error("getpeername: %s", strerror(errno));
    if (addr.sin_family != AF_INET || ntohs(addr.sin_port) == 20) {
	error("connection rejected from %s family %d port %d",
	      inet_ntoa(addr.sin_addr), addr.sin_family, htons(addr.sin_port));
    }

    /* do the security thing */
    amfree(buf);
    buf = stralloc(get_client_line());
    check_security_buffer(buf);

    /* get the number of arguments */
    match_list = alloc(sizeof(match_list_t));
    match_list->next = NULL;
    match_list->hostname = "";
    match_list->datestamp = "";
    match_list->level = "";
    match_list->diskname = "";

   do {
	amfree(buf);
	buf = stralloc(get_client_line());
	if(strncmp(buf, "LABEL=", 6) == 0) {
	    tapes = unmarshal_tapelist_str(buf+6);
	}
	else if(strncmp(buf, "FSF=", 4) == 0) {
	    rst_flags->fsf = atoi(buf + 4);
	}
	else if(strncmp(buf, "HEADER", 6) == 0) {
	    rst_flags->headers = 1;
	}
	else if(strncmp(buf, "FEATURES=", 9) == 0) {
	    char *our_feature_string = NULL;
	    their_feature_string = stralloc(buf+9);
	    am_release_feature_set(their_features);
	    their_features = am_string_to_feature(their_feature_string);
	    amfree(their_feature_string);
	    our_feature_string = am_feature_to_string(our_features);
	    printf("%s", our_feature_string);
	    fflush(stdout);
	    amfree(our_feature_string);
	}
	else if(strncmp(buf, "DEVICE=", 7) == 0) {
	    rst_flags->alt_tapedev= stralloc(buf+7);
	}
	else if(strncmp(buf, "HOST=", 5) == 0) {
	    match_list->hostname = stralloc(buf+5);
	}
	else if(strncmp(buf, "DISK=", 5) == 0) {
	    match_list->diskname = stralloc(buf+5);
	}
	else if(strncmp(buf, "DATESTAMP=", 10) == 0) {
	    match_list->datestamp = stralloc(buf+10);
	}
	else if(strncmp(buf, "END", 3) == 0) {
	    re_end = 1;
	}
	else if(strncmp(buf, "CONFIG=", 7) == 0) {
	    re_config = stralloc(buf+7);
	}
	else if(buf[0] != '\0' && buf[0] >= '0' && buf[0] <= '9') {
/* XXX does nothing?     amrestore_nargs = atoi(buf); */
	    re_end = 1;
	}
	else {
	}
    } while (re_end == 0);
    amfree(buf);

    if(!tapes && rst_flags->alt_tapedev){
	dbprintf(("%s: Looks like we're restoring from a holding file...\n", debug_prefix_time(NULL)));
        tapes = unmarshal_tapelist_str(rst_flags->alt_tapedev);
	tapes->isafile = 1;
	amfree(rst_flags->alt_tapedev);
	rst_flags->alt_tapedev = NULL;
    }

    if(re_config) {
	char *conffile;
	config_dir = vstralloc(CONFIG_DIR, "/", re_config, "/", NULL);
	conffile = stralloc2(config_dir, CONFFILE_NAME);
	if (read_conffile(conffile)) {
	    dbprintf(("%s: config '%s' not found\n",
		      debug_prefix_time(NULL), re_config));
	    amfree(re_config);
	    re_config = NULL;
	}
	amfree(conffile);
    }

    if(tapes && 
       (!rst_flags->alt_tapedev  ||
        (re_config && ( strcmp(rst_flags->alt_tapedev,
                               getconf_str(CNF_AMRECOVER_CHANGER)) == 0 ||
                        strcmp(rst_flags->alt_tapedev,
                               getconf_str(CNF_TPCHANGER)) == 0 ) ) ) ) {
	/* We need certain options, if restoring from more than one tape */
        if(tapes->next && !am_has_feature(their_features, fe_recover_splits)) {
            error("%s: Client must support split dumps to restore requested data.",  get_pname());
            /* NOTREACHED */
        }
	dbprintf(("%s: Restoring from changer, checking labels\n", get_pname()));
	rst_flags->check_labels = 1;
	use_changer = 1;
    }

    /* If we'll be stepping on the tape server's devices, lock them. */
    if(re_config  &&
       (use_changer || (rst_flags->alt_tapedev &&
                        strcmp(rst_flags->alt_tapedev,
                               getconf_str(CNF_TAPEDEV)) == 0) ) ) {
	dbprintf(("%s: Locking devices\n", get_pname()));
	parent_pid = getpid();
	atexit(cleanup);
	get_lock = lock_logfile();
    }

    /* Init the tape changer */
    if(tapes && use_changer && changer_init() == 0) {
	dbprintf(("%s: No changer available\n", debug_prefix_time(NULL)));
    }

    /* Read the default block size from the tape type */
    if(re_config && (conf_tapetype = getconf_str(CNF_TAPETYPE)) != NULL) {
	tape = lookup_tapetype(conf_tapetype);
	rst_flags->blocksize = tape->blocksize * 1024;
    }

    if(rst_flags->fsf && re_config && 
       getconf_int(CNF_AMRECOVER_DO_FSF) == 0) {
	rst_flags->fsf = 0;
    }

    if(re_config && getconf_int(CNF_AMRECOVER_CHECK_LABEL) == 0) {
	rst_flags->check_labels = 0;
    }

    /* establish a distinct data connection for dumpfile data */
    if(am_has_feature(their_features, fe_recover_splits)){
	int data_fd;
	char *buf;
	
	dbprintf(("%s: Client understands split dumpfiles\n", get_pname()));
	
	if((data_sock = stream_server(&data_port, STREAM_BUFSIZE, -1)) < 0){
	    error("%s: could not create data socket: %s", get_pname(),
		  strerror(errno));
	}
	dbprintf(("%s: Local port %d set aside for data\n", get_pname(),
		  data_port));
	
	printf("CONNECT %d\n", data_port); /* tell client where to connect */
	fflush(stdout);
	
	if((data_fd = stream_accept(data_sock, TIMEOUT, -1, -1)) < 0){
	    error("stream_accept failed for client data connection: %s\n",
		  strerror(errno));
	}

	buf = get_client_line_fd(data_fd);

	check_security_buffer(buf);
	rst_flags->pipe_to_fd = data_fd;
        prompt_stream = stdout;
    }
    else {
	rst_flags->pipe_to_fd = fileno(stdout);
        prompt_stream = stderr;
    }
    dbprintf(("%s: Sending output to file descriptor %d\n",
	      get_pname(), rst_flags->pipe_to_fd));
    
    
    /* make sure our restore flags aren't crazy */
    if(check_rst_flags(rst_flags) == -1){
	if(rst_flags->pipe_to_fd != -1) aclose(rst_flags->pipe_to_fd);
	exit(1);
    }
    
    /* actual restoration */
    search_tapes(prompt_stream, use_changer, tapes, match_list, rst_flags,
		 their_features);
    dbprintf(("%s: Restoration finished\n", debug_prefix_time(NULL)));
    
    /* cleanup */
    if(rst_flags->pipe_to_fd != -1) aclose(rst_flags->pipe_to_fd);
    free_tapelist(tapes);
    
    am_release_feature_set(their_features);

    amfree(rst_flags->alt_tapedev);
    amfree(rst_flags);
    amfree(match_list->hostname);
    amfree(match_list->diskname);
    amfree(match_list->datestamp);
    amfree(match_list);
    amfree(config_dir);
    amfree(re_config);
    dbclose();
    return 0;
}

static void
cleanup(void)
{
    if(parent_pid == getpid()) {
	if(get_lock) unlink(rst_conf_logfile);
    }
}

