/* Evolution calendar - weather backend factory
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * Authors: David Trowbridge <trowbrds@cs.colorado.edu>
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

#include <config.h>

#include <libedata-cal/e-cal-backend-factory.h>
#include "e-cal-backend-weather.h"

#define FACTORY_NAME "weather"

typedef ECalBackendFactory ECalBackendWeatherEventsFactory;
typedef ECalBackendFactoryClass ECalBackendWeatherEventsFactoryClass;

/* Module Entry Points */
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

/* Forward Declarations */
GType e_cal_backend_weather_events_factory_get_type (void);

G_DEFINE_DYNAMIC_TYPE (
	ECalBackendWeatherEventsFactory,
	e_cal_backend_weather_events_factory,
	E_TYPE_CAL_BACKEND_FACTORY)

static void
e_cal_backend_weather_events_factory_class_init (ECalBackendFactoryClass *class)
{
	class->factory_name = FACTORY_NAME;
	class->component_kind = ICAL_VEVENT_COMPONENT;
	class->backend_type = E_TYPE_CAL_BACKEND_WEATHER;
}

static void
e_cal_backend_weather_events_factory_class_finalize (ECalBackendFactoryClass *class)
{
}

static void
e_cal_backend_weather_events_factory_init (ECalBackendFactory *factory)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	e_cal_backend_weather_events_factory_register_type (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
}

