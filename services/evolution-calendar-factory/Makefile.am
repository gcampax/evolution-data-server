NULL =

service_in_files = org.gnome.evolution.dataserver.Calendar.service.in
servicedir = $(datadir)/dbus-1/services
service_DATA = $(service_in_files:.service.in=.service)
@EVO_SUBST_SERVICE_RULE@

CLEANFILES = $(service_DATA)
EXTRA_DIST = $(service_in_files)

libexec_PROGRAMS = evolution-calendar-factory

evolution_calendar_factory_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	-DG_LOG_DOMAIN=\"evolution-calendar-factory\" \
	-DLOCALEDIR=\"$(localedir)\" \
	-I$(top_srcdir) \
	-I$(top_srcdir)/calendar \
	-I$(top_builddir) \
	-I$(top_builddir)/calendar \
	$(EVOLUTION_CALENDAR_CFLAGS) \
	$(FACTORY_GTK_CFLAGS) \
	$(DBUS_GLIB_CFLAGS) \
	$(NULL)

evolution_calendar_factory_SOURCES = \
	evolution-calendar-factory.c \
	evolution-calendar-factory-migrate-basedir.c \
	$(NULL)

evolution_calendar_factory_LDADD = \
	$(top_builddir)/calendar/libedata-cal/libedata-cal-1.2.la \
	$(top_builddir)/libebackend/libebackend-1.2.la \
	$(top_builddir)/libedataserver/libedataserver-1.2.la \
	$(EVOLUTION_CALENDAR_LIBS) \
	$(FACTORY_GTK_LIBS) \
	$(DBUS_GLIB_LIBS) \
	$(NULL)

-include $(top_srcdir)/git.mk
