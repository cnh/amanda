/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1999 University of Maryland at College Park
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
 * $Id: util.c,v 1.22 2006/05/25 14:07:30 martinea Exp $
 */

#include "amanda.h"
#include "util.h"
#include "arglist.h"
#include "clock.h"

int allow_overwrites;
int token_pushed;

tok_t tok, pushed_tok;
val_t tokenval;

int conf_line_num, got_parserror;
FILE *conf_conf = (FILE *)NULL;
char *conf_confname = NULL;

#define NET_READ_DEBUG

#ifdef NET_READ_DEBUG
#define netprintf(x)    dbprintf(x)
#else
#define netprintf(x)
#endif

static int make_socket(void);

/*
 * Keep calling read() until we've read buflen's worth of data, or EOF,
 * or we get an error.
 *
 * Returns the number of bytes read, 0 on EOF, or negative on error.
 */
ssize_t
fullread(
    int		fd,
    void *	vbuf,
    size_t	buflen)
{
    ssize_t nread, tot = 0;
    char *buf = vbuf;	/* cast to char so we can ++ it */

    while (buflen > 0) {
	nread = read(fd, buf, buflen);
	if (nread < 0) {
	    if ((errno == EINTR) || (errno == EAGAIN))
		continue;
	    return ((tot > 0) ? tot : -1);
	}

	if (nread == 0)
	    break;

	tot += nread;
	buf += nread;
	buflen -= nread;
    }
    return (tot);
}

/*
 * Keep calling write() until we've written buflen's worth of data,
 * or we get an error.
 *
 * Returns the number of bytes written, or negative on error.
 */
ssize_t
fullwrite(
    int		fd,
    const void *vbuf,
    size_t	buflen)
{
    ssize_t nwritten, tot = 0;
    const char *buf = vbuf;	/* cast to char so we can ++ it */

    while (buflen > 0) {
	nwritten = write(fd, buf, buflen);
	if (nwritten < 0) {
	    if ((errno == EINTR) || (errno == EAGAIN))
		continue;
	    return ((tot > 0) ? tot : -1);
	}
	tot += nwritten;
	buf += nwritten;
	buflen -= nwritten;
    }
    return (tot);
}

static int
make_socket(void)
{
    int s;
    int save_errno;
    int on=1;
    int r;

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        save_errno = errno;
        dbprintf(("%s: make_socket: socket() failed: %s\n",
                  debug_prefix(NULL),
                  strerror(save_errno)));
        errno = save_errno;
        return -1;
    }
    if (s < 0 || s >= FD_SETSIZE) {
        aclose(s);
        errno = EMFILE;                         /* out of range */
        return -1;
    }

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

#ifdef SO_KEEPALIVE
    r = setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
		   (void *)&on, sizeof(on));
    if (r == -1) {
	save_errno = errno;
	dbprintf(("%s: make_socket: setsockopt() failed: %s\n",
                  debug_prefix(NULL),
                  strerror(save_errno)));
	aclose(s);
	errno = save_errno;
	return -1;
    }
#endif

    return s;
}

/* addrp is my address */
/* svaddr is the address of the remote machine */
int
connect_portrange(
    struct sockaddr_in *addrp,
    int			first_port,
    int			last_port,
    char *		proto,
    struct sockaddr_in *svaddr,
    int			nonblock)
{
    int			s;
    int			save_errno;
    struct servent *	servPort;
    int			port;
    socklen_t		len;

    assert(first_port > 0 && first_port <= last_port && last_port < 65536);

    if ((s = make_socket()) == -1) return -1;

    for (port = first_port; port <= last_port; port++) {
	servPort = getservbyport((int)htons((in_port_t)port), proto);
	if ((servPort == NULL) || strstr(servPort->s_name, "amanda")){
	    dbprintf(("%s: connect_portrange: trying port=%d\n",
		      debug_prefix_time(NULL), port));
	    addrp->sin_port = (in_port_t)htons((in_port_t)port);
	    if (bind(s, (struct sockaddr *)addrp, sizeof(*addrp)) >= 0) {
		/* find out what port was actually used */

		len = sizeof(*addrp);
		if (getsockname(s, (struct sockaddr *)addrp, &len) == -1) {
		    save_errno = errno;
		    dbprintf((
			   "%s: connect_portrange: getsockname() failed: %s\n",
			   debug_prefix(NULL),
			   strerror(save_errno)));
		    aclose(s);
		    if ((s = make_socket()) == -1) return -1;
		    errno = save_errno;
		}

		else {
		    if (nonblock)
			fcntl(s, F_SETFL,
			      fcntl(s, F_GETFL, 0)|O_NONBLOCK);

		    if (connect(s, (struct sockaddr *)svaddr,
			       sizeof(*svaddr)) == -1 && !nonblock) {
			save_errno = errno;
			dbprintf((
			"%s: connect_portrange: connect to %s.%d failed: %s\n",
				  debug_prefix_time(NULL),
				  inet_ntoa(svaddr->sin_addr),
				  ntohs(svaddr->sin_port),
				  strerror(save_errno)));
			aclose(s);
			if (save_errno != EADDRNOTAVAIL) {
			    dbprintf(("errno %d strerror %s\n",
				      errno, strerror(errno)));
			    errno = save_errno;
			    return -1;
			}
			if ((s = make_socket()) == -1) return -1;
		    }
		    else {
			return s;
		    }
		}
	    }
	    /*
	     * If the error was something other then port in use, stop.
	     */
	    else if (errno != EADDRINUSE) {
		save_errno = errno;
		dbprintf(("errno %d strerror %s\n",
			  errno, strerror(errno)));
		errno = save_errno;
		return -1;
	    }
	}
	port++;
    }
    return -1;
}


/*
 * Bind to a port in the given range.  Takes a begin,end pair of port numbers.
 *
 * Returns negative on error (EGAIN if all ports are in use).  Returns 0
 * on success.
 */
int
bind_portrange(
    int			s,
    struct sockaddr_in *addrp,
    in_port_t		first_port,
    in_port_t		last_port,
    char *		proto)
{
    in_port_t port;
    in_port_t cnt;
    struct servent *servPort;
    const in_port_t num_ports = (in_port_t)(last_port - first_port + 1);

    assert(first_port <= last_port);

    /*
     * We pick a different starting port based on our pid and the current
     * time to avoid always picking the same reserved port twice.
     */
    port = (in_port_t)(((getpid() + time(0)) % num_ports) + first_port);

    /*
     * Scan through the range, trying all available ports that are either 
     * not taken in /etc/services or registered for *amanda*.  Wrap around
     * if we don't happen to start at the beginning.
     */
    for (cnt = 0; cnt < num_ports; cnt++) {
	servPort = getservbyport((int)htons(port), proto);
	if ((servPort == NULL) || strstr(servPort->s_name, "amanda")) {
	    if (servPort == NULL) {
		dbprintf(("%s: bind_portrange2: Try  port %d: Available   - ",
		      debug_prefix_time(NULL), port));
	    } else {
		dbprintf(("%s: bind_portrange2: Try  port %d: Owned by %s - ",
		      debug_prefix_time(NULL), port, servPort->s_name));
	    }
	    addrp->sin_port = (in_port_t)htons(port);
	    if (bind(s, (struct sockaddr *)addrp, (socklen_t)sizeof(*addrp)) >= 0) {
	        dbprintf(("Success\n"));
		return 0;
	    }
	    dbprintf(("%s\n", strerror(errno)));
	} else {
	        dbprintf(("%s: bind_portrange2: Skip port %d: Owned by %s.\n",
		      debug_prefix_time(NULL), port, servPort->s_name));
	}
	if (++port > last_port)
	    port = first_port;
    }
    dbprintf(("%s: bind_portrange: all ports between %d and %d busy\n",
		  debug_prefix_time(NULL),
		  first_port,
		  last_port));
    errno = EAGAIN;
    return -1;
}

/*
 * Construct a datestamp (YYYYMMDD) from a time_t.
 */
char *
construct_datestamp(
    time_t *t)
{
    struct tm *tm;
    char datestamp[3*NUM_STR_SIZE];
    time_t when;

    if (t == NULL) {
	when = time((time_t *)NULL);
    } else {
	when = *t;
    }
    tm = localtime(&when);
    snprintf(datestamp, SIZEOF(datestamp),
             "%04d%02d%02d", tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday);
    return stralloc(datestamp);
}

/*
 * Construct a timestamp (YYYYMMDDHHMMSS) from a time_t.
 */
char *
construct_timestamp(
    time_t *t)
{
    struct tm *tm;
    char timestamp[6*NUM_STR_SIZE];
    time_t when;

    if (t == NULL) {
	when = time((time_t *)NULL);
    } else {
	when = *t;
    }
    tm = localtime(&when);
    snprintf(timestamp, SIZEOF(timestamp),
	     "%04d%02d%02d%02d%02d%02d",
	     tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
	     tm->tm_hour, tm->tm_min, tm->tm_sec);
    return stralloc(timestamp);
}


int
needs_quotes(
    const char * str)
{
    return (match("[ \t\f\r\n\"]", str) != 0);
}


/*
 * For backward compatibility we are trying for minimal quoting.
 * We only quote a string if it contains whitespace or is misquoted...
 */

char *
quote_string(
    const char *str)
{
    char *  s;
    char *  ret;

    if ((str == NULL) || (*str == '\0')) {
	ret = stralloc("\"\"");
    } else if ((match("[\\\"[:space:][:cntrl:]]", str)) == 0) {
	/*
	 * String does not need to be quoted since it contains
	 * neither whitespace, control or quote characters.
	 */
	ret = stralloc(str);
    } else {
	/*
	 * Allocate maximum possible string length.
	 * (a string of all quotes plus room for leading ", trailing " and NULL)
	 */
	ret = s = alloc((strlen(str) * 2) + 2 + 1);
	*(s++) = '"';
	while (*str != '\0') {
            if (*str == '\t') {
                *(s++) = '\\';
                *(s++) = 't';
		str++;
		continue;
	    } else if (*str == '\n') {
                *(s++) = '\\';
                *(s++) = 'n';
		str++;
		continue;
	    } else if (*str == '\r') {
                *(s++) = '\\';
                *(s++) = 'r';
		str++;
		continue;
	    } else if (*str == '\f') {
                *(s++) = '\\';
                *(s++) = 'f';
		str++;
		continue;
	    }
            if (*str == '"')
                *(s++) = '\\';
            *(s++) = *(str++);
        }
        *(s++) = '"';
        *s = '\0';
    }
    return (ret);
}


char *
unquote_string(
    const char *str)
{
    char * ret;

    if ((str == NULL) || (*str == '\0')) {
	ret = stralloc("");
    } else {
	char * in;
	char * out;

	ret = in = out = stralloc(str);
	while (*in != '\0') {
	    if (*in == '"') {
	        in++;
		continue;
	    }

	    if (*in == '\\') {
		in++;
		if (*in == 'n') {
		    in++;
		    *(out++) = '\n';
		    continue;
		} else if (*in == 't') {
		    in++;
		    *(out++) = '\t';
		    continue;
		} else if (*in == 'r') {
		    in++;
		    *(out++) = '\r';
		    continue;
		} else if (*in == 'f') {
		    in++;
		    *(out++) = '\f';
		    continue;
		}
	    }
	    *(out++) = *(in++);
	}
        *out = '\0';
    }
    return (ret);
}

char *
sanitize_string(
    const char *str)
{
    char * s;
    char * ret;

    if ((str == NULL) || (*str == '\0')) {
	ret = stralloc("");
    } else {
	ret = stralloc(str);
	for (s = ret; *s != '\0'; s++) {
	    if (iscntrl(*s))
		*s = '?';
	}
    }
    return (ret);
}

char *
strquotedstr(void)
{
    char *  tok = strtok(NULL, " ");

    if ((tok != NULL) && (tok[0] == '"')) {
	char *	t;
	size_t	len;

        len = strlen(tok);
	do {
	    t = strtok(NULL, " ");
	    tok[len] = ' ';
	    len = strlen(tok);
	} while ((t != NULL) &&
	         (tok[len - 1] != '"') && (tok[len - 2] != '\\'));
    }
    return tok;
}

ssize_t
hexdump(
    const char *buffer,
    size_t	len)
{
    ssize_t rc = -1;

    FILE *stream = popen("od -w10 -c -x -", "w");
	
    if (stream != NULL) {
	fflush(stdout);
	rc = (ssize_t)fwrite(buffer, len, 1, stream);
	if (ferror(stream))
	    rc = -1;
	fclose(stream);
    }
    return rc;
}

/*
   Return 0 if the following characters are present
   * ( ) < > [ ] , ; : ! $ \ / "
   else returns 1
*/

int
validate_mailto(
    const char *mailto)
{
    return !match("\\*|<|>|\\(|\\)|\\[|\\]|,|;|:|\\\\|/|\"|\\!|\\$|\\|", mailto);
}


void
get_simple(
    val_t *	var,
    int *	seen,
    tok_t	type)
{
    ckseen(seen);

    switch(type) {
    case CONF_STRING:
    case CONF_IDENT:
	get_conftoken(type);
	var->s = newstralloc(var->s, tokenval.s);
	malloc_mark(var->s);
	break;

    case CONF_INT:
	var->i = get_int();
	break;

    case CONF_LONG:
	var->l = get_long();
	break;

    case CONF_SIZE:
	var->size = get_size();
	break;

    case CONF_AM64:
	var->am64 = get_am64_t();
	break;

    case CONF_BOOL:
	var->i = get_bool();
	break;

    case CONF_REAL:
	get_conftoken(CONF_REAL);
	var->r = tokenval.r;
	break;

    case CONF_TIME:
	var->t = get_time();
	break;

    default:
	error("error [unknown get_simple type: %d]", type);
	/* NOTREACHED */
    }
    return;
}

time_t
get_time(void)
{
    time_t hhmm;

    get_conftoken(CONF_ANY);
    switch(tok) {
    case CONF_INT:
#if SIZEOF_TIME_T < SIZEOF_INT
	if ((am64_t)tokenval.i >= (am64_t)TIME_MAX)
	    conf_parserror("value too large");
#endif
	hhmm = (time_t)tokenval.i;
	break;

    case CONF_LONG:
#if SIZEOF_TIME_T < SIZEOF_LONG
	if ((am64_t)tokenval.l >= (am64_t)TIME_MAX)
	    conf_parserror("value too large");
#endif
	hhmm = (time_t)tokenval.l;
	break;

    case CONF_SIZE:
#if SIZEOF_TIME_T < SIZEOF_SSIZE_T
	if ((am64_t)tokenval.size >= (am64_t)TIME_MAX)
	    conf_parserror("value too large");
#endif
	hhmm = (time_t)tokenval.size;
	break;

    case CONF_AM64:
#if SIZEOF_TIME_T < SIZEOF_LONG_LONG
	if ((am64_t)tokenval.am64 >= (am64_t)TIME_MAX)
	    conf_parserror("value too large");
#endif
	hhmm = (time_t)tokenval.am64;
	break;

    case CONF_AMINFINITY:
	hhmm = TIME_MAX;
	break;

    default:
	conf_parserror("a time is expected");
	hhmm = 0;
	break;
    }
    return hhmm;
}

keytab_t numb_keytable[] = {
    { "B", CONF_MULT1 },
    { "BPS", CONF_MULT1 },
    { "BYTE", CONF_MULT1 },
    { "BYTES", CONF_MULT1 },
    { "DAY", CONF_MULT1 },
    { "DAYS", CONF_MULT1 },
    { "INF", CONF_AMINFINITY },
    { "K", CONF_MULT1K },
    { "KB", CONF_MULT1K },
    { "KBPS", CONF_MULT1K },
    { "KBYTE", CONF_MULT1K },
    { "KBYTES", CONF_MULT1K },
    { "KILOBYTE", CONF_MULT1K },
    { "KILOBYTES", CONF_MULT1K },
    { "KPS", CONF_MULT1K },
    { "M", CONF_MULT1M },
    { "MB", CONF_MULT1M },
    { "MBPS", CONF_MULT1M },
    { "MBYTE", CONF_MULT1M },
    { "MBYTES", CONF_MULT1M },
    { "MEG", CONF_MULT1M },
    { "MEGABYTE", CONF_MULT1M },
    { "MEGABYTES", CONF_MULT1M },
    { "G", CONF_MULT1G },
    { "GB", CONF_MULT1G },
    { "GBPS", CONF_MULT1G },
    { "GBYTE", CONF_MULT1G },
    { "GBYTES", CONF_MULT1G },
    { "GIG", CONF_MULT1G },
    { "GIGABYTE", CONF_MULT1G },
    { "GIGABYTES", CONF_MULT1G },
    { "MPS", CONF_MULT1M },
    { "TAPE", CONF_MULT1 },
    { "TAPES", CONF_MULT1 },
    { "WEEK", CONF_MULT7 },
    { "WEEKS", CONF_MULT7 },
    { NULL, CONF_IDENT }
};

int
get_int(void)
{
    int val;
    keytab_t *save_kt;

    save_kt = keytable;
    keytable = numb_keytable;

    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_INT:
	val = tokenval.i;
	break;

    case CONF_LONG:
#if SIZEOF_INT < SIZEOF_LONG
	if ((am64_t)tokenval.l > (am64_t)INT_MAX)
	    conf_parserror("value too large");
	if ((am64_t)tokenval.l < (am64_t)INT_MIN)
	    conf_parserror("value too small");
#endif
	val = (int)tokenval.l;
	break;

    case CONF_SIZE:
#if SIZEOF_INT < SIZEOF_SSIZE_T
	if ((am64_t)tokenval.size > (am64_t)INT_MAX)
	    conf_parserror("value too large");
	if ((am64_t)tokenval.size < (am64_t)INT_MIN)
	    conf_parserror("value too small");
#endif
	val = (int)tokenval.size;
	break;

    case CONF_AM64:
#if SIZEOF_INT < SIZEOF_LONG_LONG
	if (tokenval.am64 > (am64_t)INT_MAX)
	    conf_parserror("value too large");
	if (tokenval.am64 < (am64_t)INT_MIN)
	    conf_parserror("value too small");
#endif
	val = (int)tokenval.am64;
	break;

    case CONF_AMINFINITY:
	val = INT_MAX;
	break;

    default:
	conf_parserror("an int is expected");
	val = 0;
	break;
    }

    /* get multiplier, if any */
    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_NL:			/* multiply by one */
    case CONF_MULT1:
    case CONF_MULT1K:
	break;

    case CONF_MULT7:
	if (val > (INT_MAX / 7))
	    conf_parserror("value too large");
	if (val < (INT_MIN / 7))
	    conf_parserror("value too small");
	val *= 7;
	break;

    case CONF_MULT1M:
	if (val > (INT_MAX / 1024))
	    conf_parserror("value too large");
	if (val < (INT_MIN / 1024))
	    conf_parserror("value too small");
	val *= 1024;
	break;

    case CONF_MULT1G:
	if (val > (INT_MAX / (1024 * 1024)))
	    conf_parserror("value too large");
	if (val < (INT_MIN / (1024 * 1024)))
	    conf_parserror("value too small");
	val *= 1024 * 1024;
	break;

    default:	/* it was not a multiplier */
	unget_conftoken();
	break;
    }

    keytable = save_kt;
    return val;
}

long
get_long(void)
{
    long val;
    keytab_t *save_kt;

    save_kt = keytable;
    keytable = numb_keytable;

    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_LONG:
	val = tokenval.l;
	break;

    case CONF_INT:
#if SIZEOF_LONG < SIZEOF_INT
	if ((am64_t)tokenval.i > (am64_t)LONG_MAX)
	    conf_parserror("value too large");
	if ((am64_t)tokenval.i < (am64_t)LONG_MIN)
	    conf_parserror("value too small");
#endif
	val = (long)tokenval.i;
	break;

    case CONF_SIZE:
#if SIZEOF_LONG < SIZEOF_SSIZE_T
	if ((am64_t)tokenval.size > (am64_t)LONG_MAX)
	    conf_parserror("value too large");
	if ((am64_t)tokenval.size < (am64_t)LONG_MIN)
	    conf_parserror("value too small");
#endif
	val = (long)tokenval.size;
	break;

    case CONF_AM64:
#if SIZEOF_LONG < SIZEOF_LONG_LONG
	if (tokenval.am64 > (am64_t)LONG_MAX)
	    conf_parserror("value too large");
	if (tokenval.am64 < (am64_t)LONG_MIN)
	    conf_parserror("value too small");
#endif
	val = (long)tokenval.am64;
	break;

    case CONF_AMINFINITY:
	val = (long)LONG_MAX;
	break;

    default:
	conf_parserror("a long is expected");
	val = 0;
	break;
    }

    /* get multiplier, if any */
    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_NL:			/* multiply by one */
    case CONF_MULT1:
    case CONF_MULT1K:
	break;

    case CONF_MULT7:
	if (val > (LONG_MAX / 7L))
	    conf_parserror("value too large");
	if (val < (LONG_MIN / 7L))
	    conf_parserror("value too small");
	val *= 7L;
	break;

    case CONF_MULT1M:
	if (val > (LONG_MAX / 1024L))
	    conf_parserror("value too large");
	if (val < (LONG_MIN / 1024L))
	    conf_parserror("value too small");
	val *= 1024L;
	break;

    case CONF_MULT1G:
	if (val > (LONG_MAX / (1024L * 1024L)))
	    conf_parserror("value too large");
	if (val < (LONG_MIN / (1024L * 1024L)))
	    conf_parserror("value too small");
	val *= 1024L * 1024L;
	break;

    default:	/* it was not a multiplier */
	unget_conftoken();
	break;
    }

    keytable = save_kt;
    return val;
}

ssize_t
get_size(void)
{
    ssize_t val;
    keytab_t *save_kt;

    save_kt = keytable;
    keytable = numb_keytable;

    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_SIZE:
	val = tokenval.size;
	break;

    case CONF_INT:
#if SIZEOF_SIZE_T < SIZEOF_INT
	if ((am64_t)tokenval.i > (am64_t)SSIZE_MAX)
	    conf_parserror("value too large");
	if ((am64_t)tokenval.i < (am64_t)SSIZE_MIN)
	    conf_parserror("value too small");
#endif
	val = (ssize_t)tokenval.i;
	break;

    case CONF_LONG:
#if SIZEOF_SIZE_T < SIZEOF_LONG
	if ((am64_t)tokenval.l > (am64_t)SSIZE_MAX)
	    conf_parserror("value too large");
	if ((am64_t)tokenval.l < (am64_t)SSIZE_MIN)
	    conf_parserror("value too small");
#endif
	val = (ssize_t)tokenval.l;
	break;

    case CONF_AM64:
#if SIZEOF_SIZE_T < SIZEOF_LONG_LONG
	if (tokenval.am64 > (am64_t)SSIZE_MAX)
	    conf_parserror("value too large");
	if (tokenval.am64 < (am64_t)SSIZE_MIN)
	    conf_parserror("value too small");
#endif
	val = (ssize_t)tokenval.am64;
	break;

    case CONF_AMINFINITY:
	val = (ssize_t)SSIZE_MAX;
	break;

    default:
	conf_parserror("an integer is expected");
	val = 0;
	break;
    }

    /* get multiplier, if any */
    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_NL:			/* multiply by one */
    case CONF_MULT1:
    case CONF_MULT1K:
	break;

    case CONF_MULT7:
	if (val > (ssize_t)(SSIZE_MAX / 7))
	    conf_parserror("value too large");
	if (val < (ssize_t)(SSIZE_MIN / 7))
	    conf_parserror("value too small");
	val *= (ssize_t)7;
	break;

    case CONF_MULT1M:
	if (val > (ssize_t)(SSIZE_MAX / (ssize_t)1024))
	    conf_parserror("value too large");
	if (val < (ssize_t)(SSIZE_MIN / (ssize_t)1024))
	    conf_parserror("value too small");
	val *= (ssize_t)1024;
	break;

    case CONF_MULT1G:
	if (val > (ssize_t)(SSIZE_MAX / (1024 * 1024)))
	    conf_parserror("value too large");
	if (val < (ssize_t)(SSIZE_MIN / (1024 * 1024)))
	    conf_parserror("value too small");
	val *= (ssize_t)(1024 * 1024);
	break;

    default:	/* it was not a multiplier */
	unget_conftoken();
	break;
    }

    keytable = save_kt;
    return val;
}

am64_t
get_am64_t(void)
{
    am64_t val;
    keytab_t *save_kt;

    save_kt = keytable;
    keytable = numb_keytable;

    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_INT:
	val = (am64_t)tokenval.i;
	break;

    case CONF_LONG:
	val = (am64_t)tokenval.l;
	break;

    case CONF_SIZE:
	val = (am64_t)tokenval.size;
	break;

    case CONF_AM64:
	val = tokenval.am64;
	break;

    case CONF_AMINFINITY:
	val = AM64_MAX;
	break;

    default:
	conf_parserror("an am64 is expected %d", tok);
	val = 0;
	break;
    }

    /* get multiplier, if any */
    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_NL:			/* multiply by one */
    case CONF_MULT1:
    case CONF_MULT1K:
	break;

    case CONF_MULT7:
	if (val > AM64_MAX/7 || val < AM64_MIN/7)
	    conf_parserror("value too large");
	val *= 7;
	break;

    case CONF_MULT1M:
	if (val > AM64_MAX/1024 || val < AM64_MIN/1024)
	    conf_parserror("value too large");
	val *= 1024;
	break;

    case CONF_MULT1G:
	if (val > AM64_MAX/(1024*1024) || val < AM64_MIN/(1024*1024))
	    conf_parserror("value too large");
	val *= 1024*1024;
	break;

    default:	/* it was not a multiplier */
	unget_conftoken();
	break;
    }

    keytable = save_kt;

    return val;
}

keytab_t bool_keytable[] = {
    { "Y", CONF_ATRUE },
    { "YES", CONF_ATRUE },
    { "T", CONF_ATRUE },
    { "TRUE", CONF_ATRUE },
    { "ON", CONF_ATRUE },
    { "N", CONF_AFALSE },
    { "NO", CONF_AFALSE },
    { "F", CONF_AFALSE },
    { "FALSE", CONF_AFALSE },
    { "OFF", CONF_AFALSE },
    { NULL, CONF_IDENT }
};

int
get_bool(void)
{
    int val;
    keytab_t *save_kt;

    save_kt = keytable;
    keytable = bool_keytable;

    get_conftoken(CONF_ANY);

    switch(tok) {
    case CONF_INT:
	if (tokenval.i != 0)
	    val = 1;
	else
	    val = 0;
	break;

    case CONF_LONG:
	if (tokenval.l != 0L)
	    val = 1;
	else
	    val = 0;
	break;

    case CONF_SIZE:
	if (tokenval.size != (size_t)0)
	    val = 1;
	else
	    val = 0;
	break;

    case CONF_AM64:
	if (tokenval.am64 != (am64_t)0)
	    val = 1;
	else
	    val = 0;
	break;

    case CONF_ATRUE:
	val = 1;
	break;

    case CONF_AFALSE:
	val = 0;
	break;

    case CONF_NL:
    default:
	unget_conftoken();
	val = 2; /* no argument - most likely TRUE */
	break;
    }

    keytable = save_kt;
    return val;
}

void ckseen(
    int *	seen)
{
    if (*seen && !allow_overwrites) {
	conf_parserror("duplicate parameter, prev def on line %d", *seen);
    }
    *seen = conf_line_num;
}

printf_arglist_function(void conf_parserror, const char *, format)
{
    va_list argp;

    /* print error message */

    fprintf(stderr, "\"%s\", line %d: ", conf_confname, conf_line_num);
    arglist_start(argp, format);
    vfprintf(stderr, format, argp);
    arglist_end(argp);
    fputc('\n', stderr);

    got_parserror = 1;
}

tok_t
lookup_keyword(
    char *	str)
{
    keytab_t *kwp;

    /* switch to binary search if performance warrants */

    for(kwp = keytable; kwp->keyword != NULL; kwp++) {
	if (strcmp(kwp->keyword, str) == 0) break;
    }
    return kwp->token;
}

char tkbuf[4096];

/* push the last token back (can only unget ANY tokens) */
void
unget_conftoken(void)
{
    token_pushed = 1;
    pushed_tok = tok;
    tok = CONF_UNKNOWN;
    return;
}

void
get_conftoken(
    tok_t	exp)
{
    int ch, d;
    am64_t am64;
    char *buf;
    int token_overflow;

    if (token_pushed) {
	token_pushed = 0;
	tok = pushed_tok;

	/*
	** If it looked like a key word before then look it
	** up again in the current keyword table.
	*/
	switch(tok) {
	case CONF_LONG:    case CONF_AM64:    case CONF_SIZE:
	case CONF_INT:     case CONF_REAL:    case CONF_STRING:
	case CONF_LBRACE:  case CONF_RBRACE:  case CONF_COMMA:
	case CONF_NL:      case CONF_END:     case CONF_UNKNOWN:
	case CONF_TIME:
	    break;

	default:
	    if (exp == CONF_IDENT)
		tok = CONF_IDENT;
	    else
		tok = lookup_keyword(tokenval.s);
	    break;
	}
    }
    else {
	ch = getc(conf_conf);

	while(ch != EOF && ch != '\n' && isspace(ch))
	    ch = getc(conf_conf);
	if (ch == '#') {		/* comment - eat everything but eol/eof */
	    while((ch = getc(conf_conf)) != EOF && ch != '\n') {
		(void)ch; /* Quiet empty loop complaints */	
	    }
	}

	if (isalpha(ch)) {		/* identifier */
	    buf = tkbuf;
	    token_overflow = 0;
	    do {
		if (islower(ch)) ch = toupper(ch);
		if (buf < tkbuf+sizeof(tkbuf)-1) {
		    *buf++ = (char)ch;
		} else {
		    *buf = '\0';
		    if (!token_overflow) {
			conf_parserror("token too long: %.20s...", tkbuf);
		    }
		    token_overflow = 1;
		}
		ch = getc(conf_conf);
	    } while(isalnum(ch) || ch == '_' || ch == '-');

	    ungetc(ch, conf_conf);
	    *buf = '\0';

	    tokenval.s = tkbuf;

	    if (token_overflow) tok = CONF_UNKNOWN;
	    else if (exp == CONF_IDENT) tok = CONF_IDENT;
	    else tok = lookup_keyword(tokenval.s);
	}
	else if (isdigit(ch)) {	/* integer */
	    int sign;
	    if (1) {
		sign = 1;
	    } else {
	    negative_number: /* look for goto negative_number below */
		sign = -1;
	    }
	    tokenval.am64 = 0;
	    do {
		tokenval.am64 = tokenval.am64 * 10 + (ch - '0');
		ch = getc(conf_conf);
	    } while(isdigit(ch));
	    if (ch != '.') {
		if (exp == CONF_INT) {
		    tok = CONF_INT;
		    tokenval.i *= sign;
		}
		else if (exp == CONF_LONG) {
		    tok = CONF_LONG;
		    tokenval.l *= sign;
		}
		else if (exp != CONF_REAL) {
		    tok = CONF_AM64;
		    tokenval.am64 *= sign;
		} else {
		    /* automatically convert to real when expected */
		    am64 = tokenval.am64;
		    tokenval.r = sign * (double) am64;
		    tok = CONF_REAL;
		}
	    }
	    else {
		/* got a real number, not an int */
		am64 = tokenval.am64;
		tokenval.r = sign * (double) am64;
		am64=0; d=1;
		ch = getc(conf_conf);
		while(isdigit(ch)) {
		    am64 = am64 * 10 + (ch - '0');
		    d = d * 10;
		    ch = getc(conf_conf);
		}
		tokenval.r += sign * ((double)am64)/d;
		tok = CONF_REAL;
	    }
	    ungetc(ch, conf_conf);
	} else switch(ch) {
	case '"':			/* string */
	    buf = tkbuf;
	    token_overflow = 0;
	    ch = getc(conf_conf);
	    while(ch != '"' && ch != '\n' && ch != EOF) {
		if (buf < tkbuf+sizeof(tkbuf)-1) {
		    *buf++ = (char)ch;
		} else {
		    *buf = '\0';
		    if (!token_overflow) {
			conf_parserror("string too long: %.20s...", tkbuf);
		    }
		    token_overflow = 1;
		}
		ch = getc(conf_conf);
	    }
	    if (ch != '"') {
		conf_parserror("missing end quote");
		ungetc(ch, conf_conf);
	    }
	    *buf = '\0';
	    tokenval.s = tkbuf;
	    if (token_overflow) tok = CONF_UNKNOWN;
	    else tok = CONF_STRING;
	    break;

	case '-':
	    ch = getc(conf_conf);
	    if (isdigit(ch))
		goto negative_number;
	    else {
		ungetc(ch, conf_conf);
		tok = CONF_UNKNOWN;
	    }
	    break;

	case ',':
	    tok = CONF_COMMA;
	    break;

	case '{':
	    tok = CONF_LBRACE;
	    break;

	case '}':
	    tok = CONF_RBRACE;
	    break;

	case '\n':
	    tok = CONF_NL;
	    break;

	case EOF:
	    tok = CONF_END;
	    break;

	default:
	    tok = CONF_UNKNOWN;
	    break;
	}
    }

    if (exp != CONF_ANY && tok != exp) {
	char *str;
	keytab_t *kwp;

	switch(exp) {
	case CONF_LBRACE:
	    str = "\"{\"";
	    break;

	case CONF_RBRACE:
	    str = "\"}\"";
	    break;

	case CONF_COMMA:
	    str = "\",\"";
	    break;

	case CONF_NL:
	    str = "end of line";
	    break;

	case CONF_END:
	    str = "end of file";
	    break;

	case CONF_INT:
	    str = "an integer";
	    break;

	case CONF_REAL:
	    str = "a real number";
	    break;

	case CONF_STRING:
	    str = "a quoted string";
	    break;

	case CONF_IDENT:
	    str = "an identifier";
	    break;

	default:
	    for(kwp = keytable; kwp->keyword != NULL; kwp++) {
		if (exp == kwp->token)
		    break;
	    }
	    if (kwp->keyword == NULL)
		str = "token not";
	    else
		str = kwp->keyword;
	    break;
	}
	conf_parserror("%s is expected", str);
	tok = exp;
	if (tok == CONF_INT)
	    tokenval.i = 0;
	else
	    tokenval.s = "";
    }
}

void
dump_sockaddr(
	struct sockaddr_in *	sa)
{
	dbprintf(("%s: (sockaddr_in *)%p = { %d, %hd, %s }\n",
		debug_prefix(NULL), sa, sa->sin_family, sa->sin_port,
		inet_ntoa(sa->sin_addr)));
}

#ifndef HAVE_LIBREADLINE
/*
 * simple readline() replacements
 */

char *
readline(
    const char *prompt)
{
    printf("%s", prompt);
    fflush(stdout);
    fflush(stderr);
    return agets(stdin);
}

void 
add_history(
    const char *line)
{
    (void)line; 	/* Quite unused parameter warning */
}
#endif
