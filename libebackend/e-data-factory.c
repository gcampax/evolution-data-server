/*
 * e-data-factory.c
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
 */

/**
 * SECTION: e-data-factory
 * @short_description: an abstract base class for a D-Bus server
 * @include: libebackend/e-data-factory
 **/

#include "e-data-factory.h"

#include <config.h>

#if GLIB_CHECK_VERSION(2,31,0)
#define USE_NETWORK_MONITOR
#endif

#include <libebackend/e-extensible.h>
#include <libebackend/e-backend-factory.h>

#define E_DATA_FACTORY_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_DATA_FACTORY, EDataFactoryPrivate))

struct _EDataFactoryPrivate {
	/* The mutex guards the 'backends' hash table.  The
	 * 'backend_factories' hash table doesn't really need
	 * guarding since it gets populated during construction
	 * and is read-only thereafter. */
	GMutex *mutex;

	/* ESource UID -> EBackend */
	GHashTable *backends;

	/* Hash Key -> EBackendFactory */
	GHashTable *backend_factories;

#ifndef USE_NETWORK_MONITOR
	GSettings *settings;
#endif

	gboolean online;
};

enum {
	PROP_0,
	PROP_ONLINE
};

/* Forward Declarations */
static void	e_data_factory_initable_init	(GInitableIface *interface);

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (
	EDataFactory, e_data_factory, E_TYPE_DBUS_SERVER,
	G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, e_data_factory_initable_init)
	G_IMPLEMENT_INTERFACE (E_TYPE_EXTENSIBLE, NULL))

#ifdef USE_NETWORK_MONITOR

static void
network_changed (GNetworkMonitor *monitor,
                 gboolean available,
                 EDataFactory *factory)
{
	e_data_factory_set_online (factory, available);
}

static void
data_factory_init_online_monitoring (EDataFactory *factory)
{
	GNetworkMonitor *monitor;
	gboolean network_available;

	monitor = g_network_monitor_get_default ();
	g_signal_connect (
		monitor, "network-changed",
		G_CALLBACK (network_changed), factory);

	network_available = g_network_monitor_get_network_available (monitor);
	e_data_factory_set_online (factory, network_available);
}

#else

static void
data_factory_online_changed (GSettings *settings,
                             const gchar *key,
                             EDataFactory *factory)
{
	gboolean start_offline;

	start_offline = g_settings_get_boolean (
		factory->priv->settings, "start-offline");

	e_data_factory_set_online (factory, !start_offline);
}

static void
data_factory_init_online_monitoring (EDataFactory *factory)
{
	const gchar *schema;
	gboolean start_offline;

	/* XXX For the record, we're doing this completely wrong.
	 *     EDataFactory should monitor network availability itself
	 *     instead of relying on one particular client application
	 *     to tell us when we're offline.  But I'll deal with this
	 *     at some later date. */

	schema = "org.gnome.evolution.eds-shell";
	factory->priv->settings = g_settings_new (schema);

	g_signal_connect (
		factory->priv->settings, "changed::start-offline",
		G_CALLBACK (data_factory_online_changed), factory);

	start_offline = g_settings_get_boolean (
		factory->priv->settings, "start-offline");

	e_data_factory_set_online (factory, !start_offline);
}

#endif

static void
data_factory_last_client_gone_cb (EBackend *backend,
                                  EDataFactory *factory)
{
	ESource *source;
	const gchar *uid;

	source = e_backend_get_source (backend);
	uid = e_source_peek_uid (source);
	g_return_if_fail (uid != NULL);

	g_mutex_lock (factory->priv->mutex);
	g_hash_table_remove (factory->priv->backends, uid);
	g_mutex_unlock (factory->priv->mutex);
}

static void
data_factory_set_property (GObject *object,
                           guint property_id,
                           const GValue *value,
                           GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ONLINE:
			e_data_factory_set_online (
				E_DATA_FACTORY (object),
				g_value_get_boolean (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
data_factory_get_property (GObject *object,
                           guint property_id,
                           GValue *value,
                           GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ONLINE:
			g_value_set_boolean (
				value, e_data_factory_get_online (
				E_DATA_FACTORY (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
data_factory_dispose (GObject *object)
{
	EDataFactoryPrivate *priv;

	priv = E_DATA_FACTORY_GET_PRIVATE (object);

	g_hash_table_remove_all (priv->backends);
	g_hash_table_remove_all (priv->backend_factories);

#ifdef USE_NETWORK_MONITOR
	g_signal_handlers_disconnect_by_func (
		g_network_monitor_get_default (),
		G_CALLBACK (network_changed), object);
#else
	if (priv->settings != NULL) {
		g_object_unref (priv->settings);
		priv->settings = NULL;
	}
#endif

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_data_factory_parent_class)->dispose (object);
}

static void
data_factory_finalize (GObject *object)
{
	EDataFactoryPrivate *priv;

	priv = E_DATA_FACTORY_GET_PRIVATE (object);

	g_mutex_free (priv->mutex);

	g_hash_table_destroy (priv->backends);
	g_hash_table_destroy (priv->backend_factories);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_data_factory_parent_class)->finalize (object);
}

static gboolean
data_factory_initable_init (GInitable *initable,
                            GCancellable *cancellable,
                            GError **error)
{
	EDataFactoryPrivate *priv;
	GList *list, *link;

	priv = E_DATA_FACTORY_GET_PRIVATE (initable);

	/* Load all module libraries containing extensions. */

	e_dbus_server_load_modules (E_DBUS_SERVER (initable));

	/* Collect all backend factories into a hash table. */

	list = e_extensible_list_extensions (
		E_EXTENSIBLE (initable), E_TYPE_BACKEND_FACTORY);

	for (link = list; link != NULL; link = g_list_next (link)) {
		EBackendFactory *backend_factory;
		const gchar *hash_key;

		backend_factory = E_BACKEND_FACTORY (link->data);
		hash_key = e_backend_factory_get_hash_key (backend_factory);

		if (hash_key != NULL) {
			g_hash_table_insert (
				priv->backend_factories,
				g_strdup (hash_key),
				g_object_ref (backend_factory));
			g_print (
				"Registering %s ('%s')\n",
				G_OBJECT_TYPE_NAME (backend_factory),
				hash_key);
		}
	}

	g_list_free (list);

	return TRUE;
}

static void
e_data_factory_class_init (EDataFactoryClass *class)
{
	GObjectClass *object_class;

	g_type_class_add_private (class, sizeof (EDataFactoryPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = data_factory_set_property;
	object_class->get_property = data_factory_get_property;
	object_class->dispose = data_factory_dispose;
	object_class->finalize = data_factory_finalize;

	g_object_class_install_property (
		object_class,
		PROP_ONLINE,
		g_param_spec_boolean (
			"online",
			"Online",
			"Whether the server is online",
			TRUE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			G_PARAM_STATIC_STRINGS));
}

static void
e_data_factory_initable_init (GInitableIface *interface)
{
	interface->init = data_factory_initable_init;
}

static void
e_data_factory_init (EDataFactory *factory)
{
	factory->priv = E_DATA_FACTORY_GET_PRIVATE (factory);

	factory->priv->mutex = g_mutex_new ();

	factory->priv->backends = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) g_object_unref);

	factory->priv->backend_factories = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) g_object_unref);

	data_factory_init_online_monitoring (factory);
}

EBackend *
e_data_factory_get_backend (EDataFactory *factory,
                            const gchar *hash_key,
                            ESource *source)
{
	EBackendFactory *backend_factory;
	EBackend *backend;
	const gchar *uid;

	g_return_val_if_fail (E_IS_DATA_FACTORY (factory), NULL);
	g_return_val_if_fail (hash_key != NULL, NULL);
	g_return_val_if_fail (E_IS_SOURCE (source), NULL);

	uid = e_source_peek_uid (source);
	g_return_val_if_fail (uid != NULL, NULL);

	g_mutex_lock (factory->priv->mutex);

	/* Check if we already have a backend for the given source. */
	backend = g_hash_table_lookup (factory->priv->backends, uid);

	if (backend != NULL)
		goto exit;

	/* Find a suitable backend factory using the hash key. */
	backend_factory = g_hash_table_lookup (
		factory->priv->backend_factories, hash_key);

	if (backend_factory == NULL)
		goto exit;

	/* Create a new backend for the given source and store it. */
	backend = e_backend_factory_new_backend (backend_factory, source);

	if (backend == NULL)
		goto exit;

	g_object_bind_property (
		factory, "online",
		backend, "online",
		G_BINDING_SYNC_CREATE);

	g_signal_connect (
		backend, "last-client-gone",
		G_CALLBACK (data_factory_last_client_gone_cb), factory);

	g_hash_table_insert (
		factory->priv->backends,
		g_strdup (uid), backend);

exit:
	g_mutex_unlock (factory->priv->mutex);

	return backend;
}

gboolean
e_data_factory_get_online (EDataFactory *factory)
{
	g_return_val_if_fail (E_IS_DATA_FACTORY (factory), FALSE);

	return factory->priv->online;
}

void
e_data_factory_set_online (EDataFactory *factory,
                           gboolean online)
{
	g_return_if_fail (E_IS_DATA_FACTORY (factory));

	/* Avoid unnecessary "notify" signals. */
	if (online == factory->priv->online)
		return;

	factory->priv->online = online;

	g_object_notify (G_OBJECT (factory), "online");

	g_print (
		"%s is now %s.\n",
		G_OBJECT_TYPE_NAME (factory),
		online ? "online" : "offline");
}
