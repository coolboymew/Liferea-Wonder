/*
 * @file kdeconnect.c  telling paired phones about new items via KDE Connect
 *
 * Copyright (C) 2026 Liferea contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "kdeconnect.h"

#include <gio/gio.h>

#include "common.h"
#include "debug.h"

#define KDECONNECT_SERVICE	"org.kde.kdeconnect"
#define KDECONNECT_PATH		"/modules/kdeconnect"
#define KDECONNECT_DAEMON	"org.kde.kdeconnect.daemon"
#define KDECONNECT_PING		"org.kde.kdeconnect.device.ping"

/* updates of several feeds usually come together: wait this long for
   more before sending one message */
#define KDECONNECT_COLLECT_SECONDS	5

static GString	*pending = NULL;	/* "Feed (n), Other feed (m)" */
static guint	sendId = 0;

static void
kdeconnect_ping_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
	g_autoptr(GError)	error = NULL;
	g_autoptr(GVariant)	result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

	if (error)
		debug (DEBUG_UPDATE, "KDE Connect: ping to %s failed: %s", (gchar *)user_data, error->message);
	else
		debug (DEBUG_UPDATE, "KDE Connect: pinged %s", (gchar *)user_data);
	g_free (user_data);
}

static void
kdeconnect_devices_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
	GDBusConnection		*bus = G_DBUS_CONNECTION (source);
	g_autofree gchar	*message = (gchar *)user_data;
	g_autoptr(GError)	error = NULL;
	g_autoptr(GVariant)	result = g_dbus_connection_call_finish (bus, res, &error);
	g_autoptr(GVariantIter)	devices = NULL;
	const gchar		*id;

	if (!result) {
		/* KDE Connect not installed or not running */
		debug (DEBUG_UPDATE, "KDE Connect: no device list: %s", error->message);
		return;
	}

	g_variant_get (result, "(as)", &devices);
	while (g_variant_iter_next (devices, "&s", &id)) {
		g_autofree gchar *path = g_strdup_printf ("%s/devices/%s/ping", KDECONNECT_PATH, id);

		g_dbus_connection_call (bus, KDECONNECT_SERVICE, path, KDECONNECT_PING, "sendPing",
		                        g_variant_new ("(s)", message), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
		                        10000, NULL, kdeconnect_ping_cb, g_strdup (id));
	}
}

static gboolean
kdeconnect_send (gpointer user_data)
{
	g_autoptr(GDBusConnection)	bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
	g_autofree gchar		*feeds = g_string_free (pending, FALSE);
	gchar				*message;

	sendId = 0;
	pending = NULL;
	if (!bus)
		return G_SOURCE_REMOVE;

	message = g_strdup_printf (_("Liferea-Wonder: new items in %s"), feeds);
	debug (DEBUG_UPDATE, "KDE Connect: sending \"%s\"", message);

	/* paired devices that are reachable now */
	g_dbus_connection_call (bus, KDECONNECT_SERVICE, KDECONNECT_PATH, KDECONNECT_DAEMON, "devices",
	                        g_variant_new ("(bb)", TRUE, TRUE), G_VARIANT_TYPE ("(as)"),
	                        G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, NULL, kdeconnect_devices_cb, message);

	return G_SOURCE_REMOVE;
}

void
kdeconnect_notify_new_items (const gchar *feedTitle, guint count)
{
	if (!pending)
		pending = g_string_new (NULL);
	g_string_append_printf (pending, "%s%s (%u)", pending->len?", ":"", feedTitle?feedTitle:"", count);

	if (!sendId)
		sendId = g_timeout_add_seconds (KDECONNECT_COLLECT_SECONDS, kdeconnect_send, NULL);
}
