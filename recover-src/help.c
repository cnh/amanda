/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1991, 1996 University of Maryland at College Park
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
 * $Id: help.c,v 1.3 1998/06/01 19:46:48 jrj Exp $
 *
 * implements the "help" command in amrecover
 */

#include "amrecover.h"

/* print a list of valid commands */
void help_list P((void))
{
    printf("valid commands are:\n\n");

    printf("add path1 ...     - add to extraction list (shell wildcards)\n");
    printf("addx path1 ...    - add to extraction list (regular expressions)\n");
    printf("cd directory      - change cwd on virtual file system\n");
    printf("clear             - clear extraction list\n");
    printf("delete path1 ...  - delete from extraction list (shell wildcards)\n");
    printf("deletex path1 ... - delete from extraction list (regular expressions)\n");
    printf("extract           - extract selected files from tapes\n");
    printf("exit\n");
    printf("help\n");
    printf("history           - show dump history of disk\n");
    printf("list [filename]   - show extraction list, optionally writing to file\n");
    printf("lcd directory     - change cwd on local file system\n");
    printf("ls                - list directory on virtual file system\n");
    printf("lpwd              - show cwd on local file system\n");
    printf("pwd               - show cwd on virtual file system\n");
    printf("quit\n");
    printf("setdate {YYYY-MM-DD|--MM-DD|---DD} - set date of look\n");
    printf("setdisk diskname [mountpoint] - select disk on dump host\n");
    printf("sethost host      - select dump host\n");

    printf("\n");
}
