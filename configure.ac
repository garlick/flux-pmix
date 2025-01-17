##
# Prologue
##
AC_INIT([flux-pmix],
        m4_esyscmd([git describe --always | awk '/.*/ {sub(/^v/, ""); printf "%s",$1; exit}']))
AC_CONFIG_AUX_DIR([config])
AC_CONFIG_MACRO_DIR([config])
AC_CONFIG_SRCDIR([NEWS.md])
AC_CANONICAL_SYSTEM

AM_INIT_AUTOMAKE([subdir-objects tar-pax foreign])
AM_SILENT_RULES([yes])
AM_CONFIG_HEADER([config/config.h])
AM_MAINTAINER_MODE([enable])

AC_DEFINE([_GNU_SOURCE], 1,
          [Define _GNU_SOURCE so that we get all necessary prototypes])

##
# Generate project versions from PACKAGE_VERSION (set from git describe above)
##
AX_SPLIT_VERSION
AX_POINT_VERSION=$(echo $AX_POINT_VERSION | $SED 's/-.*$//')
AC_SUBST([AX_MAJOR_VERSION])
AC_SUBST([AX_MINOR_VERSION])
AC_SUBST([AX_POINT_VERSION])

##
# Initialize pkg-config for PKG_CHECK_MODULES to avoid conditional issues
##
PKG_PROG_PKG_CONFIG

# Checks for programs
##
AC_PROG_CC_C99
AM_PROG_CC_C_O
AX_COMPILER_VENDOR
AX_COMPILER_VERSION
AS_CASE($ax_cv_c_compiler_vendor,
  [clang | gnu], [
    WARNING_CFLAGS="-Wall -Werror -Wno-strict-aliasing -Wno-error=deprecated-declarations"
  ]
)
AC_SUBST([WARNING_CFLAGS])

LT_INIT

AC_HEADER_STDC

PKG_CHECK_MODULES([ZMQ], [libczmq >= 3.0.0 libzmq >= 4.0.4])
PKG_CHECK_MODULES([PMIX], [pmix >= 4.0.0])
PKG_CHECK_MODULES([JANSSON], [jansson >= 2.10], [], [])

PKG_CHECK_MODULES([OMPI], [ompi >= 3.0.0], [
  OMPI_PREFIX=`pkg-config --variable=prefix ompi`
  if test -z "${OMPI_PREFIX}"; then
    AC_MSG_ERROR([failed to determine ompi prefix from pkg-config])
  fi
  AC_SUBST(OMPI_PREFIX)
])

X_AC_CHECK_PTHREADS

AX_FLUX_CORE
AX_CODE_COVERAGE

AS_VAR_SET(shell_lua_mpidir, $sysconfdir/flux/shell/lua.d/mpi)
AC_SUBST(shell_lua_mpidir)

AS_VAR_SET(shell_plugindir, $libdir/flux/shell/plugins)
AC_SUBST(shell_plugindir)

fluxplugin_ldflags="-avoid-version -export-symbols-regex '^flux_plugin_init\$\$' --disable-static -shared -export-dynamic"
AC_SUBST(fluxplugin_ldflags)

##
# Epilogue
##
AC_CONFIG_FILES( \
  Makefile \
  src/common/libtap/Makefile \
  src/common/libccan/Makefile \
  src/shell/plugins/Makefile \
  src/shell/lua.d/Makefile \
  t/Makefile \
  t/sharness.d/00-setup.sh \
  t/src/Makefile \
  t/osu-micro-benchmarks/Makefile \
  t/etc/rc.lua \
)

AC_OUTPUT
