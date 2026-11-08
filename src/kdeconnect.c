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
#include "conf.h"
#include "debug.h"
#include "feedlist.h"
#include "node.h"
#include "subscription.h"

#define KDECONNECT_SERVICE	"org.kde.kdeconnect"
#define KDECONNECT_PATH		"/modules/kdeconnect"
#define KDECONNECT_DAEMON	"org.kde.kdeconnect.daemon"
#define KDECONNECT_PING		"org.kde.kdeconnect.device.ping"

/* updates of several feeds usually come together: wait this long for
   more before sending one message */
#define KDECONNECT_COLLECT_SECONDS	5

static GString	*pending = NULL;	/* "Feed (n), Other feed (m)" */
static GHashTable *pendingFeeds = NULL;	/* feed titles in "pending" */
static guint	sendId = 0;

/* "any unread" mode (kdeconnect-new-only off): feeds seen with unread items */
#define KDECONNECT_WATCH_SECONDS	2
static GHashTable *hadUnread = NULL;	/* node ids */

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

/* sends the message to the paired devices that are reachable now */
static void
kdeconnect_send_message (gchar *message)
{
	g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

	if (!bus) {
		g_free (message);
		return;
	}

	debug (DEBUG_UPDATE, "KDE Connect: sending \"%s\"", message);
	g_dbus_connection_call (bus, KDECONNECT_SERVICE, KDECONNECT_PATH, KDECONNECT_DAEMON, "devices",
	                        g_variant_new ("(bb)", TRUE, TRUE), G_VARIANT_TYPE ("(as)"),
	                        G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, NULL, kdeconnect_devices_cb, message);
}

static gboolean
kdeconnect_send (gpointer user_data)
{
	g_autofree gchar *feeds = g_string_free (pending, FALSE);

	sendId = 0;
	pending = NULL;
	g_clear_pointer (&pendingFeeds, g_hash_table_destroy);
	kdeconnect_send_message (g_strdup_printf (_("Liferea-Wonder: new items in %s"), feeds));

	return G_SOURCE_REMOVE;
}

void
kdeconnect_notify_site_check (const gchar *feedTitle)
{
	kdeconnect_send_message (g_strdup_printf (_("Liferea-Wonder: %s needs a site check, open Liferea-Wonder and pass it"), feedTitle?feedTitle:""));
}

void
kdeconnect_notify_new_items (const gchar *feedTitle, guint count)
{
	if (!pending)
		pending = g_string_new (NULL);
	if (!pendingFeeds)
		pendingFeeds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	/* the same feed once per message */
	if (!g_hash_table_add (pendingFeeds, g_strdup (feedTitle?feedTitle:"")))
		return;
	g_string_append_printf (pending, "%s%s (%u)", pending->len?", ":"", feedTitle?feedTitle:"", count);

	if (!sendId)
		sendId = g_timeout_add_seconds (KDECONNECT_COLLECT_SECONDS, kdeconnect_send, NULL);
}

/* Feeds with phone messages that have unread items now and had none at
   the last look (or at startup) get a message, unless only newly
   received entries count */
static void
kdeconnect_watch_node (Node *node, gboolean announce)
{
	if (node->subscription && node->subscription->kdeConnect) {
		gboolean had = hadUnread && g_hash_table_contains (hadUnread, node->id);

		if (node->unreadCount > 0 && !had) {
			g_hash_table_add (hadUnread, g_strdup (node->id));
			if (announce)
				kdeconnect_notify_new_items (node_get_title (node), node->unreadCount);
		} else if (node->unreadCount == 0 && had) {
			g_hash_table_remove (hadUnread, node->id);
		}
	}

	for (GSList *iter = node->children; iter; iter = g_slist_next (iter))
		kdeconnect_watch_node ((Node *)iter->data, announce);
}

static gboolean
kdeconnect_watch (gpointer user_data)
{
	gboolean newOnly = TRUE;

	if (!feedlist_get_root ())
		return G_SOURCE_CONTINUE;

	conf_get_bool_value (KDECONNECT_NEW_ONLY, &newOnly);
	if (!hadUnread)
		hadUnread = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	/* followed in both modes, so switching the option on sends nothing old */
	kdeconnect_watch_node (feedlist_get_root (), !newOnly);

	return G_SOURCE_CONTINUE;
}

void
kdeconnect_watch_start (void)
{
	static guint watchId = 0;

	if (!watchId)
		watchId = g_timeout_add_seconds (KDECONNECT_WATCH_SECONDS, kdeconnect_watch, NULL);
}
