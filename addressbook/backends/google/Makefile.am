ebook_backend_LTLIBRARIES = libebookbackendgoogle.la

if HAVE_GOA
GOA_SOURCES = \
	e-gdata-goa-authorizer.c \
	e-gdata-goa-authorizer.h
endif

libebookbackendgoogle_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	-DG_LOG_DOMAIN=\"libebookbackendgoogle\" \
	-I$(top_srcdir) \
	-I$(top_builddir) \
	-I$(top_srcdir)/addressbook \
	-I$(top_builddir)/addressbook \
	$(EVOLUTION_ADDRESSBOOK_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(GDATA_CFLAGS) \
	$(GOA_CFLAGS) \
	$(OAUTH_CFLAGS)

libebookbackendgoogle_la_SOURCES = \
	e-book-backend-google-factory.c \
	e-book-backend-google.c \
	e-book-backend-google.h \
	$(GOA_SOURCES)

libebookbackendgoogle_la_LIBADD = \
	$(top_builddir)/addressbook/libebook/libebook-1.2.la \
	$(top_builddir)/addressbook/libedata-book/libedata-book-1.2.la \
	$(top_builddir)/libedataserver/libedataserver-1.2.la \
	$(top_builddir)/libebackend/libebackend-1.2.la \
	$(EVOLUTION_ADDRESSBOOK_LIBS) \
	$(SOUP_LIBS) \
	$(GDATA_LIBS) \
	$(GOA_LIBS) \
	$(OAUTH_LIBS)

libebookbackendgoogle_la_LDFLAGS = \
	-module -avoid-version $(NO_UNDEFINED)

-include $(top_srcdir)/git.mk
