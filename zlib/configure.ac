dnl Process this with autoconf to create configure

AC_INIT([zlib], [1.3.1])
AC_CONFIG_SRCDIR([zlib.h])

if test -n "${with_target_subdir}"; then
  AM_ENABLE_MULTILIB(, ..)
fi

AC_CANONICAL_SYSTEM

# This works around an automake problem.
mkinstalldirs="`cd $ac_aux_dir && ${PWDCMD-pwd}`/mkinstalldirs"
AC_SUBST(mkinstalldirs)

AM_INIT_AUTOMAKE

AM_MAINTAINER_MODE

dnl We use these options to decide which functions to include.
AC_ARG_WITH(target-subdir,
[  --with-target-subdir=SUBDIR
                          configuring in a subdirectory])

if test -z "$with_target_subdir" || test "$with_target_subdir" = "."; then
   COMPPATH=.
else
   COMPPATH=..
fi
AC_SUBST(COMPPATH)

AC_ARG_WITH(cross-host,
[  --with-cross-host=HOST  configuring with a cross compiler])

dnl Default to --enable-multilib
AC_ARG_ENABLE(multilib,
[  --enable-multilib       build many library versions (default)],
[case "${enableval}" in
  yes) multilib=yes ;;
  no)  multilib=no ;;
  *)   AC_MSG_ERROR(bad value ${enableval} for multilib option) ;;
 esac], [test -z "$with_target_subdir" && multilib=no || multilib=yes])dnl

AC_ARG_WITH(system-zlib,
[  --with-system-zlib      use installed libz])

# Make sure we don't test executables when making cross-tools.
GCC_NO_EXECUTABLES

# The same as in boehm-gc and libstdc++. Have to borrow it from there.
# We must force CC to /not/ be precious variables; otherwise
# the wrong, non-multilib-adjusted value will be used in multilibs.
# As a side effect, we have to subst CFLAGS ourselves.

m4_rename([_AC_ARG_VAR_PRECIOUS],[real_PRECIOUS])
m4_define([_AC_ARG_VAR_PRECIOUS],[])
AC_PROG_CC
m4_rename_force([real_PRECIOUS],[_AC_ARG_VAR_PRECIOUS])

AC_SUBST(CFLAGS)

# Add CET specific flags if CET is enabled
GCC_CET_FLAGS(CET_FLAGS)
AC_SUBST(CET_FLAGS)

AC_PROG_LIBTOOL
AM_CONDITIONAL([ENABLE_DARWIN_AT_RPATH], [test x$enable_darwin_at_rpath = xyes])

# Find CPP now so that any conditional tests below won't do it and
# thereby make the resulting definitions conditional.
AC_PROG_CPP

if test -n "$with_cross_host"; then
   # We are being configured with a cross compiler.  AC_REPLACE_FUNCS
   # may not work correctly, because the compiler may not be able to
   # link executables.

   # We assume newlib.  This lets us hard-code the functions we know
   # we'll have.
   AC_DEFINE(HAVE_MEMCPY)
   AC_DEFINE(HAVE_STRERROR)
else
   AC_FUNC_MMAP
   AC_CHECK_FUNCS(memcpy strerror)
fi

AC_CHECK_HEADERS(unistd.h)

GCC_WITH_TOOLEXECLIBDIR

if test -n "$with_cross_host" &&
   test x"$with_cross_host" != x"no"; then
  toolexecdir='$(exec_prefix)/$(target_alias)'
  case ${with_toolexeclibdir} in
    no)
      toolexeclibdir='$(toolexecdir)/lib'
      ;;
    *)
      toolexeclibdir=${with_toolexeclibdir}
      ;;
  esac
else
  toolexecdir='$(libdir)/gcc-lib/$(target_alias)'
  toolexeclibdir='$(libdir)'
fi
if test "$GCC" = yes && $CC -print-multi-os-directory > /dev/null 2>&1; then
  multiosdir=/`$CC -print-multi-os-directory`
  case $multiosdir in
    /.) multiosdir= ;; # Avoid trailing /.
  esac
else
  multiosdir=
fi
toolexeclibdir=${toolexeclibdir}${multiosdir}
AC_SUBST(toolexecdir)
AC_SUBST(toolexeclibdir)

AM_CONDITIONAL(TARGET_LIBRARY, test -n "$with_target_subdir")

if test "${multilib}" = "yes"; then
  multilib_arg="--enable-multilib"
else
  multilib_arg=
fi

# Enable --enable-host-shared.
AC_ARG_ENABLE(host-shared,
[AS_HELP_STRING([--enable-host-shared],
		[build host code as shared libraries])])
AC_SUBST(enable_host_shared)

# Enable --enable-host-pie.
AC_ARG_ENABLE(host-pie,
[AS_HELP_STRING([--enable-host-pie],
		[build host code as PIE])])
AC_SUBST(enable_host_pie)

if test x$enable_host_shared = xyes; then
  PICFLAG=-fPIC
elif test x$enable_host_pie = xyes; then
  PICFLAG=-fPIE
else
  PICFLAG=
fi

AC_SUBST(PICFLAG)
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
