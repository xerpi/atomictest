AC_INIT([atomictest], [0.1])
AM_INIT_AUTOMAKE([foreign -Wall])
AC_PROG_CC
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile src/Makefile])
PKG_CHECK_MODULES(DRM, libdrm)
PKG_CHECK_MODULES(GBM, [gbm >= 10.2])
PKG_CHECK_MODULES(LIBUDEV, [libudev >= 136])
PKG_CHECK_MODULES(LIBINPUT, [libinput >= 0.8.0])
AC_OUTPUT
