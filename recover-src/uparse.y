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
 * $Id: uparse.y,v 1.4 1998/06/01 19:46:50 jrj Exp $
 *
 * parser for amrecover interactive language
 */
%{
#include "amanda.h"
#include "amrecover.h"

void yyerror P((char *s));
extern int yylex P((void));
%}

/* DECLARATIONS */
%union {
  int intval;
  double floatval;
  char *strval;
  int subtok;
}

	/* literal keyword tokens */

%token SETHOST SETDISK SETDATE SETMODE CD QUIT DHIST LS ADD ADDX EXTRACT
%token LIST DELETE DELETEX PWD CLEAR HELP LCD LPWD

        /* typed tokens */

%token <strval> PATH
%token <strval> DATE

/* GRAMMAR */
%%

ucommand:
	set_command
  |     display_command
  |     quit_command
  |     add_command
  |     addx_command
  |     delete_command
  |     deletex_command
  |     local_command
  |	help_command
  |     extract_command
  ;

set_command:
	SETDATE DATE { set_date($2); }
  |     SETHOST PATH { set_host($2); }
  |     SETDISK PATH PATH { set_disk($2, $3); }
  |     SETDISK PATH { set_disk($2, NULL); }
  |     CD PATH { set_directory($2); }
  ;

display_command:
	DHIST { list_disk_history(); }
  |     LS { list_directory(); }
  |     LIST PATH { display_extract_list($2); }
  |     LIST { display_extract_list(NULL); }
  |     PWD { show_directory(); }
  |     CLEAR { clear_extract_list(); }    
  ;

quit_command:
	QUIT { quit(); }
  ;

add_command:
	ADD add_path
  ;

add_path:
	add_path PATH { add_glob($2); }
  |     PATH { add_glob($1); }
  ;

addx_command:
	ADDX addx_path
  ;

addx_path:
	addx_path PATH { add_regex($2); }
  |     PATH { add_regex($1); }
  ;

delete_command:
	DELETE delete_path
  ;

delete_path:
	delete_path PATH { delete_glob($2); }
  |     PATH { delete_glob($1); }
  ;

deletex_command:
	DELETEX deletex_path
  ;

deletex_path:
	deletex_path PATH { delete_regex($2); }
  |     PATH { delete_regex($1); }
  ;

local_command:
	LPWD { char buf[STR_SIZE]; puts(getcwd(buf, sizeof(buf))); }
  |     LCD PATH {
		if (chdir($2) == -1) {
			perror($2);
		}
	}

help_command:
	HELP { help_list(); }
  ;

extract_command:
	EXTRACT { extract_files(); }

/* ADDITIONAL C CODE */
%%

void yyerror(s)
char *s;
{
  printf("Invalid command - %s\n", s);
}
