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
/* $Id: amidxtaped.c,v 1.26 1998/11/19 23:05:14 kashmir Exp $
 *
 * This daemon extracts a dump image off a tape for amrecover and
 * returns it over the network. It basically, reads a number of
 * arguments from stdin (it is invoked via inet), one per line,
 * preceeded by the number of them, and forms them into an argv
 * structure, then execs amrestore
 */

#include "amanda.h"
#include "version.h"

#include "tapeio.h"

static char *get_client_line P((void));
static int check_security P((struct sockaddr_in *, char *, unsigned long,
    char **));

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
		dbprintf(("read error: %s\n", strerror(errno)));
	    } else {
		dbprintf(("EOF reached\n"));
	    }
	    if(line) {
		dbprintf(("unprocessed input:\n"));
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
	 * Hmmm.  We got a "line" from areads(), which means it saw
	 * a '\n' (or EOF, etc), but there was not a '\r' before it.
	 * Put a '\n' back in the buffer and loop for more.
	 */
	strappend(line, "\n");
    }
    dbprintf(("> %s\n", line));
    return line;
}

int main(argc, argv)
int argc;
char **argv;
{
#ifdef FORCE_USERID
    char *pwname;
    struct passwd *pwptr;
#endif	/* FORCE_USERID */
    int amrestore_nargs;
    char **amrestore_args;
    char *buf = NULL;
    int i;
    char *amrestore_path;
    pid_t pid;
    int isafile;
    struct stat stat_tape;
    char *tapename = NULL;
    int fd;
    char *s, *fp;
    int ch;
    char *errstr = NULL;
    struct sockaddr_in addr;
    amwait_t status;

    for(fd = 3; fd < FD_SETSIZE; fd++) {
	/*
	 * Make sure nobody spoofs us with a lot of extra open files
	 * that would cause an open we do to get a very high file
	 * descriptor, which in turn might be used as an index into
	 * an array (e.g. an fd_set).
	 */
	close(fd);
    }

    set_pname("amidxtaped");

#ifdef FORCE_USERID

    /* we'd rather not run as root */

    if(geteuid() == 0) {
	pwname = CLIENT_LOGIN;
	if((pwptr = getpwnam(pwname)) == NULL)
	    error("error [cannot find user %s in passwd file]\n", pwname);

	initgroups(pwname, pwptr->pw_gid);
	setgid(pwptr->pw_gid);
	setuid(pwptr->pw_uid);
    }

#endif	/* FORCE_USERID */

    /* initialize */
    /* close stderr first so that debug file becomes it - amrestore
       chats to stderr, which we don't want going to client */
    /* if no debug file, ship to bit bucket */
    (void)close(STDERR_FILENO);
#ifdef DEBUG_CODE
    dbopen();
    dbprintf(("%s: version %s\n", argv[0], version()));
    if(dbfd() != -1 && dbfd() != STDERR_FILENO)
    {
	if(dup2(dbfd(),STDERR_FILENO) != STDERR_FILENO)
	{
	    perror("amidxtaped can't redirect stderr to the debug file");
	    dbprintf(("amidxtaped can't redirect stderr to the debug file"));
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

    i = sizeof (addr);
    if (getpeername(0, (struct sockaddr *)&addr, &i) == -1)
	error("getpeername: %s", strerror(errno));
    if (addr.sin_family != AF_INET || ntohs(addr.sin_port) == 20) {
	error("connection rejected from %s family %d port %d",
	      inet_ntoa(addr.sin_addr), addr.sin_family, htons(addr.sin_port));
    }

    /* do the security thing */
    amfree(buf);
    buf = stralloc(get_client_line());
    s = buf;
    ch = *s++;

    skip_whitespace(s, ch);
    if (ch == '\0')
    {
	error("cannot parse SECURITY line");
    }
    fp = s-1;
    skip_non_whitespace(s, ch);
    s[-1] = '\0';
    if (strcmp(fp, "SECURITY") != 0)
    {
	error("cannot parse SECURITY line");
    }
    skip_whitespace(s, ch);
    if (!check_security(&addr, s-1, 0, &errstr)) {
	error("security check failed: %s", errstr);
    }

    /* get the number of arguments */
    amfree(buf);
    buf = stralloc(get_client_line());
    amrestore_nargs = atoi(buf);
    dbprintf(("amrestore_nargs=%d\n", amrestore_nargs));

    if ((amrestore_args = (char **)malloc((amrestore_nargs+2)*sizeof(char *)))
	== NULL) 
    {
	dbprintf(("Couldn't malloc amrestore_args\n"));
	dbclose();
	return 1;
    }

    i = 0;
    amrestore_args[i++] = "amrestore";
    while (i <= amrestore_nargs)
    {
	amrestore_args[i++] = stralloc(get_client_line());
    }
    amrestore_args[i] = NULL;

    amrestore_path = vstralloc(sbindir, "/", "amrestore", NULL);

    /* so got all the arguments, now ready to execv */
    dbprintf(("Ready to execv amrestore with:\n"));
    dbprintf(("path = %s\n", amrestore_path));
    for (i = 0; amrestore_args[i] != NULL; i++)
    {
	dbprintf(("argv[%d] = \"%s\"\n", i, amrestore_args[i]));
    }

    if ((pid = fork()) == 0)
    {
	/* child */
	(void)execv(amrestore_path, amrestore_args);

	/* only get here if exec failed */
	dbprintf(("Child couldn't exec amrestore\n"));
	return 1;
	/*NOT REACHED*/
    }

    /* this is the parent */
    if (pid == -1)
    {
	dbprintf(("Error forking child: %s\n", strerror(errno)));
	dbclose();
	return 1;
    }

    /* wait for the child to do the restore */
    if (waitpid(pid, &status, 0) == -1)
    {
	dbprintf(("Error waiting for child"));
	dbclose();
	return 1;
    }
    /* amrestore often sees the pipe reader (ie restore) quit in the middle
       of the file because it has extracted all of the files needed. This
       results in an exit status of 2. This unfortunately is the exit
       status returned by many errors. Only when the exit status is 1 is it
       guaranteed that an error existed. In all cases we should rewind the
       tape if we can so that a retry starts from the correct place */
    if (WIFEXITED(status) != 0)
    {

	dbprintf(("amidxtaped: amrestore terminated normally with status: %d\n",
		  WEXITSTATUS(status)));
    }
    else
    {
	dbprintf(("amidxtaped: amrestore terminated abnormally.\n"));
    }

    /* rewind tape */
    /* the first non-option argument is the tape device */
    for (i = 1; i <= amrestore_nargs; i++)
	if (amrestore_args[i][0] != '-')
	    break;
    if (i > amrestore_nargs)
    {
	dbprintf(("Couldn't find tape in arguments\n"));
	dbclose();
	return 1;
    }

    tapename = stralloc(amrestore_args[i]);
    if (stat(tapename, &stat_tape) != 0)
      error("could not stat %s", tapename);
    isafile = S_ISREG((stat_tape.st_mode));
    if (!isafile) {
	char *errstr = NULL;

	dbprintf(("Rewinding tape: "));
	errstr = tape_rewind(tapename);

	if (errstr != NULL) {
	    dbprintf(("%s\n", errstr));
	    amfree(errstr);
	} else {
	    dbprintf(("done\n"));
	}
    }
    amfree(tapename);
    dbclose();
    return 0;
}

static int
check_security(addr, str, cksum, errstr)
struct sockaddr_in *addr;
char *str;
unsigned long cksum;
char **errstr;
{
    char *remotehost = NULL, *remoteuser = NULL, *localuser = NULL;
    char *bad_bsd = NULL;
    struct hostent *hp;
    struct passwd *pwptr;
    int myuid, i, j;
    char *s, *fp;
    int ch;
#ifdef USE_AMANDAHOSTS
    FILE *fPerm;
    char *pbuf = NULL;
    char *ptmp;
    int pbuf_len;
    int amandahostsauth = 0;
#else
    int saved_stderr;
#endif

    *errstr = NULL;

    /* what host is making the request? */

    hp = gethostbyaddr((char *)&addr->sin_addr, sizeof(addr->sin_addr),
		       AF_INET);
    if(hp == NULL) {
	/* XXX include remote address in message */
	*errstr = vstralloc("[",
			    "addr ", inet_ntoa(addr->sin_addr), ": ",
			    "hostname lookup failed",
			    "]", NULL);
	return 0;
    }
    remotehost = stralloc(hp->h_name);

    /* Now let's get the hostent for that hostname */
    hp = gethostbyname( remotehost );
    if(hp == NULL) {
	/* XXX include remote hostname in message */
	*errstr = vstralloc("[",
			    "host ", remotehost, ": ",
			    "hostname lookup failed",
			    "]", NULL);
	amfree(remotehost);
	return 0;
    }

    /* Verify that the hostnames match -- they should theoretically */
    if( strncasecmp( remotehost, hp->h_name, strlen(remotehost)+1 ) != 0 ) {
	*errstr = vstralloc("[",
			    "hostnames do not match: ",
			    remotehost, " ", hp->h_name,
			    "]", NULL);
	amfree(remotehost);
	return 0;
    }

    /* Now let's verify that the ip which gave us this hostname
     * is really an ip for this hostname; or is someone trying to
     * break in? (THIS IS THE CRUCIAL STEP)
     */
    for (i = 0; hp->h_addr_list[i]; i++) {
	if (memcmp(hp->h_addr_list[i],
		   (char *) &addr->sin_addr, sizeof(addr->sin_addr)) == 0)
	    break;                     /* name is good, keep it */
    }

    /* If we did not find it, your DNS is messed up or someone is trying
     * to pull a fast one on you. :(
     */

   /*   Check even the aliases list. Work around for Solaris if dns goes over NIS */

    if( !hp->h_addr_list[i] ) {
        for (j = 0; hp->h_aliases[j] !=0 ; j++) {
	     if ( strcmp(hp->h_aliases[j],inet_ntoa(addr->sin_addr)) == 0)
	         break;                          /* name is good, keep it */
        }
    }
    if( !hp->h_addr_list[i] && !hp->h_aliases[j] ) {
	*errstr = vstralloc("[",
			    "ip address ", inet_ntoa(addr->sin_addr),
			    " is not in the ip list for ", remotehost,
			    "]",
			    NULL);
	amfree(remotehost);
	return 0;
    }

    /* next, make sure the remote port is a "reserved" one */

    if(ntohs(addr->sin_port) >= IPPORT_RESERVED) {
	char number[NUM_STR_SIZE];

	ap_snprintf(number, sizeof(number), "%d", ntohs(addr->sin_port));
	*errstr = vstralloc("[",
			    "host ", remotehost, ": ",
			    "port ", number, " not secure",
			    "]", NULL);
	amfree(remotehost);
	return 0;
    }

    /* extract the remote user name from the message */

    s = str;
    ch = *s++;

    bad_bsd = vstralloc("[",
			"host ", remotehost, ": ",
			"bad bsd security line",
			"]", NULL);

#define sc "USER"
    if(strncmp(s - 1, sc, sizeof(sc)-1) != 0) {
	*errstr = bad_bsd;
	bad_bsd = NULL;
	amfree(remotehost);
	return 0;
    }
    s += sizeof(sc)-1;
    ch = s[-1];
#undef sc

    skip_whitespace(s, ch);
    if(ch == '\0') {
	*errstr = bad_bsd;
	bad_bsd = NULL;
	amfree(remotehost);
	return 0;
    }
    fp = s - 1;
    skip_non_whitespace(s, ch);
    s[-1] = '\0';
    remoteuser = stralloc(fp);
    s[-1] = ch;
    amfree(bad_bsd);

    /* lookup our local user name */

    myuid = getuid();
    if((pwptr = getpwuid(myuid)) == NULL)
        error("error [getpwuid(%d) fails]", myuid);

    localuser = stralloc(pwptr->pw_name);

    dbprintf(("bsd security: remote host %s user %s local user %s\n",
	      remotehost, remoteuser, localuser));

    /*
     * note that some versions of ruserok (eg SunOS 3.2) look in
     * "./.rhosts" rather than "~localuser/.rhosts", so we have to
     * chdir ourselves.  Sigh.
     *
     * And, believe it or not, some ruserok()'s try an initgroup just
     * for the hell of it.  Since we probably aren't root at this point
     * it'll fail, and initgroup "helpfully" will blatt "Setgroups: Not owner"
     * into our stderr output even though the initgroup failure is not a
     * problem and is expected.  Thanks a lot.  Not.
     */
    chdir(pwptr->pw_dir);       /* pamper braindead ruserok's */
#ifndef USE_AMANDAHOSTS
    saved_stderr = dup(2);
    close(2);			/*  " */

#if defined(TEST)
    {
	char *dir = stralloc(pwptr->pw_dir);

	dbprintf(("calling ruserok(%s, %d, %s, %s)\n",
	          remotehost, myuid == 0, remoteuser, localuser));
	if (myuid == 0) {
	    dbprintf(("because you are running as root, "));
	    dbprintf(("/etc/hosts.equiv will not be used\n"));
	} else {
	    show_stat_info("/etc/hosts.equiv", NULL);
	}
	show_stat_info(dir, "/.rhosts");
	amfree(dir);
    }
#endif

    if(ruserok(remotehost, myuid == 0, remoteuser, localuser) == -1) {
	dup2(saved_stderr,2);
	close(saved_stderr);
	*errstr = vstralloc("[",
			    "access as ", localuser, " not allowed",
			    " from ", remoteuser, "@", remotehost,
			    "]", NULL);
	dbprintf(("check failed: %s\n", *errstr));
	amfree(remotehost);
	amfree(localuser);
	amfree(remoteuser);
	return 0;
    }

    dup2(saved_stderr,2);
    close(saved_stderr);
    chdir("/");		/* now go someplace where I can't drop core :-) */
    dbprintf(("bsd security check passed\n"));
    amfree(remotehost);
    amfree(localuser);
    amfree(remoteuser);
    return 1;
#else
    /* We already chdired to ~amandauser */

#if defined(TEST)
    show_stat_info(pwptr->pw_dir, "/.amandahosts");
#endif

    if((fPerm = fopen(".amandahosts", "r")) == NULL) {
#if defined(TEST)
	dbprintf(("fopen failed: %s\n", strerror(errno)));
#endif
	*errstr = vstralloc("[",
			    "access as ", localuser, " not allowed",
			    " from ", remoteuser, "@", remotehost,
			    "]", NULL);
	dbprintf(("check failed: %s\n", *errstr));
	amfree(remotehost);
	amfree(localuser);
	amfree(remoteuser);
	return 0;
    }

    for(; (pbuf = agets(fPerm)) != NULL; free(pbuf)) {
#if defined(TEST)
	dbprintf(("processing line: <%s>\n", pbuf));
#endif
	pbuf_len = strlen(pbuf);
	s = pbuf;
	ch = *s++;

	/* Find end of remote host */
	skip_non_whitespace(s, ch);
	if(s - 1 == pbuf) {
	    memset(pbuf, '\0', pbuf_len);	/* leave no trace */
	    continue;				/* no remotehost field */
	}
	s[-1] = '\0';				/* terminate remotehost field */

	/* Find start of remote user */
	skip_whitespace(s, ch);
	if(ch == '\0') {
	    ptmp = localuser;			/* no remoteuser field */
	} else {
	    ptmp = s-1;				/* start of remoteuser field */

	    /* Find end of remote user */
	    skip_non_whitespace(s, ch);
	    s[-1] = '\0';			/* terminate remoteuser field */
	}
#if defined(TEST)
	dbprintf(("comparing %s with\n", pbuf));
	dbprintf(("          %s (%s)\n",
		  remotehost,
		  (strcasecmp(pbuf, remotehost) == 0) ? "match" : "no match"));
	dbprintf(("      and %s with\n", ptmp));
	dbprintf(("          %s (%s)\n",
		  remoteuser,
		  (strcasecmp(ptmp, remoteuser) == 0) ? "match" : "no match"));
#endif
	if(strcasecmp(pbuf, remotehost) == 0 && strcasecmp(ptmp, remoteuser) == 0) {
	    amandahostsauth = 1;
	    break;
	}
	memset(pbuf, '\0', pbuf_len);		/* leave no trace */
    }
    afclose(fPerm);
    amfree(pbuf);

    if( amandahostsauth ) {
	chdir("/");      /* now go someplace where I can't drop core :-) */
	dbprintf(("amandahosts security check passed\n"));
	amfree(remotehost);
	amfree(localuser);
	amfree(remoteuser);
	return 1;
    }

    *errstr = vstralloc("[",
			"access as ", localuser, " not allowed",
			" from ", remoteuser, "@", remotehost,
			"]", NULL);
    dbprintf(("check failed: %s\n", *errstr));

    amfree(remotehost);
    amfree(localuser);
    amfree(remoteuser);
    return 0;

#endif
}
