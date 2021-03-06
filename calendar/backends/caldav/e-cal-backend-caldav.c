/*
 * Evolution calendar - caldav backend
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 *
 * Authors:
 *		Christian Kellner <gicmo@gnome.org>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#include <config.h>
#include <string.h>
#include <unistd.h>
#include <gconf/gconf-client.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <libedataserver/e-data-server-util.h>
#include <libedataserver/e-xml-hash-utils.h>
#include <libedataserver/e-proxy.h>
#include <libecal/e-cal-recur.h>
#include <libecal/e-cal-util.h>
#include <libecal/e-cal-time-util.h>
#include <libedata-cal/e-cal-backend-cache.h>
#include <libedata-cal/e-cal-backend-file-store.h>
#include <libedata-cal/e-cal-backend-util.h>
#include <libedata-cal/e-cal-backend-sexp.h>

/* LibXML2 includes */
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

/* LibSoup includes */
#include <libsoup/soup.h>

#include "e-cal-backend-caldav.h"

#define d(x)

#define E_CAL_BACKEND_CALDAV_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_CAL_BACKEND_CALDAV, ECalBackendCalDAVPrivate))

#define CALDAV_CTAG_KEY "CALDAV_CTAG"
#define CALDAV_MAX_MULTIGET_AMOUNT 100 /* what's the maximum count of items to fetch within a multiget request */
#define LOCAL_PREFIX "file://"

/* in seconds */
#define DEFAULT_REFRESH_TIME 60

#define EDC_ERROR(_code) e_data_cal_create_error (_code, NULL)
#define EDC_ERROR_EX(_code, _msg) e_data_cal_create_error (_code, _msg)

typedef enum {

	SLAVE_SHOULD_SLEEP,
	SLAVE_SHOULD_WORK,
	SLAVE_SHOULD_DIE

} SlaveCommand;

/* Private part of the ECalBackendHttp structure */
struct _ECalBackendCalDAVPrivate {

	/* The local disk cache */
	ECalBackendStore *store;

	/* should we sync for offline mode? */
	gboolean do_offline;

	/* TRUE after caldav_open */
	gboolean loaded;
	/* TRUE when server reachable */
	gboolean opened;

	/* lock to indicate a busy state */
	GMutex *busy_lock;

	/* cond to synch threads */
	GCond *cond;

	/* cond to know the slave gone */
	GCond *slave_gone_cond;

	/* BG synch thread */
	const GThread *synch_slave; /* just for a reference, whether thread exists */
	SlaveCommand slave_cmd;
	gboolean slave_busy; /* whether is slave working */
	GTimeVal refresh_time;

	/* The main soup session  */
	SoupSession *session;
	EProxy *proxy;

	/* well, guess what */
	gboolean read_only;

	/* clandar uri */
	gchar *uri;

	/* Authentication info */
	ECredentials *credentials;
	gboolean auth_required;

	/* object cleanup */
	gboolean disposed;

	/* support for 'getctag' extension */
	gboolean ctag_supported;
	gchar *ctag_to_store;

	/* TRUE when 'calendar-schedule' supported on the server */
	gboolean calendar_schedule;
	/* with 'calendar-schedule' supported, here's an outbox url
	 * for queries of free/busy information */
	gchar *schedule_outbox_url;

	/* "Temporary hack" to indicate it's talking to a google calendar.
	 * The proper solution should be to subclass whole backend and change only
	 * necessary parts in it, but this will give us more freedom, as also direct
	 * caldav calendars can profit from this. */
	gboolean is_google;
};

/* ************************************************************************* */
/* Debugging */

#define DEBUG_MESSAGE "message"
#define DEBUG_MESSAGE_HEADER "message:header"
#define DEBUG_MESSAGE_BODY "message:body"
#define DEBUG_SERVER_ITEMS "items"
#define DEBUG_ATTACHMENTS "attachments"

static void convert_to_inline_attachment (ECalBackendCalDAV *cbdav, icalcomponent *icalcomp);
static void convert_to_url_attachment (ECalBackendCalDAV *cbdav, icalcomponent *icalcomp);
static void remove_cached_attachment (ECalBackendCalDAV *cbdav, const gchar *uid);

static gboolean caldav_debug_all = FALSE;
static GHashTable *caldav_debug_table = NULL;

static void
add_debug_key (const gchar *start,
               const gchar *end)
{
	gchar *debug_key;
	gchar *debug_value;

	if (start == end) {
		return;
	}

	debug_key = debug_value = g_strndup (start, end - start);

	debug_key = g_strchug (debug_key);
	debug_key = g_strchomp (debug_key);

	if (strlen (debug_key) == 0) {
		g_free (debug_value);
		return;
	}

	g_hash_table_insert (caldav_debug_table,
			     debug_key,
			     debug_value);

	d(g_debug ("Adding %s to enabled debugging keys", debug_key));
}

static gpointer
caldav_debug_init_once (gpointer data)
{
	const gchar *dbg;

	dbg = g_getenv ("CALDAV_DEBUG");

	if (dbg) {
		const gchar *ptr;

		d(g_debug ("Got debug env variable: [%s]", dbg));

		caldav_debug_table = g_hash_table_new (g_str_hash,
						       g_str_equal);

		ptr = dbg;

		while (*ptr != '\0') {
			if (*ptr == ',' || *ptr == ':') {

				add_debug_key (dbg, ptr);

				if (*ptr == ',') {
					dbg = ptr + 1;
				}
			}

			ptr++;
		}

		if (ptr - dbg > 0) {
			add_debug_key (dbg, ptr);
		}

		if (g_hash_table_lookup (caldav_debug_table, "all")) {
			caldav_debug_all = TRUE;
			g_hash_table_destroy (caldav_debug_table);
			caldav_debug_table = NULL;
		}
	}

	return NULL;
}

static void
caldav_debug_init (void)
{
	static GOnce debug_once = G_ONCE_INIT;

	g_once (&debug_once,
		caldav_debug_init_once,
		NULL);
}

static gboolean
caldav_debug_show (const gchar *component)
{
	if (G_UNLIKELY (caldav_debug_all)) {
		return TRUE;
	} else if (G_UNLIKELY (caldav_debug_table != NULL) &&
		   g_hash_table_lookup (caldav_debug_table, component)) {
		return TRUE;
	}

	return FALSE;
}

#define DEBUG_MAX_BODY_SIZE (100 * 1024 * 1024)

static void
caldav_debug_setup (SoupSession *session)
{
	SoupLogger *logger;
	SoupLoggerLogLevel level;

	if (caldav_debug_show (DEBUG_MESSAGE_BODY))
		level = SOUP_LOGGER_LOG_BODY;
	else if (caldav_debug_show (DEBUG_MESSAGE_HEADER))
		level = SOUP_LOGGER_LOG_HEADERS;
	else
		level = SOUP_LOGGER_LOG_MINIMAL;

	logger = soup_logger_new (level, DEBUG_MAX_BODY_SIZE);
	soup_session_add_feature (session, SOUP_SESSION_FEATURE (logger));
	g_object_unref (logger);
}

/* TODO Do not replicate this in every backend */
static icaltimezone *
resolve_tzid (const gchar *tzid,
              gpointer user_data)
{
	icaltimezone *zone;

	zone = (!strcmp (tzid, "UTC"))
		? icaltimezone_get_utc_timezone ()
		: icaltimezone_get_builtin_timezone_from_tzid (tzid);

	if (!zone)
		zone = e_cal_backend_internal_get_timezone (E_CAL_BACKEND (user_data), tzid);

	return zone;
}

static gboolean
put_component_to_store (ECalBackendCalDAV *cbdav,
                        ECalComponent *comp)
{
	time_t time_start, time_end;

	e_cal_util_get_component_occur_times (
		comp, &time_start, &time_end,
		resolve_tzid, cbdav,  icaltimezone_get_utc_timezone (),
		e_cal_backend_get_kind (E_CAL_BACKEND (cbdav)));

	return e_cal_backend_store_put_component_with_time_range (
		cbdav->priv->store, comp, time_start, time_end);
}

static ECalBackendSyncClass *parent_class = NULL;

static icaltimezone *caldav_internal_get_timezone (ECalBackend *backend, const gchar *tzid);
static void caldav_source_changed_cb (ESource *source, ECalBackendCalDAV *cbdav);

static gboolean remove_comp_from_cache (ECalBackendCalDAV *cbdav, const gchar *uid, const gchar *rid);
static gboolean put_comp_to_cache (ECalBackendCalDAV *cbdav, icalcomponent *icalcomp, const gchar *href, const gchar *etag);

/* ************************************************************************* */
/* Misc. utility functions */
#define X_E_CALDAV "X-EVOLUTION-CALDAV-"
#define X_E_CALDAV_ATTACHMENT_NAME X_E_CALDAV "ATTACHMENT-NAME"

static void
icomp_x_prop_set (icalcomponent *comp,
                  const gchar *key,
                  const gchar *value)
{
	icalproperty *xprop;

	/* Find the old one first */
	xprop = icalcomponent_get_first_property (comp, ICAL_X_PROPERTY);

	while (xprop) {
		const gchar *str = icalproperty_get_x_name (xprop);

		if (!strcmp (str, key)) {
			if (value) {
				icalproperty_set_value_from_string (xprop, value, "NO");
			} else {
				icalcomponent_remove_property (comp, xprop);
				icalproperty_free (xprop);
			}
			break;
		}

		xprop = icalcomponent_get_next_property (comp, ICAL_X_PROPERTY);
	}

	if (!xprop && value) {
		xprop = icalproperty_new_x (value);
		icalproperty_set_x_name (xprop, key);
		icalcomponent_add_property (comp, xprop);
	}
}

static gchar *
icomp_x_prop_get (icalcomponent *comp,
                  const gchar *key)
{
	icalproperty *xprop;

	/* Find the old one first */
	xprop = icalcomponent_get_first_property (comp, ICAL_X_PROPERTY);

	while (xprop) {
		const gchar *str = icalproperty_get_x_name (xprop);

		if (!strcmp (str, key)) {
			break;
		}

		xprop = icalcomponent_get_next_property (comp, ICAL_X_PROPERTY);
	}

	if (xprop) {
		return icalproperty_get_value_as_string_r (xprop);
	}

	return NULL;
}

/* passing NULL as 'href' removes the property */
static void
ecalcomp_set_href (ECalComponent *comp,
                   const gchar *href)
{
	icalcomponent *icomp;

	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_if_fail (icomp != NULL);

	icomp_x_prop_set (icomp, X_E_CALDAV "HREF", href);
}

static gchar *
ecalcomp_get_href (ECalComponent *comp)
{
	icalcomponent *icomp;
	gchar          *str;

	str = NULL;
	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_val_if_fail (icomp != NULL, NULL);

	str =  icomp_x_prop_get (icomp, X_E_CALDAV "HREF");

	return str;
}

/* passing NULL as 'etag' removes the property */
static void
ecalcomp_set_etag (ECalComponent *comp,
                   const gchar *etag)
{
	icalcomponent *icomp;

	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_if_fail (icomp != NULL);

	icomp_x_prop_set (icomp, X_E_CALDAV "ETAG", etag);
}

static gchar *
ecalcomp_get_etag (ECalComponent *comp)
{
	icalcomponent *icomp;
	gchar          *str;

	str = NULL;
	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_val_if_fail (icomp != NULL, NULL);

	str =  icomp_x_prop_get (icomp, X_E_CALDAV "ETAG");

	/* libical 0.48 escapes quotes, thus unescape them */
	if (str && strchr (str, '\\')) {
		gint ii, jj;

		for (ii = 0, jj = 0; str[ii]; ii++) {
			if (str[ii] == '\\') {
				ii++;
				if (!str[ii])
					break;
			}

			str[jj] = str[ii];
			jj++;
		}

		str[jj] = 0;
	}

	return str;
}

/*typedef enum {
 *
	/ * object is in synch,
	 * now isnt that ironic? :) * /
	ECALCOMP_IN_SYNCH = 0,
 *
	/ * local changes * /
	ECALCOMP_LOCALLY_CREATED,
	ECALCOMP_LOCALLY_DELETED,
	ECALCOMP_LOCALLY_MODIFIED
 *
} ECalCompSyncState;
 *
/ * oos = out of synch * /
static void
ecalcomp_set_synch_state (ECalComponent *comp,
 *                        ECalCompSyncState state)
{
	icalcomponent *icomp;
	gchar          *state_string;
 *
	icomp = e_cal_component_get_icalcomponent (comp);
 *
	state_string = g_strdup_printf ("%d", state);
 *
	icomp_x_prop_set (icomp, X_E_CALDAV "ETAG", state_string);
 *
	g_free (state_string);
}*/

static gchar *
ecalcomp_gen_href (ECalComponent *comp)
{
	gchar *href, *uid, *tmp;
	icalcomponent *icomp;

	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_val_if_fail (icomp != NULL, NULL);

	uid = g_strdup (icalcomponent_get_uid (icomp));
	if (!uid || !*uid) {
		g_free (uid);
		uid = e_cal_component_gen_uid ();

		tmp = uid ? strchr (uid, '@') : NULL;
		if (tmp)
			*tmp = '\0';

		tmp = NULL;
	} else
		tmp = isodate_from_time_t (time (NULL));

	/* quite long, but ensures uniqueness quite well, without using UUIDs */
	href = g_strconcat (uid ? uid : "no-uid", tmp ? "-" : "", tmp ? tmp : "", ".ics", NULL);

	g_free (tmp);
	g_free (uid);

	icomp_x_prop_set (icomp, X_E_CALDAV "HREF", href);

	return g_strdelimit (href, " /'\"`&();|<>$%{}!\\:*?#@", '_');
}

/* ensure etag is quoted (to workaround potential server bugs) */
static gchar *
quote_etag (const gchar *etag)
{
	gchar *ret;

	if (etag && (strlen (etag) < 2 || etag[strlen (etag) - 1] != '\"')) {
		ret = g_strdup_printf ("\"%s\"", etag);
	} else {
		ret = g_strdup (etag);
	}

	return ret;
}

/* ************************************************************************* */

static gboolean
status_code_to_result (SoupMessage *message,
                       ECalBackendCalDAVPrivate *priv,
                       GError **perror)
{
	g_return_val_if_fail (message != NULL, FALSE);

	if (SOUP_STATUS_IS_SUCCESSFUL (message->status_code)) {
		return TRUE;
	}

	switch (message->status_code) {
	case SOUP_STATUS_CANT_CONNECT:
	case SOUP_STATUS_CANT_CONNECT_PROXY:
		g_propagate_error (perror,
			e_data_cal_create_error_fmt (
				OtherError,
				_("Server is unreachable (%s)"),
					message->reason_phrase && *message->reason_phrase ? message->reason_phrase :
					(soup_status_get_phrase (message->status_code) ? soup_status_get_phrase (message->status_code) : _("Unknown error"))));
		if (priv) {
			priv->opened = FALSE;
			priv->read_only = TRUE;
		}
		break;
	case 404:
		g_propagate_error (perror, EDC_ERROR (NoSuchCal));
		break;

	case 403:
		g_propagate_error (perror, EDC_ERROR (AuthenticationFailed));
		break;

	case 401:
		if (priv && priv->auth_required)
			g_propagate_error (perror, EDC_ERROR (AuthenticationFailed));
		else
			g_propagate_error (perror, EDC_ERROR (AuthenticationRequired));
		break;

	default:
		d(g_debug ("CalDAV:%s: Unhandled status code %d\n", G_STRFUNC, status_code));
		g_propagate_error (perror,
			e_data_cal_create_error_fmt (
				OtherError,
				_("Unexpected HTTP status code %d returned (%s)"),
					message->status_code,
					message->reason_phrase && *message->reason_phrase ? message->reason_phrase :
					(soup_status_get_phrase (message->status_code) ? soup_status_get_phrase (message->status_code) : _("Unknown error"))));
		break;
	}

	return FALSE;
}

/* !TS, call with lock held */
static gboolean
check_state (ECalBackendCalDAV *cbdav,
             gboolean *online,
             GError **perror)
{
	*online = FALSE;

	if (!cbdav->priv->loaded) {
		g_propagate_error (perror, EDC_ERROR_EX (OtherError, "Not loaded"));
		return FALSE;
	}

	if (!e_backend_get_online (E_BACKEND (cbdav))) {

		if (!cbdav->priv->do_offline) {
			g_propagate_error (perror, EDC_ERROR (RepositoryOffline));
			return FALSE;
		}

	} else {
		*online = TRUE;
	}

	return TRUE;
}

/* ************************************************************************* */
/* XML Parsing code */

static xmlXPathObjectPtr
xpath_eval (xmlXPathContextPtr ctx,
            const gchar *format,
            ...)
{
	xmlXPathObjectPtr  result;
	va_list            args;
	gchar              *expr;

	if (ctx == NULL) {
		return NULL;
	}

	va_start (args, format);
	expr = g_strdup_vprintf (format, args);
	va_end (args);

	result = xmlXPathEvalExpression ((xmlChar *) expr, ctx);
	g_free (expr);

	if (result == NULL) {
		return NULL;
	}

	if (result->type == XPATH_NODESET &&
	    xmlXPathNodeSetIsEmpty (result->nodesetval)) {
		xmlXPathFreeObject (result);
		return NULL;
	}

	return result;
}

#if 0
static gboolean
parse_status_node (xmlNodePtr node,
                   guint *status_code)
{
	xmlChar  *content;
	gboolean  res;

	content = xmlNodeGetContent (node);

	res = soup_headers_parse_status_line ((gchar *) content,
					      NULL,
					      status_code,
					      NULL);
	xmlFree (content);

	return res;
}
#endif

static gchar *
xp_object_get_string (xmlXPathObjectPtr result)
{
	gchar *ret = NULL;

	if (result == NULL)
		return ret;

	if (result->type == XPATH_STRING) {
		ret = g_strdup ((gchar *) result->stringval);
	}

	xmlXPathFreeObject (result);
	return ret;
}

/* like get_string but will quote the etag if necessary */
static gchar *
xp_object_get_etag (xmlXPathObjectPtr result)
{
	gchar *ret = NULL;
	gchar *str;

	if (result == NULL)
		return ret;

	if (result->type == XPATH_STRING) {
		str = (gchar *) result->stringval;

		ret = quote_etag (str);
	}

	xmlXPathFreeObject (result);
	return ret;
}

static guint
xp_object_get_status (xmlXPathObjectPtr result)
{
	gboolean res;
	guint    ret = 0;

	if (result == NULL)
		return ret;

	if (result->type == XPATH_STRING) {
		res = soup_headers_parse_status_line ((gchar *) result->stringval,
							NULL,
							&ret,
							NULL);

		if (!res) {
			ret = 0;
		}
	}

	xmlXPathFreeObject (result);
	return ret;
}

#if 0
static gint
xp_object_get_number (xmlXPathObjectPtr result)
{
	gint ret = -1;

	if (result == NULL)
		return ret;

	if (result->type == XPATH_STRING) {
		ret = result->boolval;
	}

	xmlXPathFreeObject (result);
	return ret;
}
#endif

/*** *** *** *** *** *** */
#define XPATH_HREF "string(/D:multistatus/D:response[%d]/D:href)"
#define XPATH_STATUS "string(/D:multistatus/D:response[%d]/D:propstat/D:status)"
#define XPATH_GETETAG_STATUS "string(/D:multistatus/D:response[%d]/D:propstat/D:prop/D:getetag/../../D:status)"
#define XPATH_GETETAG "string(/D:multistatus/D:response[%d]/D:propstat/D:prop/D:getetag)"
#define XPATH_CALENDAR_DATA "string(/D:multistatus/D:response[%d]/D:propstat/D:prop/C:calendar-data)"
#define XPATH_GETCTAG_STATUS "string(/D:multistatus/D:response/D:propstat/D:prop/CS:getctag/../../D:status)"
#define XPATH_GETCTAG "string(/D:multistatus/D:response/D:propstat/D:prop/CS:getctag)"
#define XPATH_OWNER_STATUS "string(/D:multistatus/D:response/D:propstat/D:prop/D:owner/D:href/../../../D:status)"
#define XPATH_OWNER "string(/D:multistatus/D:response/D:propstat/D:prop/D:owner/D:href)"
#define XPATH_SCHEDULE_OUTBOX_URL_STATUS "string(/D:multistatus/D:response/D:propstat/D:prop/C:schedule-outbox-URL/D:href/../../../D:status)"
#define XPATH_SCHEDULE_OUTBOX_URL "string(/D:multistatus/D:response/D:propstat/D:prop/C:schedule-outbox-URL/D:href)"

typedef struct _CalDAVObject CalDAVObject;

struct _CalDAVObject {

	gchar *href;
	gchar *etag;

	guint status;

	gchar *cdata;
};

static void
caldav_object_free (CalDAVObject *object,
                    gboolean free_object_itself)
{
	g_free (object->href);
	g_free (object->etag);
	g_free (object->cdata);

	if (free_object_itself) {
		g_free (object);
	}
}

static gboolean
parse_report_response (SoupMessage *soup_message,
                       CalDAVObject **objs,
                       gint *len)
{
	xmlXPathContextPtr xpctx;
	xmlXPathObjectPtr  result;
	xmlDocPtr          doc;
	gint                i, n;
	gboolean           res;

	g_return_val_if_fail (soup_message != NULL, FALSE);
	g_return_val_if_fail (objs != NULL || len != NULL, FALSE);

	res = TRUE;
	doc = xmlReadMemory (soup_message->response_body->data,
			     soup_message->response_body->length,
			     "response.xml",
			     NULL,
			     0);

	if (doc == NULL) {
		return FALSE;
	}

	xpctx = xmlXPathNewContext (doc);

	xmlXPathRegisterNs (xpctx, (xmlChar *) "D",
			    (xmlChar *) "DAV:");

	xmlXPathRegisterNs (xpctx, (xmlChar *) "C",
			    (xmlChar *) "urn:ietf:params:xml:ns:caldav");

	result = xpath_eval (xpctx, "/D:multistatus/D:response");

	if (result == NULL || result->type != XPATH_NODESET) {
		*len = 0;
		res = FALSE;
		goto out;
	}

	n = xmlXPathNodeSetGetLength (result->nodesetval);
	*len = n;

	*objs = g_new0 (CalDAVObject, n);

	for (i = 0; i < n; i++) {
		CalDAVObject *object;
		xmlXPathObjectPtr xpres;

		object = *objs + i;
		/* see if we got a status child in the response element */

		xpres = xpath_eval (xpctx, XPATH_HREF, i + 1);
		/* use full path from a href, to let calendar-multiget work properly */
		object->href = xp_object_get_string (xpres);

		xpres = xpath_eval (xpctx,XPATH_STATUS , i + 1);
		object->status = xp_object_get_status (xpres);

		if (object->status && object->status != 200) {
			continue;
		}

		xpres = xpath_eval (xpctx, XPATH_GETETAG_STATUS, i + 1);
		object->status = xp_object_get_status (xpres);

		if (object->status != 200) {
			continue;
		}

		xpres = xpath_eval (xpctx, XPATH_GETETAG, i + 1);
		object->etag = xp_object_get_etag (xpres);

		xpres = xpath_eval (xpctx, XPATH_CALENDAR_DATA, i + 1);
		object->cdata = xp_object_get_string (xpres);
	}

out:
	if (result != NULL)
		xmlXPathFreeObject (result);
	xmlXPathFreeContext (xpctx);
	xmlFreeDoc (doc);
	return res;
}

/* returns whether was able to read the xpath_value from the server's response; *value contains the result */
static gboolean
parse_propfind_response (SoupMessage *message,
                         const gchar *xpath_status,
                         const gchar *xpath_value,
                         gchar **value)
{
	xmlXPathContextPtr xpctx;
	xmlDocPtr          doc;
	gboolean           res = FALSE;

	g_return_val_if_fail (message != NULL, FALSE);
	g_return_val_if_fail (value != NULL, FALSE);

	doc = xmlReadMemory (message->response_body->data,
			     message->response_body->length,
			     "response.xml",
			     NULL,
			     0);

	if (doc == NULL) {
		return FALSE;
	}

	xpctx = xmlXPathNewContext (doc);
	xmlXPathRegisterNs (xpctx, (xmlChar *) "D", (xmlChar *) "DAV:");
	xmlXPathRegisterNs (xpctx, (xmlChar *) "C", (xmlChar *) "urn:ietf:params:xml:ns:caldav");
	xmlXPathRegisterNs (xpctx, (xmlChar *) "CS", (xmlChar *) "http://calendarserver.org/ns/");

	if (xpath_status == NULL || xp_object_get_status (xpath_eval (xpctx, xpath_status)) == 200) {
		gchar *txt = xp_object_get_string (xpath_eval (xpctx, xpath_value));

		if (txt && *txt) {
			gint len = strlen (txt);

			if (*txt == '\"' && len > 2 && txt [len - 1] == '\"') {
				/* dequote */
				*value = g_strndup (txt + 1, len - 2);
			} else {
				*value = txt;
				txt = NULL;
			}

			res = (*value) != NULL;
		}

		g_free (txt);
	}

	xmlXPathFreeContext (xpctx);
	xmlFreeDoc (doc);

	return res;
}

/* ************************************************************************* */
/* Authentication helpers for libsoup */

static void
soup_authenticate (SoupSession *session,
                   SoupMessage *msg,
                   SoupAuth *auth,
                   gboolean retrying,
                   gpointer data)
{
	ECalBackendCalDAV *cbdav;

	cbdav = E_CAL_BACKEND_CALDAV (data);

	/* do not send same password twice, but keep it for later use */
	if (!retrying && cbdav->priv->credentials && e_credentials_has_key (cbdav->priv->credentials, E_CREDENTIALS_KEY_USERNAME)) {
		soup_auth_authenticate (auth, e_credentials_peek (cbdav->priv->credentials, E_CREDENTIALS_KEY_USERNAME), e_credentials_peek (cbdav->priv->credentials, E_CREDENTIALS_KEY_PASSWORD));
		e_credentials_clear_peek (cbdav->priv->credentials);
	}
}

/* ************************************************************************* */
/* direct CalDAV server access functions */

static void
redirect_handler (SoupMessage *msg,
                  gpointer user_data)
{
	if (SOUP_STATUS_IS_REDIRECTION (msg->status_code)) {
		SoupSession *soup_session = user_data;
		SoupURI *new_uri;
		const gchar *new_loc;

		new_loc = soup_message_headers_get (msg->response_headers, "Location");
		if (!new_loc)
			return;

		new_uri = soup_uri_new_with_base (soup_message_get_uri (msg), new_loc);
		if (!new_uri) {
			soup_message_set_status_full (msg,
						      SOUP_STATUS_MALFORMED,
						      "Invalid Redirect URL");
			return;
		}

		if (new_uri->host && g_str_has_suffix (new_uri->host, "yahoo.com")) {
			/* yahoo! returns port 7070, which is unreachable;
			 * it also requires https being used (below call resets port as well) */
			soup_uri_set_scheme (new_uri, SOUP_URI_SCHEME_HTTPS);
		}

		soup_message_set_uri (msg, new_uri);
		soup_session_requeue_message (soup_session, msg);

		soup_uri_free (new_uri);
	}
}

static void
send_and_handle_redirection (SoupSession *soup_session,
                             SoupMessage *msg,
                             gchar **new_location)
{
	gchar *old_uri = NULL;

	g_return_if_fail (msg != NULL);

	if (new_location)
		old_uri = soup_uri_to_string (soup_message_get_uri (msg), FALSE);

	soup_message_set_flags (msg, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_add_header_handler (msg, "got_body", "Location", G_CALLBACK (redirect_handler), soup_session);
	soup_message_headers_append (msg->request_headers, "Connection", "close");
	soup_session_send_message (soup_session, msg);

	if (new_location) {
		gchar *new_loc = soup_uri_to_string (soup_message_get_uri (msg), FALSE);

		if (new_loc && old_uri && !g_str_equal (new_loc, old_uri))
			*new_location = new_loc;
		else
			g_free (new_loc);
	}

	g_free (old_uri);
}

static gchar *
caldav_generate_uri (ECalBackendCalDAV *cbdav,
                     const gchar *target)
{
	gchar *uri;
	const gchar *slash;

	slash = strrchr (target, '/');
	if (slash)
		target = slash + 1;

	/* uri *have* trailing slash already */
	uri = g_strconcat (cbdav->priv->uri, target, NULL);

	return uri;
}

static gboolean
caldav_server_open_calendar (ECalBackendCalDAV *cbdav,
                             gboolean *server_unreachable,
                             GError **perror)
{
	SoupMessage               *message;
	const gchar                *header;
	gboolean                   calendar_access;
	gboolean                   put_allowed;
	gboolean                   delete_allowed;

	g_return_val_if_fail (cbdav != NULL, FALSE);
	g_return_val_if_fail (server_unreachable != NULL, FALSE);

	/* FIXME: setup text_uri */

	message = soup_message_new (SOUP_METHOD_OPTIONS, cbdav->priv->uri);
	if (message == NULL) {
		g_propagate_error (perror, EDC_ERROR (NoSuchCal));
		return FALSE;
	}
	soup_message_headers_append (message->request_headers,
				     "User-Agent", "Evolution/" VERSION);

	send_and_handle_redirection (cbdav->priv->session, message, NULL);

	if (!SOUP_STATUS_IS_SUCCESSFUL (message->status_code)) {
		switch (message->status_code) {
		case SOUP_STATUS_CANT_CONNECT:
		case SOUP_STATUS_CANT_CONNECT_PROXY:
			*server_unreachable = TRUE;
			break;
		}

		status_code_to_result (message, cbdav->priv, perror);

		g_object_unref (message);
		return FALSE;
	}

	/* parse the dav header, we are intreseted in the
	 * calendar-access bit only at the moment */
	header = soup_message_headers_get (message->response_headers, "DAV");
	if (header) {
		calendar_access = soup_header_contains (header, "calendar-access");
		cbdav->priv->calendar_schedule = soup_header_contains (header, "calendar-schedule");
	} else {
		calendar_access = FALSE;
		cbdav->priv->calendar_schedule = FALSE;
	}

	/* parse the Allow header and look for PUT, DELETE at the
	 * moment (maybe we should check more here, for REPORT eg) */
	header = soup_message_headers_get (message->response_headers, "Allow");
	if (header) {
		put_allowed = soup_header_contains (header, "PUT");
		delete_allowed = soup_header_contains (header, "DELETE");
	} else
		put_allowed = delete_allowed = FALSE;

	g_object_unref (message);

	if (calendar_access) {
		cbdav->priv->read_only = !(put_allowed && delete_allowed);
		return TRUE;
	}

	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
	return FALSE;
}

static void
caldav_notify_auth_required (ECalBackendCalDAV *cbdav)
{
	ECredentials *credentials;
	guint prompt_flags;
	gchar *prompt_flags_str;

	g_return_if_fail (cbdav != NULL);
	g_return_if_fail (cbdav->priv != NULL);

	cbdav->priv->opened = FALSE;
	cbdav->priv->slave_cmd = SLAVE_SHOULD_SLEEP;

	if (!e_backend_get_online (E_BACKEND (cbdav)))
		return;

	if (cbdav->priv->credentials)
		credentials = e_credentials_new_clone (cbdav->priv->credentials);
	else
		credentials = e_credentials_new ();
	prompt_flags = E_CREDENTIALS_PROMPT_FLAG_REMEMBER_FOREVER
		     | E_CREDENTIALS_PROMPT_FLAG_SECRET
		     | E_CREDENTIALS_PROMPT_FLAG_ONLINE
		     | E_CREDENTIALS_PROMPT_FLAG_REPROMPT;

	prompt_flags_str = e_credentials_util_prompt_flags_to_string (prompt_flags);
	e_credentials_set (credentials, E_CREDENTIALS_KEY_PROMPT_FLAGS, prompt_flags_str);
	g_free (prompt_flags_str);

	e_cal_backend_notify_auth_required (E_CAL_BACKEND (cbdav), TRUE, credentials);

	e_credentials_free (credentials);
}

/* Returns whether calendar changed on the server. This works only when server
 * supports 'getctag' extension. */
static gboolean
check_calendar_changed_on_server (ECalBackendCalDAV *cbdav)
{
	xmlOutputBufferPtr	  buf;
	SoupMessage              *message;
	xmlDocPtr		  doc;
	xmlNodePtr		  root, node;
	xmlNsPtr		  ns, nsdav;
	gboolean		  result = TRUE;

	g_return_val_if_fail (cbdav != NULL, TRUE);

	/* no support for 'getctag', thus update cache */
	if (!cbdav->priv->ctag_supported)
		return TRUE;

	/* Prepare the soup message */
	message = soup_message_new ("PROPFIND", cbdav->priv->uri);
	if (message == NULL)
		return FALSE;

	doc = xmlNewDoc ((xmlChar *) "1.0");
	root = xmlNewDocNode (doc, NULL, (xmlChar *) "propfind", NULL);
	xmlDocSetRootElement (doc, root);
	nsdav = xmlNewNs (root, (xmlChar *) "DAV:", NULL);
	ns = xmlNewNs (root, (xmlChar *) "http://calendarserver.org/ns/", (xmlChar *) "CS");

	node = xmlNewTextChild (root, nsdav, (xmlChar *) "prop", NULL);
	node = xmlNewTextChild (node, nsdav, (xmlChar *) "getctag", NULL);
	xmlSetNs (node, ns);

	buf = xmlAllocOutputBuffer (NULL);
	xmlNodeDumpOutput (buf, doc, root, 0, 1, NULL);
	xmlOutputBufferFlush (buf);

	soup_message_headers_append (message->request_headers,
				     "User-Agent", "Evolution/" VERSION);
	soup_message_headers_append (message->request_headers,
				     "Depth", "0");

	soup_message_set_request (message,
				  "application/xml",
				  SOUP_MEMORY_COPY,
				  (gchar *) buf->buffer->content,
				  buf->buffer->use);

	/* Send the request now */
	send_and_handle_redirection (cbdav->priv->session, message, NULL);

	/* Clean up the memory */
	xmlOutputBufferClose (buf);
	xmlFreeDoc (doc);

	/* Check the result */
	if (message->status_code == 401) {
		caldav_notify_auth_required (cbdav);
	} else if (message->status_code != 207) {
		/* does not support it, but report calendar changed to update cache */
		cbdav->priv->ctag_supported = FALSE;
	} else {
		gchar *ctag = NULL;

		if (parse_propfind_response (message, XPATH_GETCTAG_STATUS, XPATH_GETCTAG, &ctag)) {
			const gchar *my_ctag;

			my_ctag = e_cal_backend_store_get_key_value (
				cbdav->priv->store, CALDAV_CTAG_KEY);

			if (ctag && my_ctag && g_str_equal (ctag, my_ctag)) {
				/* ctag is same, no change in the calendar */
				result = FALSE;
			} else {
				/* do not store ctag now, do it rather after complete sync */
				g_free (cbdav->priv->ctag_to_store);
				cbdav->priv->ctag_to_store = ctag;
				ctag = NULL;
			}

			g_free (ctag);
		} else {
			cbdav->priv->ctag_supported = FALSE;
		}
	}

	g_object_unref (message);

	return result;
}

/* only_hrefs is a list of requested objects to fetch; it has precedence from
 * start_time/end_time, which are used only when both positive.
 * Times are supposed to be in UTC, if set.
 */
static gboolean
caldav_server_list_objects (ECalBackendCalDAV *cbdav,
                            CalDAVObject **objs,
                            gint *len,
                            GSList *only_hrefs,
                            time_t start_time,
                            time_t end_time)
{
	xmlOutputBufferPtr   buf;
	SoupMessage         *message;
	xmlNodePtr           node;
	xmlNodePtr           sn;
	xmlNodePtr           root;
	xmlDocPtr            doc;
	xmlNsPtr             nsdav;
	xmlNsPtr             nscd;
	gboolean             result;

	/* Allocate the soup message */
	message = soup_message_new ("REPORT", cbdav->priv->uri);
	if (message == NULL)
		return FALSE;

	/* Maybe we should just do a g_strdup_printf here? */
	/* Prepare request body */
	doc = xmlNewDoc ((xmlChar *) "1.0");
	if (!only_hrefs)
		root = xmlNewDocNode (doc, NULL, (xmlChar *) "calendar-query", NULL);
	else
		root = xmlNewDocNode (doc, NULL, (xmlChar *) "calendar-multiget", NULL);
	nscd = xmlNewNs (root, (xmlChar *) "urn:ietf:params:xml:ns:caldav", (xmlChar *) "C");
	xmlSetNs (root, nscd);
	xmlDocSetRootElement (doc, root);

	/* Add webdav tags */
	nsdav = xmlNewNs (root, (xmlChar *) "DAV:", (xmlChar *) "D");
	node = xmlNewTextChild (root, nsdav, (xmlChar *) "prop", NULL);
	xmlNewTextChild (node, nsdav, (xmlChar *) "getetag", NULL);
	if (only_hrefs) {
		GSList *l;

		xmlNewTextChild (node, nscd, (xmlChar *) "calendar-data", NULL);
		for (l = only_hrefs; l; l = l->next) {
			if (l->data) {
				xmlNewTextChild (root, nsdav, (xmlChar *) "href", (xmlChar *) l->data);
			}
		}
	} else {
		node = xmlNewTextChild (root, nscd, (xmlChar *) "filter", NULL);
		node = xmlNewTextChild (node, nscd, (xmlChar *) "comp-filter", NULL);
		xmlSetProp (node, (xmlChar *) "name", (xmlChar *) "VCALENDAR");

		sn = xmlNewTextChild (node, nscd, (xmlChar *) "comp-filter", NULL);
		switch (e_cal_backend_get_kind (E_CAL_BACKEND (cbdav))) {
			default:
			case ICAL_VEVENT_COMPONENT:
				xmlSetProp (sn, (xmlChar *) "name", (xmlChar *) "VEVENT");
				break;
			case ICAL_VJOURNAL_COMPONENT:
				xmlSetProp (sn, (xmlChar *) "name", (xmlChar *) "VJOURNAL");
				break;
			case ICAL_VTODO_COMPONENT:
				xmlSetProp (sn, (xmlChar *) "name", (xmlChar *) "VTODO");
				break;
		}

		if (start_time > 0 || end_time > 0) {
			gchar *tmp;

			sn = xmlNewTextChild (sn, nscd, (xmlChar *) "time-range", NULL);

			if (start_time > 0) {
				tmp = isodate_from_time_t (start_time);
				xmlSetProp (sn, (xmlChar *) "start", (xmlChar *) tmp);
				g_free (tmp);
			}

			if (end_time > 0) {
				tmp = isodate_from_time_t (end_time);
				xmlSetProp (sn, (xmlChar *) "end", (xmlChar *) tmp);
				g_free (tmp);
			}
		}
	}

	buf = xmlAllocOutputBuffer (NULL);
	xmlNodeDumpOutput (buf, doc, root, 0, 1, NULL);
	xmlOutputBufferFlush (buf);

	/* Prepare the soup message */
	soup_message_headers_append (message->request_headers,
				     "User-Agent", "Evolution/" VERSION);
	soup_message_headers_append (message->request_headers,
				     "Depth", "1");

	soup_message_set_request (message,
				  "application/xml",
				  SOUP_MEMORY_COPY,
				  (gchar *) buf->buffer->content,
				  buf->buffer->use);

	/* Send the request now */
	send_and_handle_redirection (cbdav->priv->session, message, NULL);

	/* Clean up the memory */
	xmlOutputBufferClose (buf);
	xmlFreeDoc (doc);

	/* Check the result */
	if (message->status_code != 207) {
		switch (message->status_code) {
		case SOUP_STATUS_CANT_CONNECT:
		case SOUP_STATUS_CANT_CONNECT_PROXY:
			cbdav->priv->opened = FALSE;
			cbdav->priv->slave_cmd = SLAVE_SHOULD_SLEEP;
			cbdav->priv->read_only = TRUE;
			e_cal_backend_notify_readonly (
				E_CAL_BACKEND (cbdav), cbdav->priv->read_only);
			break;
		case 401:
			caldav_notify_auth_required (cbdav);
			break;
		default:
			g_warning ("Server did not response with 207, but with code %d (%s)", message->status_code, soup_status_get_phrase (message->status_code) ? soup_status_get_phrase (message->status_code) : "Unknown code");
			break;
		}

		g_object_unref (message);
		return FALSE;
	}

	/* Parse the response body */
	result = parse_report_response (message, objs, len);

	g_object_unref (message);
	return result;
}

static gboolean
caldav_server_download_attachment (ECalBackendCalDAV *cbdav,
                                   const gchar *attachment_uri,
                                   gchar **content,
                                   gsize *len,
                                   GError **error)
{
	SoupMessage *message;

	g_return_val_if_fail (cbdav != NULL, FALSE);
	g_return_val_if_fail (cbdav->priv != NULL, FALSE);
	g_return_val_if_fail (attachment_uri != NULL, FALSE);
	g_return_val_if_fail (content != NULL, FALSE);
	g_return_val_if_fail (len != NULL, FALSE);

	message = soup_message_new (SOUP_METHOD_GET, attachment_uri);
	if (message == NULL) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return FALSE;
	}

	soup_message_headers_append (message->request_headers, "User-Agent", "Evolution/" VERSION);
	send_and_handle_redirection (cbdav->priv->session, message, NULL);

	if (!SOUP_STATUS_IS_SUCCESSFUL (message->status_code)) {
		status_code_to_result (message, cbdav->priv, error);

		if (message->status_code == 401)
			caldav_notify_auth_required (cbdav);

		g_object_unref (message);
		return FALSE;
	}

	*len = message->response_body->length;
	*content = g_memdup (message->response_body->data, *len);

	g_object_unref (message);

	return TRUE;
}

static gboolean
caldav_server_get_object (ECalBackendCalDAV *cbdav,
                          CalDAVObject *object,
                          GError **perror)
{
	SoupMessage              *message;
	const gchar               *hdr;
	gchar                     *uri;

	g_assert (object != NULL && object->href != NULL);

	uri = caldav_generate_uri (cbdav, object->href);
	message = soup_message_new (SOUP_METHOD_GET, uri);
	if (message == NULL) {
		g_free (uri);
		g_propagate_error (perror, EDC_ERROR (NoSuchCal));
		return FALSE;
	}

	soup_message_headers_append (message->request_headers,
				     "User-Agent", "Evolution/" VERSION);

	send_and_handle_redirection (cbdav->priv->session, message, NULL);

	if (!SOUP_STATUS_IS_SUCCESSFUL (message->status_code)) {
		status_code_to_result (message, cbdav->priv, perror);

		if (message->status_code == 401)
			caldav_notify_auth_required (cbdav);
		else
			g_warning ("Could not fetch object '%s' from server, status:%d (%s)", uri, message->status_code, soup_status_get_phrase (message->status_code) ? soup_status_get_phrase (message->status_code) : "Unknown code");
		g_object_unref (message);
		g_free (uri);
		return FALSE;
	}

	hdr = soup_message_headers_get (message->response_headers, "Content-Type");

	if (hdr == NULL || g_ascii_strncasecmp (hdr, "text/calendar", 13)) {
		g_propagate_error (perror, EDC_ERROR (InvalidObject));
		g_object_unref (message);
		g_warning ("Object to fetch '%s' not of type text/calendar", uri);
		g_free (uri);
		return FALSE;
	}

	hdr = soup_message_headers_get (message->response_headers, "ETag");

	if (hdr != NULL) {
		g_free (object->etag);
		object->etag = quote_etag (hdr);
	} else if (!object->etag) {
		g_warning ("UUHH no ETag, now that's bad! (at '%s')", uri);
	}
	g_free (uri);

	g_free (object->cdata);
	object->cdata = g_strdup (message->response_body->data);

	g_object_unref (message);

	return TRUE;
}

static void
caldav_post_freebusy (ECalBackendCalDAV *cbdav,
                      const gchar *url,
                      gchar **post_fb,
                      GError **error)
{
	SoupMessage *message;

	e_return_data_cal_error_if_fail (cbdav != NULL, InvalidArg);
	e_return_data_cal_error_if_fail (url != NULL, InvalidArg);
	e_return_data_cal_error_if_fail (post_fb != NULL, InvalidArg);
	e_return_data_cal_error_if_fail (*post_fb != NULL, InvalidArg);

	message = soup_message_new (SOUP_METHOD_POST, url);
	if (message == NULL) {
		g_propagate_error (error, EDC_ERROR (NoSuchCal));
		return;
	}

	soup_message_headers_append (message->request_headers, "User-Agent", "Evolution/" VERSION);
	soup_message_set_request (message,
				  "text/calendar; charset=utf-8",
				  SOUP_MEMORY_COPY,
				  *post_fb, strlen (*post_fb));

	send_and_handle_redirection (cbdav->priv->session, message, NULL);

	if (!SOUP_STATUS_IS_SUCCESSFUL (message->status_code)) {
		status_code_to_result (message, cbdav->priv, error);
		if (message->status_code == 401)
			caldav_notify_auth_required (cbdav);
		else
			g_warning ("Could not post free/busy request to '%s', status:%d (%s)", url, message->status_code, soup_status_get_phrase (message->status_code) ? soup_status_get_phrase (message->status_code) : "Unknown code");

		g_object_unref (message);

		return;
	}

	g_free (*post_fb);
	*post_fb = g_strdup (message->response_body->data);

	g_object_unref (message);
}

static gboolean
caldav_server_put_object (ECalBackendCalDAV *cbdav,
                          CalDAVObject *object,
                          icalcomponent *icalcomp,
                          GError **perror)
{
	SoupMessage              *message;
	const gchar               *hdr;
	gchar                     *uri;

	hdr = NULL;

	g_assert (object != NULL && object->cdata != NULL);

	uri = caldav_generate_uri (cbdav, object->href);
	message = soup_message_new (SOUP_METHOD_PUT, uri);
	g_free (uri);
	if (message == NULL) {
		g_propagate_error (perror, EDC_ERROR (NoSuchCal));
		return FALSE;
	}

	soup_message_headers_append (message->request_headers,
				     "User-Agent", "Evolution/" VERSION);

	/* For new items we use the If-None-Match so we don't
	 * acidently override resources, for item updates we
	 * use the If-Match header to avoid the Lost-update
	 * problem */
	if (object->etag == NULL) {
		soup_message_headers_append (message->request_headers, "If-None-Match", "*");
	} else {
		soup_message_headers_append (message->request_headers,
					     "If-Match", object->etag);
	}

	soup_message_set_request (message,
				  "text/calendar; charset=utf-8",
				  SOUP_MEMORY_COPY,
				  object->cdata,
				  strlen (object->cdata));

	uri = NULL;
	send_and_handle_redirection (cbdav->priv->session, message, &uri);

	if (uri) {
		gchar *file = strrchr (uri, '/');

		/* there was a redirect, update href properly */
		if (file) {
			gchar *decoded;

			g_free (object->href);

			decoded = soup_uri_decode (file + 1);
			object->href = soup_uri_encode (decoded ? decoded : (file + 1), NULL);

			g_free (decoded);
		}

		g_free (uri);
	}

	if (status_code_to_result (message, cbdav->priv, perror)) {
		gboolean was_get = FALSE;

		hdr = soup_message_headers_get (message->response_headers, "ETag");
		if (hdr != NULL) {
			g_free (object->etag);
			object->etag = quote_etag (hdr);
		} else {
			/* no ETag header returned, check for it with a GET */
			hdr = soup_message_headers_get (message->response_headers, "Location");
			if (hdr) {
				/* reflect possible href change first */
				gchar *file = strrchr (hdr, '/');

				if (file) {
					gchar *decoded;

					g_free (object->href);

					decoded = soup_uri_decode (file + 1);
					object->href = soup_uri_encode (decoded ? decoded : (file + 1), NULL);

					g_free (decoded);
				}
			}
		}

		was_get = TRUE;

		if (caldav_server_get_object (cbdav, object, perror)) {
			icalcomponent *use_comp = NULL;

			if (object->cdata && was_get) {
				/* maybe server also modified component, thus rather store the server's */
				use_comp = icalparser_parse_string (object->cdata);
			}

			if (!use_comp)
				use_comp = icalcomp;

			put_comp_to_cache (cbdav, use_comp, object->href, object->etag);

			if (use_comp != icalcomp)
				icalcomponent_free (use_comp);
		}
	} else if (message->status_code == 401) {
		caldav_notify_auth_required (cbdav);
	}

	g_object_unref (message);

	return TRUE;
}

static void
caldav_server_delete_object (ECalBackendCalDAV *cbdav,
                             CalDAVObject *object,
                             GError **perror)
{
	SoupMessage              *message;
	gchar                     *uri;

	g_assert (object != NULL && object->href != NULL);

	uri = caldav_generate_uri (cbdav, object->href);
	message = soup_message_new (SOUP_METHOD_DELETE, uri);
	g_free (uri);
	if (message == NULL) {
		g_propagate_error (perror, EDC_ERROR (NoSuchCal));
		return;
	}

	soup_message_headers_append (message->request_headers,
				     "User-Agent", "Evolution/" VERSION);

	if (object->etag != NULL) {
		soup_message_headers_append (message->request_headers,
					     "If-Match", object->etag);
	}

	send_and_handle_redirection (cbdav->priv->session, message, NULL);

	status_code_to_result (message, cbdav->priv, perror);

	if (message->status_code == 401)
		caldav_notify_auth_required (cbdav);

	g_object_unref (message);
}

static gboolean
caldav_receive_schedule_outbox_url (ECalBackendCalDAV *cbdav)
{
	SoupMessage *message;
	xmlOutputBufferPtr buf;
	xmlDocPtr doc;
	xmlNodePtr root, node;
	xmlNsPtr nsdav;
	gchar *owner = NULL;

	g_return_val_if_fail (cbdav != NULL, FALSE);

	g_return_val_if_fail (cbdav->priv != NULL, FALSE);
	g_return_val_if_fail (cbdav->priv->schedule_outbox_url == NULL, TRUE);

	/* Prepare the soup message */
	message = soup_message_new ("PROPFIND", cbdav->priv->uri);
	if (message == NULL)
		return FALSE;

	doc = xmlNewDoc ((xmlChar *) "1.0");
	root = xmlNewDocNode (doc, NULL, (xmlChar *) "propfind", NULL);
	xmlDocSetRootElement (doc, root);
	nsdav = xmlNewNs (root, (xmlChar *) "DAV:", NULL);

	node = xmlNewTextChild (root, nsdav, (xmlChar *) "prop", NULL);
	xmlNewTextChild (node, nsdav, (xmlChar *) "owner", NULL);

	buf = xmlAllocOutputBuffer (NULL);
	xmlNodeDumpOutput (buf, doc, root, 0, 1, NULL);
	xmlOutputBufferFlush (buf);

	soup_message_headers_append (message->request_headers, "User-Agent", "Evolution/" VERSION);
	soup_message_headers_append (message->request_headers, "Depth", "0");

	soup_message_set_request (message,
				  "application/xml",
				  SOUP_MEMORY_COPY,
				  (gchar *) buf->buffer->content,
				  buf->buffer->use);

	/* Send the request now */
	send_and_handle_redirection (cbdav->priv->session, message, NULL);

	/* Clean up the memory */
	xmlOutputBufferClose (buf);
	xmlFreeDoc (doc);

	/* Check the result */
	if (message->status_code == 207 && parse_propfind_response (message, XPATH_OWNER_STATUS, XPATH_OWNER, &owner) && owner && *owner) {
		xmlNsPtr nscd;
		SoupURI *suri;

		g_object_unref (message);

		/* owner is a full path to the user's URL, thus change it in
		 * calendar's uri when asking for schedule-outbox-URL */
		suri = soup_uri_new (cbdav->priv->uri);
		soup_uri_set_path (suri, owner);
		g_free (owner);
		owner = soup_uri_to_string (suri, FALSE);
		soup_uri_free (suri);

		message = soup_message_new ("PROPFIND", owner);
		if (message == NULL) {
			g_free (owner);
			return FALSE;
		}

		doc = xmlNewDoc ((xmlChar *) "1.0");
		root = xmlNewDocNode (doc, NULL, (xmlChar *) "propfind", NULL);
		xmlDocSetRootElement (doc, root);
		nsdav = xmlNewNs (root, (xmlChar *) "DAV:", NULL);
		nscd = xmlNewNs (root, (xmlChar *) "urn:ietf:params:xml:ns:caldav", (xmlChar *) "C");

		node = xmlNewTextChild (root, nsdav, (xmlChar *) "prop", NULL);
		xmlNewTextChild (node, nscd, (xmlChar *) "schedule-outbox-URL", NULL);

		buf = xmlAllocOutputBuffer (NULL);
		xmlNodeDumpOutput (buf, doc, root, 0, 1, NULL);
		xmlOutputBufferFlush (buf);

		soup_message_headers_append (message->request_headers, "User-Agent", "Evolution/" VERSION);
		soup_message_headers_append (message->request_headers, "Depth", "0");

		soup_message_set_request (message,
				  "application/xml",
				  SOUP_MEMORY_COPY,
				  (gchar *) buf->buffer->content,
				  buf->buffer->use);

		/* Send the request now */
		send_and_handle_redirection (cbdav->priv->session, message, NULL);

		if (message->status_code == 207 && parse_propfind_response (message, XPATH_SCHEDULE_OUTBOX_URL_STATUS, XPATH_SCHEDULE_OUTBOX_URL, &cbdav->priv->schedule_outbox_url)) {
			if (!*cbdav->priv->schedule_outbox_url) {
				g_free (cbdav->priv->schedule_outbox_url);
				cbdav->priv->schedule_outbox_url = NULL;
			} else {
				/* make it a full URI */
				suri = soup_uri_new (cbdav->priv->uri);
				soup_uri_set_path (suri, cbdav->priv->schedule_outbox_url);
				g_free (cbdav->priv->schedule_outbox_url);
				cbdav->priv->schedule_outbox_url = soup_uri_to_string (suri, FALSE);
				soup_uri_free (suri);
			}
		}

		/* Clean up the memory */
		xmlOutputBufferClose (buf);
		xmlFreeDoc (doc);
	} else if (message->status_code == 401) {
		caldav_notify_auth_required (cbdav);
	}

	if (message)
		g_object_unref (message);

	g_free (owner);

	return cbdav->priv->schedule_outbox_url != NULL;
}

/* ************************************************************************* */
/* Synchronization foo */

static gboolean extract_timezones (ECalBackendCalDAV *cbdav, icalcomponent *icomp);

struct cache_comp_list
{
	GSList *slist;
};

static gboolean
remove_complist_from_cache_and_notify_cb (gpointer key,
                                          gpointer value,
                                          gpointer data)
{
	GSList *l;
	struct cache_comp_list *ccl = value;
	ECalBackendCalDAV *cbdav = data;

	for (l = ccl->slist; l; l = l->next) {
		ECalComponent *old_comp = l->data;
		ECalComponentId *id;

		id = e_cal_component_get_id (old_comp);
		if (!id) {
			continue;
		}

		if (e_cal_backend_store_remove_component (cbdav->priv->store, id->uid, id->rid)) {
			e_cal_backend_notify_component_removed ((ECalBackend *) cbdav, id, old_comp, NULL);
		}

		e_cal_component_free_id (id);
	}
	remove_cached_attachment (cbdav, (const gchar *) key);

	return FALSE;
}

static void
free_comp_list (gpointer cclist)
{
	struct cache_comp_list *ccl = cclist;

	g_return_if_fail (ccl != NULL);

	g_slist_foreach (ccl->slist, (GFunc) g_object_unref, NULL);
	g_slist_free (ccl->slist);
	g_free (ccl);
}

#define etags_match(_tag1, _tag2) ((_tag1 == _tag2) ? TRUE :                 \
				   g_str_equal (_tag1 != NULL ? _tag1 : "",  \
						_tag2 != NULL ? _tag2 : ""))

/* start_time/end_time is an interval for checking changes. If both greater than zero,
 * only the interval is checked and the removed items are not notified, as they can
 * be still there.
*/
static void
synchronize_cache (ECalBackendCalDAV *cbdav,
                   time_t start_time,
                   time_t end_time)
{
	ECalBackend *bkend;
	CalDAVObject *sobjs, *object;
	GSList *c_objs, *c_iter; /* list of all items known from our cache */
	GTree *c_uid2complist;  /* cache components list (with detached instances) sorted by (master's) uid */
	GHashTable *c_href2uid; /* connection between href and a (master's) uid */
	GSList *hrefs_to_update, *htu; /* list of href-s to update */
	gint i, len;

	if (!check_calendar_changed_on_server (cbdav)) {
		/* no changes on the server, no update required */
		return;
	}

	bkend  = E_CAL_BACKEND (cbdav);
	len    = 0;
	sobjs  = NULL;

	/* get list of server objects */
	if (!caldav_server_list_objects (cbdav, &sobjs, &len, NULL, start_time, end_time))
		return;

	c_objs = e_cal_backend_store_get_components (cbdav->priv->store);

	if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
		printf ("CalDAV - found %d objects on the server, locally stored %d objects\n", len, g_slist_length (c_objs)); fflush (stdout);
	}

	/* do not store changes in cache immediately - makes things significantly quicker */
	e_cal_backend_store_freeze_changes (cbdav->priv->store);

	c_uid2complist = g_tree_new_full ((GCompareDataFunc) g_strcmp0, NULL, g_free, free_comp_list);
	c_href2uid = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	/* fill indexed hash and tree with cached components */
	for (c_iter = c_objs; c_iter; c_iter = g_slist_next (c_iter)) {
		ECalComponent *ccomp = E_CAL_COMPONENT (c_iter->data);
		const gchar *uid = NULL;
		struct cache_comp_list *ccl;
		gchar *href;

		e_cal_component_get_uid (ccomp, &uid);
		if (!uid) {
			g_warning ("broken component with NULL Id");
			continue;
		}

		href = ecalcomp_get_href (ccomp);

		if (href == NULL) {
			g_warning ("href of object NULL :(");
			continue;
		}

		ccl = g_tree_lookup (c_uid2complist, uid);
		if (ccl) {
			ccl->slist = g_slist_prepend (ccl->slist, g_object_ref (ccomp));
		} else {
			ccl = g_new0 (struct cache_comp_list, 1);
			ccl->slist = g_slist_append (NULL, g_object_ref (ccomp));

			/* make a copy, which will be used in the c_href2uid too */
			uid = g_strdup (uid);

			g_tree_insert (c_uid2complist, (gpointer) uid, ccl);
		}

		if (g_hash_table_lookup (c_href2uid, href) == NULL) {
			/* uid is from a component or c_uid2complist key, thus will not be
			 * freed before a removal from c_uid2complist, thus do not duplicate it,
			 * rather save memory */
			g_hash_table_insert (c_href2uid, href, (gpointer) uid);
		} else {
			g_free (href);
		}
	}

	/* clear it now, we do not need it later */
	g_slist_foreach (c_objs, (GFunc) g_object_unref, NULL);
	g_slist_free (c_objs);
	c_objs = NULL;

	hrefs_to_update = NULL;

	/* see if we have to update or add some objects */
	for (i = 0, object = sobjs; i < len && cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK; i++, object++) {
		ECalComponent *ccomp = NULL;
		gchar *etag = NULL;
		const gchar *uid;
		struct cache_comp_list *ccl;

		if (object->status != 200) {
			/* just continue here, so that the object
			 * doesnt get removed from the cobjs list
			 * - therefore it will be removed */
			continue;
		}

		uid = g_hash_table_lookup (c_href2uid, object->href);
		if (uid) {
			ccl = g_tree_lookup (c_uid2complist, uid);
			if (ccl) {
				GSList *sl;
				for (sl = ccl->slist; sl && !etag; sl = sl->next) {
					ccomp = sl->data;
					if (ccomp)
						etag = ecalcomp_get_etag (ccomp);
				}

				if (!etag)
					ccomp = NULL;
			}
		}

		if (!etag || !etags_match (etag, object->etag)) {
			hrefs_to_update = g_slist_prepend (hrefs_to_update, object->href);
		} else if (uid && ccl) {
			/* all components cover by this uid are up-to-date */
			GSList *p;

			for (p = ccl->slist; p; p = p->next) {
				g_object_unref (p->data);
			}

			g_slist_free (ccl->slist);
			ccl->slist = NULL;
		}

		g_free (etag);
	}

	/* free hash table, as it is not used anymore */
	g_hash_table_destroy (c_href2uid);
	c_href2uid = NULL;

	if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
		printf ("CalDAV - recognized %d items to update\n", g_slist_length (hrefs_to_update)); fflush (stdout);
	}

	htu = hrefs_to_update;
	while (htu && cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK) {
		gint count = 0;
		GSList *to_fetch = NULL;

		while (count < CALDAV_MAX_MULTIGET_AMOUNT && htu) {
			to_fetch = g_slist_prepend (to_fetch, htu->data);
			htu = htu->next;
			count++;
		}

		if (to_fetch && cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK) {
			CalDAVObject *up_sobjs = NULL;

			if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
				printf ("CalDAV - going to fetch %d items\n", g_slist_length (to_fetch)); fflush (stdout);
			}

			count = 0;
			if (!caldav_server_list_objects (cbdav, &up_sobjs, &count, to_fetch, 0, 0)) {
				fprintf (stderr, "CalDAV - failed to retrieve bunch of items\n"); fflush (stderr);
				break;
			}

			if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
				printf ("CalDAV - fetched bunch of %d items\n", count); fflush (stdout);
			}

			/* we are going to update cache */
			/* they are downloaded, so process them */
			for (i = 0, object = up_sobjs; i < count /*&& cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK */; i++, object++) {
				if (object->status == 200 && object->href && object->etag && object->cdata && *object->cdata) {
					icalcomponent *icomp = icalparser_parse_string (object->cdata);

					if (icomp) {
						icalcomponent_kind kind = icalcomponent_isa (icomp);

						extract_timezones (cbdav, icomp);

						if (kind == ICAL_VCALENDAR_COMPONENT) {
							icalcomponent *subcomp;

							kind = e_cal_backend_get_kind (bkend);

							for (subcomp = icalcomponent_get_first_component (icomp, kind);
							     subcomp;
							     subcomp = icalcomponent_get_next_component (icomp, kind)) {
								ECalComponent *new_comp, *old_comp;

								convert_to_url_attachment (cbdav, subcomp);
								new_comp = e_cal_component_new ();
								if (e_cal_component_set_icalcomponent (new_comp, icalcomponent_new_clone (subcomp))) {
									const gchar *uid = NULL;
									struct cache_comp_list *ccl;

									e_cal_component_get_uid (new_comp, &uid);
									if (!uid) {
										g_warning ("%s: no UID on component!", G_STRFUNC);
										g_object_unref (new_comp);
										continue;
									}

									ecalcomp_set_href (new_comp, object->href);
									ecalcomp_set_etag (new_comp, object->etag);

									old_comp = NULL;
									ccl = g_tree_lookup (c_uid2complist, uid);
									if (ccl) {
										gchar *nc_rid = e_cal_component_get_recurid_as_string (new_comp);
										GSList *p;

										for (p = ccl->slist; p && !old_comp; p = p->next) {
											gchar *oc_rid;

											old_comp = p->data;

											oc_rid = e_cal_component_get_recurid_as_string (old_comp);
											if (g_strcmp0 (nc_rid, oc_rid) != 0) {
												old_comp = NULL;
											}

											g_free (oc_rid);
										}

										g_free (nc_rid);
									}

									put_component_to_store (cbdav, new_comp);

									if (old_comp == NULL) {
										e_cal_backend_notify_component_created (bkend, new_comp);
									} else {
										e_cal_backend_notify_component_modified (bkend, old_comp, new_comp);

										ccl->slist = g_slist_remove (ccl->slist, old_comp);
										g_object_unref (old_comp);
									}
								}

								g_object_unref (new_comp);
							}
						}

						icalcomponent_free (icomp);
					}
				}

				/* these free immediately */
				caldav_object_free (object, FALSE);
			}

			/* cache update done for fetched items */
			g_free (up_sobjs);
		}

		/* do not free 'data' itself, it's part of 'sobjs' */
		g_slist_free (to_fetch);
	}

	/* if not interrupted and not using the time range... */
	if (cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK && (!start_time || !end_time)) {
		/* ...remove old (not on server anymore) items from our cache and notify of a removal */
		g_tree_foreach (c_uid2complist, remove_complist_from_cache_and_notify_cb, cbdav);
	}

	if (cbdav->priv->ctag_to_store) {
		/* store only when wasn't interrupted */
		if (cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK && start_time == 0 && end_time == 0) {
			e_cal_backend_store_put_key_value (cbdav->priv->store, CALDAV_CTAG_KEY, cbdav->priv->ctag_to_store);
		}

		g_free (cbdav->priv->ctag_to_store);
		cbdav->priv->ctag_to_store = NULL;
	}

	/* save cache changes to disk finally */
	e_cal_backend_store_thaw_changes (cbdav->priv->store);

	for (i = 0, object = sobjs; i < len; i++, object++) {
		caldav_object_free (object, FALSE);
	}

	g_tree_destroy (c_uid2complist);
	g_slist_free (hrefs_to_update);
	g_free (sobjs);
}

static gboolean
is_google_uri (const gchar *uri)
{
	SoupURI *suri;
	gboolean res;

	g_return_val_if_fail (uri != NULL, FALSE);

	suri = soup_uri_new (uri);
	g_return_val_if_fail (suri != NULL, FALSE);

	res = suri->host && g_ascii_strcasecmp (suri->host, "www.google.com") == 0;

	soup_uri_free (suri);

	return res;
}

/* ************************************************************************* */

static gpointer
caldav_synch_slave_loop (gpointer data)
{
	ECalBackendCalDAV        *cbdav;
	time_t now;
	icaltimezone *utc = icaltimezone_get_utc_timezone ();
	gboolean know_unreachable;

	cbdav = E_CAL_BACKEND_CALDAV (data);

	g_mutex_lock (cbdav->priv->busy_lock);

	know_unreachable = !cbdav->priv->opened;

	while (cbdav->priv->slave_cmd != SLAVE_SHOULD_DIE) {
		GTimeVal alarm_clock;
		if (cbdav->priv->slave_cmd == SLAVE_SHOULD_SLEEP) {
			/* just sleep until we get woken up again */
			g_cond_wait (cbdav->priv->cond, cbdav->priv->busy_lock);

			/* check if we should die, work or sleep again */
			continue;
		}

		/* Ok here we go, do some real work
		 * Synch it baby one more time ...
		 */
		cbdav->priv->slave_busy = TRUE;

		if (!cbdav->priv->opened) {
			gboolean server_unreachable = FALSE;
			GError *local_error = NULL;
			gboolean online;

			if (caldav_server_open_calendar (cbdav, &server_unreachable, &local_error)) {
				cbdav->priv->opened = TRUE;
				cbdav->priv->slave_cmd = SLAVE_SHOULD_WORK;
				g_cond_signal (cbdav->priv->cond);

				cbdav->priv->is_google = is_google_uri (cbdav->priv->uri);
				know_unreachable = FALSE;
			} else if (local_error) {
				cbdav->priv->opened = FALSE;
				cbdav->priv->read_only = TRUE;

				if (!know_unreachable) {
					gchar *msg;

					know_unreachable = TRUE;

					msg = g_strdup_printf (_("Server is unreachable, calendar is opened in read-only mode.\nError message: %s"), local_error->message);
					e_cal_backend_notify_error (E_CAL_BACKEND (cbdav), msg);
					g_free (msg);
				}

				g_clear_error (&local_error);
			} else {
				cbdav->priv->opened = FALSE;
				cbdav->priv->read_only = TRUE;
				know_unreachable = TRUE;
			}

			e_cal_backend_notify_readonly (E_CAL_BACKEND (cbdav), cbdav->priv->read_only);

			online = e_backend_get_online (E_BACKEND (cbdav));
			e_cal_backend_notify_online (E_CAL_BACKEND (cbdav), online);
		}

		if (cbdav->priv->opened) {
			time (&now);
			/* check for events in the month before/after today first,
			 * to show user actual data as soon as possible */
			synchronize_cache (cbdav, time_add_week_with_zone (now, -5, utc), time_add_week_with_zone (now, +5, utc));

			if (cbdav->priv->slave_cmd != SLAVE_SHOULD_SLEEP) {
				/* and then check for changes in a whole calendar */
				synchronize_cache (cbdav, 0, 0);
			}

			if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
				GSList *c_objs;

				c_objs = e_cal_backend_store_get_components (cbdav->priv->store);

				printf ("CalDAV - finished syncing with %d items in a cache\n", g_slist_length (c_objs)); fflush (stdout);

				g_slist_foreach (c_objs, (GFunc) g_object_unref, NULL);
				g_slist_free (c_objs);
			}
		}

		cbdav->priv->slave_busy = FALSE;

		/* puhh that was hard, get some rest :) */
		g_get_current_time (&alarm_clock);
		alarm_clock.tv_sec += cbdav->priv->refresh_time.tv_sec;
		g_cond_timed_wait (cbdav->priv->cond,
				   cbdav->priv->busy_lock,
				   &alarm_clock);

	}

	/* signal we are done */
	g_cond_signal (cbdav->priv->slave_gone_cond);

	cbdav->priv->synch_slave = NULL;

	/* we got killed ... */
	g_mutex_unlock (cbdav->priv->busy_lock);
	return NULL;
}

static gchar *
maybe_append_email_domain (const gchar *username,
                           const gchar *may_append)
{
	if (!username || !*username)
		return NULL;

	if (strchr (username, '@'))
		return g_strdup (username);

	return g_strconcat (username, may_append, NULL);
}

static gchar *
get_usermail (ECalBackend *backend)
{
	ECalBackendCalDAV        *cbdav;
	ESource *source;
	gchar *res = NULL;

	g_return_val_if_fail (backend != NULL, NULL);

	source = e_backend_get_source (E_BACKEND (backend));
	if (source) {
		res = e_source_get_duped_property (source, "usermail");
		if (res && *res)
			return res;

		g_free (res);
		res = NULL;
	}

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	if (cbdav->priv->is_google && cbdav->priv->credentials) {
		res = maybe_append_email_domain (e_credentials_peek (cbdav->priv->credentials, E_CREDENTIALS_KEY_USERNAME), "@gmail.com");
	}

	return res;
}

/* ************************************************************************* */
/* ********** ECalBackendSync virtual function implementation *************  */

static gboolean
caldav_get_backend_property (ECalBackendSync *backend,
                             EDataCal *cal,
                             GCancellable *cancellable,
                             const gchar *prop_name,
                             gchar **prop_value,
                             GError **perror)
{
	gboolean processed = TRUE;

	g_return_val_if_fail (prop_name != NULL, FALSE);
	g_return_val_if_fail (prop_value != NULL, FALSE);

	if (g_str_equal (prop_name, CLIENT_BACKEND_PROPERTY_CAPABILITIES)) {
		ESource *source;
		GString *caps;
		gchar *usermail;

		caps = g_string_new (CAL_STATIC_CAPABILITY_NO_THISANDFUTURE ","
				     CAL_STATIC_CAPABILITY_NO_THISANDPRIOR ","
				     CAL_STATIC_CAPABILITY_REFRESH_SUPPORTED);

		usermail = get_usermail (E_CAL_BACKEND (backend));
		if (!usermail || !*usermail)
			g_string_append (caps, "," CAL_STATIC_CAPABILITY_NO_EMAIL_ALARMS);
		g_free (usermail);

		source = e_backend_get_source (E_BACKEND (backend));
		if (source) {
			const gchar *prop = e_source_get_property (source, "autoschedule");

			if (prop && g_str_equal (prop, "1"))
				g_string_append (caps, "," CAL_STATIC_CAPABILITY_CREATE_MESSAGES
						       "," CAL_STATIC_CAPABILITY_SAVE_SCHEDULES);
		}

		*prop_value = g_string_free (caps, FALSE);
	} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS) ||
		   g_str_equal (prop_name, CAL_BACKEND_PROPERTY_ALARM_EMAIL_ADDRESS)) {
		*prop_value = get_usermail (E_CAL_BACKEND (backend));
	} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_DEFAULT_OBJECT)) {
		ECalComponent *comp;

		comp = e_cal_component_new ();

		switch (e_cal_backend_get_kind (E_CAL_BACKEND (backend))) {
		case ICAL_VEVENT_COMPONENT:
			e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_EVENT);
			break;
		case ICAL_VTODO_COMPONENT:
			e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_TODO);
			break;
		case ICAL_VJOURNAL_COMPONENT:
			e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_JOURNAL);
			break;
		default:
			g_object_unref (comp);
			g_propagate_error (perror, EDC_ERROR (ObjectNotFound));
			return TRUE;
		}

		*prop_value = e_cal_component_get_as_string (comp);
		g_object_unref (comp);
	} else {
		processed = FALSE;
	}

	return processed;
}

static gboolean
initialize_backend (ECalBackendCalDAV *cbdav,
                    GError **perror)
{
	ECalBackend              *backend;
	ESource                  *source;
	const gchar		 *os_val;
	gchar			 *uri;
	gsize                     len;
	const gchar              *refresh;
	const gchar              *cache_dir;

	backend = E_CAL_BACKEND (cbdav);
	cache_dir = e_cal_backend_get_cache_dir (backend);
	source = e_backend_get_source (E_BACKEND (backend));
	uri = e_source_get_uri (source);

	if (!g_signal_handler_find (G_OBJECT (source), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, caldav_source_changed_cb, cbdav))
		g_signal_connect (G_OBJECT (source), "changed", G_CALLBACK (caldav_source_changed_cb), cbdav);

	os_val = e_source_get_property (source, "offline_sync");

	if (!os_val || !g_str_equal (os_val, "1")) {
		cbdav->priv->do_offline = FALSE;
	}

	os_val = e_source_get_property (source, "auth");
	cbdav->priv->auth_required = os_val != NULL;

	os_val = e_source_get_property(source, "ssl");

	g_free (cbdav->priv->uri);
	cbdav->priv->uri = NULL;
	if (g_str_has_prefix (uri, "caldav://")) {
		const gchar *proto;

		if (os_val && os_val[0] == '1') {
			proto = "https://";
		} else {
			proto = "http://";
		}

		cbdav->priv->uri = g_strconcat (proto, uri + 9, NULL);

		g_free (uri);
	} else {
		cbdav->priv->uri = uri;
	}

	if (cbdav->priv->uri) {
		SoupURI *suri = soup_uri_new (cbdav->priv->uri);

		/* properly encode uri */
		if (suri && suri->path) {
			gchar *tmp, *path;

			if (suri->path && strchr (suri->path, '%')) {
				/* If path contains anything already encoded, then decode it first,
				 * thus it'll be managed properly. For example, the '#' in a path
				 * is in URI shown as %23 and not doing this decode makes it being
				 * like %2523, which is not what is wanted here. */
				tmp = soup_uri_decode (suri->path);
				soup_uri_set_path (suri, tmp);
				g_free (tmp);
			}

			tmp = soup_uri_encode (suri->path, NULL);
			path = soup_uri_normalize (tmp, "/");

			soup_uri_set_path (suri, path);

			g_free (tmp);
			g_free (path);
			g_free (cbdav->priv->uri);

			cbdav->priv->uri = soup_uri_to_string (suri, FALSE);
		}

		soup_uri_free (suri);
	}

	g_return_val_if_fail (cbdav->priv->uri != NULL, FALSE);

	/* remove trailing slashes... */
	len = strlen (cbdav->priv->uri);
	while (len--) {
		if (cbdav->priv->uri[len] == '/') {
			cbdav->priv->uri[len] = '\0';
		} else {
			break;
		}
	}

	/* ...and append exactly one slash */
	if (cbdav->priv->uri && *cbdav->priv->uri) {
		gchar *tmp = cbdav->priv->uri;

		cbdav->priv->uri = g_strconcat (cbdav->priv->uri, "/", NULL);

		g_free (tmp);
	}

	if (cbdav->priv->store == NULL) {
		/* remove the old cache while migrating to ECalBackendStore */
		e_cal_backend_cache_remove (cache_dir, "cache.xml");
		cbdav->priv->store = e_cal_backend_file_store_new (cache_dir);

		if (cbdav->priv->store == NULL) {
			g_propagate_error (perror, EDC_ERROR_EX (OtherError, "Cannot create local store"));
			return FALSE;
		}

		e_cal_backend_store_load (cbdav->priv->store);
	}

	/* Set the local attachment store */
	if (g_mkdir_with_parents (cache_dir, 0700) < 0) {
		g_propagate_error (perror, EDC_ERROR_EX (OtherError, "mkdir failed"));
		return FALSE;
	}

	refresh = e_source_get_property (source, "refresh");
	cbdav->priv->refresh_time.tv_sec  = (refresh && atoi (refresh) > 0) ? (60 * atoi (refresh)) : (DEFAULT_REFRESH_TIME);

	if (!cbdav->priv->synch_slave) {
		GThread *slave;

		cbdav->priv->slave_cmd = SLAVE_SHOULD_SLEEP;
		slave = g_thread_create (caldav_synch_slave_loop, cbdav, FALSE, NULL);

		if (slave == NULL) {
			g_propagate_error (perror, EDC_ERROR_EX (OtherError, "Could not create synch slave"));
		}

		cbdav->priv->synch_slave = slave;
	}

	return TRUE;
}

static void
proxy_settings_changed (EProxy *proxy,
                        gpointer user_data)
{
	SoupURI *proxy_uri = NULL;
	ECalBackendCalDAVPrivate *priv = (ECalBackendCalDAVPrivate *) user_data;

	if (!priv || !priv->uri || !priv->session)
		return;

	/* use proxy if necessary */
	if (e_proxy_require_proxy_for_uri (proxy, priv->uri)) {
		proxy_uri = e_proxy_peek_uri_for (proxy, priv->uri);
	}

	g_object_set (priv->session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
}

static void
open_calendar (ECalBackendCalDAV *cbdav,
               GError **error)
{
	gboolean server_unreachable = FALSE;
	GError *local_error = NULL;

	g_return_if_fail (cbdav != NULL);

	/* set forward proxy */
	proxy_settings_changed (cbdav->priv->proxy, cbdav->priv);

	if (caldav_server_open_calendar (cbdav, &server_unreachable, &local_error)) {
		cbdav->priv->slave_cmd = SLAVE_SHOULD_WORK;
		g_cond_signal (cbdav->priv->cond);

		cbdav->priv->is_google = is_google_uri (cbdav->priv->uri);
	} else if (server_unreachable) {
		cbdav->priv->opened = FALSE;
		cbdav->priv->read_only = TRUE;
		if (local_error) {
			gchar *msg = g_strdup_printf (_("Server is unreachable, calendar is opened in read-only mode.\nError message: %s"), local_error->message);
			e_cal_backend_notify_error (E_CAL_BACKEND (cbdav), msg);
			g_free (msg);
			g_clear_error (&local_error);
		}
	} else if (local_error) {
		g_propagate_error (error, local_error);
	}
}

static void
caldav_do_open (ECalBackendSync *backend,
                EDataCal *cal,
                GCancellable *cancellable,
                gboolean only_if_exists,
                GError **perror)
{
	ECalBackendCalDAV        *cbdav;
	gboolean online;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	g_mutex_lock (cbdav->priv->busy_lock);

	/* let it decide the 'getctag' extension availability again */
	cbdav->priv->ctag_supported = TRUE;

	if (!cbdav->priv->loaded && !initialize_backend (cbdav, perror)) {
		g_mutex_unlock (cbdav->priv->busy_lock);
		return;
	}

	online = e_backend_get_online (E_BACKEND (backend));

	if (!cbdav->priv->do_offline && !online) {
		g_mutex_unlock (cbdav->priv->busy_lock);
		g_propagate_error (perror, EDC_ERROR (RepositoryOffline));
		return;
	}

	cbdav->priv->loaded = TRUE;
	cbdav->priv->opened = TRUE;
	cbdav->priv->is_google = FALSE;

	if (online) {
		GError *local_error = NULL;

		open_calendar (cbdav, &local_error);

		if (g_error_matches (local_error, E_DATA_CAL_ERROR, AuthenticationRequired) || g_error_matches (local_error, E_DATA_CAL_ERROR, AuthenticationFailed)) {
			g_clear_error (&local_error);
			e_cal_backend_notify_auth_required (E_CAL_BACKEND (cbdav), TRUE, cbdav->priv->credentials);
		} else {
			e_cal_backend_notify_opened (E_CAL_BACKEND (backend), NULL);
		}

		if (local_error)
			g_propagate_error (perror, local_error);
	} else {
		cbdav->priv->read_only = TRUE;
		e_cal_backend_notify_opened (E_CAL_BACKEND (backend), NULL);
	}

	e_cal_backend_notify_readonly (E_CAL_BACKEND (backend), cbdav->priv->read_only);
	e_cal_backend_notify_online (E_CAL_BACKEND (backend), online);

	g_mutex_unlock (cbdav->priv->busy_lock);
}

static void
caldav_authenticate_user (ECalBackendSync *backend,
                          GCancellable *cancellable,
                          ECredentials *credentials,
                          GError **error)
{
	ECalBackendCalDAV        *cbdav;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	g_mutex_lock (cbdav->priv->busy_lock);

	e_credentials_free (cbdav->priv->credentials);
	cbdav->priv->credentials = NULL;

	if (!credentials || !e_credentials_has_key (credentials, E_CREDENTIALS_KEY_USERNAME)) {
		g_mutex_unlock (cbdav->priv->busy_lock);
		g_propagate_error (error, EDC_ERROR (AuthenticationRequired));
		return;
	}

	cbdav->priv->credentials = e_credentials_new_clone (credentials);

	open_calendar (cbdav, error);

	g_mutex_unlock (cbdav->priv->busy_lock);
}

static void
caldav_refresh (ECalBackendSync *backend,
                EDataCal *cal,
                GCancellable *cancellable,
                GError **perror)
{
	ECalBackendCalDAV        *cbdav;
	gboolean                  online;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	g_mutex_lock (cbdav->priv->busy_lock);

	if (!cbdav->priv->loaded
	    || cbdav->priv->slave_cmd == SLAVE_SHOULD_DIE
	    || !check_state (cbdav, &online, NULL)
	    || !online) {
		g_mutex_unlock (cbdav->priv->busy_lock);
		return;
	}

	cbdav->priv->slave_cmd = SLAVE_SHOULD_WORK;

	/* wake it up */
	g_cond_signal (cbdav->priv->cond);
	g_mutex_unlock (cbdav->priv->busy_lock);
}

static void
caldav_remove (ECalBackendSync *backend,
               EDataCal *cal,
               GCancellable *cancellable,
               GError **perror)
{
	ECalBackendCalDAV        *cbdav;
	gboolean                  online;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	/* first tell it to die, then wait for its lock */
	cbdav->priv->slave_cmd = SLAVE_SHOULD_DIE;

	g_mutex_lock (cbdav->priv->busy_lock);

	if (!cbdav->priv->loaded) {
		g_mutex_unlock (cbdav->priv->busy_lock);
		return;
	}

	if (!check_state (cbdav, &online, NULL)) {
		/* lie here a bit, but otherwise the calendar will not be removed, even it should */
		g_print (G_STRLOC ": Failed to check state");
	}

	e_cal_backend_store_remove (cbdav->priv->store);
	cbdav->priv->store = NULL;
	cbdav->priv->loaded = FALSE;
	cbdav->priv->opened = FALSE;

	if (cbdav->priv->synch_slave) {
		g_cond_signal (cbdav->priv->cond);

		/* wait until the slave died */
		g_cond_wait (cbdav->priv->slave_gone_cond, cbdav->priv->busy_lock);
	}

	g_mutex_unlock (cbdav->priv->busy_lock);
}

static void
remove_comp_from_cache_cb (gpointer value,
                           gpointer user_data)
{
	ECalComponent *comp = value;
	ECalBackendStore *store = user_data;
	ECalComponentId *id;

	g_return_if_fail (comp != NULL);
	g_return_if_fail (store != NULL);

	id = e_cal_component_get_id (comp);
	g_return_if_fail (id != NULL);

	e_cal_backend_store_remove_component (store, id->uid, id->rid);
	e_cal_component_free_id (id);
}

static gboolean
remove_comp_from_cache (ECalBackendCalDAV *cbdav,
                        const gchar *uid,
                        const gchar *rid)
{
	gboolean res = FALSE;

	if (!rid || !*rid) {
		/* get with detached instances */
		GSList *objects = e_cal_backend_store_get_components_by_uid (cbdav->priv->store, uid);

		if (objects) {
			g_slist_foreach (objects, (GFunc) remove_comp_from_cache_cb, cbdav->priv->store);
			g_slist_foreach (objects, (GFunc) g_object_unref, NULL);
			g_slist_free (objects);

			res = TRUE;
		}
	} else {
		res = e_cal_backend_store_remove_component (cbdav->priv->store, uid, rid);
	}

	return res;
}

static void
add_detached_recur_to_vcalendar_cb (gpointer value,
                                    gpointer user_data)
{
	icalcomponent *recurrence = e_cal_component_get_icalcomponent (value);
	icalcomponent *vcalendar = user_data;

	icalcomponent_add_component (
		vcalendar,
		icalcomponent_new_clone (recurrence));
}

static gint
sort_master_first (gconstpointer a,
                   gconstpointer b)
{
	icalcomponent *ca, *cb;

	ca = e_cal_component_get_icalcomponent ((ECalComponent *) a);
	cb = e_cal_component_get_icalcomponent ((ECalComponent *) b);

	if (!ca) {
		if (!cb)
			return 0;
		else
			return -1;
	} else if (!cb) {
		return 1;
	}

	return icaltime_compare (icalcomponent_get_recurrenceid (ca), icalcomponent_get_recurrenceid (cb));
}

/* Returns new icalcomponent, with all detached instances stored in a cache.
 * The cache lock should be locked when called this function.
*/
static icalcomponent *
get_comp_from_cache (ECalBackendCalDAV *cbdav,
                     const gchar *uid,
                     const gchar *rid,
                     gchar **href,
                     gchar **etag)
{
	icalcomponent *icalcomp = NULL;

	if (rid == NULL || !*rid) {
		/* get with detached instances */
		GSList *objects = e_cal_backend_store_get_components_by_uid (cbdav->priv->store, uid);

		if (!objects) {
			return NULL;
		}

		if (g_slist_length (objects) == 1) {
			ECalComponent *comp = objects->data;

			/* will be unreffed a bit later */
			if (comp)
				icalcomp = icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp));
		} else {
			/* if we have detached recurrences, return a VCALENDAR */
			icalcomp = e_cal_util_new_top_level ();

			objects = g_slist_sort (objects, sort_master_first);

			/* add all detached recurrences and the master object */
			g_slist_foreach (objects, add_detached_recur_to_vcalendar_cb, icalcomp);
		}

		/* every component has set same href and etag, thus it doesn't matter where it will be read */
		if (href)
			*href = ecalcomp_get_href (objects->data);
		if (etag)
			*etag = ecalcomp_get_etag (objects->data);

		g_slist_foreach (objects, (GFunc) g_object_unref, NULL);
		g_slist_free (objects);
	} else {
		/* get the exact object */
		ECalComponent *comp = e_cal_backend_store_get_component (cbdav->priv->store, uid, rid);

		if (comp) {
			icalcomp = icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp));
			if (href)
				*href = ecalcomp_get_href (comp);
			if (etag)
				*etag = ecalcomp_get_etag (comp);
			g_object_unref (comp);
		}
	}

	return icalcomp;
}

static gboolean
put_comp_to_cache (ECalBackendCalDAV *cbdav,
                   icalcomponent *icalcomp,
                   const gchar *href,
                   const gchar *etag)
{
	icalcomponent_kind my_kind;
	ECalComponent *comp;
	gboolean res = FALSE;

	g_return_val_if_fail (cbdav != NULL, FALSE);
	g_return_val_if_fail (icalcomp != NULL, FALSE);

	my_kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbdav));
	comp = e_cal_component_new ();

	if (icalcomponent_isa (icalcomp) == ICAL_VCALENDAR_COMPONENT) {
		icalcomponent *subcomp;

		/* remove all old components from the cache first */
		for (subcomp = icalcomponent_get_first_component (icalcomp, my_kind);
		     subcomp;
		     subcomp = icalcomponent_get_next_component (icalcomp, my_kind)) {
			remove_comp_from_cache (cbdav, icalcomponent_get_uid (subcomp), NULL);
		}

		/* then put new. It's because some detached instances could be removed on the server. */
		for (subcomp = icalcomponent_get_first_component (icalcomp, my_kind);
		     subcomp;
		     subcomp = icalcomponent_get_next_component (icalcomp, my_kind)) {
			/* because reusing the same comp doesn't clear recur_id member properly */
			g_object_unref (comp);
			comp = e_cal_component_new ();

			if (e_cal_component_set_icalcomponent (comp, icalcomponent_new_clone (subcomp))) {
				if (href)
					ecalcomp_set_href (comp, href);
				if (etag)
					ecalcomp_set_etag (comp, etag);

				if (put_component_to_store (cbdav, comp))
					res = TRUE;
			}
		}
	} else if (icalcomponent_isa (icalcomp) == my_kind) {
		remove_comp_from_cache (cbdav, icalcomponent_get_uid (icalcomp), NULL);

		if (e_cal_component_set_icalcomponent (comp, icalcomponent_new_clone (icalcomp))) {
			if (href)
				ecalcomp_set_href (comp, href);
			if (etag)
				ecalcomp_set_etag (comp, etag);

			res = put_component_to_store (cbdav, comp);
		}
	}

	g_object_unref (comp);

	return res;
}

static void
remove_property (gpointer prop,
                 gpointer icomp)
{
	icalcomponent_remove_property (icomp, prop);
	icalproperty_free (prop);
}

static void
strip_unneeded_x_props (icalcomponent *icomp)
{
	icalproperty *prop;
	GSList *to_remove = NULL;

	g_return_if_fail (icomp != NULL);
	g_return_if_fail (icalcomponent_isa (icomp) != ICAL_VCALENDAR_COMPONENT);

	for (prop = icalcomponent_get_first_property (icomp, ICAL_X_PROPERTY);
	     prop;
	     prop = icalcomponent_get_next_property (icomp, ICAL_X_PROPERTY)) {
		if (g_str_has_prefix (icalproperty_get_x_name (prop), X_E_CALDAV)) {
			to_remove = g_slist_prepend (to_remove, prop);
		}
	}

	for (prop = icalcomponent_get_first_property (icomp, ICAL_XLICERROR_PROPERTY);
	     prop;
	     prop = icalcomponent_get_next_property (icomp, ICAL_XLICERROR_PROPERTY)) {
		to_remove = g_slist_prepend (to_remove, prop);
	}

	g_slist_foreach (to_remove, remove_property, icomp);
	g_slist_free (to_remove);
}

static gboolean
is_stored_on_server (ECalBackendCalDAV *cbdav,
                     const gchar *uri)
{
	SoupURI *my_uri, *test_uri;
	gboolean res;

	g_return_val_if_fail (cbdav != NULL, FALSE);
	g_return_val_if_fail (cbdav->priv != NULL, FALSE);
	g_return_val_if_fail (cbdav->priv->uri != NULL, FALSE);
	g_return_val_if_fail (uri != NULL, FALSE);

	my_uri = soup_uri_new (cbdav->priv->uri);
	g_return_val_if_fail (my_uri != NULL, FALSE);

	test_uri = soup_uri_new (uri);
	if (!test_uri) {
		soup_uri_free (my_uri);
		return FALSE;
	}

	res = my_uri->host && test_uri->host && g_ascii_strcasecmp (my_uri->host, test_uri->host) == 0;

	soup_uri_free (my_uri);
	soup_uri_free (test_uri);

	return res;
}

static void
convert_to_inline_attachment (ECalBackendCalDAV *cbdav,
                              icalcomponent *icalcomp)
{
	icalcomponent *cclone;
	icalproperty *p;
	GSList *to_remove = NULL;

	g_return_if_fail (icalcomp != NULL);

	cclone = icalcomponent_new_clone (icalcomp);

	/* Remove local url attachments first */
	for (p = icalcomponent_get_first_property (icalcomp, ICAL_ATTACH_PROPERTY);
	     p;
	     p = icalcomponent_get_next_property (icalcomp, ICAL_ATTACH_PROPERTY)) {
		icalattach *attach;

		attach = icalproperty_get_attach ((const icalproperty *) p);
		if (icalattach_get_is_url (attach)) {
			const gchar *url;

			url = icalattach_get_url (attach);
			if (g_str_has_prefix (url, LOCAL_PREFIX))
				to_remove = g_slist_prepend (to_remove, p);
		}
	}
	g_slist_foreach (to_remove, remove_property, icalcomp);
	g_slist_free (to_remove);

	/* convert local url attachments to inline attachments now */
	for (p = icalcomponent_get_first_property (cclone, ICAL_ATTACH_PROPERTY);
	     p;
	     p = icalcomponent_get_next_property (cclone, ICAL_ATTACH_PROPERTY)) {
		icalattach *attach;
		GFile *file;
		GError *error = NULL;
		const gchar *uri;
		gchar *basename;
		gchar *content;
		gsize len;

		attach = icalproperty_get_attach ((const icalproperty *) p);
		if (!icalattach_get_is_url (attach))
			continue;

		uri = icalattach_get_url (attach);
		if (!g_str_has_prefix (uri, LOCAL_PREFIX))
			continue;

		file = g_file_new_for_uri (uri);
		basename = g_file_get_basename (file);
		if (g_file_load_contents (file, NULL, &content, &len, NULL, &error)) {
			icalproperty *prop;
			icalparameter *param;
			gchar *encoded;

			/*
			 * do a base64 encoding so it can
			 * be embedded in a soap message
			 */
			encoded = g_base64_encode ((guchar *) content, len);
			attach = icalattach_new_from_data (encoded, NULL, NULL);
			g_free (content);
			g_free (encoded);

			prop = icalproperty_new_attach (attach);
			icalattach_unref (attach);

			param = icalparameter_new_value (ICAL_VALUE_BINARY);
			icalproperty_add_parameter (prop, param);

			param = icalparameter_new_encoding (ICAL_ENCODING_BASE64);
			icalproperty_add_parameter (prop, param);

			param = icalparameter_new_x (basename);
			icalparameter_set_xname (param, X_E_CALDAV_ATTACHMENT_NAME);
			icalproperty_add_parameter (prop, param);

			icalcomponent_add_property (icalcomp, prop);
		} else {
			g_warning ("%s\n", error->message);
			g_clear_error (&error);
		}
		g_free (basename);
		g_object_unref (file);
	}
	icalcomponent_free (cclone);
}

static void
convert_to_url_attachment (ECalBackendCalDAV *cbdav,
                           icalcomponent *icalcomp)
{
	ECalBackend *backend;
	GSList *to_remove = NULL, *to_remove_after_download = NULL;
	icalcomponent *cclone;
	icalproperty *p;
	gint fileindex;

	g_return_if_fail (cbdav != NULL);
	g_return_if_fail (icalcomp != NULL);

	backend = E_CAL_BACKEND (cbdav);
	cclone = icalcomponent_new_clone (icalcomp);

	/* Remove all inline attachments first */
	for (p = icalcomponent_get_first_property (icalcomp, ICAL_ATTACH_PROPERTY);
	     p;
	     p = icalcomponent_get_next_property (icalcomp, ICAL_ATTACH_PROPERTY)) {
		icalattach *attach;

		attach = icalproperty_get_attach ((const icalproperty *) p);
		if (!icalattach_get_is_url (attach))
			to_remove = g_slist_prepend (to_remove, p);
		else if (is_stored_on_server (cbdav, icalattach_get_url (attach)))
			to_remove_after_download = g_slist_prepend (to_remove_after_download, p);
	}
	g_slist_foreach (to_remove, remove_property, icalcomp);
	g_slist_free (to_remove);

	/* convert inline attachments to url attachments now */
	for (p = icalcomponent_get_first_property (cclone, ICAL_ATTACH_PROPERTY), fileindex = 0;
	     p;
	     p = icalcomponent_get_next_property (cclone, ICAL_ATTACH_PROPERTY), fileindex++) {
		icalattach *attach;
		gsize len = -1;
		gchar *decoded = NULL;
		gchar *basename, *local_filename;

		attach = icalproperty_get_attach ((const icalproperty *) p);
		if (icalattach_get_is_url (attach)) {
			const gchar *attach_url = icalattach_get_url (attach);
			GError *error = NULL;

			if (!is_stored_on_server (cbdav, attach_url))
				continue;

			if (!caldav_server_download_attachment (cbdav, attach_url, &decoded, &len, &error)) {
				if (caldav_debug_show (DEBUG_ATTACHMENTS))
					g_print ("CalDAV::%s: Failed to download from a server: %s\n", G_STRFUNC, error ? error->message : "Unknown error");
				continue;
			}
		}

		basename = icalproperty_get_parameter_as_string_r (p, X_E_CALDAV_ATTACHMENT_NAME);
		local_filename = e_cal_backend_create_cache_filename (backend, icalcomponent_get_uid (icalcomp), basename, fileindex);
		g_free (basename);

		if (local_filename) {
			GError *error = NULL;

			if (decoded == NULL) {
				gchar *content;

				content = (gchar *) icalattach_get_data (attach);
				decoded = (gchar *) g_base64_decode (content, &len);
			}

			if (g_file_set_contents (local_filename, decoded, len, &error)) {
				icalproperty *prop;
				gchar *url;

				url = g_filename_to_uri (local_filename, NULL, NULL);
				attach = icalattach_new_from_url (url);
				prop = icalproperty_new_attach (attach);
				icalattach_unref (attach);
				icalcomponent_add_property (icalcomp, prop);
				g_free (url);
			} else {
				g_warning ("%s\n", error->message);
				g_clear_error (&error);
			}

			g_free (local_filename);
		}
	}

	icalcomponent_free (cclone);

	g_slist_foreach (to_remove_after_download, remove_property, icalcomp);
	g_slist_free (to_remove_after_download);
}

static void
remove_files (const gchar *dir,
              const gchar *fileprefix)
{
	GDir *d;

	g_return_if_fail (dir != NULL);
	g_return_if_fail (fileprefix != NULL);
	g_return_if_fail (*fileprefix != '\0');

	d = g_dir_open (dir, 0, NULL);
	if (d) {
		const gchar *entry;
		gint len = strlen (fileprefix);

		while ((entry = g_dir_read_name (d)) != NULL) {
			if (entry && strncmp (entry, fileprefix, len) == 0) {
				gchar *path;

				path = g_build_filename (dir, entry, NULL);
				if (!g_file_test (path, G_FILE_TEST_IS_DIR))
					g_unlink (path);
				g_free (path);
			}
		}
		g_dir_close (d);
	}
}

static void
remove_cached_attachment (ECalBackendCalDAV *cbdav,
                          const gchar *uid)
{
	GSList *l;
	guint len;
	gchar *dir;
	gchar *fileprefix;

	g_return_if_fail (cbdav != NULL);
	g_return_if_fail (uid != NULL);

	l = e_cal_backend_store_get_components_by_uid (cbdav->priv->store, uid);
	len = g_slist_length (l);
	g_slist_foreach (l, (GFunc) g_object_unref, NULL);
	g_slist_free (l);
	if (len > 0)
		return;

	dir = e_cal_backend_create_cache_filename (E_CAL_BACKEND (cbdav), uid, "a", 0);
	if (!dir)
		return;

	fileprefix = g_strrstr (dir, G_DIR_SEPARATOR_S);
	if (fileprefix) {
		*fileprefix = '\0';
		fileprefix++;

		if (*fileprefix)
			fileprefix[strlen (fileprefix) - 1] = '\0';

		remove_files (dir, fileprefix);
	}

	g_free (dir);
}

/* callback for icalcomponent_foreach_tzid */
typedef struct {
	ECalBackendStore *store;
	icalcomponent *vcal_comp;
	icalcomponent *icalcomp;
} ForeachTzidData;

static void
add_timezone_cb (icalparameter *param,
                 gpointer data)
{
	icaltimezone *tz;
	const gchar *tzid;
	icalcomponent *vtz_comp;
	ForeachTzidData *f_data = (ForeachTzidData *) data;

	tzid = icalparameter_get_tzid (param);
	if (!tzid)
		return;

	tz = icalcomponent_get_timezone (f_data->vcal_comp, tzid);
	if (tz)
		return;

	tz = icalcomponent_get_timezone (f_data->icalcomp, tzid);
	if (!tz) {
		tz = icaltimezone_get_builtin_timezone_from_tzid (tzid);
		if (!tz)
			tz = (icaltimezone *) e_cal_backend_store_get_timezone (f_data->store, tzid);
		if (!tz)
			return;
	}

	vtz_comp = icaltimezone_get_component (tz);
	if (!vtz_comp)
		return;

	icalcomponent_add_component (f_data->vcal_comp,
				     icalcomponent_new_clone (vtz_comp));
}

static void
add_timezones_from_component (ECalBackendCalDAV *cbdav,
                              icalcomponent *vcal_comp,
                              icalcomponent *icalcomp)
{
	ForeachTzidData f_data;

	g_return_if_fail (cbdav != NULL);
	g_return_if_fail (vcal_comp != NULL);
	g_return_if_fail (icalcomp != NULL);

	f_data.store = cbdav->priv->store;
	f_data.vcal_comp = vcal_comp;
	f_data.icalcomp = icalcomp;

	icalcomponent_foreach_tzid (icalcomp, add_timezone_cb, &f_data);
}

/* also removes X-EVOLUTION-CALDAV from all the components */
static gchar *
pack_cobj (ECalBackendCalDAV *cbdav,
           icalcomponent *icomp)
{
	icalcomponent *calcomp;
	gchar          *objstr;

	if (icalcomponent_isa (icomp) != ICAL_VCALENDAR_COMPONENT) {
		icalcomponent *cclone;

		calcomp = e_cal_util_new_top_level ();

		cclone = icalcomponent_new_clone (icomp);
		strip_unneeded_x_props (cclone);
		convert_to_inline_attachment (cbdav, cclone);
		icalcomponent_add_component (calcomp, cclone);
		add_timezones_from_component (cbdav, calcomp, cclone);
	} else {
		icalcomponent *subcomp;
		icalcomponent_kind my_kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbdav));

		calcomp = icalcomponent_new_clone (icomp);
		for (subcomp = icalcomponent_get_first_component (calcomp, my_kind);
		     subcomp;
		     subcomp = icalcomponent_get_next_component (calcomp, my_kind)) {
			strip_unneeded_x_props (subcomp);
			convert_to_inline_attachment (cbdav, subcomp);
			add_timezones_from_component (cbdav, calcomp, subcomp);
		}
	}

	objstr = icalcomponent_as_ical_string_r (calcomp);
	icalcomponent_free (calcomp);

	g_assert (objstr);

	return objstr;

}

static void
sanitize_component (ECalBackend *cb,
                    ECalComponent *comp)
{
	ECalComponentDateTime dt;
	icaltimezone *zone;

	/* Check dtstart, dtend and due's timezone, and convert it to local
	 * default timezone if the timezone is not in our builtin timezone
	 * list */
	e_cal_component_get_dtstart (comp, &dt);
	if (dt.value && dt.tzid) {
		zone = caldav_internal_get_timezone (cb, dt.tzid);
		if (!zone) {
			g_free ((gchar *) dt.tzid);
			dt.tzid = g_strdup ("UTC");
			e_cal_component_set_dtstart (comp, &dt);
		}
	}
	e_cal_component_free_datetime (&dt);

	e_cal_component_get_dtend (comp, &dt);
	if (dt.value && dt.tzid) {
		zone = caldav_internal_get_timezone (cb, dt.tzid);
		if (!zone) {
			g_free ((gchar *) dt.tzid);
			dt.tzid = g_strdup ("UTC");
			e_cal_component_set_dtend (comp, &dt);
		}
	}
	e_cal_component_free_datetime (&dt);

	e_cal_component_get_due (comp, &dt);
	if (dt.value && dt.tzid) {
		zone = caldav_internal_get_timezone (cb, dt.tzid);
		if (!zone) {
			g_free ((gchar *) dt.tzid);
			dt.tzid = g_strdup ("UTC");
			e_cal_component_set_due (comp, &dt);
		}
	}
	e_cal_component_free_datetime (&dt);
	e_cal_component_abort_sequence (comp);
}

static gboolean
cache_contains (ECalBackendCalDAV *cbdav,
                const gchar *uid,
                const gchar *rid)
{
	gboolean res;
	ECalComponent *comp;

	g_return_val_if_fail (cbdav != NULL, FALSE);
	g_return_val_if_fail (uid != NULL, FALSE);

	g_return_val_if_fail (cbdav->priv->store != NULL, FALSE);

	comp = e_cal_backend_store_get_component (cbdav->priv->store, uid, rid);
	res = comp != NULL;

	if (comp)
		g_object_unref (comp);

	return res;
}

/* Returns subcomponent of icalcomp, which is a master object, or icalcomp itself, if it's not a VCALENDAR;
 * Do not free returned pointer, it'll be freed together with the icalcomp.
*/
static icalcomponent *
get_master_comp (ECalBackendCalDAV *cbdav,
                 icalcomponent *icalcomp)
{
	icalcomponent *master = icalcomp;

	g_return_val_if_fail (cbdav != NULL, NULL);
	g_return_val_if_fail (icalcomp != NULL, NULL);

	if (icalcomponent_isa (icalcomp) == ICAL_VCALENDAR_COMPONENT) {
		icalcomponent *subcomp;
		icalcomponent_kind my_kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbdav));

		master = NULL;

		for (subcomp = icalcomponent_get_first_component (icalcomp, my_kind);
		     subcomp;
		     subcomp = icalcomponent_get_next_component (icalcomp, my_kind)) {
			struct icaltimetype sub_rid = icalcomponent_get_recurrenceid (subcomp);

			if (icaltime_is_null_time (sub_rid)) {
				master = subcomp;
				break;
			}
		}
	}

	return master;
}

static gboolean
remove_instance (ECalBackendCalDAV *cbdav,
                 icalcomponent *icalcomp,
                 struct icaltimetype rid,
                 CalObjModType mod,
                 gboolean also_exdate)
{
	icalcomponent *master = icalcomp;
	gboolean res = FALSE;

	g_return_val_if_fail (icalcomp != NULL, res);
	g_return_val_if_fail (!icaltime_is_null_time (rid), res);

	/* remove an instance only */
	if (icalcomponent_isa (icalcomp) == ICAL_VCALENDAR_COMPONENT) {
		icalcomponent *subcomp;
		icalcomponent_kind my_kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbdav));
		gint left = 0;
		gboolean start_first = FALSE;

		master = NULL;

		/* remove old instance first */
		for (subcomp = icalcomponent_get_first_component (icalcomp, my_kind);
		     subcomp;
		     subcomp = start_first ? icalcomponent_get_first_component (icalcomp, my_kind) : icalcomponent_get_next_component (icalcomp, my_kind)) {
			struct icaltimetype sub_rid = icalcomponent_get_recurrenceid (subcomp);

			start_first = FALSE;

			if (icaltime_is_null_time (sub_rid)) {
				master = subcomp;
				left++;
			} else if (icaltime_compare (sub_rid, rid) == 0) {
				icalcomponent_remove_component (icalcomp, subcomp);
				icalcomponent_free (subcomp);
				if (master) {
					break;
				} else {
					/* either no master or master not as the first component, thus rescan */
					left = 0;
					start_first = TRUE;
				}
			} else {
				left++;
			}
		}

		/* whether left at least one instance or a master object */
		res = left > 0;
	} else {
		res = TRUE;
	}

	if (master && also_exdate) {
		e_cal_util_remove_instances (master, rid, mod);
	}

	return res;
}

static icalcomponent *
replace_master (ECalBackendCalDAV *cbdav,
                icalcomponent *old_comp,
                icalcomponent *new_master)
{
	icalcomponent *old_master;
	if (icalcomponent_isa (old_comp) != ICAL_VCALENDAR_COMPONENT) {
		icalcomponent_free (old_comp);
		return new_master;
	}

	old_master = get_master_comp (cbdav, old_comp);
	if (!old_master) {
		/* no master, strange */
		icalcomponent_free (new_master);
	} else {
		icalcomponent_remove_component (old_comp, old_master);
		icalcomponent_free (old_master);

		icalcomponent_add_component (old_comp, new_master);
	}

	return old_comp;
}

/* the resulting component should be unreffed when done with it;
 * the fallback_comp is cloned, if used */
static ECalComponent *
get_ecalcomp_master_from_cache_or_fallback (ECalBackendCalDAV *cbdav,
                                            const gchar *uid,
                                            const gchar *rid,
                                            ECalComponent *fallback_comp)
{
	ECalComponent *comp = NULL;
	icalcomponent *icalcomp;

	g_return_val_if_fail (cbdav != NULL, NULL);
	g_return_val_if_fail (uid != NULL, NULL);

	icalcomp = get_comp_from_cache (cbdav, uid, rid, NULL, NULL);
	if (icalcomp) {
		icalcomponent *master = get_master_comp (cbdav, icalcomp);

		if (master) {
			comp = e_cal_component_new_from_icalcomponent (icalcomponent_new_clone (master));
		}

		icalcomponent_free (icalcomp);
	}

	if (!comp && fallback_comp)
		comp = e_cal_component_clone (fallback_comp);

	return comp;
}

/* a busy_lock is supposed to be locked already, when calling this function */
static void
do_create_object (ECalBackendCalDAV *cbdav,
                  const gchar *in_calobj,
                  gchar **uid,
                  ECalComponent **new_component,
                  GError **perror)
{
	ECalComponent            *comp;
	gboolean                  online, did_put = FALSE;
	struct icaltimetype current;
	icalcomponent *icalcomp;
	const gchar *comp_uid;

	if (!check_state (cbdav, &online, perror))
		return;

	comp = e_cal_component_new_from_string (in_calobj);

	if (comp == NULL) {
		g_propagate_error (perror, EDC_ERROR (InvalidObject));
		return;
	}

	icalcomp = e_cal_component_get_icalcomponent (comp);
	if (icalcomp == NULL) {
		g_object_unref (comp);
		g_propagate_error (perror, EDC_ERROR (InvalidObject));
		return;
	}

	comp_uid = icalcomponent_get_uid (icalcomp);
	if (!comp_uid) {
		gchar *new_uid;

		new_uid = e_cal_component_gen_uid ();
		if (!new_uid) {
			g_object_unref (comp);
			g_propagate_error (perror, EDC_ERROR (InvalidObject));
			return;
		}

		icalcomponent_set_uid (icalcomp, new_uid);
		comp_uid = icalcomponent_get_uid (icalcomp);

		g_free (new_uid);
	}

	/* check the object is not in our cache */
	if (cache_contains (cbdav, comp_uid, NULL)) {
		g_object_unref (comp);
		g_propagate_error (perror, EDC_ERROR (ObjectIdAlreadyExists));
		return;
	}

	/* Set the created and last modified times on the component */
	current = icaltime_current_time_with_zone (icaltimezone_get_utc_timezone ());
	e_cal_component_set_created (comp, &current);
	e_cal_component_set_last_modified (comp, &current);

	/* sanitize the component*/
	sanitize_component ((ECalBackend *) cbdav, comp);

	if (online) {
		CalDAVObject object;

		object.href  = ecalcomp_gen_href (comp);
		object.etag  = NULL;
		object.cdata = pack_cobj (cbdav, icalcomp);

		did_put = caldav_server_put_object (cbdav, &object, icalcomp, perror);

		caldav_object_free (&object, FALSE);
	} else {
		/* mark component as out of synch */
		/*ecalcomp_set_synch_state (comp, ECALCOMP_LOCALLY_CREATED); */
	}

	if (did_put) {
		if (uid)
			*uid = g_strdup (comp_uid);

		if (new_component)
			*new_component = get_ecalcomp_master_from_cache_or_fallback (cbdav, comp_uid, NULL, comp);
	}

	g_object_unref (comp);
}

/* a busy_lock is supposed to be locked already, when calling this function */
static void
do_modify_object (ECalBackendCalDAV *cbdav,
                  const gchar *calobj,
                  CalObjModType mod,
                  ECalComponent **old_component,
                  ECalComponent **new_component,
                  GError **error)
{
	ECalComponent            *comp;
	icalcomponent            *cache_comp;
	gboolean                  online, did_put = FALSE;
	ECalComponentId		 *id;
	struct icaltimetype current;
	gchar *href = NULL, *etag = NULL;

	if (new_component)
		*new_component = NULL;

	if (!check_state (cbdav, &online, error))
		return;

	comp = e_cal_component_new_from_string (calobj);

	if (comp == NULL) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	if (!e_cal_component_get_icalcomponent (comp) ||
	    icalcomponent_isa (e_cal_component_get_icalcomponent (comp)) != e_cal_backend_get_kind (E_CAL_BACKEND (cbdav))) {
		g_object_unref (comp);
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	/* Set the last modified time on the component */
	current = icaltime_current_time_with_zone (icaltimezone_get_utc_timezone ());
	e_cal_component_set_last_modified (comp, &current);

	/* sanitize the component */
	sanitize_component ((ECalBackend *) cbdav, comp);

	id = e_cal_component_get_id (comp);
	e_return_data_cal_error_if_fail (id != NULL, InvalidObject);

	/* fetch full component from cache, it will be pushed to the server */
	cache_comp = get_comp_from_cache (cbdav, id->uid, NULL, &href, &etag);

	if (cache_comp == NULL) {
		e_cal_component_free_id (id);
		g_object_unref (comp);
		g_free (href);
		g_free (etag);
		g_propagate_error (error, EDC_ERROR (ObjectNotFound));
		return;
	}

	if (!online) {
		/* mark component as out of synch */
		/*ecalcomp_set_synch_state (comp, ECALCOMP_LOCALLY_MODIFIED);*/
	}

	if (old_component) {
		*old_component = NULL;

		if (e_cal_component_is_instance (comp)) {
			/* set detached instance as the old object, if any */
			ECalComponent *old_instance = e_cal_backend_store_get_component (cbdav->priv->store, id->uid, id->rid);

			/* This will give a reference to 'old_component' */
			if (old_instance) {
				*old_component = e_cal_component_clone (old_instance);
				g_object_unref (old_instance);
			}
		}

		if (!*old_component) {
			icalcomponent *master = get_master_comp (cbdav, cache_comp);

			if (master) {
				/* set full component as the old object */
				*old_component = e_cal_component_new_from_icalcomponent (icalcomponent_new_clone (master));
			}
		}
	}

	switch (mod) {
	case CALOBJ_MOD_ONLY_THIS:
	case CALOBJ_MOD_THIS:
		if (e_cal_component_is_instance (comp)) {
			icalcomponent *new_comp = e_cal_component_get_icalcomponent (comp);

			/* new object is only this instance */
			if (new_component)
				*new_component = e_cal_component_clone (comp);

			/* add the detached instance */
			if (icalcomponent_isa (cache_comp) == ICAL_VCALENDAR_COMPONENT) {
				/* do not modify the EXDATE, as the component will be put back */
				remove_instance (cbdav, cache_comp, icalcomponent_get_recurrenceid (new_comp), mod, FALSE);
			} else {
				/* this is only a master object, thus make is a VCALENDAR component */
				icalcomponent *icomp;

				icomp = e_cal_util_new_top_level ();
				icalcomponent_add_component (icomp, cache_comp);

				/* no need to free the cache_comp, as it is inside icomp */
				cache_comp = icomp;
			}

			if (cache_comp && cbdav->priv->is_google) {
				icalcomponent_set_sequence (cache_comp, icalcomponent_get_sequence (cache_comp) + 1);
				icalcomponent_set_sequence (new_comp, icalcomponent_get_sequence (new_comp) + 1);
			}

			/* add the detached instance finally */
			icalcomponent_add_component (cache_comp, icalcomponent_new_clone (new_comp));
		} else {
			cache_comp = replace_master (cbdav, cache_comp, icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp)));
		}
		break;
	case CALOBJ_MOD_ALL:
		cache_comp = replace_master (cbdav, cache_comp, icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp)));
		break;
	case CALOBJ_MOD_THISANDPRIOR:
	case CALOBJ_MOD_THISANDFUTURE:
		break;
	}

	if (online) {
		CalDAVObject object;

		object.href  = href;
		object.etag  = etag;
		object.cdata = pack_cobj (cbdav, cache_comp);

		did_put = caldav_server_put_object (cbdav, &object, cache_comp, error);

		caldav_object_free (&object, FALSE);
		href = NULL;
		etag = NULL;
	} else {
		/* mark component as out of synch */
		/*ecalcomp_set_synch_state (comp, ECALCOMP_LOCALLY_MODIFIED);*/
	}

	if (did_put) {
		if (new_component && !*new_component) {
			/* read the comp from cache again, as some servers can modify it on put */
			*new_component = get_ecalcomp_master_from_cache_or_fallback (cbdav, id->uid, id->rid, NULL);

			g_warn_if_fail (*new_component != NULL);
		}
	}

	e_cal_component_free_id (id);
	icalcomponent_free (cache_comp);
	g_object_unref (comp);
	g_free (href);
	g_free (etag);
}

/* a busy_lock is supposed to be locked already, when calling this function */
static void
do_remove_object (ECalBackendCalDAV *cbdav,
                  const gchar *uid,
                  const gchar *rid,
                  CalObjModType mod,
                  ECalComponent **old_component,
                  ECalComponent **new_component,
                  GError **perror)
{
	icalcomponent            *cache_comp;
	gboolean                  online;
	gchar *href = NULL, *etag = NULL;

	if (new_component)
		*new_component = NULL;

	if (!check_state (cbdav, &online, perror))
		return;

	cache_comp = get_comp_from_cache (cbdav, uid, NULL, &href, &etag);

	if (cache_comp == NULL) {
		g_propagate_error (perror, EDC_ERROR (ObjectNotFound));
		return;
	}

	if (old_component) {
		ECalComponent *old = e_cal_backend_store_get_component (cbdav->priv->store, uid, rid);

		if (old) {
			*old_component = e_cal_component_clone (old);
			g_object_unref (old);
		} else {
			icalcomponent *master = get_master_comp (cbdav, cache_comp);

			if (master) {
				*old_component = e_cal_component_new_from_icalcomponent (icalcomponent_new_clone (master));
			}
		}
	}

	switch (mod) {
	case CALOBJ_MOD_ONLY_THIS:
	case CALOBJ_MOD_THIS:
		if (rid && *rid) {
			/* remove one instance from the component */
			if (remove_instance (cbdav, cache_comp, icaltime_from_string (rid), mod, mod != CALOBJ_MOD_ONLY_THIS)) {
				if (new_component) {
					icalcomponent *master = get_master_comp (cbdav, cache_comp);

					if (master)
						*new_component = e_cal_component_new_from_icalcomponent (icalcomponent_new_clone (master));
				}
			} else {
				/* this was the last instance, thus delete whole component */
				rid = NULL;
				remove_comp_from_cache (cbdav, uid, NULL);
			}
		} else {
			/* remove whole object */
			remove_comp_from_cache (cbdav, uid, NULL);
		}
		break;
	case CALOBJ_MOD_ALL:
		remove_comp_from_cache (cbdav, uid, NULL);
		break;
	case CALOBJ_MOD_THISANDPRIOR:
	case CALOBJ_MOD_THISANDFUTURE:
		break;
	}

	if (online) {
		CalDAVObject caldav_object;

		caldav_object.href  = href;
		caldav_object.etag  = etag;
		caldav_object.cdata = NULL;

		if (mod == CALOBJ_MOD_THIS && rid && *rid) {
			caldav_object.cdata = pack_cobj (cbdav, cache_comp);

			caldav_server_put_object (cbdav, &caldav_object, cache_comp, perror);
		} else
			caldav_server_delete_object (cbdav, &caldav_object, perror);

		caldav_object_free (&caldav_object, FALSE);
		href = NULL;
		etag = NULL;
	} else {
		/* mark component as out of synch */
		/*if (mod == CALOBJ_MOD_THIS && rid && *rid)
			ecalcomp_set_synch_state (cache_comp_master, ECALCOMP_LOCALLY_MODIFIED);
		else
			ecalcomp_set_synch_state (cache_comp_master, ECALCOMP_LOCALLY_DELETED);*/
	}
	remove_cached_attachment (cbdav, uid);

	icalcomponent_free (cache_comp);
	g_free (href);
	g_free (etag);
}

static void
extract_objects (icalcomponent *icomp,
                 icalcomponent_kind ekind,
                 GSList **objects,
                 GError **error)
{
	icalcomponent         *scomp;
	icalcomponent_kind     kind;

	e_return_data_cal_error_if_fail (icomp, InvalidArg);
	e_return_data_cal_error_if_fail (objects, InvalidArg);

	kind = icalcomponent_isa (icomp);

	if (kind == ekind) {
		*objects = g_slist_prepend (NULL, icomp);
		return;
	}

	if (kind != ICAL_VCALENDAR_COMPONENT) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	*objects = NULL;
	scomp = icalcomponent_get_first_component (icomp,
						   ekind);

	while (scomp) {
		/* Remove components from toplevel here */
		*objects = g_slist_prepend (*objects, scomp);
		icalcomponent_remove_component (icomp, scomp);

		scomp = icalcomponent_get_next_component (icomp, ekind);
	}
}

static gboolean
extract_timezones (ECalBackendCalDAV *cbdav,
                   icalcomponent *icomp)
{
	GSList *timezones = NULL, *iter;
	icaltimezone *zone;
	GError *err = NULL;

	g_return_val_if_fail (cbdav != NULL, FALSE);
	g_return_val_if_fail (icomp != NULL, FALSE);

	extract_objects (icomp, ICAL_VTIMEZONE_COMPONENT, &timezones, &err);
	if (err) {
		g_error_free (err);
		return FALSE;
	}

	zone = icaltimezone_new ();
	for (iter = timezones; iter; iter = iter->next) {
		if (icaltimezone_set_component (zone, iter->data)) {
			e_cal_backend_store_put_timezone (cbdav->priv->store, zone);
		} else {
			icalcomponent_free (iter->data);
		}
	}

	icaltimezone_free (zone, TRUE);
	g_slist_free (timezones);

	return TRUE;
}

static void
process_object (ECalBackendCalDAV *cbdav,
                ECalComponent *ecomp,
                gboolean online,
                icalproperty_method method,
                GError **error)
{
	ECalBackend              *backend;
	struct icaltimetype       now;
	gchar *new_obj_str;
	gboolean is_declined, is_in_cache;
	CalObjModType mod;
	ECalComponentId *id = e_cal_component_get_id (ecomp);
	GError *err = NULL;

	backend = E_CAL_BACKEND (cbdav);

	e_return_data_cal_error_if_fail (id != NULL, InvalidObject);

	/* ctime, mtime */
	now = icaltime_current_time_with_zone (icaltimezone_get_utc_timezone ());
	e_cal_component_set_created (ecomp, &now);
	e_cal_component_set_last_modified (ecomp, &now);

	/* just to check whether component exists in a cache */
	is_in_cache = cache_contains (cbdav, id->uid, NULL) || cache_contains (cbdav, id->uid, id->rid);

	new_obj_str = e_cal_component_get_as_string (ecomp);
	mod = e_cal_component_is_instance (ecomp) ? CALOBJ_MOD_THIS : CALOBJ_MOD_ALL;

	switch (method) {
	case ICAL_METHOD_PUBLISH:
	case ICAL_METHOD_REQUEST:
	case ICAL_METHOD_REPLY:
		is_declined = e_cal_backend_user_declined (e_cal_component_get_icalcomponent (ecomp));
		if (is_in_cache) {
			if (!is_declined) {
				ECalComponent *new_component = NULL, *old_component = NULL;

				do_modify_object (cbdav, new_obj_str, mod,
						  &old_component, &new_component, &err);
				if (!err) {
					if (!old_component)
						e_cal_backend_notify_component_created (backend, new_component);
					else
						e_cal_backend_notify_component_modified (backend, old_component, new_component);
				}

				if (new_component)
					g_object_unref (new_component);
				if (old_component)
					g_object_unref (old_component);
			} else {
				ECalComponent *new_component = NULL, *old_component = NULL;

				do_remove_object (cbdav, id->uid, id->rid, mod, &old_component, &new_component, &err);
				if (!err) {
					if (new_component) {
						e_cal_backend_notify_component_modified (backend, old_component, new_component);
					} else {
						e_cal_backend_notify_component_removed (backend, id, old_component, NULL);
					}
				}

				if (new_component)
					g_object_unref (new_component);
				if (old_component)
					g_object_unref (old_component);
			}
		} else if (!is_declined) {
			ECalComponent *new_component = NULL;

			do_create_object (cbdav, new_obj_str, NULL, &new_component, &err);

			if (!err) {
				e_cal_backend_notify_component_created (backend, new_component);
			}

			if (new_component)
				g_object_unref (new_component);

		}
		break;
	case ICAL_METHOD_CANCEL:
		if (is_in_cache) {
			ECalComponent *new_component = NULL, *old_component = NULL;

			do_remove_object (cbdav, id->uid, id->rid, CALOBJ_MOD_THIS, &old_component, &new_component, &err);
			if (!err) {
				if (new_component) {
					e_cal_backend_notify_component_modified (backend, old_component, new_component);
				} else {
					e_cal_backend_notify_component_removed (backend, id, old_component, NULL);
				}
			}

			if (new_component)
				g_object_unref (new_component);
			if (old_component)
				g_object_unref (old_component);
		} else {
			err = EDC_ERROR (ObjectNotFound);
		}
		break;

	default:
		err = EDC_ERROR (UnsupportedMethod);
		break;
	}

	e_cal_component_free_id (id);
	g_free (new_obj_str);

	if (err)
		g_propagate_error (error, err);
}

static void
do_receive_objects (ECalBackendSync *backend,
                    EDataCal *cal,
                    GCancellable *cancellable,
                    const gchar *calobj,
                    GError **perror)
{
	ECalBackendCalDAV        *cbdav;
	icalcomponent            *icomp;
	icalcomponent_kind        kind;
	icalproperty_method       tmethod;
	gboolean                  online;
	GSList                   *objects, *iter;
	GError *err = NULL;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	if (!check_state (cbdav, &online, perror))
		return;

	icomp = icalparser_parse_string (calobj);

	/* Try to parse cal object string */
	if (icomp == NULL) {
		g_propagate_error (perror, EDC_ERROR (InvalidObject));
		return;
	}

	kind = e_cal_backend_get_kind (E_CAL_BACKEND (backend));
	extract_objects (icomp, kind, &objects, &err);

	if (err) {
		icalcomponent_free (icomp);
		g_propagate_error (perror, err);
		return;
	}

	/* Extract optional timezone compnents */
	extract_timezones (cbdav, icomp);

	tmethod = icalcomponent_get_method (icomp);

	for (iter = objects; iter && !err; iter = iter->next) {
		icalcomponent       *scomp;
		ECalComponent       *ecomp;
		icalproperty_method  method;

		scomp = (icalcomponent *) iter->data;
		ecomp = e_cal_component_new ();

		e_cal_component_set_icalcomponent (ecomp, scomp);

		if (icalcomponent_get_first_property (scomp, ICAL_METHOD_PROPERTY)) {
			method = icalcomponent_get_method (scomp);
		} else {
			method = tmethod;
		}

		process_object (cbdav, ecomp, online, method, &err);
		g_object_unref (ecomp);
	}

	g_slist_free (objects);

	icalcomponent_free (icomp);

	if (err)
		g_propagate_error (perror, err);
}

#define caldav_busy_stub(_func_name, _params, _call_func, _call_params)	\
static void								\
_func_name _params							\
{									\
	ECalBackendCalDAV        *cbdav;				\
	SlaveCommand		  old_slave_cmd;			\
	gboolean		  was_slave_busy;			\
									\
	cbdav = E_CAL_BACKEND_CALDAV (backend);				\
									\
	/* this is done before locking */				\
	old_slave_cmd = cbdav->priv->slave_cmd;				\
	was_slave_busy = cbdav->priv->slave_busy;			\
	if (was_slave_busy) {						\
		/* let it pause its work and do our job */		\
		cbdav->priv->slave_cmd = SLAVE_SHOULD_SLEEP;		\
	}								\
									\
	g_mutex_lock (cbdav->priv->busy_lock);				\
	_call_func _call_params;					\
									\
	/* this is done before unlocking */				\
	if (was_slave_busy) {						\
		cbdav->priv->slave_cmd = old_slave_cmd;			\
		g_cond_signal (cbdav->priv->cond);			\
	}								\
									\
	g_mutex_unlock (cbdav->priv->busy_lock);			\
}

caldav_busy_stub (
        caldav_create_object,
                  (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const gchar *in_calobj,
                  gchar **uid,
                  ECalComponent **new_component,
                  GError **perror),
        do_create_object,
                  (cbdav,
                  in_calobj,
                  uid,
                  new_component,
                  perror))

caldav_busy_stub (
        caldav_modify_object,
                  (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const gchar *calobj,
                  CalObjModType mod,
                  ECalComponent **old_component,
                  ECalComponent **new_component,
                  GError **perror),
        do_modify_object,
                  (cbdav,
                  calobj,
                  mod,
                  old_component,
                  new_component,
                  perror))

caldav_busy_stub (
        caldav_remove_object,
                  (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const gchar *uid,
                  const gchar *rid,
                  CalObjModType mod,
                  ECalComponent **old_component,
                  ECalComponent **new_component,
                  GError **perror),
        do_remove_object,
                  (cbdav,
                  uid,
                  rid,
                  mod,
                  old_component,
                  new_component,
                  perror))

caldav_busy_stub (
        caldav_receive_objects,
                  (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const gchar *calobj,
                  GError **perror),
        do_receive_objects,
                  (backend,
                  cal,
                  cancellable,
                  calobj,
                  perror))

static void
caldav_send_objects (ECalBackendSync *backend,
                     EDataCal *cal,
                     GCancellable *cancellable,
                     const gchar *calobj,
                     GSList **users,
                     gchar **modified_calobj,
                     GError **perror)
{
	*users = NULL;
	*modified_calobj = g_strdup (calobj);
}

static void
caldav_get_object (ECalBackendSync *backend,
                   EDataCal *cal,
                   GCancellable *cancellable,
                   const gchar *uid,
                   const gchar *rid,
                   gchar **object,
                   GError **perror)
{
	ECalBackendCalDAV        *cbdav;
	icalcomponent            *icalcomp;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	*object = NULL;
	icalcomp = get_comp_from_cache (cbdav, uid, rid, NULL, NULL);

	if (!icalcomp) {
		g_propagate_error (perror, EDC_ERROR (ObjectNotFound));
		return;
	}

	*object = icalcomponent_as_ical_string_r (icalcomp);
	icalcomponent_free (icalcomp);
}

static void
caldav_add_timezone (ECalBackendSync *backend,
                     EDataCal *cal,
                     GCancellable *cancellable,
                     const gchar *tzobj,
                     GError **error)
{
	icalcomponent *tz_comp;
	ECalBackendCalDAV *cbdav;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	e_return_data_cal_error_if_fail (E_IS_CAL_BACKEND_CALDAV (cbdav), InvalidArg);
	e_return_data_cal_error_if_fail (tzobj != NULL, InvalidArg);

	tz_comp = icalparser_parse_string (tzobj);
	if (!tz_comp) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	if (icalcomponent_isa (tz_comp) == ICAL_VTIMEZONE_COMPONENT) {
		icaltimezone *zone;

		zone = icaltimezone_new ();
		icaltimezone_set_component (zone, tz_comp);

		e_cal_backend_store_put_timezone (cbdav->priv->store, zone);

		icaltimezone_free (zone, TRUE);
	} else {
		icalcomponent_free (tz_comp);
	}
}

static void
caldav_get_object_list (ECalBackendSync *backend,
                        EDataCal *cal,
                        GCancellable *cancellable,
                        const gchar *sexp_string,
                        GSList **objects,
                        GError **perror)
{
	ECalBackendCalDAV        *cbdav;
	ECalBackendSExp	 *sexp;
	ECalBackend *bkend;
	gboolean                  do_search;
	GSList			 *list, *iter;
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	sexp = e_cal_backend_sexp_new (sexp_string);

	if (sexp == NULL) {
		g_propagate_error (perror, EDC_ERROR (InvalidQuery));
		return;
	}

	if (g_str_equal (sexp_string, "#t")) {
		do_search = FALSE;
	} else {
		do_search = TRUE;
	}

	*objects = NULL;

	prunning_by_time = e_cal_backend_sexp_evaluate_occur_times (sexp, &occur_start, &occur_end);

	list = prunning_by_time ?
		e_cal_backend_store_get_components_occuring_in_range (cbdav->priv->store, occur_start, occur_end)
		: e_cal_backend_store_get_components (cbdav->priv->store);

	bkend = E_CAL_BACKEND (backend);

	for (iter = list; iter; iter = g_slist_next (iter)) {
		ECalComponent *comp = E_CAL_COMPONENT (iter->data);

		if (!do_search ||
		    e_cal_backend_sexp_match_comp (sexp, comp, bkend)) {
			*objects = g_slist_prepend (*objects, e_cal_component_get_as_string (comp));
		}

		g_object_unref (comp);
	}

	g_object_unref (sexp);
	g_slist_free (list);
}

static void
caldav_start_view (ECalBackend *backend,
                   EDataCalView *query)
{
	ECalBackendCalDAV        *cbdav;
	ECalBackendSExp	 *sexp;
	ECalBackend              *bkend;
	gboolean                  do_search;
	GSList			 *list, *iter;
	const gchar               *sexp_string;
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;
	cbdav = E_CAL_BACKEND_CALDAV (backend);

	sexp_string = e_data_cal_view_get_text (query);
	sexp = e_cal_backend_sexp_new (sexp_string);

	/* FIXME:check invalid sexp */

	if (g_str_equal (sexp_string, "#t")) {
		do_search = FALSE;
	} else {
		do_search = TRUE;
	}
	prunning_by_time = e_cal_backend_sexp_evaluate_occur_times (sexp,
									    &occur_start,
									    &occur_end);

	bkend = E_CAL_BACKEND (backend);

	list = prunning_by_time ?
		e_cal_backend_store_get_components_occuring_in_range (cbdav->priv->store, occur_start, occur_end)
		: e_cal_backend_store_get_components (cbdav->priv->store);

	for (iter = list; iter; iter = g_slist_next (iter)) {
		ECalComponent *comp = E_CAL_COMPONENT (iter->data);

		if (!do_search ||
		    e_cal_backend_sexp_match_comp (sexp, comp, bkend)) {
			e_data_cal_view_notify_components_added_1 (query, comp);
		}

		g_object_unref (comp);
	}

	g_object_unref (sexp);
	g_slist_free (list);

	e_data_cal_view_notify_complete (query, NULL /* Success */);
}

static void
caldav_get_free_busy (ECalBackendSync *backend,
                      EDataCal *cal,
                      GCancellable *cancellable,
                      const GSList *users,
                      time_t start,
                      time_t end,
                      GSList **freebusy,
                      GError **error)
{
	ECalBackendCalDAV *cbdav;
	icalcomponent *icalcomp;
	ECalComponent *comp;
	ECalComponentDateTime dt;
	struct icaltimetype dtvalue;
	icaltimezone *utc;
	gchar *str, *usermail;
	const GSList *u;
	GSList *attendees = NULL, *to_free = NULL;
	GError *err = NULL;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	e_return_data_cal_error_if_fail (users != NULL, InvalidArg);
	e_return_data_cal_error_if_fail (freebusy != NULL, InvalidArg);
	e_return_data_cal_error_if_fail (start < end, InvalidArg);

	if (!cbdav->priv->calendar_schedule) {
		g_propagate_error (error, EDC_ERROR_EX (OtherError, _("Calendar doesn't support Free/Busy")));
		return;
	}

	if (!cbdav->priv->schedule_outbox_url) {
		caldav_receive_schedule_outbox_url (cbdav);
		if (!cbdav->priv->schedule_outbox_url) {
			cbdav->priv->calendar_schedule = FALSE;
			g_propagate_error (error, EDC_ERROR_EX (OtherError, "Schedule outbox url not found"));
			return;
		}
	}

	comp = e_cal_component_new ();
	e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_FREEBUSY);

	str = e_cal_component_gen_uid ();
	e_cal_component_set_uid (comp, str);
	g_free (str);

	utc = icaltimezone_get_utc_timezone ();
	dt.value = &dtvalue;
	dt.tzid = icaltimezone_get_tzid (utc);

	dtvalue = icaltime_current_time_with_zone (utc);
	e_cal_component_set_dtstamp (comp, &dtvalue);

	dtvalue = icaltime_from_timet_with_zone (start, FALSE, utc);
	e_cal_component_set_dtstart (comp, &dt);

	dtvalue = icaltime_from_timet_with_zone (end, FALSE, utc);
	e_cal_component_set_dtend (comp, &dt);

	usermail = get_usermail (E_CAL_BACKEND (backend));
	if (usermail && !*usermail) {
		g_free (usermail);
		usermail = NULL;
	}

	if ((cbdav->priv->credentials && e_credentials_has_key (cbdav->priv->credentials, E_CREDENTIALS_KEY_USERNAME)) || usermail) {
		ECalComponentOrganizer organizer = {NULL};

		organizer.value = usermail ? usermail : e_credentials_peek (cbdav->priv->credentials, E_CREDENTIALS_KEY_USERNAME);
		organizer.value = g_strconcat ("mailto:", organizer.value, NULL);

		e_cal_component_set_organizer (comp, &organizer);

		g_free ((gchar *) organizer.value);
	}

	g_free (usermail);

	for (u = users; u; u = u->next) {
		ECalComponentAttendee *ca;
		gchar *temp = g_strconcat ("mailto:", (const gchar *)u->data, NULL);

		ca = g_new0 (ECalComponentAttendee, 1);

		ca->value = temp;
		ca->cutype = ICAL_CUTYPE_INDIVIDUAL;
		ca->status = ICAL_PARTSTAT_NEEDSACTION;
		ca->role = ICAL_ROLE_CHAIR;

		to_free = g_slist_prepend (to_free, temp);
		attendees = g_slist_append (attendees, ca);
	}

	e_cal_component_set_attendee_list (comp, attendees);

	g_slist_foreach (attendees, (GFunc) g_free, NULL);
	g_slist_free (attendees);

	g_slist_foreach (to_free, (GFunc) g_free, NULL);
	g_slist_free (to_free);

	e_cal_component_abort_sequence (comp);

	/* put the free/busy request to a VCALENDAR */
	icalcomp = e_cal_util_new_top_level ();
	icalcomponent_set_method (icalcomp, ICAL_METHOD_REQUEST);
	icalcomponent_add_component (icalcomp, icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp)));

	str = icalcomponent_as_ical_string_r (icalcomp);

	icalcomponent_free (icalcomp);
	g_object_unref (comp);

	e_return_data_cal_error_if_fail (str != NULL, OtherError);

	caldav_post_freebusy (cbdav, cbdav->priv->schedule_outbox_url, &str, &err);

	if (!err) {
		/* parse returned xml */
		xmlDocPtr doc;

		doc = xmlReadMemory (str, strlen (str), "response.xml", NULL, 0);
		if (doc != NULL) {
			xmlXPathContextPtr xpctx;
			xmlXPathObjectPtr result;

			xpctx = xmlXPathNewContext (doc);
			xmlXPathRegisterNs (xpctx, (xmlChar *) "D", (xmlChar *) "DAV:");
			xmlXPathRegisterNs (xpctx, (xmlChar *) "C", (xmlChar *) "urn:ietf:params:xml:ns:caldav");

			result = xpath_eval (xpctx, "/C:schedule-response/C:response");

			if (result == NULL || result->type != XPATH_NODESET) {
				err = EDC_ERROR_EX (OtherError, "Unexpected result in schedule-response");
			} else {
				gint i, n;

				n = xmlXPathNodeSetGetLength (result->nodesetval);
				for (i = 0; i < n; i++) {
					gchar *tmp;

					tmp = xp_object_get_string (xpath_eval (xpctx, "string(/C:schedule-response/C:response[%d]/C:calendar-data)", i + 1));
					if (tmp && *tmp) {
						GSList *objects = NULL, *o;

						icalcomp = icalparser_parse_string (tmp);
						if (icalcomp)
							extract_objects (icalcomp, ICAL_VFREEBUSY_COMPONENT, &objects, &err);
						if (icalcomp && !err) {
							for (o = objects; o; o = o->next) {
								gchar *obj_str = icalcomponent_as_ical_string_r (o->data);

								if (obj_str && *obj_str)
									*freebusy = g_slist_append (*freebusy, obj_str);
								else
									g_free (obj_str);
							}
						}

						g_slist_foreach (objects, (GFunc) icalcomponent_free, NULL);
						g_slist_free (objects);

						if (icalcomp)
							icalcomponent_free (icalcomp);
						if (err)
							g_error_free (err);
						err = NULL;
					}

					g_free (tmp);
				}
			}

			if (result != NULL)
				xmlXPathFreeObject (result);
			xmlXPathFreeContext (xpctx);
			xmlFreeDoc (doc);
		}
	}

	g_free (str);

	if (err)
		g_propagate_error (error, err);
}

static void
caldav_notify_online_cb (ECalBackend *backend,
                         GParamSpec *pspec)
{
	ECalBackendCalDAV        *cbdav;
	gboolean online;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	/*g_mutex_lock (cbdav->priv->busy_lock);*/

	online = e_backend_get_online (E_BACKEND (backend));

	if (!cbdav->priv->loaded) {
		e_cal_backend_notify_online (backend, online);
		/*g_mutex_unlock (cbdav->priv->busy_lock);*/
		return;
	}

	if (online) {
		/* Wake up the slave thread */
		cbdav->priv->slave_cmd = SLAVE_SHOULD_WORK;
		g_cond_signal (cbdav->priv->cond);
	} else {
		soup_session_abort (cbdav->priv->session);
		cbdav->priv->slave_cmd = SLAVE_SHOULD_SLEEP;
	}

	e_cal_backend_notify_online (backend, online);

	/*g_mutex_unlock (cbdav->priv->busy_lock);*/
}

static icaltimezone *
caldav_internal_get_timezone (ECalBackend *backend,
                              const gchar *tzid)
{
	icaltimezone *zone;
	ECalBackendCalDAV *cbdav;

	cbdav = E_CAL_BACKEND_CALDAV (backend);
	zone = NULL;

	if (cbdav->priv->store)
		zone = (icaltimezone *) e_cal_backend_store_get_timezone (cbdav->priv->store, tzid);

	if (!zone && E_CAL_BACKEND_CLASS (parent_class)->internal_get_timezone)
		zone = E_CAL_BACKEND_CLASS (parent_class)->internal_get_timezone (backend, tzid);

	return zone;
}

static void
caldav_source_changed_cb (ESource *source,
                          ECalBackendCalDAV *cbdav)
{
	SlaveCommand old_slave_cmd;
	gboolean old_slave_busy;

	g_return_if_fail (source != NULL);
	g_return_if_fail (cbdav != NULL);

	old_slave_cmd = cbdav->priv->slave_cmd;
	old_slave_busy = cbdav->priv->slave_busy;
	if (old_slave_busy) {
		cbdav->priv->slave_cmd = SLAVE_SHOULD_SLEEP;
		g_mutex_lock (cbdav->priv->busy_lock);
	}

	initialize_backend (cbdav, NULL);

	/* always wakeup thread, even when it was sleeping */
	g_cond_signal (cbdav->priv->cond);

	if (old_slave_busy) {
		cbdav->priv->slave_cmd = old_slave_cmd;
		g_mutex_unlock (cbdav->priv->busy_lock);
	}
}

/* ************************************************************************* */
/* ***************************** GObject Foo ******************************* */

G_DEFINE_TYPE (ECalBackendCalDAV, e_cal_backend_caldav, E_TYPE_CAL_BACKEND_SYNC)

static void
e_cal_backend_caldav_dispose (GObject *object)
{
	ECalBackendCalDAVPrivate *priv;
	ESource *source;

	priv = E_CAL_BACKEND_CALDAV_GET_PRIVATE (object);

	/* tell the slave to stop before acquiring a lock,
	 * as it can work at the moment, and lock can be locked */
	priv->slave_cmd = SLAVE_SHOULD_DIE;

	g_mutex_lock (priv->busy_lock);

	if (priv->disposed) {
		g_mutex_unlock (priv->busy_lock);
		return;
	}

	source = e_backend_get_source (E_BACKEND (object));
	if (source)
		g_signal_handlers_disconnect_by_func (G_OBJECT (source), caldav_source_changed_cb, object);

	/* stop the slave  */
	if (priv->synch_slave) {
		g_cond_signal (priv->cond);

		/* wait until the slave died */
		g_cond_wait (priv->slave_gone_cond, priv->busy_lock);
	}

	g_object_unref (priv->session);
	g_object_unref (priv->proxy);

	e_credentials_free (priv->credentials);
	priv->credentials = NULL;

	g_free (priv->uri);
	g_free (priv->schedule_outbox_url);

	if (priv->store != NULL) {
		g_object_unref (priv->store);
	}

	priv->disposed = TRUE;
	g_mutex_unlock (priv->busy_lock);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
e_cal_backend_caldav_finalize (GObject *object)
{
	ECalBackendCalDAVPrivate *priv;

	priv = E_CAL_BACKEND_CALDAV_GET_PRIVATE (object);

	g_mutex_free (priv->busy_lock);
	g_cond_free (priv->cond);
	g_cond_free (priv->slave_gone_cond);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
e_cal_backend_caldav_init (ECalBackendCalDAV *cbdav)
{
	cbdav->priv = E_CAL_BACKEND_CALDAV_GET_PRIVATE (cbdav);
	cbdav->priv->session = soup_session_sync_new ();
	g_object_set (cbdav->priv->session, SOUP_SESSION_TIMEOUT, 90, NULL);

	cbdav->priv->proxy = e_proxy_new ();
	e_proxy_setup_proxy (cbdav->priv->proxy);
	g_signal_connect (cbdav->priv->proxy, "changed", G_CALLBACK (proxy_settings_changed), cbdav->priv);

	if (G_UNLIKELY (caldav_debug_show (DEBUG_MESSAGE)))
		caldav_debug_setup (cbdav->priv->session);

	cbdav->priv->disposed = FALSE;
	cbdav->priv->loaded   = FALSE;
	cbdav->priv->opened = FALSE;

	/* Thinks the 'getctag' extension is available the first time, but unset it when realizes it isn't. */
	cbdav->priv->ctag_supported = TRUE;
	cbdav->priv->ctag_to_store = NULL;

	cbdav->priv->schedule_outbox_url = NULL;

	cbdav->priv->is_google = FALSE;

	cbdav->priv->busy_lock = g_mutex_new ();
	cbdav->priv->cond = g_cond_new ();
	cbdav->priv->slave_gone_cond = g_cond_new ();

	/* Slave control ... */
	cbdav->priv->slave_cmd = SLAVE_SHOULD_SLEEP;
	cbdav->priv->slave_busy = FALSE;
	cbdav->priv->refresh_time.tv_usec = 0;
	cbdav->priv->refresh_time.tv_sec  = DEFAULT_REFRESH_TIME;

	g_signal_connect (cbdav->priv->session, "authenticate",
			  G_CALLBACK (soup_authenticate), cbdav);

	e_cal_backend_sync_set_lock (E_CAL_BACKEND_SYNC (cbdav), FALSE);

	g_signal_connect (
		cbdav, "notify::online",
		G_CALLBACK (caldav_notify_online_cb), NULL);
}

static void
e_cal_backend_caldav_class_init (ECalBackendCalDAVClass *class)
{
	GObjectClass *object_class;
	ECalBackendClass *backend_class;
	ECalBackendSyncClass *sync_class;

	object_class = (GObjectClass *) class;
	backend_class = (ECalBackendClass *) class;
	sync_class = (ECalBackendSyncClass *) class;

	caldav_debug_init ();

	parent_class = (ECalBackendSyncClass *) g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (ECalBackendCalDAVPrivate));

	object_class->dispose  = e_cal_backend_caldav_dispose;
	object_class->finalize = e_cal_backend_caldav_finalize;

	sync_class->get_backend_property_sync	= caldav_get_backend_property;

	sync_class->open_sync			= caldav_do_open;
	sync_class->authenticate_user_sync	= caldav_authenticate_user;
	sync_class->refresh_sync		= caldav_refresh;
	sync_class->remove_sync			= caldav_remove;

	sync_class->create_object_sync		= caldav_create_object;
	sync_class->modify_object_sync		= caldav_modify_object;
	sync_class->remove_object_sync		= caldav_remove_object;

	sync_class->receive_objects_sync	= caldav_receive_objects;
	sync_class->send_objects_sync		= caldav_send_objects;
	sync_class->get_object_sync		= caldav_get_object;
	sync_class->get_object_list_sync	= caldav_get_object_list;
	sync_class->add_timezone_sync		= caldav_add_timezone;
	sync_class->get_free_busy_sync		= caldav_get_free_busy;

	backend_class->start_view		= caldav_start_view;

	backend_class->internal_get_timezone	= caldav_internal_get_timezone;
}
