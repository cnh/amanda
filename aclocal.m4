dnl aclocal.m4 generated automatically by aclocal 1.1o

dnl Check for the argument type for shmat() and shmdt()
AC_DEFUN(AMANDA_FUNC_SHM_ARG_TYPE,
    [
	AC_CACHE_CHECK(
	    [for shmdt() argument type],
	    amanda_cv_shmdt_arg_type,
	    [
		if test "$ac_cv_func_shmget" = yes; then
		    cat <<EOF >conftest.$ac_ext
			#include "confdefs.h"
			#ifdef HAVE_SYS_TYPES_H
			# include <sys/types.h>
			#endif
			#ifdef HAVE_SYS_IPC_H
			# include <sys/ipc.h>
			#endif
			#ifdef HAVE_SYS_SHM_H
			# include <sys/shm.h>
			#endif

			#ifdef __cplusplus
			extern "C" void *shmat(int, void *, int);
			#else
			void *shmat();
			#endif

			int main()
			{
			    int i;
			    return 0;
			}
EOF
		    ${CC-cc} -c $CFLAGS $CPPFLAGS conftest.$ac_ext >/dev/null 2>/dev/null
		    if test $? = 0; then
			amanda_cv_shmdt_arg_type=void
		    else
			amanda_cv_shmdt_arg_type=char
		    fi
		    rm -f conftest*
		else
		    amanda_cv_shmdt_arg_type=nothing
		fi
	    ]
	)
	AC_DEFINE_UNQUOTED(SHM_ARG_TYPE,$amanda_cv_shmdt_arg_type)
    ]
)

dnl Figure out the select() argument type.
AC_DEFUN(AMANDA_FUNC_SELECT_ARG_TYPE,
    [
	AC_CACHE_CHECK(
	    [for select() argument type],
	    amanda_cv_select_arg_type,
	    [
		rm -f conftest.c
		cat <<EOF >conftest.$ac_ext
		    #include "confdefs.h"
		    #ifdef HAVE_SYS_TIME_H
		    # include <sys/time.h>
		    #endif
		    #ifdef HAVE_SYS_TYPES_H
		    # include <sys/types.h>
		    #endif
		    #ifdef HAVE_SYS_SELECT_H
		    #  include <sys/select.h>
		    #endif
		    #ifdef HAVE_SYS_SOCKET_H
		    #  include <sys/socket.h>
		    #endif
		    #ifdef HAVE_UNISTD_H
		    # include <unistd.h>
		    #endif

		    int main()
		    {
		    #ifdef FD_SET_POINTER
			(void)select(0, (fd_set *) 0, (fd_set *) 0, (fd_set *) 0, 0);
		    #else
			(void)select(0, (int *) 0, (int *) 0, (int *) 0, 0);
		    #endif
			return 0;
		    }
EOF

		dnl Figure out the select argument type by first trying to
		dnl compile with the fd_set argument.  If the compile fails,
		dnl then we know to use the int.  If it suceeds, then try to
		dnl use the int.  If the int fails, then use fd_set.  If
		dnl both suceeed, then do a line count on the number of
		dnl lines that the compiler spit out, assuming that the
		dnl compile outputing more lines had more errors.
		amanda_cv_select_arg_type=no
		select_compile="${CC-cc} -c $CFLAGS $CPPFLAGS"
		$select_compile -DFD_SET_POINTER conftest.$ac_ext 1>conftest.fd_set 2>&1
		if test $? -ne 0; then
		    amanda_cv_select_arg_type=int
		fi
		if test "$amanda_cv_select_arg_type" = no; then
		    $select_compile conftest.$ac_ext 1>conftest.int 2>&1
		    if test $? -ne 0; then
			amanda_cv_select_arg_type=fd_set
		    fi
		fi
		if test "$amanda_cv_select_arg_type" = no; then
		    wc_fdset=`wc -l <conftest.fd_set`
		    wc_int=`wc -l <conftest.int`
		    if test "$wc_fdset" -le "$wc_int"; then
			amanda_cv_select_arg_type=fd_set
		    else
			amanda_cv_select_arg_type=int
		    fi
		fi
		rm -f conftest*
	    ]
	)
	AC_DEFINE_UNQUOTED(SELECT_ARG_TYPE,$amanda_cv_select_arg_type)
    ]
)

dnl Check if setsockopt can use the SO_SNDTIMEO option.
dnl This defines HAVE_SO_SNDTIMEO if setsockopt works
dnl with SO_SNDTIMEO.
AC_DEFUN(AMANDA_FUNC_SETSOCKOPT_SO_SNDTIMEO,
    [
	AC_CACHE_CHECK(
	    [for setsockopt SO_SNDTIMEO option],
	    amanda_cv_setsockopt_SO_SNDTIMEO,
	    [
		AC_TRY_RUN(
		    [
			#include <sys/types.h>
			#include <sys/socket.h>
			#ifdef TIME_WITH_SYS_TIME
			#  include <sys/time.h>
			#  include <time.h>
			#else
			#  ifdef HAVE_SYS_TIME_H
			#    include <sys/time.h>
			#  else
			#    include <time.h>
			#  endif
			#endif

			main() {
			#ifdef SO_SNDTIMEO
			    int sock = socket(AF_INET, SOCK_STREAM, 0);
			    struct timeval timeout;
			    timeout.tv_sec = 1;
			    timeout.tv_usec = 0;
			    return (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
		                     (void *)&timeout, sizeof(timeout)));
			#else
			    return -1;
			#endif
			}
		    ],
		    amanda_cv_setsockopt_SO_SNDTIMEO=yes,
		    amanda_cv_setsockopt_SO_SNDTIMEO=no,
		    amanda_cv_setsockopt_SO_SNDTIMEO=no
		)
	    ]
	)
	if test "$amanda_cv_setsockopt_SO_SNDTIMEO" = yes; then
	    AC_DEFINE(HAVE_SO_SNDTIMEO)
	fi
    ]
)

dnl Find out how {awk,gawk,nawk,mawk} likes to assign variables, if it
dnl can do so at all.
AC_DEFUN(AMANDA_PROG_AWK_VAR,
    [
	AC_REQUIRE([AC_PROG_AWK])
	AC_CACHE_CHECK(
	    [for $AWK command line variable assignment],
	    amanda_cv_awk_var_assignment,
	    [
		echo 'BEGIN{print i; exit}' > conftest.awk
		result=`$AWK -f conftest.awk i=xx | wc -c`
		if test "$result" -le 1; then
		    result=`$AWK -f conftest.awk -v i=xx | wc -c`
		    if test "$result" -le 1; then
			amanda_cv_awk_var_assignment=no
		    else
			amanda_cv_awk_var_assignment="yes with -v"
		    fi
		else
		    amanda_cv_awk_var_assignment="yes"
		fi
		rm -fr conftest.awk
	    ]
	)
	AWK_VAR_ASSIGNMENT_OPT=
	case "$amanda_cv_awk_var_assignment" in
	    no)
		HAVE_AWK_WITH_VAR=no
		;;
	    yes)
		HAVE_AWK_WITH_VAR=yes
		;;
	    "yes with -v")
		HAVE_AWK_WITH_VAR=yes
		AWK_VAR_ASSIGNMENT_OPT=-v
		;;
	esac
	AC_SUBST(AWK_VAR_ASSIGNMENT_OPT)
    ]
)	

dnl Check for the one or two argument version of gettimeofday.
AC_DEFUN(AMANDA_FUNC_GETTIMEOFDAY_ARGS,
    [
	AC_REQUIRE([AC_HEADER_TIME])
	AC_CACHE_CHECK(
	    [for gettimeofday number of arguments],
	    amanda_cv_gettimeofday_args,
	    [
		AC_TRY_COMPILE(
		    [
			#ifdef TIME_WITH_SYS_TIME
			#  include <sys/time.h>
			#  include <time.h>
			#else
			#  ifdef HAVE_SYS_TIME_H
			#    include <sys/time.h>
			#  else
			#    include <time.h>
			#  endif
			#endif
		    ],
		    [
			struct timeval val;
			struct timezone zone;
			gettimeofday(&val, &zone);
		    ],
		    amanda_cv_gettimeofday_args=2,
		    amanda_cv_gettimeofday_args=1
		)
	    ]
	)
	if test "$amanda_cv_gettimeofday_args" = 2; then
	    AC_DEFINE(HAVE_TWO_ARG_GETTIMEOFDAY)
	fi
    ]
)



dnl Check if the compiler understands volatile.
AC_DEFUN(AMANDA_C_VOLATILE,
    [
	AC_CACHE_CHECK(
	    [for working volatile],
	    amanda_cv_c_volatile,
	    [
		AC_TRY_COMPILE(,
		    [
			volatile int aaa = 0;
		    ],
		    amanda_cv_c_volatile=yes,
		    amanda_cv_c_volatile=no
		)
	    ]
	)
	if test $amanda_cv_c_volatile = no; then
	    AC_DEFINE(volatile, )
	fi
    ]
)


dnl Check for if pid_t is a long, int, or short.
AC_DEFUN(AMANDA_TYPE_PID_T,
    [
	AC_REQUIRE([AC_TYPE_PID_T])
	AC_CACHE_CHECK([for pid_t type], amanda_cv_pid_type,
	    [
		amanda_cv_pid_type=unknown
		if test "$ac_cv_type_pid_t" = no; then
		    amanda_cv_pid_type=int
		fi
		for TEST_amanda_cv_pid_type in long short int; do
		    if test $amanda_cv_pid_type = unknown; then
			AC_EGREP_CPP(typedef.*${TEST_amanda_cv_pid_type}.*pid_t,
			    [
				#include <sys/types.h>
				#if STDC_HEADERS
				#include <stdlib.h>
				#include <stddef.h>
				#endif
			    ],
			amanda_cv_pid_type=$TEST_amanda_cv_pid_type)
		    fi
		    if test $amanda_cv_pid_type = unknown; then
			AC_EGREP_CPP(ZZZZ.*${TEST_amanda_cv_pid_type},
			    [
				#include <sys/types.h>
				#if STDC_HEADERS
				#include <stdlib.h>
				#include <stddef.h>
				#endif
				ZZZZ pid_t
			],
			amanda_cv_pid_type=$TEST_amanda_cv_pid_type)
		    fi
		done
		if test $amanda_cv_pid_type = unknown; then
		    amanda_cv_pid_type=int
		fi
	    ]
	)
	case $amanda_cv_pid_type in
	    int)	AC_DEFINE_UNQUOTED(PRINTF_PID_T,"%d") ;;
	    long)	AC_DEFINE_UNQUOTED(PRINTF_PID_T,"%ld") ;;
	    short)	AC_DEFINE_UNQUOTED(PRINTF_PID_T,"%d") ;;
	esac
    ]
)

dnl
dnl
dnl ICE_CHECK_DECL (FUNCTION, HEADER-FILE...)
dnl If FUNCTION is available, define `HAVE_FUNCTION'.  If it is declared
dnl in one of the headers named in the whitespace-separated list 
dnl HEADER_FILE, define `HAVE_FUNCTION_DECL` (in all capitals).
dnl
AC_DEFUN(ICE_CHECK_DECL,
[
changequote(,)dnl
ice_tr=`echo $1 | tr '[a-z]' '[A-Z]'`
changequote([,])dnl
ice_have_tr=HAVE_$ice_tr
ice_have_decl_tr=${ice_have_tr}_DECL
ice_have_$1=no
AC_CHECK_FUNCS($1, ice_have_$1=yes)
if test "${ice_have_$1}" = yes; then
AC_MSG_CHECKING(for $1 declaration in $2)
AC_CACHE_VAL(ice_cv_have_$1_decl,
[
ice_cv_have_$1_decl=no
changequote(,)dnl
ice_re_params='[a-zA-Z_][a-zA-Z0-9_]*'
ice_re_word='(^|[^a-zA-Z0-9_])'
changequote([,])dnl
for header in $2; do
# Check for ordinary declaration
AC_EGREP_HEADER([${ice_re_word}$1[ 	]*\(], $header, 
	ice_cv_have_$1_decl=yes)
if test "$ice_cv_have_$1_decl" = yes; then
	break
fi
# Check for "fixed" declaration like "getpid _PARAMS((int))"
AC_EGREP_HEADER([${ice_re_word}$1[ 	]*$ice_re_params\(\(], $header, 
	ice_cv_have_$1_decl=yes)
if test "$ice_cv_have_$1_decl" = yes; then
	break
fi
done
])
AC_MSG_RESULT($ice_cv_have_$1_decl)
if test "$ice_cv_have_$1_decl" = yes; then
AC_DEFINE_UNQUOTED(${ice_have_decl_tr})
fi
fi
])dnl

# Do all the work for Automake.  This macro actually does too much --
# some checks are only needed if your package does certain things.
# But this isn't really a big deal.

# serial 1

dnl Usage:
dnl AM_INIT_AUTOMAKE(package,version, [no-define])

AC_DEFUN(AM_INIT_AUTOMAKE,
[AC_REQUIRE([AM_PROG_INSTALL])
PACKAGE=[$1]
AC_SUBST(PACKAGE)
VERSION=[$2]
AC_SUBST(VERSION)
ifelse([$3],,
AC_DEFINE_UNQUOTED(PACKAGE, "$PACKAGE")
AC_DEFINE_UNQUOTED(VERSION, "$VERSION"))
AM_SANITY_CHECK
AC_ARG_PROGRAM
dnl FIXME This is truly gross.
missing_dir=`cd $ac_aux_dir && pwd`
AM_MISSING_PROG(ACLOCAL, aclocal, $missing_dir)
AM_MISSING_PROG(AUTOCONF, autoconf, $missing_dir)
AM_MISSING_PROG(AUTOMAKE, automake, $missing_dir)
AM_MISSING_PROG(AUTOHEADER, autoheader, $missing_dir)
AM_MISSING_PROG(MAKEINFO, makeinfo, $missing_dir)
AC_PROG_MAKE_SET])


# serial 1

AC_DEFUN(AM_PROG_INSTALL,
[AC_REQUIRE([AC_PROG_INSTALL])
test -z "$INSTALL_SCRIPT" && INSTALL_SCRIPT='${INSTALL_PROGRAM}'
AC_SUBST(INSTALL_SCRIPT)dnl
])

#
# Check to make sure that the build environment is sane.
#

AC_DEFUN(AM_SANITY_CHECK,
[AC_MSG_CHECKING([whether build environment is sane])
# Just in case
sleep 1
echo timestamp > conftestfile
# Do `set' in a subshell so we don't clobber the current shell's
# arguments.  Must try -L first in case configure is actually a
# symlink; some systems play weird games with the mod time of symlinks
# (eg FreeBSD returns the mod time of the symlink's containing
# directory).
if (
   set X `ls -Lt $srcdir/configure conftestfile 2> /dev/null`
   if test "$@" = "X"; then
      # -L didn't work.
      set X `ls -t $srcdir/configure conftestfile`
   fi
   test "[$]2" = conftestfile
   )
then
   # Ok.
   :
else
   AC_MSG_ERROR([newly created file is older than distributed files!
Check your system clock])
fi
rm -f conftest*
AC_MSG_RESULT(yes)])

dnl AM_MISSING_PROG(NAME, PROGRAM, DIRECTORY)
dnl The program must properly implement --version.
AC_DEFUN(AM_MISSING_PROG,
[AC_MSG_CHECKING(for working $2)
# Run test in a subshell; some versions of sh will print an error if
# an executable is not found, even if stderr is redirected.
# Redirect stdin to placate older versions of autoconf.  Sigh.
if ($2 --version) < /dev/null > /dev/null 2>&1; then
   $1=$2
   AC_MSG_RESULT(found)
else
   $1="$3/missing $2"
   AC_MSG_RESULT(missing)
fi
AC_SUBST($1)])

# Like AC_CONFIG_HEADER, but automatically create stamp file.

AC_DEFUN(AM_CONFIG_HEADER,
[AC_PREREQ([2.12])
AC_CONFIG_HEADER([$1])
dnl When config.status generates a header, we must update the stamp-h file.
dnl This file resides in the same directory as the config header
dnl that is generated.  We must strip everything past the first ":",
dnl and everything past the last "/".
AC_OUTPUT_COMMANDS(changequote(<<,>>)dnl
ifelse(patsubst(<<$1>>, <<[^ ]>>, <<>>), <<>>,
<<test -z "<<$>>CONFIG_HEADERS" || echo timestamp > patsubst(<<$1>>, <<^\([^:]*/\)?.*>>, <<\1>>)stamp-h<<>>dnl>>,
<<am_indx=1
for am_file in <<$1>>; do
  case " <<$>>CONFIG_HEADERS " in
  *" <<$>>am_file "*<<)>>
    echo timestamp > `echo <<$>>am_file | sed -e 's%:.*%%' -e 's%[^/]*$%%'`stamp-h$am_indx
    ;;
  esac
  am_indx=`expr "<<$>>am_indx" + 1`
done<<>>dnl>>)
changequote([,]))])

