/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1991,1993 University of Maryland
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
 * $Id: amlabel.c,v 1.13 1998/04/08 16:25:04 amcore Exp $
 *
 * write an Amanda label on a tape
 */
#include "amanda.h"
#include "conffile.h"
#include "tapefile.h"
#include "tapeio.h"
#include "changer.h"
#ifdef HAVE_LIBVTBLC
#include <vtblc.h>

static int vtbl_no      = -1;
static char *datestr    = NULL;
#endif /* HAVE_LIBVTBLC */

char *pname = "amlabel";

int slotcommand;

/* local functions */

void usage P((char *argv0));

void usage(argv0)
char *argv0;
{
    fprintf(stderr, "Usage: %s [-f] <conf> <label> [slot <slot-number>]\n",
	    argv0);
    exit(1);
}

int main(argc, argv)
int argc;
char **argv;
{
    char *confdir, *outslot = NULL;
    char *errstr, *confname, *label, *oldlabel=NULL, *tapename = NULL;
    char *labelstr, *slotstr;
    char *olddatestamp=NULL, *tapefilename;
    unsigned long malloc_hist_1, malloc_size_1;
    unsigned long malloc_hist_2, malloc_size_2;
    int fd;
    int force, tape_ok;
    tape_t *tp;
#ifdef HAVE_LIBVTBLC
    char *rawtapedev = NULL;
    int first_seg, last_seg;
#endif /* HAVE_LIBVTBLC */

    for(fd = 3; fd < FD_SETSIZE; fd++) {
	/*
	 * Make sure nobody spoofs us with a lot of extra open files
	 * that would cause an open we do to get a very high file
	 * descriptor, which in turn might be used as an index into
	 * an array (e.g. an fd_set).
	 */
	close(fd);
    }

    set_pname("amlabel");

    malloc_size_1 = malloc_inuse(&malloc_hist_1);

    erroutput_type = ERR_INTERACTIVE;

    if(strcmp(argv[1],"-f"))
	 force=0;
    else force=1;

    if(argc != 3+force && argc != 5+force)
	usage(argv[0]);

    confname = argv[1+force];
    label = argv[2+force];

    if(argc == 5+force) {
	if(strcmp(argv[3+force], "slot"))
	    usage(argv[0]);
	slotstr = argv[4+force];
	slotcommand = 1;
    }
    else {
	slotstr = "current";
	slotcommand = 0;
    }

    confdir = vstralloc(CONFIG_DIR, "/", confname, NULL);
    if(chdir(confdir) != 0)
	error("could not cd to confdir %s: %s", confdir, strerror(errno));

    if(read_conffile(CONFFILE_NAME))
	error("could not read amanda config file");

    labelstr = getconf_str(CNF_LABELSTR);

    tapefilename = getconf_str(CNF_TAPELIST);
    if(read_tapelist(tapefilename))
	error("parse error in %s", tapefilename);

    if(!match(labelstr, label))
	error("label %s doesn't match labelstr \"%s\"", label, labelstr);

    if((tp = lookup_tapelabel(label))!=NULL) {
	if(!force)
	    error("label %s already on a tape\n",label);
    }

    if(!changer_init()) {
	if(slotcommand) {
	    fprintf(stderr,
	     "%s: no tpchanger specified in %s/%s, so slot command invalid\n",
		    argv[0], confdir, CONFFILE_NAME);
	    usage(argv[0]);
	}
	tapename = stralloc(getconf_str(CNF_TAPEDEV));
#ifdef HAVE_LIBVTBLC
	rawtapedev = stralloc(getconf_str(CNF_RAWTAPEDEV));
#endif /* HAVE_LIBVTBLC */
    }
    else {
	if(changer_loadslot(slotstr, &outslot, &tapename)) {
	    error("could not load slot \"%s\": %s", slotstr, changer_resultstr);
	}

	printf("labeling tape in slot %s (%s):\n", outslot, tapename);
    }

#ifdef HAVE_LINUX_ZFTAPE_H
    if (is_zftape(tapename) == 1){
	if((fd = tape_open(tapename, O_WRONLY)) == -1) {
	    errstr = newstralloc2(errstr, "amlabel: ",
				  (errno == EACCES) ? "tape is write-protected"
				  : strerror(errno));
	    error(errstr);
	}
    }
#endif /* HAVE_LINUX_ZFTAPE_H */

    printf("rewinding"); fflush(stdout);

#ifdef HAVE_LINUX_ZFTAPE_H
    if (is_zftape(tapename) == 1){
	if(tapefd_rewind(fd) == -1) {
	    putchar('\n');
	    error(strerror(errno));
	}
    }
    else
#endif /* HAVE_LINUX_ZFTAPE_H */
    if((errstr = tape_rewind(tapename)) != NULL) {
	putchar('\n');
	error(errstr);
    }

    tape_ok=1;
    printf(", reading label");fflush(stdout);
    if((errstr = tape_rdlabel(tapename, &olddatestamp, &oldlabel)) != NULL) {
	printf(", %s\n",errstr);
	tape_ok=1;
    }
    else {
	/* got an amanda tape */
	printf(" %s",oldlabel);
	if(!match(labelstr, oldlabel)) {
	    printf(", tape is in another amanda configuration");
	    if(!force)
		tape_ok=0;
	}
	else {
	    if((tp = lookup_tapelabel(oldlabel)) != NULL) {
		printf(", tape is active");
		if(!force)
		    tape_ok=0;
	    }
	}
	printf("\n");
    }
	
    printf("rewinding"); fflush(stdout);

#ifdef HAVE_LINUX_ZFTAPE_H
    if (is_zftape(tapename) == 1){
	if(tapefd_rewind(fd) == -1) {
	    putchar('\n');
	    error(strerror(errno));
	}
    }
    else
#endif /* HAVE_LINUX_ZFTAPE_H */
    if((errstr = tape_rewind(tapename)) != NULL) {
	putchar('\n');
	error(errstr);
    }

    if(tape_ok) {
	printf(", writing label %s", label); fflush(stdout);

#ifdef HAVE_LINUX_ZFTAPE_H
	if (is_zftape(tapename) == 1){
	    if((errstr = tapefd_wrlabel(fd, "X", label)) != NULL) {
		putchar('\n');
		error(errstr);
	    }
	}
	else
#endif /* HAVE_LINUX_ZFTAPE_H */
	if((errstr = tape_wrlabel(tapename, "X", label)) != NULL) {
	    putchar('\n');
	    error(errstr);
	}

#ifdef HAVE_LINUX_ZFTAPE_H
	if (is_zftape(tapename) == 1){
	    tapefd_weof(fd, 1);
	}
#endif /* HAVE_LINUX_ZFTAPE_H */

#ifdef HAVE_LINUX_ZFTAPE_H
	if (is_zftape(tapename) == 1){
	    if((errstr = tapefd_wrendmark(fd, "X")) != NULL) {
		putchar('\n');
		error(errstr);
	    }
	}
	else
#endif /* HAVE_LINUX_ZFTAPE_H */
	if((errstr = tape_wrendmark(tapename, "X")) != NULL) {
	    putchar('\n');
	    error(errstr);
	}

#ifdef HAVE_LINUX_ZFTAPE_H
	if (is_zftape(tapename) == 1){
	    tapefd_weof(fd, 1);

	    printf(",\nrewinding"); fflush(stdout); 
     
	    if(tapefd_rewind(fd) == -1) { 
		putchar('\n'); 
		error(strerror(errno)); 
	    } 
	    close(fd);
#ifdef HAVE_LIBVTBLC
	    /* update volume table */
	    printf(", updating volume table"); fflush(stdout);
    
	    if ((fd = raw_tape_open(rawtapedev, O_RDWR)) == -1) {
		if(errno == EACCES) {
		    errstr = newstralloc(errstr,
					 "updating volume table: raw tape device is write protected");
		} else {
		    errstr = newstralloc2(errstr,
					  "updating volume table: ", strerror(errno));
		}
		putchar('\n');
		error(errstr);
	    }
	    /* read volume table */
	    if ((num_volumes = read_vtbl(fd, volumes, vtbl_buffer,
					 &first_seg, &last_seg)) == -1 ) {
		errstr = newstralloc2(errstr,
				      "reading volume table: ", strerror(errno));
		putchar('\n');
		error(errstr);
	    }
	    /* set date and volume label for first entry */
	    vtbl_no = 0;
	    datestr = NULL; 
	    if (set_date(datestr, volumes, num_volumes, vtbl_no)){
		errstr = newstralloc2(errstr,
				      "setting date for entry 1: ", strerror(errno));
		putchar('\n');
		error(errstr);
	    }
	    if(set_label(label, volumes, num_volumes, vtbl_no)){
		errstr = newstralloc2(errstr,
				      "setting label for entry 1: ", strerror(errno));
		putchar('\n');
		error(errstr);
	    }
	    /* set date and volume label for last entry */
	    vtbl_no = 1;
	    datestr = NULL; 
	    if (set_date(datestr, volumes, num_volumes, vtbl_no)){
		errstr = newstralloc2(errstr,
				      "setting date for entry 2: ", strerror(errno));
		putchar('\n');
		error(errstr);
	    }
	    if(set_label("AMANDA Tape End", volumes, num_volumes, vtbl_no)){
		errstr = newstralloc2(errstr,
				      "setting label for entry 2: ", strerror(errno));
		putchar('\n');
		error(errstr);
	    }
	    /* write volume table back */
	    if (write_vtbl(fd, volumes, vtbl_buffer, num_volumes, first_seg,
			   op_mode == trunc)) {
		errstr = newstralloc2(errstr,
				      "writing volume table: ", strerror(errno));
		putchar('\n');
		error(errstr);
	    }  
	    close(fd);
#endif /* HAVE_LIBVTBLC */
	}
#endif /* HAVE_LINUX_ZFTAPE_H */

	printf(", done.\n");
    }
    else {
	printf("\ntape not labeled\n");
    }

    amfree(outslot);
    amfree(tapename);
    amfree(confdir);

    malloc_size_2 = malloc_inuse(&malloc_hist_2);

    if(malloc_size_1 != malloc_size_2) {
	malloc_list(fileno(stderr), malloc_hist_1, malloc_hist_2);
    }

    return 0;
}
