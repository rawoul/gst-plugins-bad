plugin_LTLIBRARIES = libgstopenjpeg.la

libgstopenjpeg_la_SOURCES = gstopenjpegdec.c gstopenjpegenc.c gstopenjpeg.c
libgstopenjpeg_la_CFLAGS = \
	$(GST_PLUGINS_BASE_CFLAGS) $(GST_CFLAGS) $(OPENJPEG_CFLAGS)
libgstopenjpeg_la_LIBADD = \
	$(GST_PLUGINS_BASE_LIBS) -lgstvideo-$(GST_API_VERSION) \
	$(GST_LIBS) $(OPENJPEG_LIBS)
libgstopenjpeg_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstopenjpeg_la_LIBTOOLFLAGS = --tag=disable-static

noinst_HEADERS = \
	gstopenjpegdec.h \
	gstopenjpegenc.h
