plugin_LTLIBRARIES = libgstd3dvideosink.la

libgstd3dvideosink_la_SOURCES = d3dvideosink.c d3dhelpers.c
libgstd3dvideosink_la_CFLAGS = $(GST_PLUGINS_BAD_CFLAGS) $(GST_PLUGINS_BASE_CFLAGS) $(GST_CFLAGS) $(DIRECT3D_CFLAGS) $(DIRECTX_CFLAGS)
libgstd3dvideosink_la_LIBADD = $(GST_BASE_LIBS) $(GST_PLUGINS_BASE_LIBS) \
	-lgstvideo-$(GST_API_VERSION) \
	$(DIRECT3D_LIBS) $(DIRECTX_LDFAGS)
libgstd3dvideosink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstd3dvideosink_la_LIBTOOLFLAGS = --tag=disable-static

noinst_HEADERS = d3dvideosink.h d3dhelpers.h
