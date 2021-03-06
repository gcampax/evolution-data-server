/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* Evolution calendar - iCalendar http backend
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * Authors: Hans Petter Jansson <hpj@ximian.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Based in part on the file backend.
 */

#include <config.h>
#include <string.h>
#include <unistd.h>
#include <gconf/gconf-client.h>
#include <glib/gi18n-lib.h>
#include "libedataserver/e-xml-hash-utils.h"
#include "libedataserver/e-proxy.h"
#include <libecal/e-cal-recur.h>
#include <libecal/e-cal-util.h>
#include <libecal/e-cal-time-util.h>
#include <libedata-cal/e-cal-backend-cache.h>
#include <libedata-cal/e-cal-backend-store.h>
#include <libedata-cal/e-cal-backend-file-store.h>
#include <libedata-cal/e-cal-backend-util.h>
#include <libedata-cal/e-cal-backend-sexp.h>
#include <libsoup/soup.h>
#include "e-cal-backend-http.h"

#define E_CAL_BACKEND_HTTP_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_CAL_BACKEND_HTTP, ECalBackendHttpPrivate))

#define EDC_ERROR(_code) e_data_cal_create_error (_code, NULL)
#define EDC_ERROR_EX(_code, _msg) e_data_cal_create_error (_code, _msg)

G_DEFINE_TYPE (ECalBackendHttp, e_cal_backend_http, E_TYPE_CAL_BACKEND_SYNC)

/* Private part of the ECalBackendHttp structure */
struct _ECalBackendHttpPrivate {
	/* signal handler id for source's 'changed' signal */
	gulong source_changed_id;
	/* URI to get remote calendar data from */
	gchar *uri;

	/* The file cache */
	ECalBackendStore *store;

	/* Soup handles for remote file */
	SoupSession *soup_session;

	/* Reload */
	guint reload_timeout_id;
	guint is_loading : 1;

	/* Flags */
	gboolean opened;
	gboolean requires_auth;

	ECredentials *credentials;
};



#define d(x)

static void e_cal_backend_http_dispose (GObject *object);
static void e_cal_backend_http_finalize (GObject *object);
static gboolean begin_retrieval_cb (ECalBackendHttp *cbhttp);
static void e_cal_backend_http_add_timezone (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const gchar *tzobj, GError **perror);

/* Dispose handler for the file backend */
static void
e_cal_backend_http_dispose (GObject *object)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;

	cbhttp = E_CAL_BACKEND_HTTP (object);
	priv = cbhttp->priv;

	if (priv->reload_timeout_id) {
		g_source_remove (priv->reload_timeout_id);
		priv->reload_timeout_id = 0;
	}

	if (priv->soup_session) {
		soup_session_abort (priv->soup_session);
		g_object_unref (priv->soup_session);
		priv->soup_session = NULL;
	}

	e_credentials_free (priv->credentials);
	priv->credentials = NULL;

	if (priv->source_changed_id) {
		g_signal_handler_disconnect (e_backend_get_source (E_BACKEND (cbhttp)), priv->source_changed_id);
		priv->source_changed_id = 0;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_cal_backend_http_parent_class)->dispose (object);
}

/* Finalize handler for the file backend */
static void
e_cal_backend_http_finalize (GObject *object)
{
	ECalBackendHttpPrivate *priv;

	priv = E_CAL_BACKEND_HTTP_GET_PRIVATE (object);

	/* Clean up */

	if (priv->store) {
		g_object_unref (priv->store);
		priv->store = NULL;
	}

	g_free (priv->uri);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_cal_backend_http_parent_class)->finalize (object);
}



/* Calendar backend methods */

static gboolean
e_cal_backend_http_get_backend_property (ECalBackendSync *backend,
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
		*prop_value = g_strdup (CAL_STATIC_CAPABILITY_NO_EMAIL_ALARMS ","
					CAL_STATIC_CAPABILITY_REFRESH_SUPPORTED);
	} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS) ||
		   g_str_equal (prop_name, CAL_BACKEND_PROPERTY_ALARM_EMAIL_ADDRESS)) {
		/* A HTTP backend has no particular email address associated
		 * with it (although that would be a useful feature some day).
		 */
		*prop_value = NULL;
	} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_DEFAULT_OBJECT)) {
		icalcomponent *icalcomp;
		icalcomponent_kind kind;

		kind = e_cal_backend_get_kind (E_CAL_BACKEND (backend));
		icalcomp = e_cal_util_new_component (kind);
		*prop_value = icalcomponent_as_ical_string_r (icalcomp);
		icalcomponent_free (icalcomp);
	} else {
		processed = FALSE;
	}

	return processed;
}

static gchar *
webcal_to_http_method (const gchar *webcal_str,
                       gboolean secure)
{
	if (secure && (strncmp ("http://", webcal_str, sizeof ("http://") - 1) == 0))
		return g_strconcat ("https://", webcal_str + sizeof ("http://") - 1, NULL);

	if (strncmp ("webcal://", webcal_str, sizeof ("webcal://") - 1))
		return g_strdup (webcal_str);

	if (secure)
		return g_strconcat ("https://", webcal_str + sizeof ("webcal://") - 1, NULL);
	else
		return g_strconcat ("http://", webcal_str + sizeof ("webcal://") - 1, NULL);
}

static gboolean
notify_and_remove_from_cache (gpointer key,
                              gpointer value,
                              gpointer user_data)
{
	const gchar *calobj = value;
	ECalBackendHttp *cbhttp = E_CAL_BACKEND_HTTP (user_data);
	ECalComponent *comp = e_cal_component_new_from_string (calobj);
	ECalComponentId *id = e_cal_component_get_id (comp);

	e_cal_backend_store_remove_component (cbhttp->priv->store, id->uid, id->rid);
	e_cal_backend_notify_component_removed (E_CAL_BACKEND (cbhttp), id, comp, NULL);

	e_cal_component_free_id (id);
	g_object_unref (comp);

	return TRUE;
}

static void
empty_cache (ECalBackendHttp *cbhttp)
{
	ECalBackendHttpPrivate *priv;
	GSList *comps, *l;

	priv = cbhttp->priv;

	if (!priv->store)
		return;

	comps = e_cal_backend_store_get_components (priv->store);

	for (l = comps; l != NULL; l = g_slist_next (l)) {
		ECalComponentId *id;
		ECalComponent *comp = l->data;

		id = e_cal_component_get_id (comp);

		e_cal_backend_notify_component_removed ((ECalBackend *) cbhttp, id, comp, NULL);

		e_cal_component_free_id (id);
		g_object_unref (comp);
	}
	g_slist_free (comps);

	e_cal_backend_store_put_key_value (priv->store, "ETag", NULL);
	e_cal_backend_store_clean (priv->store);
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
put_component_to_store (ECalBackendHttp *cb,
                        ECalComponent *comp)
{
	time_t time_start, time_end;
	ECalBackendHttpPrivate *priv;
	ECalComponent *cache_comp;
	const gchar *uid;
	gchar *rid;

	priv = cb->priv;

	e_cal_component_get_uid (comp, &uid);
	rid = e_cal_component_get_recurid_as_string (comp);
	cache_comp = e_cal_backend_store_get_component (priv->store, uid, rid);
	g_free (rid);

	if (cache_comp) {
		gboolean changed = TRUE;
		struct icaltimetype stamp1, stamp2;

		stamp1 = icaltime_null_time ();
		stamp2 = icaltime_null_time ();

		e_cal_component_get_dtstamp (comp, &stamp1);
		e_cal_component_get_dtstamp (cache_comp, &stamp2);

		changed = (icaltime_is_null_time (stamp1) && !icaltime_is_null_time (stamp2)) ||
			  (!icaltime_is_null_time (stamp1) && icaltime_is_null_time (stamp2)) ||
			  (icaltime_compare (stamp1, stamp2) != 0);

		if (!changed) {
			struct icaltimetype *last_modified1 = NULL, *last_modified2 = NULL;

			e_cal_component_get_last_modified (comp, &last_modified1);
			e_cal_component_get_last_modified (cache_comp, &last_modified2);

			changed = (last_modified1 != NULL && last_modified2 == NULL) ||
				  (last_modified1 == NULL && last_modified2 != NULL) ||
				  (last_modified1 != NULL && last_modified2 != NULL && icaltime_compare (*last_modified1, *last_modified2) != 0);

			if (last_modified1)
				e_cal_component_free_icaltimetype (last_modified1);
			if (last_modified2)
				e_cal_component_free_icaltimetype (last_modified2);

			if (!changed) {
				gint *sequence1 = NULL, *sequence2 = NULL;

				e_cal_component_get_sequence (comp, &sequence1);
				e_cal_component_get_sequence (cache_comp, &sequence2);

				changed = (sequence1 != NULL && sequence2 == NULL) ||
					  (sequence1 == NULL && sequence2 != NULL) ||
					  (sequence1 != NULL && sequence2 != NULL && *sequence1 != *sequence2);

				if (sequence1)
					e_cal_component_free_sequence (sequence1);
				if (sequence2)
					e_cal_component_free_sequence (sequence2);
			}
		}

		g_object_unref (cache_comp);

		if (!changed)
			return FALSE;
	}

	e_cal_util_get_component_occur_times (comp, &time_start, &time_end,
				   resolve_tzid, cb, icaltimezone_get_utc_timezone (),
				   e_cal_backend_get_kind (E_CAL_BACKEND (cb)));

	e_cal_backend_store_put_component_with_time_range (priv->store, comp, time_start, time_end);

	return TRUE;
}

static void
retrieval_done (SoupSession *session,
                SoupMessage *msg,
                ECalBackendHttp *cbhttp)
{
	ECalBackendHttpPrivate *priv;
	icalcomponent *icalcomp, *subcomp;
	icalcomponent_kind kind;
	const gchar *newuri;
	SoupURI *uri_parsed;
	GHashTable *old_cache;
	GSList *comps_in_cache;

	if (!msg || msg->status_code == SOUP_STATUS_CANCELLED) {
		/* the backend probably gone in this case, thus just return */
		g_object_unref (cbhttp);
		return;
	}

	priv = cbhttp->priv;

	priv->is_loading = FALSE;
	d(g_message ("Retrieval done.\n"));

	if (!priv->uri) {
		/* uri changed meanwhile, retrieve again */
		begin_retrieval_cb (cbhttp);
		g_object_unref (cbhttp);
		return;
	}

	if (msg->status_code == SOUP_STATUS_NOT_MODIFIED) {
		/* attempts with ETag can result in 304 status code */
		priv->opened = TRUE;
		g_object_unref (cbhttp);
		return;
	}

	/* Handle redirection ourselves */
	if (SOUP_STATUS_IS_REDIRECTION (msg->status_code)) {
		newuri = soup_message_headers_get (msg->response_headers,
						   "Location");

		d(g_message ("Redirected from %s to %s\n", priv->uri, newuri));

		if (newuri) {
			if (newuri[0]=='/') {
				g_warning ("Hey! Relative URI returned! Working around...\n");

				uri_parsed = soup_uri_new (priv->uri);
				soup_uri_set_path (uri_parsed, newuri);
				soup_uri_set_query (uri_parsed, NULL);
				/* g_free (newuri); */

				newuri = soup_uri_to_string (uri_parsed, FALSE);
				g_message ("Translated URI: %s\n", newuri);
				soup_uri_free (uri_parsed);
			}

			g_free (priv->uri);

			priv->uri = webcal_to_http_method (newuri, FALSE);
			begin_retrieval_cb (cbhttp);
		} else {
			if (!priv->opened) {
				e_cal_backend_notify_error (E_CAL_BACKEND (cbhttp),
							    _("Redirected to Invalid URI"));
			}
		}

		g_object_unref (cbhttp);
		return;
	}

	/* check status code */
	if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code)) {
		if (!priv->opened) {
			if (msg->status_code == 401 || msg->status_code == 403) {
				priv->requires_auth = TRUE;
				e_cal_backend_notify_auth_required (E_CAL_BACKEND (cbhttp), TRUE, priv->credentials);
				g_object_unref (cbhttp);
				return;
			} else
				e_cal_backend_notify_error (E_CAL_BACKEND (cbhttp),
					msg->reason_phrase && *msg->reason_phrase ? msg->reason_phrase :
					(soup_status_get_phrase (msg->status_code) ? soup_status_get_phrase (msg->status_code) : _("Unknown error")));
		}

		empty_cache (cbhttp);
		g_object_unref (cbhttp);
		return;
	}

	if (priv->store) {
		const gchar *etag = soup_message_headers_get_one (msg->response_headers, "ETag");

		if (!etag || !*etag)
			etag = NULL;

		e_cal_backend_store_put_key_value (priv->store, "ETag", etag);
	}

	/* get the calendar from the response */
	icalcomp = icalparser_parse_string (msg->response_body->data);

	if (!icalcomp) {
		if (!priv->opened)
			e_cal_backend_notify_error (E_CAL_BACKEND (cbhttp), _("Bad file format."));
		empty_cache (cbhttp);
		g_object_unref (cbhttp);
		return;
	}

	if (icalcomponent_isa (icalcomp) != ICAL_VCALENDAR_COMPONENT) {
		if (!priv->opened)
			e_cal_backend_notify_error (E_CAL_BACKEND (cbhttp), _("Not a calendar."));
		icalcomponent_free (icalcomp);
		empty_cache (cbhttp);
		g_object_unref (cbhttp);
		return;
	}

	/* Update cache */
	old_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	comps_in_cache = e_cal_backend_store_get_components (priv->store);
	while (comps_in_cache != NULL) {
		const gchar *uid;
		ECalComponent *comp = comps_in_cache->data;

		e_cal_component_get_uid (comp, &uid);
		g_hash_table_insert (old_cache, g_strdup (uid), e_cal_component_get_as_string (comp));

		comps_in_cache = g_slist_remove (comps_in_cache, comps_in_cache->data);
		g_object_unref (comp);
	}

	kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbhttp));
	subcomp = icalcomponent_get_first_component (icalcomp, ICAL_ANY_COMPONENT);
	e_cal_backend_store_freeze_changes (priv->store);
	while (subcomp) {
		ECalComponent *comp;
		icalcomponent_kind subcomp_kind;
		icalproperty *prop = NULL;

		subcomp_kind = icalcomponent_isa (subcomp);
		prop = icalcomponent_get_first_property (subcomp, ICAL_UID_PROPERTY);
		if (!prop && subcomp_kind == kind) {
			g_warning (" The component does not have the  mandatory property UID \n");
			subcomp = icalcomponent_get_next_component (icalcomp, ICAL_ANY_COMPONENT);
			continue;
		}

		if (subcomp_kind == kind) {
			comp = e_cal_component_new ();
			if (e_cal_component_set_icalcomponent (comp, icalcomponent_new_clone (subcomp))) {
				const gchar *uid;
				gpointer orig_key, orig_value;

				e_cal_component_get_uid (comp, &uid);

				if (!put_component_to_store (cbhttp, comp)) {
					g_hash_table_remove (old_cache, uid);
				} else if (g_hash_table_lookup_extended (old_cache, uid, &orig_key, &orig_value)) {
					ECalComponent *orig_comp = e_cal_component_new_from_string (orig_value);

					e_cal_backend_notify_component_modified (E_CAL_BACKEND (cbhttp), orig_comp, comp);

					g_hash_table_remove (old_cache, uid);
					if (orig_comp)
						g_object_unref (orig_comp);
				} else {
					e_cal_backend_notify_component_created (E_CAL_BACKEND (cbhttp), comp);
				}
			}

			g_object_unref (comp);
		} else if (subcomp_kind == ICAL_VTIMEZONE_COMPONENT) {
			icaltimezone *zone;

			zone = icaltimezone_new ();
			icaltimezone_set_component (zone, icalcomponent_new_clone (subcomp));
			e_cal_backend_store_put_timezone (priv->store, (const icaltimezone *) zone);

			icaltimezone_free (zone, 1);
		}

		subcomp = icalcomponent_get_next_component (icalcomp, ICAL_ANY_COMPONENT);
	}

	e_cal_backend_store_thaw_changes (priv->store);

	/* notify the removals */
	g_hash_table_foreach_remove (old_cache, (GHRFunc) notify_and_remove_from_cache, cbhttp);
	g_hash_table_destroy (old_cache);

	/* free memory */
	icalcomponent_free (icalcomp);

	priv->opened = TRUE;

	g_object_unref (cbhttp);

	d(g_message ("Retrieval really done.\n"));
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
	ECalBackendHttpPrivate *priv;
	ECalBackendHttp        *cbhttp;

	cbhttp = E_CAL_BACKEND_HTTP (data);
	priv =  cbhttp->priv;

	if (!retrying && priv->credentials && e_credentials_has_key (priv->credentials, E_CREDENTIALS_KEY_USERNAME)) {
		soup_auth_authenticate (auth, e_credentials_peek (priv->credentials, E_CREDENTIALS_KEY_USERNAME), e_credentials_peek (priv->credentials, E_CREDENTIALS_KEY_PASSWORD));
		e_credentials_clear_peek (priv->credentials);
	}
}

static gboolean reload_cb                  (ECalBackendHttp *cbhttp);
static void     maybe_start_reload_timeout (ECalBackendHttp *cbhttp);

static gboolean
begin_retrieval_cb (ECalBackendHttp *cbhttp)
{
	ECalBackendHttpPrivate *priv;
	SoupMessage *soup_message;

	priv = cbhttp->priv;

	if (!e_backend_get_online (E_BACKEND (cbhttp)))
		return FALSE;

	maybe_start_reload_timeout (cbhttp);

	d(g_message ("Starting retrieval...\n"));

	if (priv->is_loading)
		return FALSE;

	priv->is_loading = TRUE;

	if (priv->uri == NULL) {
		ESource *source = e_backend_get_source (E_BACKEND (cbhttp));
		const gchar *secure_prop = e_source_get_property (source, "use_ssl");
		gchar *uri = e_source_get_uri (source);

		priv->uri = webcal_to_http_method (uri,
			(secure_prop && g_str_equal(secure_prop, "1")));

		g_free (uri);
	}

	/* create the Soup session if not already created */
	if (!priv->soup_session) {
		EProxy *proxy;
		SoupURI *proxy_uri = NULL;

		priv->soup_session = soup_session_async_new ();

		g_signal_connect (priv->soup_session, "authenticate",
				  G_CALLBACK (soup_authenticate), cbhttp);

		if (g_getenv ("WEBCAL_DEBUG") != NULL) {
			SoupLogger *logger;

			logger = soup_logger_new (SOUP_LOGGER_LOG_BODY, 1024 * 1024);
			soup_session_add_feature (priv->soup_session, SOUP_SESSION_FEATURE (logger));
			g_object_unref (logger);
		}

		/* set the HTTP proxy, if configuration is set to do so */
		proxy = e_proxy_new ();
		e_proxy_setup_proxy (proxy);
		if (e_proxy_require_proxy_for_uri (proxy, priv->uri)) {
			proxy_uri = e_proxy_peek_uri_for (proxy, priv->uri);
		}

		g_object_set (G_OBJECT (priv->soup_session), SOUP_SESSION_PROXY_URI, proxy_uri, NULL);

		g_object_unref (proxy);
	}

	/* create message to be sent to server */
	soup_message = soup_message_new (SOUP_METHOD_GET, priv->uri);
	if (soup_message == NULL) {
		priv->is_loading = FALSE;
		empty_cache (cbhttp);
		return FALSE;
	}

	soup_message_headers_append (soup_message->request_headers, "User-Agent", "Evolution/" VERSION);
	soup_message_headers_append (soup_message->request_headers, "Connection", "close");
	soup_message_set_flags (soup_message, SOUP_MESSAGE_NO_REDIRECT);
	if (priv->store) {
		const gchar *etag = e_cal_backend_store_get_key_value (priv->store, "ETag");

		if (etag && *etag)
			soup_message_headers_append (soup_message->request_headers, "If-None-Match", etag);
	}

	soup_session_queue_message (priv->soup_session, soup_message,
				    (SoupSessionCallback) retrieval_done, g_object_ref (cbhttp));

	d(g_message ("Retrieval started.\n"));
	return FALSE;
}

static gboolean
reload_cb (ECalBackendHttp *cbhttp)
{
	ECalBackendHttpPrivate *priv;

	priv = cbhttp->priv;

	if (priv->is_loading)
		return TRUE;

	d(g_message ("Reload!\n"));

	priv->reload_timeout_id = 0;
	begin_retrieval_cb (cbhttp);
	return FALSE;
}

static void
maybe_start_reload_timeout (ECalBackendHttp *cbhttp)
{
	ECalBackendHttpPrivate *priv;
	ESource *source;
	const gchar *refresh_str;

	priv = cbhttp->priv;

	d(g_message ("Setting reload timeout.\n"));

	if (priv->reload_timeout_id)
		return;

	source = e_backend_get_source (E_BACKEND (cbhttp));
	if (!source) {
		g_warning ("Could not get source for ECalBackendHttp reload.");
		return;
	}

	refresh_str = e_source_get_property (source, "refresh");

	priv->reload_timeout_id = g_timeout_add ((refresh_str ? atoi (refresh_str) : 30) * 60000,
						 (GSourceFunc) reload_cb, cbhttp);
}

static void
source_changed_cb (ESource *source,
                   ECalBackendHttp *cbhttp)
{
	ECalBackendHttpPrivate *priv;

	g_return_if_fail (cbhttp != NULL);
	g_return_if_fail (cbhttp->priv != NULL);

	priv = cbhttp->priv;

	if (priv->uri) {
		ESource *source;
		const gchar *secure_prop;
		gchar *new_uri;

		source = e_backend_get_source (E_BACKEND (cbhttp));
		secure_prop = e_source_get_property (source, "use_ssl");

		new_uri = webcal_to_http_method (
			e_source_get_uri (source),
			(secure_prop && g_str_equal(secure_prop, "1")));

		if (new_uri && !g_str_equal (priv->uri, new_uri)) {
			/* uri changed, do reload some time soon */
			g_free (priv->uri);
			priv->uri = NULL;

			if (!priv->is_loading)
				g_idle_add ((GSourceFunc) begin_retrieval_cb, cbhttp);
		}

		g_free (new_uri);
	}
}

/* Open handler for the file backend */
static void
e_cal_backend_http_open (ECalBackendSync *backend,
                         EDataCal *cal,
                         GCancellable *cancellable,
                         gboolean only_if_exists,
                         GError **perror)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;
	ESource *source;
	gboolean online;
	gchar *tmp;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	/* already opened, thus can skip all this initialization */
	if (priv->opened) {
		e_cal_backend_notify_opened (E_CAL_BACKEND (backend), NULL);
		return;
	}

	source = e_backend_get_source (E_BACKEND (backend));

	if (priv->source_changed_id == 0) {
		priv->source_changed_id = g_signal_connect (source, "changed", G_CALLBACK (source_changed_cb), cbhttp);
	}

	/* always read uri again */
	tmp = priv->uri;
	priv->uri = NULL;
	g_free (tmp);

	if (!priv->store) {
		const gchar *cache_dir;

		cache_dir = e_cal_backend_get_cache_dir (E_CAL_BACKEND (backend));

		/* remove the old cache while migrating to ECalBackendStore */
		e_cal_backend_cache_remove (cache_dir, "cache.xml");
		priv->store = e_cal_backend_file_store_new (cache_dir);
		e_cal_backend_store_load (priv->store);

		if (!priv->store) {
			g_propagate_error (perror, EDC_ERROR_EX (OtherError, _("Could not create cache file")));
			e_cal_backend_notify_opened (E_CAL_BACKEND (backend), EDC_ERROR_EX (OtherError, _("Could not create cache file")));
			return;
		}
	}

	e_cal_backend_notify_readonly (E_CAL_BACKEND (backend), TRUE);

	online = e_backend_get_online (E_BACKEND (backend));
	e_cal_backend_notify_online (E_CAL_BACKEND (backend), online);

	if (online) {
		if (e_source_get_property (source, "auth")) {
			e_cal_backend_notify_auth_required (E_CAL_BACKEND (cbhttp), TRUE, priv->credentials);
		} else if (priv->requires_auth && perror && !*perror) {
			g_propagate_error (perror, EDC_ERROR (AuthenticationRequired));
			e_cal_backend_notify_opened (E_CAL_BACKEND (backend), EDC_ERROR (AuthenticationRequired));
		} else {
			e_cal_backend_notify_opened (E_CAL_BACKEND (backend), NULL);
			g_idle_add ((GSourceFunc) begin_retrieval_cb, cbhttp);
		}
	} else {
		e_cal_backend_notify_opened (E_CAL_BACKEND (backend), NULL);
	}
}

static void
e_cal_backend_http_authenticate_user (ECalBackendSync *backend,
                                      GCancellable *cancellable,
                                      ECredentials *credentials,
                                      GError **error)
{
	ECalBackendHttp        *cbhttp;
	ECalBackendHttpPrivate *priv;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv  = cbhttp->priv;

	if (priv->credentials && credentials && e_credentials_equal_keys (priv->credentials, credentials, E_CREDENTIALS_KEY_USERNAME, E_CREDENTIALS_KEY_PASSWORD, NULL)) {
		g_propagate_error (error, EDC_ERROR (AuthenticationRequired));
		return;
	}

	e_credentials_free (priv->credentials);
	priv->credentials = NULL;

	if (!credentials || !e_credentials_has_key (credentials, E_CREDENTIALS_KEY_USERNAME)) {
		g_propagate_error (error, EDC_ERROR (AuthenticationRequired));
		return;
	}

	priv->credentials = e_credentials_new_clone (credentials);

	g_idle_add ((GSourceFunc) begin_retrieval_cb, cbhttp);
}

static void
e_cal_backend_http_refresh (ECalBackendSync *backend,
                            EDataCal *cal,
                            GCancellable *cancellable,
                            GError **perror)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	if (!priv->opened ||
	    priv->is_loading)
		return;

	if (priv->reload_timeout_id)
		g_source_remove (priv->reload_timeout_id);
	priv->reload_timeout_id = 0;

	/* wait a second, then start reloading */
	priv->reload_timeout_id = g_timeout_add (1000, (GSourceFunc) reload_cb, cbhttp);
}

static void
e_cal_backend_http_remove (ECalBackendSync *backend,
                           EDataCal *cal,
                           GCancellable *cancellable,
                           GError **perror)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	if (!priv->store)
		return;

	e_cal_backend_store_remove (priv->store);
}

/* Set_mode handler for the http backend */
static void
e_cal_backend_http_notify_online_cb (ECalBackend *backend,
                                     GParamSpec *pspec)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;
	gboolean loaded;
	gboolean online;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	online = e_backend_get_online (E_BACKEND (backend));
	loaded = e_cal_backend_is_opened (backend);

	if (!online) {
		if (loaded && priv->reload_timeout_id) {
			g_source_remove (priv->reload_timeout_id);
			priv->reload_timeout_id = 0;
		}
	} else {
		if (loaded)
			g_idle_add ((GSourceFunc) begin_retrieval_cb, backend);
	}

	if (loaded)
		e_cal_backend_notify_online (backend, online);
}

/* Get_object_component handler for the http backend */
static void
e_cal_backend_http_get_object (ECalBackendSync *backend,
                               EDataCal *cal,
                               GCancellable *cancellable,
                               const gchar *uid,
                               const gchar *rid,
                               gchar **object,
                               GError **error)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;
	ECalComponent *comp = NULL;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	e_return_data_cal_error_if_fail (uid != NULL, ObjectNotFound);

	if (!priv->store) {
		g_propagate_error (error, EDC_ERROR (ObjectNotFound));
		return;
	}

	comp = e_cal_backend_store_get_component (priv->store, uid, rid);
	if (!comp) {
		g_propagate_error (error, EDC_ERROR (ObjectNotFound));
		return;
	}

	*object = e_cal_component_get_as_string (comp);
	g_object_unref (comp);
}

/* Add_timezone handler for the file backend */
static void
e_cal_backend_http_add_timezone (ECalBackendSync *backend,
                                 EDataCal *cal,
                                 GCancellable *cancellable,
                                 const gchar *tzobj,
                                 GError **error)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;
	icalcomponent *tz_comp;
	icaltimezone *zone;

	cbhttp = (ECalBackendHttp *) backend;

	e_return_data_cal_error_if_fail (E_IS_CAL_BACKEND_HTTP (cbhttp), InvalidArg);
	e_return_data_cal_error_if_fail (tzobj != NULL, InvalidArg);

	priv = cbhttp->priv;

	tz_comp = icalparser_parse_string (tzobj);
	if (!tz_comp) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	if (icalcomponent_isa (tz_comp) != ICAL_VTIMEZONE_COMPONENT) {
		icalcomponent_free (tz_comp);
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	zone = icaltimezone_new ();
	icaltimezone_set_component (zone, tz_comp);
	e_cal_backend_store_put_timezone (priv->store, zone);
}

/* Get_objects_in_range handler for the file backend */
static void
e_cal_backend_http_get_object_list (ECalBackendSync *backend,
                                    EDataCal *cal,
                                    GCancellable *cancellable,
                                    const gchar *sexp,
                                    GSList **objects,
                                    GError **perror)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;
	GSList *components, *l;
	ECalBackendSExp *cbsexp;
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	if (!priv->store) {
		g_propagate_error (perror, EDC_ERROR (NoSuchCal));
		return;
	}

	/* process all components in the cache */
	cbsexp = e_cal_backend_sexp_new (sexp);

	*objects = NULL;
	prunning_by_time = e_cal_backend_sexp_evaluate_occur_times (cbsexp,
									    &occur_start,
									    &occur_end);

	components = prunning_by_time ?
		e_cal_backend_store_get_components_occuring_in_range (priv->store, occur_start, occur_end)
		: e_cal_backend_store_get_components (priv->store);

	for (l = components; l != NULL; l = g_slist_next (l)) {
		if (e_cal_backend_sexp_match_comp (cbsexp, E_CAL_COMPONENT (l->data), E_CAL_BACKEND (backend))) {
			*objects = g_slist_append (*objects, e_cal_component_get_as_string (l->data));
		}
	}

	g_slist_foreach (components, (GFunc) g_object_unref, NULL);
	g_slist_free (components);
	g_object_unref (cbsexp);
}

static void
e_cal_backend_http_start_view (ECalBackend *backend,
                               EDataCalView *query)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;
	GSList *components, *l;
	GSList *objects = NULL;
	ECalBackendSExp *cbsexp;
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	d(g_message (G_STRLOC ": Starting query (%s)", e_data_cal_view_get_text (query)));

	if (!priv->store) {
		GError *error = EDC_ERROR (NoSuchCal);
		e_data_cal_view_notify_complete (query, error);
		g_error_free (error);
		return;
	}

	/* process all components in the cache */
	cbsexp = e_cal_backend_sexp_new (e_data_cal_view_get_text (query));

	objects = NULL;
	prunning_by_time = e_cal_backend_sexp_evaluate_occur_times (cbsexp,
									    &occur_start,
									    &occur_end);

	components = prunning_by_time ?
		e_cal_backend_store_get_components_occuring_in_range (priv->store, occur_start, occur_end)
		: e_cal_backend_store_get_components (priv->store);

	for (l = components; l != NULL; l = g_slist_next (l)) {
		ECalComponent *comp = l->data;

		if (e_cal_backend_sexp_match_comp (cbsexp, comp, E_CAL_BACKEND (backend))) {
			objects = g_slist_append (objects, comp);
		}
	}

	e_data_cal_view_notify_components_added (query, objects);

	g_slist_free_full (components, g_object_unref);
	g_slist_free (objects);
	g_object_unref (cbsexp);

	e_data_cal_view_notify_complete (query, NULL /* Success */);
}

/***** static icaltimezone *
resolve_tzid (const gchar *tzid,
 *            gpointer user_data)
{
	icalcomponent *vcalendar_comp = user_data;
 *
	if (!tzid || !tzid[0])
		return NULL;
 *      else if (!strcmp (tzid, "UTC"))
		return icaltimezone_get_utc_timezone ();
 *
	return icalcomponent_get_timezone (vcalendar_comp, tzid);
} *****/

static gboolean
free_busy_instance (ECalComponent *comp,
                    time_t instance_start,
                    time_t instance_end,
                    gpointer data)
{
	icalcomponent *vfb = data;
	icalproperty *prop;
	icalparameter *param;
	struct icalperiodtype ipt;
	icaltimezone *utc_zone;

	utc_zone = icaltimezone_get_utc_timezone ();

	ipt.start = icaltime_from_timet_with_zone (instance_start, FALSE, utc_zone);
	ipt.end = icaltime_from_timet_with_zone (instance_end, FALSE, utc_zone);
	ipt.duration = icaldurationtype_null_duration ();

        /* add busy information to the vfb component */
	prop = icalproperty_new (ICAL_FREEBUSY_PROPERTY);
	icalproperty_set_freebusy (prop, ipt);

	param = icalparameter_new_fbtype (ICAL_FBTYPE_BUSY);
	icalproperty_add_parameter (prop, param);

	icalcomponent_add_property (vfb, prop);

	return TRUE;
}

static icalcomponent *
create_user_free_busy (ECalBackendHttp *cbhttp,
                       const gchar *address,
                       const gchar *cn,
                       time_t start,
                       time_t end)
{
	GSList *slist = NULL, *l;
	icalcomponent *vfb;
	icaltimezone *utc_zone;
	ECalBackendSExp *obj_sexp;
	ECalBackendHttpPrivate *priv;
	ECalBackendStore *store;
	gchar *query, *iso_start, *iso_end;

	priv = cbhttp->priv;
	store = priv->store;

        /* create the (unique) VFREEBUSY object that we'll return */
	vfb = icalcomponent_new_vfreebusy ();
	if (address != NULL) {
		icalproperty *prop;
		icalparameter *param;

		prop = icalproperty_new_organizer (address);
		if (prop != NULL && cn != NULL) {
			param = icalparameter_new_cn (cn);
			icalproperty_add_parameter (prop, param);
		}
		if (prop != NULL)
			icalcomponent_add_property (vfb, prop);
	}
	utc_zone = icaltimezone_get_utc_timezone ();
	icalcomponent_set_dtstart (vfb, icaltime_from_timet_with_zone (start, FALSE, utc_zone));
	icalcomponent_set_dtend (vfb, icaltime_from_timet_with_zone (end, FALSE, utc_zone));

        /* add all objects in the given interval */
	iso_start = isodate_from_time_t (start);
	iso_end = isodate_from_time_t (end);
        query = g_strdup_printf ("occur-in-time-range? (make-time \"%s\") (make-time \"%s\")",
				 iso_start, iso_end);
	obj_sexp = e_cal_backend_sexp_new (query);
	g_free (query);
	g_free (iso_start);
	g_free (iso_end);

	if (!obj_sexp)
		return vfb;

	slist = e_cal_backend_store_get_components (store);

	for (l = slist; l; l = g_slist_next (l)) {
		ECalComponent *comp = l->data;
		icalcomponent *icalcomp, *vcalendar_comp;
		icalproperty *prop;

		icalcomp = e_cal_component_get_icalcomponent (comp);
		if (!icalcomp)
			continue;

                /* If the event is TRANSPARENT, skip it. */
		prop = icalcomponent_get_first_property (icalcomp,
							 ICAL_TRANSP_PROPERTY);
		if (prop) {
			icalproperty_transp transp_val = icalproperty_get_transp (prop);
			if (transp_val == ICAL_TRANSP_TRANSPARENT ||
			    transp_val == ICAL_TRANSP_TRANSPARENTNOCONFLICT)
				continue;
		}

		if (!e_cal_backend_sexp_match_comp (obj_sexp, l->data, E_CAL_BACKEND (cbhttp)))
			continue;

		vcalendar_comp = icalcomponent_get_parent (icalcomp);
		if (!vcalendar_comp)
			vcalendar_comp = icalcomp;
		e_cal_recur_generate_instances (comp, start, end,
						free_busy_instance,
						vfb,
						resolve_tzid,
						vcalendar_comp,
						icaltimezone_get_utc_timezone ());
	}
	g_object_unref (obj_sexp);

	return vfb;
}

/* Get_free_busy handler for the file backend */
static void
e_cal_backend_http_get_free_busy (ECalBackendSync *backend,
                                  EDataCal *cal,
                                  GCancellable *cancellable,
                                  const GSList *users,
                                  time_t start,
                                  time_t end,
                                  GSList **freebusy,
                                  GError **error)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;
	gchar *address, *name;
	icalcomponent *vfb;
	gchar *calobj;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	e_return_data_cal_error_if_fail (start != -1 && end != -1, InvalidRange);
	e_return_data_cal_error_if_fail (start <= end, InvalidRange);

	if (!priv->store) {
		g_propagate_error (error, EDC_ERROR (NoSuchCal));
		return;
	}

	if (users == NULL) {
		if (e_cal_backend_mail_account_get_default (&address, &name)) {
			vfb = create_user_free_busy (cbhttp, address, name, start, end);
			calobj = icalcomponent_as_ical_string_r (vfb);
                        *freebusy = g_slist_append (*freebusy, calobj);
			icalcomponent_free (vfb);
			g_free (address);
			g_free (name);
		}
	} else {
		const GSList *l;
		for (l = users; l != NULL; l = l->next ) {
			address = l->data;
			if (e_cal_backend_mail_account_is_valid (address, &name)) {
				vfb = create_user_free_busy (cbhttp, address, name, start, end);
				calobj = icalcomponent_as_ical_string_r (vfb);
                                *freebusy = g_slist_append (*freebusy, calobj);
				icalcomponent_free (vfb);
				g_free (name);
			}
		}
	}
}

static void
e_cal_backend_http_create_object (ECalBackendSync *backend,
                                  EDataCal *cal,
                                  GCancellable *cancellable,
                                  const gchar *calobj,
                                  gchar **uid,
                                  ECalComponent **new_component,
                                  GError **perror)
{
	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

static void
e_cal_backend_http_modify_object (ECalBackendSync *backend,
                                  EDataCal *cal,
                                  GCancellable *cancellable,
                                  const gchar *calobj,
                                  CalObjModType mod,
                                  ECalComponent **old_component,
                                  ECalComponent **new_component,
                                  GError **perror)
{
	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

/* Remove_object handler for the file backend */
static void
e_cal_backend_http_remove_object (ECalBackendSync *backend,
                                  EDataCal *cal,
                                  GCancellable *cancellable,
                                  const gchar *uid,
                                  const gchar *rid,
                                  CalObjModType mod,
                                  ECalComponent **old_component,
                                  ECalComponent **new_component,
                                  GError **perror)
{
	*old_component = *new_component = NULL;

	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

/* Update_objects handler for the file backend. */
static void
e_cal_backend_http_receive_objects (ECalBackendSync *backend,
                                    EDataCal *cal,
                                    GCancellable *cancellable,
                                    const gchar *calobj,
                                    GError **perror)
{
	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

static void
e_cal_backend_http_send_objects (ECalBackendSync *backend,
                                 EDataCal *cal,
                                 GCancellable *cancellable,
                                 const gchar *calobj,
                                 GSList **users,
                                 gchar **modified_calobj,
                                 GError **perror)
{
	*users = NULL;
	*modified_calobj = NULL;

	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

static icaltimezone *
e_cal_backend_http_internal_get_timezone (ECalBackend *backend,
                                          const gchar *tzid)
{
	ECalBackendHttp *cbhttp;
	ECalBackendHttpPrivate *priv;
	icaltimezone *zone;

	cbhttp = E_CAL_BACKEND_HTTP (backend);
	priv = cbhttp->priv;

	g_return_val_if_fail (tzid != NULL, NULL);

	if (!strcmp (tzid, "UTC"))
		zone = icaltimezone_get_utc_timezone ();
	else {
		/* first try to get the timezone from the cache */
		zone = (icaltimezone *) e_cal_backend_store_get_timezone (priv->store, tzid);

		if (!zone && E_CAL_BACKEND_CLASS (e_cal_backend_http_parent_class)->internal_get_timezone)
			zone = E_CAL_BACKEND_CLASS (e_cal_backend_http_parent_class)->internal_get_timezone (backend, tzid);
	}

	return zone;
}

/* Object initialization function for the file backend */
static void
e_cal_backend_http_init (ECalBackendHttp *cbhttp)
{
	cbhttp->priv = E_CAL_BACKEND_HTTP_GET_PRIVATE (cbhttp);

	e_cal_backend_sync_set_lock (E_CAL_BACKEND_SYNC (cbhttp), TRUE);

	g_signal_connect (
		cbhttp, "notify::online",
		G_CALLBACK (e_cal_backend_http_notify_online_cb), NULL);
}

/* Class initialization function for the file backend */
static void
e_cal_backend_http_class_init (ECalBackendHttpClass *class)
{
	GObjectClass *object_class;
	ECalBackendClass *backend_class;
	ECalBackendSyncClass *sync_class;

	g_type_class_add_private (class, sizeof (ECalBackendHttpPrivate));

	object_class = (GObjectClass *) class;
	backend_class = (ECalBackendClass *) class;
	sync_class = (ECalBackendSyncClass *) class;

	object_class->dispose = e_cal_backend_http_dispose;
	object_class->finalize = e_cal_backend_http_finalize;

	sync_class->get_backend_property_sync	= e_cal_backend_http_get_backend_property;
	sync_class->open_sync			= e_cal_backend_http_open;
	sync_class->authenticate_user_sync	= e_cal_backend_http_authenticate_user;
	sync_class->refresh_sync		= e_cal_backend_http_refresh;
	sync_class->remove_sync			= e_cal_backend_http_remove;
	sync_class->create_object_sync		= e_cal_backend_http_create_object;
	sync_class->modify_object_sync		= e_cal_backend_http_modify_object;
	sync_class->remove_object_sync		= e_cal_backend_http_remove_object;
	sync_class->receive_objects_sync	= e_cal_backend_http_receive_objects;
	sync_class->send_objects_sync		= e_cal_backend_http_send_objects;
	sync_class->get_object_sync		= e_cal_backend_http_get_object;
	sync_class->get_object_list_sync	= e_cal_backend_http_get_object_list;
	sync_class->add_timezone_sync		= e_cal_backend_http_add_timezone;
	sync_class->get_free_busy_sync		= e_cal_backend_http_get_free_busy;

	backend_class->start_view		= e_cal_backend_http_start_view;
	backend_class->internal_get_timezone	= e_cal_backend_http_internal_get_timezone;
}
