/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

#include <stdlib.h>
#include <string.h>
#include <libecal/e-cal-client.h>
#include <libical/ical.h>

#include "client-test-utils.h"

static gboolean
test_sync (ECalClient *cal_client)
{
	GError *error = NULL;

	g_print ("Refresh supported: %s\n", e_client_check_refresh_supported (E_CLIENT (cal_client)) ? "yes" : "no");

	if (!e_client_refresh_sync (E_CLIENT (cal_client), NULL, &error)) {
		report_error ("refresh sync", &error);
		return FALSE;
	}

	return TRUE;
}

/* asynchronous callback with a main-loop running */
static void
async_refresh_result_ready (GObject *source_object,
                            GAsyncResult *result,
                            gpointer user_data)
{
	ECalClient *cal_client;
	GError *error = NULL;

	cal_client = E_CAL_CLIENT (source_object);

	if (!e_client_refresh_finish (E_CLIENT (cal_client), result, &error)) {
		report_error ("refresh finish", &error);
		stop_main_loop (1);
		return;
	}

	stop_main_loop (0);
}

/* synchronously in idle with main-loop running */
static gboolean
test_sync_in_idle (gpointer user_data)
{
	ECalClient *cal_client = user_data;

	g_return_val_if_fail (cal_client != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL_CLIENT (cal_client), FALSE);

	if (!test_sync (cal_client)) {
		stop_main_loop (1);
		return FALSE;
	}

	g_print ("Refresh supported: %s\n", e_client_check_refresh_supported (E_CLIENT (cal_client)) ? "yes" : "no");

	e_client_refresh (E_CLIENT (cal_client), NULL, async_refresh_result_ready, NULL);

	return FALSE;
}

/* synchronously in a dedicated thread with main-loop running */
static gpointer
test_sync_in_thread (gpointer user_data)
{
	if (!test_sync (user_data)) {
		stop_main_loop (1);
		return NULL;
	}

	g_idle_add (test_sync_in_idle, user_data);

	return NULL;
}

gint
main (gint argc,
      gchar **argv)
{
	ECalClient *cal_client;
	GError *error = NULL;
	icalcomponent *icalcomp;
	struct icaltimetype now;
	gchar *uid = NULL;

	main_initialize ();

	cal_client = new_temp_client (E_CAL_CLIENT_SOURCE_TYPE_EVENTS, NULL);
	g_return_val_if_fail (cal_client != NULL, FALSE);

	if (!e_client_open_sync (E_CLIENT (cal_client), FALSE, NULL, &error)) {
		report_error ("client open sync", &error);
		g_object_unref (cal_client);
		return 1;
	}

	now = icaltime_current_time_with_zone (icaltimezone_get_utc_timezone ());
	icalcomp = icalcomponent_new (ICAL_VEVENT_COMPONENT);
	icalcomponent_set_summary (icalcomp, "Test event summary");
	icalcomponent_set_dtstart (icalcomp, now);
	icalcomponent_set_dtend   (icalcomp, icaltime_from_timet (icaltime_as_timet (now) + 60 * 60 * 60, 0));

	if (!e_cal_client_create_object_sync (cal_client, icalcomp, &uid, NULL, &error)) {
		report_error ("create object sync", &error);
		icalcomponent_free (icalcomp);
		g_object_unref (cal_client);
		return 1;
	}

	icalcomponent_free (icalcomp);
	g_free (uid);

	/* synchronously without main-loop */
	if (!test_sync (cal_client)) {
		g_object_unref (cal_client);
		return 1;
	}

	start_in_thread_with_main_loop (test_sync_in_thread, cal_client);

	if (!e_client_remove_sync (E_CLIENT (cal_client), NULL, &error)) {
		report_error ("client remove sync", &error);
		g_object_unref (cal_client);
		return 1;
	}

	g_object_unref (cal_client);

	if (get_main_loop_stop_result () == 0)
		g_print ("Test finished successfully.\n");

	return get_main_loop_stop_result ();
}
