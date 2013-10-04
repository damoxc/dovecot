noinst_LTLIBRARIES = libmongodb.la

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-DMODULE_DIR=\""$(moduledir)"\"

libmongodb_la_SOURCES = \
	mongodb-api.c

module_LTLIBRARIES = libdriver_mongodb.la
libdriver_mongodb_la_LDFLAGS = -module -avoid-version
libdriver_mongodb_la_LIBADD = $(MONGO_LIBS)
libdriver_mongodb_la_CFLAGS = $(MONGO_CFLAGS)
libdriver_mongodb_la_SOURCES = \
	mongodb-driver.c

headers = \
	mongodb-api.h

#pkginc_libdir=$(pkgincludedir)
#pkginc_lib_HEADERS = $(headers)
