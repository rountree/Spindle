libver=`$(top_srcdir)/LIB_VERSION spindleflux`

lib_LTLIBRARIES = \
	libspindleflux.la

rcdir = $(PKGSYSCONF_DIR)
rc_DATA = spindle.rc

libspindleflux_la_SOURCES = \
	flux-spindle.c

libspindleflux_la_CPPFLAGS = \
	-I$(top_srcdir)/src/include \
	$(FLUX_CORE_CFLAGS) \
	$(FLUX_HOSTLIST_CFLAGS)

libspindleflux_la_LIBADD = \
	$(top_builddir)/src/server/startup/libspindlebe_static.la \
	$(top_builddir)/src/fe/startup/libspindlefe_static.la \
	$(FLUX_CORE_LIBS) \
	$(FLUX_HOSTLIST_LIBS)

libspindleflux_la_LDFLAGS = \
	$(fluxplugin_ldflags) \
   -Wl,--allow-multiple-definition \
   -version-info $(libver) \
	-module

spindle.rc: $(srcdir)/spindle_rc $(top_builddir)/Makefile
	@rm -f ./spindle.rc
	$(AM_V_GEN)$(SED) -e s,SPINDLE_LIB,$(libdir),g < $(srcdir)/spindle_rc > $(top_builddir)/src/flux/spindle.rc

mostlyclean-local:
	rm -f ./spindle.rc
