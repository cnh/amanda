/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1997 University of Maryland
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
 * Author: George Scott, Computer Centre, Monash University.
 */
/*
 * $Id: token.c,v 1.14 1997/12/30 05:24:27 jrj Exp $
 *
 * token bashing routines
 */

/*
** The quoting method used here was selected because it has the
** property that quoting a string that doesn't contain funny
** characters results in an unchanged string and it was easy to code.
** There are probably other algorithms that are just as effective.
**/

#include "amanda.h"
#include "arglist.h"
#include "token.h"

/* Split a string up into tokens.
** There is exactly one separator character between tokens.
** XXX Won't work too well if separator is '\'!
**
** Inspired by awk and a routine called splitter() that I snarfed from
** the net ages ago (original author long forgotten).
*/
int split(str, token, toklen, sep)
char *str;	/* String to split */
char **token;	/* Array of token pointers */
int toklen;	/* Size of token[] */
char *sep;	/* Token separators - usually " " */
{
    register char *pi, *po;
    register int fld;
    register int len;
    static char *buf = (char *)0; /* XXX - static buffer */
    int in_quotes;

    assert(str && token && toklen > 0 && sep);

    token[0] = str;

    for (fld = 1; fld < toklen; fld++) token[fld] = (char *)0;

    fld = 0;

    if (*sep == '\0' || *str == '\0' || toklen == 1) return fld;

    /* Calculate the length of the unquoted string. */

    len = 0;
    for (pi = str; *pi && *pi != '\n'; pi++) {
	switch(*pi) {
	case '\\':	/* had better not be trailing... */
	    pi++;
	    if (*pi >= '0' && *pi <= '3') pi = pi + 2;
	    len++;
	    break;
	case '"':	/* just ignore "'s */
	    break;
	default:
	    len++;
	}
    }

    /* Allocate some space */

    buf = newalloc(buf, len+1);

    /* Copy it across and tokenise it */

    in_quotes = 0;
    po = buf;
    token[++fld] = po;
    for (pi = str; *pi && *pi != '\n'; pi++) {
	if (*pi == '\\') {	/* escape */
	    pi++;
	    if (*pi >= '0' && *pi <= '3') {
		*po =       ((*pi++ - '0') << 6);
		*po = *po + ((*pi++ - '0') << 3);
		*po = *po + ((*pi   - '0')     );
	    }
	    else *po = *pi;
	    po++;
	}
	else if (*pi == '"') {	/* quotes */
	    in_quotes = !in_quotes;
	}
	else if (!in_quotes && strchr(sep, *pi)) {	/* separator */
	    *po = '\0';	/* end of token */
	    if (fld+1 >= toklen) return fld; /* too many tokens */
	    token[++fld] = po + 1;
	    po++;
	}
	else {
	    *po++ = *pi;	/* normal */
	}
    }
    *po = '\0';

    assert(po == buf + len);	/* Just checking! */

    return fld;
}

/*
** Quote all the funny characters in one token.
** - squotef - formatted string with space separator
** - quotef  - formatted string with specified separators
** - squote  - fixed string with space separator
** - quote   - fixed strings with specified separators
**/
arglist_function(char *squotef, char *, format)
{
	va_list argp;
	char linebuf[16384];

	/* Format the token */

	arglist_start(argp, format);
	ap_vsnprintf(linebuf, sizeof(linebuf), format, argp);
	arglist_end(argp);

	return quote(" ", linebuf);
}

arglist_function1(char *quotef, char *, sep, char *, format)
{
	va_list argp;
	char linebuf[16384];

	/* Format the token */

	arglist_start(argp, format);
	ap_vsnprintf(linebuf, sizeof(linebuf), format, argp);
	arglist_end(argp);

	return quote(sep, linebuf);
}

char *squote(str)
char *str;	/* the string to quote */
{
	return quote(" ", str);
}

char *quote(sepchr, str)
char *sepchr;	/* separators that also need quoting */
char *str;	/* the string to quote */
{
    register char *pi, *po;
    register int len;
    static char *buf = (char *)0; /* XXX static buffer */
    int sep, need_quotes;

    /* Calculate the length of the quoted token. */

    len = sep = 0;
    for (pi = str; *pi; pi++) {
	if (*pi < ' ' || *pi > '~')
	    len = len + 4;
	else if (*pi == '\\' || *pi == '"')
	    len = len + 2;
	else if (*sepchr && strchr(sepchr, *pi)) {
	    len = len + 1;
	    sep++;
	}
	else
	    len++;
    }

    need_quotes = (sep != 0);

    if (need_quotes) len = len + 2;

    /* Allocate some space */

    buf = newalloc(buf, len+1);	/* trailing null */

    /* Copy it across */

    po = buf;

    if (need_quotes) *po++ = '"';

    for (pi = str; *pi; pi++) {
	if (*pi < ' ' || *pi > '~') {
	    *po++ = '\\';
	    *po++ = ((*pi >> 6) & 07) + '0';
	    *po++ = ((*pi >> 3) & 07) + '0';
	    *po++ = ((*pi     ) & 07) + '0';
	}
	else if (*pi == '\\' || *pi == '"') {
	    *po++ = '\\';
	    *po++ = *pi;
	}
	else *po++ = *pi;
    }

    if (need_quotes) *po++ = '"';

    *po = '\0';

    assert(po - buf == len);	/* Just checking! */

    return buf;
}

/* Table lookup.
*/
int table_lookup(table, str)
table_t *table;
char *str;
{
	while(table->word != (char *)0) {
		if (*table->word == *str &&
		    strcmp(table->word, str) == 0) return table->value;
		table++;
	}

	return table->value;
}

/* Reverse table lookup.
*/
char *table_lookup_r(table, val)
table_t *table;
int val;
{
	while(table->word != (char *)0) {
		if (table->value == val) return table->word;
		table++;
	}

	return (char *)0;
}

#ifdef TEST

char *pname = "token test";

int main()
{
	char *str = NULL;
	char *t[20];
	int r;
	char *sr;
	int i;

	erroutput_type = ERR_INTERACTIVE;

	printf("Testing split() with \" \" token separator\n");
	while(1) {
		printf("Input string: ");
		afree(str);
		if ((str = agets(stdin)) == NULL) {
			printf("\n");
			break;
		}
		r = split(str, t, 20, " ");
		printf("%d tokens:\n", r);
		for (i=0; i <= r; i++) printf("tok[%d] = \"%s\"\n", i, t[i]);
	}
	afree(str);
	printf("\n");

	printf("Testing quote()\n");
	while(1) {
		printf("Input string: ");
		afree(str);
		if ((str = agets(stdin)) == NULL) {
			printf("\n");
			break;
		}
		sr = squote(str);
		printf("Quoted   = \"%s\"\n", sr);
		strncpy(str,sr,sizeof(str)-1);
		str[sizeof(str)-1] = '\0';
		r = split(str, t, 20, " ");
		if (r != 1) printf("split()=%d!\n", r);
		printf("Unquoted = \"%s\"\n", t[1]);
	}
	afree(str);
}

#endif
