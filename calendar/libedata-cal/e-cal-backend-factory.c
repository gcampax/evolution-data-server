/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *
 * Author:
 *   Chris Toshok (toshok@ximian.com)
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 */

#include <config.h>
#include <string.h>

#include "e-cal-backend.h"
#include "e-cal-backend-factory.h"

G_DEFINE_ABSTRACT_TYPE (
	ECalBackendFactory,
	e_cal_backend_factory,
	E_TYPE_BACKEND_FACTORY)

static const gchar *
cal_backend_factory_get_hash_key (EBackendFactory *factory)
{
	ECalBackendFactoryClass *class;
	const gchar *component_name;
	gchar *hash_key;
	gsize length;

	class = E_CAL_BACKEND_FACTORY_GET_CLASS (factory);
	g_return_val_if_fail (class->factory_name != NULL, NULL);

	switch (class->component_kind) {
		case ICAL_VEVENT_COMPONENT:
			component_name = "VEVENT";
			break;
		case ICAL_VTODO_COMPONENT:
			component_name = "VTODO";
			break;
		case ICAL_VJOURNAL_COMPONENT:
			component_name = "VJOURNAL";
			break;
		default:
			g_return_val_if_reached (NULL);
	}

	/* Hash Key: FACTORY_NAME ':' COMPONENT_NAME */
	length = strlen (class->factory_name) + strlen (component_name) + 2;
	hash_key = g_alloca (length);
	g_snprintf (
		hash_key, length, "%s:%s",
		class->factory_name, component_name);

	return g_intern_string (hash_key);
}

static EBackend *
cal_backend_factory_new_backend (EBackendFactory *factory,
                                 ESource *source)
{
	ECalBackendFactoryClass *class;

	class = E_CAL_BACKEND_FACTORY_GET_CLASS (factory);
	g_return_val_if_fail (g_type_is_a (
		class->backend_type, E_TYPE_CAL_BACKEND), NULL);

	return g_object_new (
		class->backend_type,
		"kind", class->component_kind,
		"source", source, NULL);
}

static void
e_cal_backend_factory_class_init (ECalBackendFactoryClass *class)
{
	EBackendFactoryClass *factory_class;

	factory_class = E_BACKEND_FACTORY_CLASS (class);
	factory_class->get_hash_key = cal_backend_factory_get_hash_key;
	factory_class->new_backend = cal_backend_factory_new_backend;
}

static void
e_cal_backend_factory_init (ECalBackendFactory *factory)
{
}
