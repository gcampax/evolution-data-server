/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* Evolution calendar - iCalendar file backend
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2003 Gerg� �rdi
 *
 * Authors: Federico Mena-Quintero <federico@ximian.com>
 *          Rodrigo Moya <rodrigo@ximian.com>
 *          Gerg� �rdi <cactus@cactus.rulez.org>
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
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "e-cal-backend-contacts.h"

#include <glib/gi18n-lib.h>
#include <gconf/gconf-client.h>
#include <libsoup/soup.h>

#include <libedataserver/e-xml-hash-utils.h>
#include <libedataserver/e-source-list.h>
#include <libedataserver/e-flag.h>
#include <libecal/e-cal-recur.h>
#include <libecal/e-cal-util.h>
#include <libedata-cal/e-cal-backend-util.h>
#include <libedata-cal/e-cal-backend-sexp.h>
#include <libebook/e-book-client.h>
#include <libebook/e-book-query.h>
#include <libebook/e-contact.h>

#define E_CAL_BACKEND_CONTACTS_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_CAL_BACKEND_CONTACTS, ECalBackendContactsPrivate))

#define EDC_ERROR(_code) e_data_cal_create_error (_code, NULL)

G_DEFINE_TYPE (
	ECalBackendContacts,
	e_cal_backend_contacts,
	E_TYPE_CAL_BACKEND_SYNC)

typedef enum
{
	CAL_DAYS,
	CAL_HOURS,
	CAL_MINUTES
} CalUnits;

/* Private part of the ECalBackendContacts structure */
struct _ECalBackendContactsPrivate {
        ESourceList  *addressbook_sources;

	GHashTable   *addressbooks;       /* UID -> BookRecord */
	GHashTable   *credentials;	  /* UID -> ECredentials to use in "authenticate" handler */
	gboolean      loaded;

	EBookClientView *book_view;
	GHashTable   *tracked_contacts;   /* UID -> ContactRecord */

	GHashTable *zones;

	EFlag   *init_done_flag; /* is set, when the init thread gone */

	/* properties related to track alarm settings for this backend */
	GConfClient *conf_client;
	guint notifyid1;
	guint notifyid2;
	guint notifyid3;
	guint update_alarms_id;
	gboolean alarm_enabled;
	gint alarm_interval;
	CalUnits alarm_units;
};

typedef struct _BookRecord {
	ECalBackendContacts *cbc;
	EBookClient *book_client;
	EBookClientView *book_view;
} BookRecord;

typedef struct _ContactRecord {
	ECalBackendContacts *cbc;
	EBookClient *book_client; /* where it comes from */
	EContact	    *contact;
	ECalComponent       *comp_birthday, *comp_anniversary;
} ContactRecord;

#define d(x)

#define ANNIVERSARY_UID_EXT "-anniversary"
#define BIRTHDAY_UID_EXT   "-birthday"

#define CBC_CREDENTIALS_KEY_SOURCE_UID "cbc-source-uid"
#define CBC_CREDENTIALS_KEY_ALREADY_USED "cbc-already-used"

static ECalComponent * create_birthday (ECalBackendContacts *cbc, EContact *contact);
static ECalComponent * create_anniversary (ECalBackendContacts *cbc, EContact *contact);

static void contacts_modified_cb (EBookClientView *book_view, const GSList *contacts, gpointer user_data);
static void contacts_added_cb   (EBookClientView *book_view, const GSList *contacts, gpointer user_data);
static void contacts_removed_cb (EBookClientView *book_view, const GSList *contact_ids, gpointer user_data);
static void e_cal_backend_contacts_add_timezone (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const gchar *tzobj, GError **perror);
static void setup_alarm (ECalBackendContacts *cbc, ECalComponent *comp);

static gboolean
book_client_authenticate_cb (EClient *client,
                             ECredentials *credentials,
                             ECalBackendContacts *cbc)
{
	ESource *source;
	const gchar *source_uid;
	ECredentials *use_credentials;

	g_return_val_if_fail (client != NULL, FALSE);
	g_return_val_if_fail (credentials != NULL, FALSE);
	g_return_val_if_fail (cbc != NULL, FALSE);

	source = e_client_get_source (client);

	source_uid = e_source_peek_uid (source);
	g_return_val_if_fail (source_uid != NULL, FALSE);

	use_credentials = g_hash_table_lookup (cbc->priv->credentials, source_uid);

	if (use_credentials &&
	    g_strcmp0 (e_credentials_peek (use_credentials, CBC_CREDENTIALS_KEY_ALREADY_USED), "1") != 0) {
		GSList *keys, *iter;

		e_credentials_clear (credentials);
		keys = e_credentials_list_keys (use_credentials);

		for (iter = keys; iter; iter = iter->next) {
			const gchar *key = iter->data;

			e_credentials_set (credentials, key, e_credentials_peek (use_credentials, key));
		}

		e_credentials_set (credentials, E_CREDENTIALS_KEY_FOREIGN_REQUEST, NULL);

		e_credentials_clear_peek (use_credentials);
		e_credentials_set (use_credentials, CBC_CREDENTIALS_KEY_ALREADY_USED, "1");
		g_slist_free (keys);

		return TRUE;
	}

	/* write properties on a copy of original credentials */
	credentials = e_credentials_new_clone (credentials);

	if (!e_credentials_has_key (credentials, E_CREDENTIALS_KEY_USERNAME)) {
		const gchar *username;

		username = e_source_get_property (source, "username");
		if (!username) {
			const gchar *auth;

			auth = e_source_get_property (source, "auth");
			if (g_strcmp0 (auth, "ldap/simple-binddn") == 0)
				username = e_source_get_property (source, "binddn");
			else
				username = e_source_get_property (source, "email_addr");

			if (!username)
				username = "";
		}

		e_credentials_set (credentials, E_CREDENTIALS_KEY_USERNAME, username);

		/* no username set on the source - deny authentication request */
		if (!e_credentials_has_key (credentials, E_CREDENTIALS_KEY_USERNAME))
			return FALSE;
	}

	if (!e_credentials_has_key (credentials, E_CREDENTIALS_KEY_PROMPT_KEY)) {
		gchar *prompt_key;
		SoupURI *suri;

		suri = soup_uri_new (e_client_get_uri (client));
		g_return_val_if_fail (suri != NULL, FALSE);

		soup_uri_set_user (suri, e_credentials_peek (credentials, E_CREDENTIALS_KEY_USERNAME));
		soup_uri_set_password (suri, NULL);
		soup_uri_set_fragment (suri, NULL);

		prompt_key = soup_uri_to_string (suri, FALSE);
		soup_uri_free (suri);

		e_credentials_set (credentials, E_CREDENTIALS_KEY_PROMPT_KEY, prompt_key);

		g_free (prompt_key);
	}

	if (!e_credentials_has_key (credentials, E_CREDENTIALS_KEY_PROMPT_TEXT)) {
		gchar *prompt, *reason;
		gchar *username_markup, *source_name_markup;

		reason = e_credentials_get (credentials, E_CREDENTIALS_KEY_PROMPT_REASON);
		username_markup = g_markup_printf_escaped ("<b>%s</b>", e_credentials_peek (credentials, E_CREDENTIALS_KEY_USERNAME));
		source_name_markup = g_markup_printf_escaped ("<b>%s</b>", e_source_peek_name (source));

		if (reason && *reason)
			prompt = g_strdup_printf (_("Enter password for address book %s (user %s)\nReason: %s"), source_name_markup, username_markup, reason);
		else
			prompt = g_strdup_printf (_("Enter password for address book %s (user %s)"), source_name_markup, username_markup);

		e_credentials_set (credentials, E_CREDENTIALS_KEY_PROMPT_TEXT, prompt);

		g_free (username_markup);
		g_free (source_name_markup);
		g_free (reason);
		g_free (prompt);
	}

	e_credentials_set (credentials, E_CREDENTIALS_KEY_FOREIGN_REQUEST, "1");
	e_credentials_set (credentials, CBC_CREDENTIALS_KEY_SOURCE_UID, e_source_peek_uid (source));

	/* this is a reprompt, set proper flags */
	if (use_credentials) {
		guint prompt_flags;
		gchar *prompt_flags_str;

		if (e_credentials_has_key (credentials, E_CREDENTIALS_KEY_PROMPT_FLAGS)) {
			prompt_flags = e_credentials_util_string_to_prompt_flags (e_credentials_peek (credentials, E_CREDENTIALS_KEY_PROMPT_FLAGS));
		} else {
			prompt_flags = E_CREDENTIALS_PROMPT_FLAG_REMEMBER_FOREVER
				     | E_CREDENTIALS_PROMPT_FLAG_SECRET
				     | E_CREDENTIALS_PROMPT_FLAG_ONLINE;
		}

		prompt_flags |= E_CREDENTIALS_PROMPT_FLAG_REPROMPT;
		prompt_flags_str = e_credentials_util_prompt_flags_to_string (prompt_flags);
		e_credentials_set (credentials, E_CREDENTIALS_KEY_PROMPT_FLAGS, prompt_flags_str);
		g_free (prompt_flags_str);
	}

	e_cal_backend_notify_auth_required (E_CAL_BACKEND (cbc), FALSE, credentials);

	/* this is a copy of original credentials */
	e_credentials_free (credentials);

	return FALSE;
}

static void book_client_opened_cb (EBookClient *book_client, const GError *error, ECalBackendContacts *cbc);

static gpointer
cbc_reopen_book_client_thread (gpointer user_data)
{
	EBookClient *book_client = user_data;
	gboolean done = FALSE;

	while (!done) {
		done = TRUE;

		if (!e_client_is_opened (E_CLIENT (book_client))) {
			GError *error = NULL;

			if (!e_client_open_sync (E_CLIENT (book_client), TRUE, NULL, &error) || error) {
				if (g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_BUSY)) {
					done = FALSE;
					g_usleep (500000);
				} else
					g_warning ("%s: Failed to open book: %s", G_STRFUNC, error ? error->message : "Unknown error");
			}

			g_clear_error (&error);
		}
	}

	g_object_unref (book_client);

	return NULL;
}

static void
cbc_reopen_book_client (ECalBackendContacts *cbc,
                        EBookClient *book_client)
{
	GError *error = NULL;

	g_return_if_fail (book_client != NULL);

	/* make sure signal handlers are disconnected */
	g_signal_handlers_disconnect_by_func (book_client, G_CALLBACK (book_client_authenticate_cb), cbc);
	g_signal_handlers_disconnect_by_func (book_client, G_CALLBACK (book_client_opened_cb), cbc);

	/* and connect them */
	g_signal_connect (book_client, "authenticate", G_CALLBACK (book_client_authenticate_cb), cbc);
	g_signal_connect (book_client, "opened", G_CALLBACK (book_client_opened_cb), cbc);

	g_object_ref (book_client);
	if (!g_thread_create (cbc_reopen_book_client_thread, book_client, FALSE, &error)) {
		g_object_unref (book_client);

		g_warning ("%s: Cannot create thread to reload source! (%s)", G_STRFUNC, error ? error->message : "Unknown error");
		g_clear_error (&error);
	}
}

static void
book_client_opened_cb (EBookClient *book_client,
                       const GError *error,
                       ECalBackendContacts *cbc)
{
	ESource *source;
	const gchar *source_uid;
	BookRecord *br = NULL;

	g_return_if_fail (book_client != NULL);
	g_return_if_fail (cbc != NULL);

	g_signal_handlers_disconnect_by_func (book_client, G_CALLBACK (book_client_authenticate_cb), cbc);
	g_signal_handlers_disconnect_by_func (book_client, G_CALLBACK (book_client_opened_cb), cbc);

	source = e_client_get_source (E_CLIENT (book_client));
	source_uid = e_source_peek_uid (source);
	g_return_if_fail (source_uid != NULL);

	if (source_uid) {
		br = g_hash_table_lookup (cbc->priv->addressbooks, source_uid);
		if (br && br->book_client != book_client)
			br = NULL;
	}

	if (!br)
		return;

	if (g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_AUTHENTICATION_FAILED)) {
		if (g_hash_table_lookup (cbc->priv->credentials, source_uid)) {
			cbc_reopen_book_client (cbc, br->book_client);
		}
	} else if (!error) {
		EBookQuery *query;
		EBookClientView *book_view;
		gchar *query_sexp;
		GError *error = NULL;

		if (br->book_view)
			g_object_unref (br->book_view);
		br->book_view = NULL;

		query = e_book_query_andv (
			e_book_query_orv (
				e_book_query_field_exists (E_CONTACT_FILE_AS),
				e_book_query_field_exists (E_CONTACT_FULL_NAME),
				e_book_query_field_exists (E_CONTACT_GIVEN_NAME),
				e_book_query_field_exists (E_CONTACT_NICKNAME),
				NULL),
			e_book_query_orv (
				e_book_query_field_exists (E_CONTACT_BIRTH_DATE),
				e_book_query_field_exists (E_CONTACT_ANNIVERSARY),
				NULL),
			NULL);
		query_sexp = e_book_query_to_string (query);
		e_book_query_unref (query);

		if (!e_book_client_get_view_sync (book_client, query_sexp, &book_view, NULL, &error))
			g_warning ("%s: Failed to get book view on '%s': %s", G_STRFUNC, e_source_peek_name (source), error ? error->message : "Unknown error");
		g_free (query_sexp);
		g_clear_error (&error);

		g_signal_connect (book_view, "objects-added", G_CALLBACK (contacts_added_cb), cbc);
		g_signal_connect (book_view, "objects-removed", G_CALLBACK (contacts_removed_cb), cbc);
		g_signal_connect (book_view, "objects-modified", G_CALLBACK (contacts_modified_cb), cbc);

		e_book_client_view_start (book_view, NULL);

		br->book_view = book_view;
	}
}

static void
e_cal_backend_contacts_authenticate_user (ECalBackendSync *backend,
                                          GCancellable *cancellable,
                                          ECredentials *credentials,
                                          GError **error)
{
	ECalBackendContacts *cbc;
	const gchar *source_uid;
	BookRecord *br;

	g_return_if_fail (backend != NULL);
	g_return_if_fail (credentials != NULL);

	cbc = E_CAL_BACKEND_CONTACTS (backend);
	g_return_if_fail (cbc != NULL);

	source_uid = e_credentials_peek (credentials, CBC_CREDENTIALS_KEY_SOURCE_UID);
	g_return_if_fail (source_uid != NULL);

	br = g_hash_table_lookup (cbc->priv->addressbooks, source_uid);
	if (!br || !br->book_client ||
	    /* no username means user cancelled password prompt */
	    !e_credentials_has_key (credentials, E_CREDENTIALS_KEY_USERNAME)) {
		g_hash_table_remove (cbc->priv->credentials, source_uid);
		return;
	}

	g_hash_table_insert (cbc->priv->credentials, g_strdup (source_uid), e_credentials_new_clone (credentials));

	cbc_reopen_book_client (cbc, br->book_client);
}

/* BookRecord methods */
static BookRecord *
book_record_new (ECalBackendContacts *cbc,
                 ESource *source)
{
	EBookClient *book_client;
	BookRecord *br;
	GError *error = NULL;

	book_client = e_book_client_new (source, &error);
	if (!book_client) {
		g_warning ("%s: Failed to create new book: %s", G_STRFUNC, error ? error->message : "Unknown error");
		g_clear_error (&error);

		return NULL;
	}

	g_signal_connect (book_client, "authenticate", G_CALLBACK (book_client_authenticate_cb), cbc);
	g_signal_connect (book_client, "opened", G_CALLBACK (book_client_opened_cb), cbc);

	if (!e_client_open_sync (E_CLIENT (book_client), TRUE, NULL, &error) || error) {
		g_signal_handlers_disconnect_by_func (book_client, G_CALLBACK (book_client_authenticate_cb), cbc);
		g_signal_handlers_disconnect_by_func (book_client, G_CALLBACK (book_client_opened_cb), cbc);
		g_object_unref (book_client);
		if (error) {
			g_warning ("%s: Failed to open book: %s", G_STRFUNC, error->message);
			g_error_free (error);
		}
		return NULL;
	}

	br = g_new0 (BookRecord, 1);
	br->cbc = cbc;
	br->book_client = book_client;
	br->book_view = NULL;

	return br;
}

static gboolean
remove_by_book (gpointer key,
                gpointer value,
                gpointer user_data)
{
	ContactRecord *cr = value;
	EBookClient *book_client = user_data;

	return (cr && cr->book_client == book_client);
}

static void
book_record_free (BookRecord *br)
{
	if (!br)
		return;

	g_signal_handlers_disconnect_by_func (br->book_client, G_CALLBACK (book_client_authenticate_cb), br->cbc);
	g_signal_handlers_disconnect_by_func (br->book_client, G_CALLBACK (book_client_opened_cb), br->cbc);
	g_hash_table_foreach_remove (br->cbc->priv->tracked_contacts, remove_by_book, br->book_client);
	if (br->book_view)
		g_object_unref (br->book_view);
	g_object_unref (br->book_client);

	g_free (br);
}

/* ContactRecord methods */
static ContactRecord *
contact_record_new (ECalBackendContacts *cbc,
                    EBookClient *book_client,
                    EContact *contact)
{
	ContactRecord *cr = g_new0 (ContactRecord, 1);

	cr->cbc = cbc;
	cr->book_client = book_client;
	cr->contact = contact;
	cr->comp_birthday = create_birthday (cbc, contact);
	cr->comp_anniversary = create_anniversary (cbc, contact);

	if (cr->comp_birthday)
		e_cal_backend_notify_component_created (E_CAL_BACKEND (cbc), cr->comp_birthday);

	if (cr->comp_anniversary)
		e_cal_backend_notify_component_created (E_CAL_BACKEND (cbc), cr->comp_anniversary);

	g_object_ref (G_OBJECT (contact));

	return cr;
}

static void
contact_record_free (ContactRecord *cr)
{
	ECalComponentId *id;

	g_object_unref (G_OBJECT (cr->contact));

	/* Remove the birthday event */
	if (cr->comp_birthday) {
		id = e_cal_component_get_id (cr->comp_birthday);
		e_cal_backend_notify_component_removed (E_CAL_BACKEND (cr->cbc), id, cr->comp_birthday, NULL);

		e_cal_component_free_id (id);
		g_object_unref (G_OBJECT (cr->comp_birthday));
	}

	/* Remove the anniversary event */
	if (cr->comp_anniversary) {
		id = e_cal_component_get_id (cr->comp_anniversary);

		e_cal_backend_notify_component_removed (E_CAL_BACKEND (cr->cbc), id, cr->comp_anniversary, NULL);

		e_cal_component_free_id (id);
		g_object_unref (G_OBJECT (cr->comp_anniversary));
	}

	g_free (cr);
}

/* ContactRecordCB methods */
typedef struct _ContactRecordCB {
        ECalBackendContacts *cbc;
        ECalBackendSExp     *sexp;
	gboolean	     as_string;
        GSList              *result;
} ContactRecordCB;

static ContactRecordCB *
contact_record_cb_new (ECalBackendContacts *cbc,
                       ECalBackendSExp *sexp,
                       gboolean as_string)
{
	ContactRecordCB *cb_data = g_new (ContactRecordCB, 1);

	cb_data->cbc = cbc;
	cb_data->sexp = sexp;
	cb_data->as_string = as_string;
	cb_data->result = NULL;

	return cb_data;
}

static void
contact_record_cb_free (ContactRecordCB *cb_data)
{
	if (cb_data->as_string)
		g_slist_foreach (cb_data->result, (GFunc) g_free, NULL);
	g_slist_free (cb_data->result);

	g_free (cb_data);
}

static void
contact_record_cb (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
	ContactRecordCB *cb_data = user_data;
	ContactRecord   *record = value;
	gpointer data;

	if (record->comp_birthday && e_cal_backend_sexp_match_comp (cb_data->sexp, record->comp_birthday, E_CAL_BACKEND (cb_data->cbc))) {
		if (cb_data->as_string)
			data = e_cal_component_get_as_string (record->comp_birthday);
		else
			data = record->comp_birthday;

		cb_data->result = g_slist_prepend (cb_data->result, data);
	}

	if (record->comp_anniversary && e_cal_backend_sexp_match_comp (cb_data->sexp, record->comp_anniversary, E_CAL_BACKEND (cb_data->cbc))) {
		if (cb_data->as_string)
			data = e_cal_component_get_as_string (record->comp_anniversary);
		else
			data = record->comp_anniversary;

		cb_data->result = g_slist_prepend (cb_data->result, data);
	}
}

static gboolean
is_source_usable (ESource *source,
                  ESourceGroup *group)
{
	const gchar *base_uri;
	const gchar *prop;

	base_uri = e_source_group_peek_base_uri (group);
	if (!base_uri)
		return FALSE;

	prop = e_source_get_property (source, "use-in-contacts-calendar");

	/* the later check is for backward compatibility */
	return (prop && g_str_equal (prop, "1")) || (!prop && g_str_has_prefix (base_uri, "file://")) || (!prop && g_str_has_prefix (base_uri, "local:"));
}

/* SourceList callbacks */
static void
add_source (ECalBackendContacts *cbc,
            ESource *source)
{
	BookRecord *br = book_record_new (cbc, source);
	const gchar *uid = e_source_peek_uid (source);

	if (!br)
		return;

	g_hash_table_insert (cbc->priv->addressbooks, g_strdup (uid), br);
}

static void
source_added_cb (ESourceGroup *group,
                 ESource *source,
                 gpointer user_data)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (user_data);

	g_return_if_fail (cbc);

	if (is_source_usable (source, group))
		add_source (cbc, source);
}

static void
source_removed_cb (ESourceGroup *group,
                   ESource *source,
                   gpointer user_data)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (user_data);
	const gchar          *uid = e_source_peek_uid (source);

	g_return_if_fail (cbc);

	g_hash_table_remove (cbc->priv->addressbooks, uid);
	g_hash_table_remove (cbc->priv->credentials, uid);
}

static void
source_list_changed_cb (ESourceList *source_list,
                        gpointer user_data)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (user_data);
	GSList *g, *s;

	g_return_if_fail (cbc);

	for (g = e_source_list_peek_groups (source_list); g; g = g->next) {
		ESourceGroup *group = E_SOURCE_GROUP (g->data);

		if (!group)
			continue;

		for (s = e_source_group_peek_sources (group); s; s = s->next) {
			ESource *source = E_SOURCE (s->data);
			const gchar *uid;

			if (!source)
				continue;

			uid = e_source_peek_uid (source);
			if (!uid)
				continue;

			if (is_source_usable (source, group)) {
				if (!g_hash_table_lookup (cbc->priv->addressbooks, uid))
					source_added_cb (group, source, cbc);
			} else if (g_hash_table_lookup (cbc->priv->addressbooks, uid)) {
				source_removed_cb (group, source, cbc);
			}
		}
	}
}

static void
source_group_added_cb (ESourceList *source_list,
                       ESourceGroup *group,
                       gpointer user_data)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (user_data);
	GSList *i;

	g_return_if_fail (cbc);

	for (i = e_source_group_peek_sources (group); i; i = i->next) {
		ESource *source = E_SOURCE (i->data);
		source_added_cb (group, source, cbc);
	}

	/* Watch for future changes */
	g_signal_connect (group, "source_added", G_CALLBACK (source_added_cb), cbc);
	g_signal_connect (group, "source_removed", G_CALLBACK (source_removed_cb), cbc);
}

static void
source_group_removed_cb (ESourceList *source_list,
                         ESourceGroup *group,
                         gpointer user_data)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (user_data);
	GSList *i = NULL;

	g_return_if_fail (cbc);

        /* Unload all address books from this group */
	for (i = e_source_group_peek_sources (group); i; i = i->next) {
		ESource *source = E_SOURCE (i->data);
		const gchar *uid = e_source_peek_uid (source);

		g_hash_table_remove (cbc->priv->addressbooks, uid);
		g_hash_table_remove (cbc->priv->credentials, uid);
	}
}

/************************************************************************************/

static void
contacts_modified_cb (EBookClientView *book_view,
                      const GSList *contacts,
                      gpointer user_data)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (user_data);
	EBookClient *book_client = e_book_client_view_get_client (book_view);
	const GSList *ii;

	for (ii = contacts; ii; ii = ii->next) {
		EContact *contact = E_CONTACT (ii->data);
		const gchar *uid = e_contact_get_const (contact, E_CONTACT_UID);
		EContactDate *birthday, *anniversary;

		/* Because this is a change of contact, then always remove old tracked data
		 * and if possible, add with (possibly) new values.
		*/
		g_hash_table_remove (cbc->priv->tracked_contacts, (gchar *) uid);

		birthday = e_contact_get (contact, E_CONTACT_BIRTH_DATE);
		anniversary = e_contact_get (contact, E_CONTACT_ANNIVERSARY);

		if (birthday || anniversary) {
			ContactRecord *cr = contact_record_new (cbc, book_client, contact);
			g_hash_table_insert (cbc->priv->tracked_contacts, g_strdup (uid), cr);
		}

		e_contact_date_free (birthday);
		e_contact_date_free (anniversary);
	}
}

static void
contacts_added_cb (EBookClientView *book_view,
                   const GSList *contacts,
                   gpointer user_data)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (user_data);
	EBookClient *book_client = e_book_client_view_get_client (book_view);
	const GSList *ii;

	/* See if any new contacts have BIRTHDAY or ANNIVERSARY fields */
	for (ii = contacts; ii; ii = ii->next) {
		EContact *contact = E_CONTACT (ii->data);
		EContactDate *birthday, *anniversary;

		birthday = e_contact_get (contact, E_CONTACT_BIRTH_DATE);
		anniversary = e_contact_get (contact, E_CONTACT_ANNIVERSARY);

		if (birthday || anniversary) {
			ContactRecord *cr = contact_record_new (cbc, book_client, contact);
			const gchar    *uid = e_contact_get_const (contact, E_CONTACT_UID);

			g_hash_table_insert (cbc->priv->tracked_contacts, g_strdup (uid), cr);
		}

		e_contact_date_free (birthday);
		e_contact_date_free (anniversary);
	}
}

static void
contacts_removed_cb (EBookClientView *book_view,
                     const GSList *contact_ids,
                     gpointer user_data)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (user_data);
	const GSList *ii;

	/* Stop tracking these */
	for (ii = contact_ids; ii; ii = ii->next)
		g_hash_table_remove (cbc->priv->tracked_contacts, ii->data);
}

/************************************************************************************/
static struct icaltimetype
cdate_to_icaltime (EContactDate *cdate)
{
	struct icaltimetype ret = icaltime_null_time ();

	ret.year = cdate->year;
	ret.month = cdate->month;
	ret.day = cdate->day;
	ret.is_date = TRUE;
	ret.is_utc = FALSE;
	ret.zone = NULL;
	ret.is_daylight = FALSE;

	ret.hour = ret.minute = ret.second = 0;

	return ret;
}

static void
manage_comp_alarm_update (ECalBackendContacts *cbc,
                          ECalComponent *comp)
{
	gchar *old_comp_str, *new_comp_str;
	ECalComponent *old_comp;

	g_return_if_fail (cbc != NULL);
	g_return_if_fail (comp != NULL);

	old_comp = e_cal_component_clone (comp);
	setup_alarm (cbc, comp);

	old_comp_str = e_cal_component_get_as_string (old_comp);
	new_comp_str = e_cal_component_get_as_string (comp);

	/* check if component changed and notify if so */
	if (old_comp_str && new_comp_str && !g_str_equal (old_comp_str, new_comp_str))
		e_cal_backend_notify_component_modified (E_CAL_BACKEND (cbc), old_comp, comp);

	g_free (old_comp_str);
	g_free (new_comp_str);
	g_object_unref (old_comp);
}

static void
update_alarm_cb (gpointer key,
                 gpointer value,
                 gpointer user_data)
{
	ECalBackendContacts *cbc = user_data;
	ContactRecord   *record = value;

	g_return_if_fail (cbc != NULL);
	g_return_if_fail (record != NULL);

	if (record->comp_birthday)
		manage_comp_alarm_update (cbc, record->comp_birthday);

	if (record->comp_anniversary)
		manage_comp_alarm_update (cbc, record->comp_anniversary);
}

static gboolean
update_tracked_alarms_cb (gpointer user_data)
{
	ECalBackendContacts *cbc = user_data;

	g_return_val_if_fail (cbc != NULL, FALSE);

	g_hash_table_foreach (cbc->priv->tracked_contacts, update_alarm_cb, cbc);
	cbc->priv->update_alarms_id = 0;

	return FALSE;
}

static void
alarm_config_changed_cb (GConfClient *client,
                         guint cnxn_id,
                         GConfEntry *entry,
                         gpointer user_data)
{
	ECalBackendContacts *cbc = user_data;

	g_return_if_fail (cbc != NULL);

	setup_alarm (cbc, NULL);

	if (!cbc->priv->update_alarms_id)
		cbc->priv->update_alarms_id = g_idle_add (update_tracked_alarms_cb, cbc);
}

/* When called with NULL, then just refresh local static variables on setup change from the user. */
static void
setup_alarm (ECalBackendContacts *cbc,
             ECalComponent *comp)
{
	ECalComponentAlarm *alarm;
	ECalComponentAlarmTrigger trigger;
	ECalComponentText summary;

	g_return_if_fail (cbc != NULL);

	if (!comp || cbc->priv->alarm_interval == -1) {
		gchar *str;

		#define BA_CONF_DIR		"/apps/evolution/calendar/other"
		#define BA_CONF_ENABLED		BA_CONF_DIR "/use_ba_reminder"
		#define BA_CONF_INTERVAL	BA_CONF_DIR "/ba_reminder_interval"
		#define BA_CONF_UNITS		BA_CONF_DIR "/ba_reminder_units"

		if (cbc->priv->alarm_interval == -1) {
			/* initial setup, hook callback for changes too */
			gconf_client_add_dir (cbc->priv->conf_client, BA_CONF_DIR, GCONF_CLIENT_PRELOAD_NONE, NULL);
			cbc->priv->notifyid1 = gconf_client_notify_add (cbc->priv->conf_client, BA_CONF_ENABLED,  alarm_config_changed_cb, cbc, NULL, NULL);
			cbc->priv->notifyid2 = gconf_client_notify_add (cbc->priv->conf_client, BA_CONF_INTERVAL, alarm_config_changed_cb, cbc, NULL, NULL);
			cbc->priv->notifyid3 = gconf_client_notify_add (cbc->priv->conf_client, BA_CONF_UNITS,    alarm_config_changed_cb, cbc, NULL, NULL);
		}

		cbc->priv->alarm_enabled = gconf_client_get_bool (cbc->priv->conf_client, BA_CONF_ENABLED, NULL);
		cbc->priv->alarm_interval = gconf_client_get_int (cbc->priv->conf_client, BA_CONF_INTERVAL, NULL);

		str = gconf_client_get_string (cbc->priv->conf_client, BA_CONF_UNITS, NULL);
		if (str && !strcmp (str, "days"))
			cbc->priv->alarm_units = CAL_DAYS;
		else if (str && !strcmp (str, "hours"))
			cbc->priv->alarm_units = CAL_HOURS;
		else
			cbc->priv->alarm_units = CAL_MINUTES;

		g_free (str);

		if (cbc->priv->alarm_interval <= 0)
			cbc->priv->alarm_interval = 1;

		if (!comp)
			return;

		#undef BA_CONF_DIR
		#undef BA_CONF_ENABLED
		#undef BA_CONF_INTERVAL
		#undef BA_CONF_UNITS
	}

	/* ensure no alarms left */
	e_cal_component_remove_all_alarms (comp);

	/* do not want alarms, return */
	if (!cbc->priv->alarm_enabled)
		return;

	alarm = e_cal_component_alarm_new ();
	e_cal_component_get_summary (comp, &summary);
	e_cal_component_alarm_set_description (alarm, &summary);
	e_cal_component_alarm_set_action (alarm, E_CAL_COMPONENT_ALARM_DISPLAY);

	trigger.type = E_CAL_COMPONENT_ALARM_TRIGGER_RELATIVE_START;

	memset (&trigger.u.rel_duration, 0, sizeof (trigger.u.rel_duration));

	trigger.u.rel_duration.is_neg = TRUE;

	switch (cbc->priv->alarm_units) {
	case CAL_MINUTES:
		trigger.u.rel_duration.minutes = cbc->priv->alarm_interval;
		break;

	case CAL_HOURS:
		trigger.u.rel_duration.hours = cbc->priv->alarm_interval;
		break;

	case CAL_DAYS:
		trigger.u.rel_duration.days = cbc->priv->alarm_interval;
		break;

	default:
		g_warning ("%s: wrong units %d\n", G_STRFUNC, cbc->priv->alarm_units);
		e_cal_component_alarm_free (alarm);
		return;
	}

	e_cal_component_alarm_set_trigger (alarm, trigger);
	e_cal_component_add_alarm (comp, alarm);
	e_cal_component_alarm_free (alarm);
}

/* Contact -> Event creator */
static ECalComponent *
create_component (ECalBackendContacts *cbc,
                  const gchar *uid,
                  EContactDate *cdate,
                  const gchar *summary)
{
	ECalComponent             *cal_comp;
	ECalComponentText          comp_summary;
	icalcomponent             *ical_comp;
	icalproperty		  *prop;
	struct icaltimetype        itt;
	ECalComponentDateTime      dt;
	struct icalrecurrencetype  r;
	gchar			  *since_year;
	GSList recur_list;

	g_return_val_if_fail (E_IS_CAL_BACKEND_CONTACTS (cbc), NULL);

	if (!cdate)
		return NULL;

	ical_comp = icalcomponent_new (ICAL_VEVENT_COMPONENT);

	since_year = g_strdup_printf ("%04d", cdate->year);
	prop = icalproperty_new_x (since_year);
	icalproperty_set_x_name (prop, "X-EVOLUTION-SINCE-YEAR");
	icalcomponent_add_property (ical_comp, prop);
	g_free (since_year);

	/* Create the event object */
	cal_comp = e_cal_component_new ();
	e_cal_component_set_icalcomponent (cal_comp, ical_comp);

	/* Set uid */
	d(g_message ("Creating UID: %s", uid));
	e_cal_component_set_uid (cal_comp, uid);

	/* Set all-day event's date from contact data */
	itt = cdate_to_icaltime (cdate);
	dt.value = &itt;
	dt.tzid = NULL;
	e_cal_component_set_dtstart (cal_comp, &dt);

	itt = cdate_to_icaltime (cdate);
	icaltime_adjust (&itt, 1, 0, 0, 0);
	dt.value = &itt;
	dt.tzid = NULL;
	/* We have to add 1 day to DTEND, as it is not inclusive. */
	e_cal_component_set_dtend (cal_comp, &dt);

	/* Create yearly recurrence */
	icalrecurrencetype_clear (&r);
	r.freq = ICAL_YEARLY_RECURRENCE;
	r.interval = 1;
	recur_list.data = &r;
	recur_list.next = NULL;
	e_cal_component_set_rrule_list (cal_comp, &recur_list);

	/* Create summary */
	comp_summary.value = summary;
	comp_summary.altrep = NULL;
	e_cal_component_set_summary (cal_comp, &comp_summary);

	/* Set category and visibility */
	if (g_str_has_suffix (uid, ANNIVERSARY_UID_EXT))
		e_cal_component_set_categories (cal_comp, _("Anniversary"));
	else if (g_str_has_suffix (uid, BIRTHDAY_UID_EXT))
		e_cal_component_set_categories (cal_comp, _("Birthday"));

	e_cal_component_set_classification (cal_comp, E_CAL_COMPONENT_CLASS_PRIVATE);

	/* Birthdays/anniversaries are shown as free time */
	e_cal_component_set_transparency (cal_comp, E_CAL_COMPONENT_TRANSP_TRANSPARENT);

	/* setup alarms if required */
	setup_alarm (cbc, cal_comp);

	/* Don't forget to call commit()! */
	e_cal_component_commit_sequence (cal_comp);

	return cal_comp;
}

static ECalComponent *
create_birthday (ECalBackendContacts *cbc,
                 EContact *contact)
{
	EContactDate  *cdate;
	ECalComponent *cal_comp;
	gchar          *summary;
	const gchar    *name;
	gchar *uid;

	cdate = e_contact_get (contact, E_CONTACT_BIRTH_DATE);
	name = e_contact_get_const (contact, E_CONTACT_FILE_AS);
	if (!name || !*name)
		name = e_contact_get_const (contact, E_CONTACT_FULL_NAME);
	if (!name || !*name)
		name = e_contact_get_const (contact, E_CONTACT_NICKNAME);
	if (!name)
		name = "";

	uid = g_strdup_printf ("%s%s", (gchar *) e_contact_get_const (contact, E_CONTACT_UID), BIRTHDAY_UID_EXT);
	summary = g_strdup_printf (_("Birthday: %s"), name);

	cal_comp = create_component (cbc, uid, cdate, summary);

	e_contact_date_free (cdate);
	g_free (uid);
	g_free (summary);

	return cal_comp;
}

static ECalComponent *
create_anniversary (ECalBackendContacts *cbc,
                    EContact *contact)
{
	EContactDate  *cdate;
	ECalComponent *cal_comp;
	gchar          *summary;
	const gchar    *name;
	gchar *uid;

	cdate = e_contact_get (contact, E_CONTACT_ANNIVERSARY);
	name = e_contact_get_const (contact, E_CONTACT_FILE_AS);
	if (!name || !*name)
		name = e_contact_get_const (contact, E_CONTACT_FULL_NAME);
	if (!name || !*name)
		name = e_contact_get_const (contact, E_CONTACT_NICKNAME);
	if (!name)
		name = "";

	uid = g_strdup_printf ("%s%s", (gchar *) e_contact_get_const (contact, E_CONTACT_UID), ANNIVERSARY_UID_EXT);
	summary = g_strdup_printf (_("Anniversary: %s"), name);

	cal_comp = create_component (cbc, uid, cdate, summary);

	e_contact_date_free (cdate);
	g_free (uid);
	g_free (summary);

	return cal_comp;
}

/************************************************************************************/
/* Calendar backend method implementations */

/* First the empty stubs */

static gboolean
e_cal_backend_contacts_get_backend_property (ECalBackendSync *backend,
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
		*prop_value = NULL;
	} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS) ||
		   g_str_equal (prop_name, CAL_BACKEND_PROPERTY_ALARM_EMAIL_ADDRESS)) {
		/* A contact backend has no particular email address associated
		 * with it (although that would be a useful feature some day).
		 */
		*prop_value = NULL;
	} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_DEFAULT_OBJECT)) {
		g_propagate_error (perror, EDC_ERROR (UnsupportedMethod));
	} else {
		processed = FALSE;
	}

	return processed;
}

static void
e_cal_backend_contacts_remove (ECalBackendSync *backend,
                               EDataCal *cal,
                               GCancellable *cancellable,
                               GError **perror)
{
	/* WRITE ME */
	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

static void
e_cal_backend_contacts_get_object (ECalBackendSync *backend,
                                   EDataCal *cal,
                                   GCancellable *cancellable,
                                   const gchar *uid,
                                   const gchar *rid,
                                   gchar **object,
                                   GError **perror)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (backend);
	ECalBackendContactsPrivate *priv = cbc->priv;
	ContactRecord *record;
	gchar *real_uid;

	if (!uid) {
		g_propagate_error (perror, EDC_ERROR (ObjectNotFound));
		return;
	} else if (g_str_has_suffix (uid, ANNIVERSARY_UID_EXT))
		real_uid = g_strndup (uid, strlen (uid) - strlen (ANNIVERSARY_UID_EXT));
	else if (g_str_has_suffix (uid, BIRTHDAY_UID_EXT))
		real_uid = g_strndup (uid, strlen (uid) - strlen (BIRTHDAY_UID_EXT));
	else {
		g_propagate_error (perror, EDC_ERROR (ObjectNotFound));
		return;
	}

	record = g_hash_table_lookup (priv->tracked_contacts, real_uid);
	g_free (real_uid);

	if (!record) {
		g_propagate_error (perror, EDC_ERROR (ObjectNotFound));
		return;
	}

	if (record->comp_birthday && g_str_has_suffix (uid, BIRTHDAY_UID_EXT)) {
		*object = e_cal_component_get_as_string (record->comp_birthday);

		d(g_message ("Return birthday: %s", *object));
		return;
	}

	if (record->comp_anniversary && g_str_has_suffix (uid, ANNIVERSARY_UID_EXT)) {
		*object = e_cal_component_get_as_string (record->comp_anniversary);

		d(g_message ("Return anniversary: %s", *object));
		return;
	}

	d(g_message ("Returning nothing for uid: %s", uid));

	g_propagate_error (perror, EDC_ERROR (ObjectNotFound));
}

static void
e_cal_backend_contacts_get_free_busy (ECalBackendSync *backend,
                                      EDataCal *cal,
                                      GCancellable *cancellable,
                                      const GSList *users,
                                      time_t start,
                                      time_t end,
                                      GSList **freebusy,
                                      GError **perror)
{
	/* Birthdays/anniversaries don't count as busy time */

	icalcomponent *vfb = icalcomponent_new_vfreebusy ();
	icaltimezone *utc_zone = icaltimezone_get_utc_timezone ();
	gchar *calobj;

#if 0
	icalproperty *prop;
	icalparameter *param;

	prop = icalproperty_new_organizer (address);
	if (prop != NULL && cn != NULL) {
		param = icalparameter_new_cn (cn);
		icalproperty_add_parameter (prop, param);
	}
	if (prop != NULL)
		icalcomponent_add_property (vfb, prop);
#endif

	icalcomponent_set_dtstart (vfb, icaltime_from_timet_with_zone (start, FALSE, utc_zone));
	icalcomponent_set_dtend (vfb, icaltime_from_timet_with_zone (end, FALSE, utc_zone));

	calobj = icalcomponent_as_ical_string_r (vfb);
	*freebusy = g_slist_append (NULL, calobj);
	icalcomponent_free (vfb);

	/* WRITE ME */
	/* Success */
}

static void
e_cal_backend_contacts_receive_objects (ECalBackendSync *backend,
                                        EDataCal *cal,
                                        GCancellable *cancellable,
                                        const gchar *calobj,
                                        GError **perror)
{
	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

static void
e_cal_backend_contacts_send_objects (ECalBackendSync *backend,
                                     EDataCal *cal,
                                     GCancellable *cancellable,
                                     const gchar *calobj,
                                     GSList **users,
                                     gchar **modified_calobj,
                                     GError **perror)
{
	*users = NULL;
	*modified_calobj = NULL;
	/* TODO: Investigate this */
	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

/* Then the real implementations */

static void
e_cal_backend_contacts_notify_online_cb (ECalBackend *backend,
                                         gboolean is_online)
{
	gboolean online;

	online = e_backend_get_online (E_BACKEND (backend));
	e_cal_backend_notify_online (backend, online);
	e_cal_backend_notify_readonly (backend, TRUE);
}

static gpointer
init_sources_cb (ECalBackendContacts *cbc)
{
	ECalBackendContactsPrivate *priv;
	GSList *i;

	g_return_val_if_fail (cbc != NULL, NULL);

	priv = cbc->priv;

	if (!priv->addressbook_sources)
		return NULL;

	/* Create address books for existing sources */
	for (i = e_source_list_peek_groups (priv->addressbook_sources); i; i = i->next) {
		ESourceGroup *source_group = E_SOURCE_GROUP (i->data);

		source_group_added_cb (priv->addressbook_sources, source_group, cbc);
	}

        /* Listen for source list changes */
        g_signal_connect (priv->addressbook_sources, "changed", G_CALLBACK (source_list_changed_cb), cbc);
        g_signal_connect (priv->addressbook_sources, "group_added", G_CALLBACK (source_group_added_cb), cbc);
        g_signal_connect (priv->addressbook_sources, "group_removed", G_CALLBACK (source_group_removed_cb), cbc);

	e_flag_set (priv->init_done_flag);

	return NULL;
}

static void
e_cal_backend_contacts_open (ECalBackendSync *backend,
                             EDataCal *cal,
                             GCancellable *cancellable,
                             gboolean only_if_exists,
                             GError **perror)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (backend);
	ECalBackendContactsPrivate *priv = cbc->priv;
	GError *error = NULL;

	if (priv->loaded)
		return;

	/* initialize addressbook sources in new thread to make this function quick as much as possible */
	if (!g_thread_create ((GThreadFunc) init_sources_cb, cbc, FALSE, &error)) {
		e_flag_set (priv->init_done_flag);
		g_warning ("%s: Cannot create thread to initialize sources! (%s)", G_STRFUNC, error ? error->message : "Unknown error");
		if (error)
			g_error_free (error);

		g_propagate_error (perror, EDC_ERROR (OtherError));
		e_cal_backend_notify_opened (E_CAL_BACKEND (backend), EDC_ERROR (OtherError));
		return;
	}

	priv->loaded = TRUE;
	e_cal_backend_notify_readonly (E_CAL_BACKEND (backend), TRUE);
	e_cal_backend_notify_online (E_CAL_BACKEND (backend), TRUE);
	e_cal_backend_notify_opened (E_CAL_BACKEND (backend), NULL);
}

/* Add_timezone handler for the file backend */
static void
e_cal_backend_contacts_add_timezone (ECalBackendSync *backend,
                                     EDataCal *cal,
                                     GCancellable *cancellable,
                                     const gchar *tzobj,
                                     GError **error)
{
	ECalBackendContacts *cbcontacts;
	ECalBackendContactsPrivate *priv;
	icalcomponent *tz_comp;
	icaltimezone *zone;
	const gchar *tzid;

	cbcontacts = (ECalBackendContacts *) backend;

	e_return_data_cal_error_if_fail (E_IS_CAL_BACKEND_CONTACTS (cbcontacts), InvalidArg);
	e_return_data_cal_error_if_fail (tzobj != NULL, InvalidArg);

	priv = cbcontacts->priv;

	tz_comp = icalparser_parse_string (tzobj);
	if (!tz_comp) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	if (icalcomponent_isa (tz_comp) != ICAL_VTIMEZONE_COMPONENT) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	zone = icaltimezone_new ();
	icaltimezone_set_component (zone, tz_comp);
	tzid = icaltimezone_get_tzid (zone);

	if (g_hash_table_lookup (priv->zones, tzid)) {
		icaltimezone_free (zone, TRUE);
	} else {
		g_hash_table_insert (priv->zones, g_strdup (tzid), zone);
	}
}

static void
e_cal_backend_contacts_get_object_list (ECalBackendSync *backend,
                                        EDataCal *cal,
                                        GCancellable *cancellable,
                                        const gchar *sexp_string,
                                        GSList **objects,
                                        GError **perror)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (backend);
	ECalBackendContactsPrivate *priv = cbc->priv;
	ECalBackendSExp *sexp = e_cal_backend_sexp_new (sexp_string);
	ContactRecordCB *cb_data;

	if (!sexp) {
		g_propagate_error (perror, EDC_ERROR (InvalidQuery));
		return;
	}

	cb_data = contact_record_cb_new (cbc, sexp, TRUE);
	g_hash_table_foreach (priv->tracked_contacts, contact_record_cb, cb_data);
	*objects = cb_data->result;

	/* Don't call cb_data_free as that would destroy the results
	 * in *objects */
	g_free (cb_data);
}

static void
e_cal_backend_contacts_start_view (ECalBackend *backend,
                                   EDataCalView *query)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (backend);
	ECalBackendContactsPrivate *priv = cbc->priv;
	ECalBackendSExp *sexp;
	ContactRecordCB *cb_data;

	sexp = e_data_cal_view_get_object_sexp (query);
	if (!sexp) {
		GError *error = EDC_ERROR (InvalidQuery);
		e_data_cal_view_notify_complete (query, error);
		g_error_free (error);
		return;
	}

	cb_data = contact_record_cb_new (cbc, sexp, FALSE);

	g_hash_table_foreach (priv->tracked_contacts, contact_record_cb, cb_data);
	e_data_cal_view_notify_components_added (query, cb_data->result);

	contact_record_cb_free (cb_data);

	e_data_cal_view_notify_complete (query, NULL /* Success */);
}

static icaltimezone *
e_cal_backend_contacts_internal_get_timezone (ECalBackend *backend,
                                              const gchar *tzid)
{
	ECalBackendContacts *cbc = E_CAL_BACKEND_CONTACTS (backend);

	return g_hash_table_lookup (cbc->priv->zones, tzid ? tzid : "");
}

/***********************************************************************************
 */

static void
free_zone (gpointer data)
{
	icaltimezone_free (data, TRUE);
}

/* Finalize handler for the contacts backend */
static void
e_cal_backend_contacts_finalize (GObject *object)
{
	ECalBackendContactsPrivate *priv;

	priv = E_CAL_BACKEND_CONTACTS_GET_PRIVATE (object);

	if (priv->init_done_flag) {
		e_flag_wait (priv->init_done_flag);
		e_flag_free (priv->init_done_flag);
		priv->init_done_flag = NULL;
	}

	if (priv->update_alarms_id) {
		g_source_remove (priv->update_alarms_id);
		priv->update_alarms_id = 0;
	}

	if (priv->addressbook_sources)
		g_object_unref (priv->addressbook_sources);
	g_hash_table_destroy (priv->addressbooks);
	g_hash_table_destroy (priv->credentials);
	g_hash_table_destroy (priv->tracked_contacts);
	g_hash_table_destroy (priv->zones);
	if (priv->notifyid1)
		gconf_client_notify_remove (priv->conf_client, priv->notifyid1);
	if (priv->notifyid2)
		gconf_client_notify_remove (priv->conf_client, priv->notifyid2);
	if (priv->notifyid3)
		gconf_client_notify_remove (priv->conf_client, priv->notifyid3);

	g_object_unref (priv->conf_client);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_cal_backend_contacts_parent_class)->finalize (object);
}

/* Object initialization function for the contacts backend */
static void
e_cal_backend_contacts_init (ECalBackendContacts *cbc)
{
	cbc->priv = E_CAL_BACKEND_CONTACTS_GET_PRIVATE (cbc);

	if (!e_book_client_get_sources (&cbc->priv->addressbook_sources, NULL))
		cbc->priv->addressbook_sources = NULL;

	cbc->priv->addressbooks = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) book_record_free);

	cbc->priv->credentials = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) e_credentials_free);

	cbc->priv->tracked_contacts = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) contact_record_free);

	cbc->priv->zones = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) free_zone);

	cbc->priv->init_done_flag = e_flag_new ();
	cbc->priv->conf_client = gconf_client_get_default ();
	cbc->priv->notifyid1 = 0;
	cbc->priv->notifyid2 = 0;
	cbc->priv->notifyid3 = 0;
	cbc->priv->update_alarms_id = 0;
	cbc->priv->alarm_enabled = FALSE;
	cbc->priv->alarm_interval = -1;
	cbc->priv->alarm_units = CAL_MINUTES;

	e_cal_backend_sync_set_lock (E_CAL_BACKEND_SYNC (cbc), TRUE);

	g_signal_connect (
		cbc, "notify::online",
		G_CALLBACK (e_cal_backend_contacts_notify_online_cb), NULL);
}

static void
e_cal_backend_contacts_create_object (ECalBackendSync *backend,
                                      EDataCal *cal,
                                      GCancellable *cancellable,
                                      const gchar *calobj,
                                      gchar **uid,
                                      ECalComponent **new_component,
                                      GError **perror)
{
	g_propagate_error (perror, EDC_ERROR (PermissionDenied));
}

/* Class initialization function for the contacts backend */
static void
e_cal_backend_contacts_class_init (ECalBackendContactsClass *class)
{
	GObjectClass *object_class;
	ECalBackendClass *backend_class;
	ECalBackendSyncClass *sync_class;

	g_type_class_add_private (class, sizeof (ECalBackendContactsPrivate));

	object_class = (GObjectClass *) class;
	backend_class = (ECalBackendClass *) class;
	sync_class = (ECalBackendSyncClass *) class;

	object_class->finalize = e_cal_backend_contacts_finalize;

	sync_class->get_backend_property_sync	= e_cal_backend_contacts_get_backend_property;
	sync_class->open_sync			= e_cal_backend_contacts_open;
	sync_class->remove_sync			= e_cal_backend_contacts_remove;
	sync_class->create_object_sync		= e_cal_backend_contacts_create_object;
	sync_class->receive_objects_sync	= e_cal_backend_contacts_receive_objects;
	sync_class->send_objects_sync		= e_cal_backend_contacts_send_objects;
	sync_class->get_object_sync		= e_cal_backend_contacts_get_object;
	sync_class->get_object_list_sync	= e_cal_backend_contacts_get_object_list;
	sync_class->add_timezone_sync		= e_cal_backend_contacts_add_timezone;
	sync_class->get_free_busy_sync		= e_cal_backend_contacts_get_free_busy;
	sync_class->authenticate_user_sync	= e_cal_backend_contacts_authenticate_user;

	backend_class->start_view		= e_cal_backend_contacts_start_view;
	backend_class->internal_get_timezone	= e_cal_backend_contacts_internal_get_timezone;
}
