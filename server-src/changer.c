/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1991,1994 University of Maryland
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
 * $Id: changer.c,v 1.6 1998/01/02 01:05:39 jrj Exp $
 *
 * interface routines for tape changers
 */
#include "amanda.h"
#include "conffile.h"
#include "version.h"

#include "changer.h"

/*
 * If we don't have the new-style wait access functions, use our own,
 * compatible with old-style BSD systems at least.  Note that we don't
 * care about the case w_stopval == WSTOPPED since we don't ask to see
 * stopped processes, so should never get them from wait.
 */
#ifndef WEXITSTATUS
#   define WEXITSTATUS(r)       (((union wait *) &(r))->w_retcode)
#   define WTERMSIG(r)          (((union wait *) &(r))->w_termsig)

#   undef  WIFSIGNALED
#   define WIFSIGNALED(r)       (((union wait *) &(r))->w_termsig != 0)
#endif


char *changer_resultstr = NULL;

static char *tapechanger = NULL;

/* local functions */
static int changer_command P((char *cmdstr));

int changer_init()
{
    tapechanger = getconf_str(CNF_TPCHANGER);
    return strcmp(tapechanger, "") != 0;
}


static int report_bad_resultstr()
{
    char *s;

    s = vstralloc("badly formed result from changer: ",
		  "\"", changer_resultstr, "\"",
		  NULL);
    afree(changer_resultstr);
    changer_resultstr = s;
    return 2;
}

static int run_changer_command(cmd, arg, slotstr, rest)
char *cmd;
char *arg;
char **slotstr;
char **rest;
{
    int exitcode, rc;
    char *changer_cmd = NULL;
    char *result_copy;
    char *slot;
    char *s;
    int ch;

    *slotstr = NULL;
    *rest = NULL;
    if(arg) {
	changer_cmd = vstralloc(cmd, " ", arg, NULL);
    } else {
	changer_cmd = cmd;
    }
    exitcode = changer_command(changer_cmd);
    if(changer_cmd != cmd) {
	afree(changer_cmd);
    }
    s = changer_resultstr;
    ch = *s++;

    skip_whitespace(s, ch);
    if(ch == '\0') return report_bad_resultstr();
    slot = s - 1;
    skip_non_whitespace(s, ch);
    s[-1] = '\0';
    *slotstr = stralloc(slot);
    s[-1] = ch;

    skip_whitespace(s, ch);
    *rest = s - 1;

    if(exitcode) {
	if(ch == '\0') return report_bad_resultstr();
	result_copy = s - 1;
	changer_resultstr = newstralloc(changer_resultstr, result_copy);
	return exitcode;
    }
    return 0;
}

int changer_reset(slotstr)
char **slotstr;
{
    char *rest;

    return run_changer_command("-reset", NULL, slotstr, &rest);
}

int changer_eject(slotstr)
char **slotstr;
{
    char *rest;

    return run_changer_command("-eject", NULL, slotstr, &rest);
}

int changer_loadslot(inslotstr, outslotstr, devicename)
char *inslotstr, **outslotstr, **devicename;
{
    char *rest;
    int rc;

    rc = run_changer_command("-slot", inslotstr, outslotstr, &rest);
    if(rc) return rc;

    if(*rest == '\0') return report_bad_resultstr();

    *devicename = stralloc(rest);
    return 0;
}

int changer_info(nslotsp, curslotstr, backwardsp)
int *nslotsp, *backwardsp;
char **curslotstr;
{
    char *rest;
    int rc;

    rc = run_changer_command("-info", NULL, curslotstr, &rest);
    if(rc) return rc;

    if (sscanf(rest, "%d %d", nslotsp, backwardsp) != 2) {
	return report_bad_resultstr();
    }
    return 0;
}


/* ---------------------------- */

void changer_scan(user_init, user_slot)
int (*user_init) P((int rc, int nslots, int backwards));
int (*user_slot) P((int rc, char *slotstr, char *device));
{
    char *slotstr, *device = NULL, *curslotstr = NULL;
    int nslots, checked, backwards, rc, done;

    rc = changer_info(&nslots, &curslotstr, &backwards);
    done = user_init(rc, nslots, backwards);

    slotstr = "current";
    checked = 0;

    for(; !done && checked < nslots; free(curslotstr)) {
	rc = changer_loadslot(slotstr, &curslotstr, &device);
	if(rc > 0)
	    done = user_slot(rc, curslotstr, device);
	else if(!done)
	    done = user_slot(0,  curslotstr, device);
	afree(device);

	checked += 1;
	slotstr = "next";
    }
}


/* ---------------------------- */

void changer_current(user_init, user_slot)
int (*user_init) P((int rc, int nslots, int backwards));
int (*user_slot) P((int rc, char *slotstr, char *device));
{
    char *slotstr, *device = NULL, *curslotstr = NULL;
    int nslots, checked, backwards, rc, done;

    rc = changer_info(&nslots, &curslotstr, &backwards);
    done = user_init(rc, nslots, backwards);

    slotstr = "current";
    checked = 0;

    rc = changer_loadslot(slotstr, &curslotstr, &device);
    if(rc > 0) {
	done = user_slot(rc, curslotstr, device);
    } else if(!done) {
	done = user_slot(0,  curslotstr, device);
    }
    afree(curslotstr);
    afree(device);
}

/* ---------------------------- */

static int changer_command(cmdstr)
char *cmdstr;
{
    FILE *cmdpipe;
    char *cmd = NULL;
    int exitcode;
    int len;
    int ch;
    char number[NUM_STR_SIZE];

    if (*tapechanger != '/') {
	cmd = vstralloc(libexecdir, "/", tapechanger, versionsuffix(),
			" ", cmdstr,
			NULL);
    } else {
	cmd = vstralloc(tapechanger, " ", cmdstr, NULL);
    }

/* fprintf(stderr, "changer: opening pipe from: %s\n", cmd); */

    if((cmdpipe = popen(cmd, "r")) == NULL)
	error("could not open pipe to \"%s\": %s", cmd, strerror(errno));

    afree(changer_resultstr);
    if((changer_resultstr = agets(cmdpipe)) == NULL) {
	error("could not read result from \"%s\": %s", cmd, strerror(errno));
    }

    exitcode = pclose(cmdpipe);
    cmdpipe = NULL;
    /* mark out-of-control changers as fatal error */
    if(WIFSIGNALED(exitcode)) {
	ap_snprintf(number, sizeof(number), "%d", WTERMSIG(exitcode));
	cmd = newvstralloc(cmd,
			   changer_resultstr,
			   " (got signal ", number, ")",
			   NULL);
	afree(changer_resultstr);
	changer_resultstr = cmd;
	cmd = NULL;
	exitcode = 2;
    } else {
	exitcode = WEXITSTATUS(exitcode);
    }

/* fprintf(stderr, "changer: got exit: %d str: %s\n", exitcode, resultstr); */

    afree(cmd);
    return exitcode;
}
