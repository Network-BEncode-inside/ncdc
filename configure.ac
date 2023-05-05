
AC_INIT([ncdc],[1.23.1],[projects@yorhel.nl])
AC_CONFIG_SRCDIR([src/ncdc.h])
AC_CONFIG_HEADERS([config.h])
m4_include([deps/lean.m4])
AM_INIT_AUTOMAKE([foreign subdir-objects])
PKG_PROG_PKG_CONFIG([0.18])


# Check for programs.
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_RANLIB

AC_SYS_LARGEFILE


# Use silent building
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# Check for pod2man
AC_CHECK_PROG([have_pod2man],[pod2man],[yes],[no])
AM_CONDITIONAL([USE_POD2MAN], [test "x$have_pod2man" = "xyes"])

# Check for makeheaders. If the system does not provide it, compile our own copy in deps/
AC_CHECK_PROG([have_mh],[makeheaders],[yes],[no])
AM_CONDITIONAL([HAVE_MH], [test "x$have_mh" = "xyes"])


# Check for header files.
AC_CHECK_HEADERS([zlib.h bzlib.h],[], AC_MSG_ERROR([Required header file not found]))


# Check for posix_fadvise()
AC_CHECK_FUNCS([posix_fadvise])

AC_SEARCH_LIBS([inet_pton], [nsl])
AC_SEARCH_LIBS([socket], [socket], [], [
  AC_CHECK_LIB([socket], [socket], [LIBS="-lsocket -lnsl $LIBS"], [], [-lnsl])])


# Check for sendfile() support (not required)
# The following checks are based on ProFTPD's configure.in, except ncdc only
# supports the Linux and BSD variant at the moment, as those are the only two I
# have tested so far.

AC_CACHE_CHECK([which sendfile() implementation to use], pr_cv_sendfile_func,
  pr_cv_sendfile_func="none"

  # Linux
  if test "$pr_cv_sendfile_func" = "none"; then
    AC_LINK_IFELSE([AC_LANG_PROGRAM(
      [[ #include <sys/types.h>
         #include <sys/sendfile.h>
         #include <unistd.h> ]],
      [[ int i=0;
         off_t o=0;
         size_t c=0;
         (void)sendfile(i,i,&o,c); ]])],
      [pr_cv_sendfile_func="Linux"])
  fi

  # BSD
  if test "$pr_cv_sendfile_func" = "none"; then
    AC_LINK_IFELSE([AC_LANG_PROGRAM(
      [[ #include <sys/types.h>
         #include <sys/socket.h>
         #include <sys/uio.h> ]],
      [[ int i=0;
         off_t o=0;
         size_t n=0;
         struct sf_hdtr h={};
         (void)sendfile(i,i,o,n,&h,&o,i); ]])],
      [pr_cv_sendfile_func="BSD"])
  fi
)

# set defines
if test "$pr_cv_sendfile_func" != none; then
  AC_DEFINE(HAVE_SENDFILE, 1, [Define if sendfile support.])
fi

case "$pr_cv_sendfile_func" in
  "Linux")
    AC_DEFINE(HAVE_LINUX_SENDFILE, 1, [Define if using Linux sendfile support.]);;
  "BSD")
    AC_DEFINE(HAVE_BSD_SENDFILE,   1, [Define if using BSD sendfile support.]);;
esac


# Check for ncurses
PKG_CHECK_MODULES(NCURSES, ncursesw,,[
    PKG_CHECK_MODULES(NCURSES, ncurses,,[
	AC_MSG_ERROR(ncurses library is required)
    ])
])

# Check for zlib
AC_CHECK_LIB([z],
             [deflate],
             [AC_SUBST([Z_LIBS],[-lz])],
             [AC_MSG_ERROR(zlib library is required)])

# Check for libbz2
AC_CHECK_LIB([bz2],
             [BZ2_bzReadOpen],
             [AC_SUBST([BZ2_LIBS],[-lbz2])],
             [AC_MSG_ERROR(bzip2 library is required)])

# Check for SQLite3
PKG_CHECK_EXISTS([sqlite3],[
    PKG_CHECK_MODULES([SQLITE],[sqlite3])
  ],[
    AC_CHECK_HEADERS([sqlite3.h],[],
                     [AC_MSG_ERROR([sqlite3 header file not found])])
    AC_CHECK_LIB([sqlite3],
                 [sqlite3_open],
                 [AC_SUBST([SQLITE_LIBS],[-lsqlite3])],
                 [AC_MSG_ERROR([sqlite3 library is required])])
  ]
)

# Check for modules
PKG_CHECK_MODULES([GLIB],   [glib-2.0 >= 2.32 gthread-2.0])
PKG_CHECK_MODULES([GNUTLS], [gnutls >= 3.0])
AC_ARG_WITH([geoip],
            [AS_HELP_STRING([--with-geoip], [support for IP-to-country lookups @<:@default=no@:>@])],
            [],
            [with_geoip=no])
AS_IF([test "x$with_geoip" = xyes],
      [PKG_CHECK_MODULES([GEOIP],
                         [libmaxminddb >= 1.0],
                         [AC_DEFINE(USE_GEOIP, 1, [Use libmaxminddb for IP-to-country lookups])])])




# Check whether we should use the version string from AC_INIT, or use
# git-describe to create one. This trick is copied from the pacman source.

AC_ARG_ENABLE(git-version,
  AS_HELP_STRING([--enable-git-version], [enable use of git version in version string if available]),
  [wantgitver=$enableval], [wantgitver=yes])

usegitver=no
if test "x$wantgitver" = "xyes" ; then
  AC_CHECK_PROGS([GIT], [git], [no])
  test "x$GIT" != "xno" -a -d "$srcdir/.git" && usegitver=yes
fi
AM_CONDITIONAL(USE_GIT_VERSION, test "x$usegitver" = "xyes")



# If we don't have pod2man and doc/ncdc.1 isn't available in the source
# directory, throw a warning and just go ahead without installing the man page.
installmanpage=yes
if test "x$have_pod2man" = "xno" -a \! -s "$srcdir/doc/ncdc.1"; then
  echo ""
  echo "Note: Could not find doc/ncdc.1 in the source directory nor the pod2man"
  echo "utility on your system. No manual page will be installed."
  echo ""
  installmanpage=no
fi
AM_CONDITIONAL([INSTALL_MANPAGE], [test "x$installmanpage" = "xyes"])


AC_CONFIG_FILES([Makefile])
AC_OUTPUT
