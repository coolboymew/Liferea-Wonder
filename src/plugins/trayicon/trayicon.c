/*
 * trayicon.c: Liferea Plugins using libpeas2 + libstray
 *
 * Copyright (C) 2026 Lars Windolf <lars.windolf@gmx.de>
 * 
 * Derived from PeerTube-GTK
 *
 * Copyright (C) 2021-2025 MorsMortium, Erwinjitsu and Other Contributors    
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
 * Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <libpeas.h>
#include <gtk/gtk.h>

#define STRAY_IMPL 1

#include "stray.h"
#include "../src/actions/node_actions.h"
#include "../src/conf.h"
#include "../src/debug.h"
#include "../src/feedlist.h"
#include "../src/liferea_application.h"
#include "../src/node.h"
#include "../src/subscription.h"
#include "../src/ui/liferea_shell.h"
#include "../src/plugins/liferea_activatable.h"
#include "../src/plugins/liferea_shell_activatable.h"

/*
 * Standard gettext macros
 */
#ifdef ENABLE_NLS
#  include <libintl.h>
#  undef _
#  define _(String) dgettext (PACKAGE, String)
#  define Q_(String) g_strip_context ((String), gettext (String))
#  ifdef gettext_noop
#    define N_(String) gettext_noop (String)
#  else
#    define N_(String) (String)
#  endif
#else
#  define textdomain(String) (String)
#  define gettext(String) (String)
#  define ngettext(Singular,Plural,Count) ((Count == 1) ? (Singular) : (Plural))
#  define dgettext(Domain,Message) (Message)
#  define dcgettext(Domain,Message,Type) (Message)
#  define bindtextdomain(Domain,Directory) (Domain)
#  define _(String) (String)
#  define Q_(String) g_strip_context ((String), (String))
#  define N_(String) (String)
#endif

#define TRAYICON_TYPE_PLUGIN (trayicon_plugin_get_type ())

enum {
	PROP_0,
	PROP_SHELL,
	N_PROPS
};

/* up to this many feeds waiting for a site check get a menu item */
#define TRAYICON_CHECK_ITEMS	5

typedef struct {
	GObject parent_instance;

        TrayIcon        *tray;
	GObject         *shell;
	guint            timeout_id;

	const gchar     *iconName;	/* normal icon, see trayicon_pick_icon */
	gboolean         inTray;	/* the panel shows the icon (closing the window hides it then) */

	/* colour alerts (see trayicon_update_alerts) */
	gboolean         registered;
	guint            alertMask;	/* bit (1 << frame) per frame that is on: colours, TRAYICON_LOCK */
	guint            shownColor;	/* frame currently shown (TRAYICON_PLAIN = normal icon) */
	guint            fadeTo;	/* frame being faded to */
	guint            fadeStep;
	guint            holdId;	/* timer for showing a colour before fading on */
	guint            fadeId;	/* timer for the crossfade steps */
	guint            checkId;	/* timer for looking at the feeds after a change */
	guint            ticks;		/* event timer runs, for the regular look at the feeds */
	gchar           *tooltip;	/* feeds named in the tooltip */

	/* feeds waiting for a site check (Preferences: show in the tray) */
	gboolean         lockShown;	/* feeds wait for a site check: the padlock frame is in the cycle */
	gint             checkSeparator;
	gint             checkItems[TRAYICON_CHECK_ITEMS];	/* menu items "Pass Site Check: feed" */
	gchar           *checkNodes[TRAYICON_CHECK_ITEMS];	/* their feeds' ids */
} TrayiconPlugin;

#define TRAYICON_SIZE		32
#define TRAYICON_HOLD_SECONDS	5
#define TRAYICON_FADE_STEPS	20	/* x 50ms = 1s crossfade */

/* Frames of the cycle: the alert colours (trayAlertColor), and in the
   place of TRAY_ALERT_NONE the site check padlock on a slate square */
#define TRAYICON_LOCK		TRAY_ALERT_NONE
#define TRAYICON_PLAIN		TRAY_ALERT_COUNT	/* the normal icon, no frame */

extern FeedList *feedlist;

/* indexed by trayAlertColor */
static const gdouble alertRGB[TRAY_ALERT_COUNT][3] = TRAY_ALERT_RGB;

/* Light backgrounds (yellow, white) get a dark feed symbol */
static gdouble
trayicon_symbol_level (const gdouble *rgb)
{
	gdouble luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];

	return (luminance > 0.65)?0.2:1.0;
}

typedef struct {
	GObjectClass parent_class;
} TrayiconPluginClass;

static void
trayicon_plugin_iface_init (LifereaActivatableInterface *iface);

static void
trayicon_plugin_shell_iface_init (LifereaShellActivatableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (TrayiconPlugin, trayicon_plugin, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (LIFEREA_TYPE_ACTIVATABLE, trayicon_plugin_iface_init)
                               G_IMPLEMENT_INTERFACE (LIFEREA_TYPE_SHELL_ACTIVATABLE, trayicon_plugin_shell_iface_init))

static void
trayicon_stop_alert_timers (TrayiconPlugin *plugin)
{
	g_clear_handle_id (&plugin->holdId, g_source_remove);
	g_clear_handle_id (&plugin->fadeId, g_source_remove);
	g_clear_handle_id (&plugin->checkId, g_source_remove);
}

static void
trayicon_remove (TrayiconPlugin *plugin)
{
	trayicon_stop_alert_timers (plugin);
	if (feedlist)
		g_signal_handlers_disconnect_by_data (feedlist, plugin);
	plugin->registered = FALSE;
	if (plugin->inTray && plugin->shell)
		g_object_set (plugin->shell, "close-to-tray", FALSE, NULL);
	plugin->inTray = FALSE;
	plugin->lockShown = FALSE;
	g_clear_pointer (&plugin->tooltip, g_free);
	for (guint i = 0; i < TRAYICON_CHECK_ITEMS; i++)
		g_clear_pointer (&plugin->checkNodes[i], g_free);

	if (plugin->timeout_id) {
		g_source_remove (plugin->timeout_id);
		plugin->timeout_id = 0;
	}
	if (plugin->tray) {
        	stray_destroy (plugin->tray);
		plugin->tray = NULL;
	}
}

static void
trayicon_plugin_finalize (GObject *object)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)object;

	trayicon_remove (plugin);
	g_clear_object (&plugin->shell);
	G_OBJECT_CLASS (trayicon_plugin_parent_class)->finalize (object);
}

static void
trayicon_plugin_init (TrayiconPlugin *plugin)
{
	plugin->shownColor = TRAYICON_PLAIN;
}

static void
trayicon_plugin_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)object;

	switch (prop_id) {
		case PROP_SHELL:
			g_value_set_object (value, plugin->shell);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
trayicon_plugin_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)object;

	switch (prop_id) {
		case PROP_SHELL:
			g_set_object (&plugin->shell, g_value_get_object (value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
trayicon_plugin_class_init (TrayiconPluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = trayicon_plugin_finalize;
	object_class->get_property = trayicon_plugin_get_property;
	object_class->set_property = trayicon_plugin_set_property;

	g_object_class_install_property (object_class,
	                                 PROP_SHELL,
	                                 g_param_spec_object ("shell",
	                                                       "Shell",
	                                                       "Liferea shell instance",
	                                                       LIFEREA_SHELL_TYPE,
	                                                       G_PARAM_READWRITE));
}

static void
on_toggle_visibility (G_GNUC_UNUSED int id, gpointer userdata)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)userdata;
	gboolean visible = FALSE;

	g_object_get (G_OBJECT (plugin->shell), "visibility", &visible, NULL);
	g_object_set (G_OBJECT (plugin->shell), "visibility", !visible, NULL);
}

static void
on_quit (G_GNUC_UNUSED int id, G_GNUC_UNUSED void *userdata)
{
        liferea_application_shutdown ();
}

/* Left click brings the window up: shows it when hidden, raises it
   when behind other windows (hiding is in the menu and the close button) */
static void
on_button_cb (TrayButton button, G_GNUC_UNUSED int x, G_GNUC_UNUSED int y, void *userdata)
{
	TrayiconPlugin		*plugin = (TrayiconPlugin *)userdata;
	g_autofree gchar	*token = stray_take_activation_token (plugin->tray);
	GtkWindow		*window = GTK_WINDOW (liferea_shell_get_window ());

	if (button != STRAY_BUTTON_LEFT)
		return;

	/* on Wayland only a token from the panel allows raising the window */
	if (token)
		gtk_window_set_startup_id (window, token);
	g_object_set (G_OBJECT (plugin->shell), "visibility", TRUE, NULL);
}

/*static void
on_close_checkbox_changed (G_GNUC_UNUSED int id, G_GNUC_UNUSED void *userdata)
{
}*/

/* Draws a rounded square of the given colour with a white feed symbol
   and hands it to the tray as pixmap */
static void
trayicon_draw_color (TrayiconPlugin *plugin, gdouble r, gdouble g, gdouble b, gdouble symbol, gdouble lockAlpha)
{
	cairo_surface_t	*surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, TRAYICON_SIZE, TRAYICON_SIZE);
	cairo_t		*cr = cairo_create (surface);
	const gdouble	s = TRAYICON_SIZE, radius = s * 0.2;
	guint32		pixels[TRAYICON_SIZE * TRAYICON_SIZE];
	guchar		*data;
	gint		stride;

	cairo_new_sub_path (cr);
	cairo_arc (cr, s - radius - 1, radius + 1, radius, -G_PI / 2, 0);
	cairo_arc (cr, s - radius - 1, s - radius - 1, radius, 0, G_PI / 2);
	cairo_arc (cr, radius + 1, s - radius - 1, radius, G_PI / 2, G_PI);
	cairo_arc (cr, radius + 1, radius + 1, radius, G_PI, 3 * G_PI / 2);
	cairo_close_path (cr);
	cairo_set_source_rgb (cr, r, g, b);
	cairo_fill_preserve (cr);

	/* thin outline so light colours stay visible on light panels */
	cairo_set_source_rgba (cr, 0, 0, 0, 0.35);
	cairo_set_line_width (cr, 1);
	cairo_stroke (cr);

	/* feed symbol: a dot and two quarter circles */
	cairo_set_source_rgb (cr, symbol, symbol, symbol);
	cairo_arc (cr, s * 0.31, s * 0.69, s * 0.08, 0, 2 * G_PI);
	cairo_fill (cr);
	cairo_set_line_width (cr, s * 0.1);
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
	cairo_arc (cr, s * 0.25, s * 0.75, s * 0.3, -G_PI / 2, 0);
	cairo_stroke (cr);
	cairo_arc (cr, s * 0.25, s * 0.75, s * 0.52, -G_PI / 2, 0);
	cairo_stroke (cr);

	/* a feed waits for a site check: yellow padlock in the corner
	   (fading in and out with its frame) */
	if (lockAlpha > 0) {
		const gdouble x = s * 0.52, y = s * 0.50, w = s * 0.46, h = s * 0.48;

		cairo_push_group (cr);

		/* shackle */
		cairo_set_line_width (cr, s * 0.09);
		cairo_set_source_rgb (cr, 0.15, 0.15, 0.15);
		cairo_arc (cr, x + w / 2, y + h * 0.30, w * 0.30, G_PI, 2 * G_PI);
		cairo_stroke (cr);
		cairo_set_line_width (cr, s * 0.05);
		cairo_set_source_rgb (cr, 0.98, 0.80, 0.10);
		cairo_arc (cr, x + w / 2, y + h * 0.30, w * 0.30, G_PI, 2 * G_PI);
		cairo_stroke (cr);
		/* body */
		cairo_rectangle (cr, x + w * 0.08, y + h * 0.36, w * 0.84, h * 0.60);
		cairo_set_source_rgb (cr, 0.98, 0.80, 0.10);
		cairo_fill_preserve (cr);
		cairo_set_source_rgb (cr, 0.15, 0.15, 0.15);
		cairo_set_line_width (cr, s * 0.04);
		cairo_stroke (cr);
		/* keyhole */
		cairo_arc (cr, x + w / 2, y + h * 0.60, w * 0.08, 0, 2 * G_PI);
		cairo_fill (cr);

		cairo_pop_group_to_source (cr);
		cairo_paint_with_alpha (cr, lockAlpha);
	}

	cairo_surface_flush (surface);
	data = cairo_image_surface_get_data (surface);
	stride = cairo_image_surface_get_stride (surface);

	/* cairo uses premultiplied alpha, the tray expects plain ARGB */
	for (gint y = 0; y < TRAYICON_SIZE; y++) {
		for (gint x = 0; x < TRAYICON_SIZE; x++) {
			guint32 p = ((guint32 *)(data + y * stride))[x];
			guint a = p >> 24;

			if (a > 0 && a < 255) {
				guint pr = MIN (255u, (((p >> 16) & 0xff) * 255 + a / 2) / a);
				guint pg = MIN (255u, (((p >> 8) & 0xff) * 255 + a / 2) / a);
				guint pb = MIN (255u, ((p & 0xff) * 255 + a / 2) / a);
				p = (a << 24) | (pr << 16) | (pg << 8) | pb;
			}
			pixels[y * TRAYICON_SIZE + x] = p;
		}
	}

	stray_set_icon_pixmap (plugin->tray, TRAYICON_SIZE, TRAYICON_SIZE, pixels);

	cairo_destroy (cr);
	cairo_surface_destroy (surface);
}

/* Next frame that is on after "from", round the cycle */
static guint
trayicon_next_color (guint mask, guint from)
{
	for (guint i = 1; i <= TRAY_ALERT_COUNT; i++) {
		guint c = (from + i) % TRAY_ALERT_COUNT;
		if (mask & (1u << c))
			return c;
	}

	return TRAYICON_PLAIN;
}

/* frame "from" faded to frame "to" by t (0..1) */
static void
trayicon_draw_frames (TrayiconPlugin *plugin, guint from, guint to, gdouble t)
{
	const gdouble	*a = alertRGB[from], *b = alertRGB[to];

	trayicon_draw_color (plugin,
	                     a[0] + (b[0] - a[0]) * t,
	                     a[1] + (b[1] - a[1]) * t,
	                     a[2] + (b[2] - a[2]) * t,
	                     trayicon_symbol_level (a) + (trayicon_symbol_level (b) - trayicon_symbol_level (a)) * t,
	                     (from == TRAYICON_LOCK?1 - t:0) + (to == TRAYICON_LOCK?t:0));
}

static gboolean trayicon_hold_cb (gpointer user_data);

static gboolean
trayicon_fade_cb (gpointer user_data)
{
	TrayiconPlugin	*plugin = (TrayiconPlugin *)user_data;

	trayicon_draw_frames (plugin, plugin->shownColor, plugin->fadeTo, (gdouble)++plugin->fadeStep / TRAYICON_FADE_STEPS);

	if (plugin->fadeStep < TRAYICON_FADE_STEPS)
		return G_SOURCE_CONTINUE;

	plugin->shownColor = plugin->fadeTo;
	plugin->fadeId = 0;
	plugin->holdId = g_timeout_add_seconds (TRAYICON_HOLD_SECONDS, trayicon_hold_cb, plugin);
	return G_SOURCE_REMOVE;
}

static gboolean
trayicon_hold_cb (gpointer user_data)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)user_data;

	plugin->holdId = 0;
	plugin->fadeTo = trayicon_next_color (plugin->alertMask, plugin->shownColor);
	debug (DEBUG_GUI, "tray alert fading from frame %u to %u", plugin->shownColor, plugin->fadeTo);
	plugin->fadeStep = 0;
	plugin->fadeId = g_timeout_add (1000 / TRAYICON_FADE_STEPS, trayicon_fade_cb, plugin);

	return G_SOURCE_REMOVE;
}

/* A feed's colour is on while it has unread items (also ones it had
   before, at startup or marked unread by hand); with "only newly
   received entries" only after an update brought new ones, until one of
   its items is read */
static void
trayicon_collect_alerts (Node *node, gboolean newOnly, guint *mask, GString *names)
{
	if (node->subscription &&
	    node->subscription->trayAlert != TRAY_ALERT_NONE &&
	    (!newOnly || node->subscription->trayAlertPending) &&
	    node->unreadCount > 0) {
		*mask |= 1u << node->subscription->trayAlert;
		g_string_append_printf (names, "%s%s", names->len?", ":"", node_get_title (node));
	}

	for (GSList *iter = node->children; iter; iter = g_slist_next (iter))
		trayicon_collect_alerts ((Node *)iter->data, newOnly, mask, names);
}

static void
trayicon_collect_checks (Node *node, GPtrArray *nodes)
{
	if (node->subscription && node->subscription->siteCheckNeeded)
		g_ptr_array_add (nodes, node);

	for (GSList *iter = node->children; iter; iter = g_slist_next (iter))
		trayicon_collect_checks ((Node *)iter->data, nodes);
}

static void
on_pass_check (int id, gpointer userdata)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)userdata;

	for (guint i = 0; i < TRAYICON_CHECK_ITEMS; i++)
		if (plugin->checkItems[i] == id && plugin->checkNodes[i])
			node_pass_site_check (node_is_used_id (plugin->checkNodes[i]));
}

/* Feeds waiting for a site check: padlock on the icon, one menu item
   each. Returns TRUE if the padlock changed. */
static gboolean
trayicon_update_checks (TrayiconPlugin *plugin)
{
	g_autoptr(GPtrArray)	nodes = g_ptr_array_new ();
	gboolean		show = TRUE, lock;
	TrayMenu		*menu = plugin->tray->menu;

	conf_get_bool_value (SITE_CHECK_TRAY, &show);
	if (show)
		trayicon_collect_checks (feedlist_get_root (), nodes);

	for (guint i = 0; i < TRAYICON_CHECK_ITEMS; i++) {
		Node *node = (i < nodes->len)?g_ptr_array_index (nodes, i):NULL;

		if (g_strcmp0 (node?node->id:NULL, plugin->checkNodes[i]) == 0)
			continue;
		g_free (plugin->checkNodes[i]);
		plugin->checkNodes[i] = node?g_strdup (node->id):NULL;
		if (node) {
			g_autofree gchar *label = g_strdup_printf (_("Pass Site Check: %s"), node_get_title (node));
			stray_menu_set_item_label (menu, plugin->checkItems[i], label);
		}
		stray_menu_set_item_visible (menu, plugin->checkItems[i], node != NULL);
	}
	stray_menu_set_item_visible (menu, plugin->checkSeparator, nodes->len > 0);

	lock = nodes->len > 0;
	if (lock == plugin->lockShown)
		return FALSE;

	debug (DEBUG_GUI, "tray site check padlock %s (%u feeds)", lock?"on":"off", nodes->len);
	plugin->lockShown = lock;
	return TRUE;
}

/* Feeds can be given an alert colour (feed properties). While such a
   feed has unread items the icon takes its colour; with several colours
   (and the site check padlock) on it shows each for 5 seconds and fades
   to the next. */
static void
trayicon_update_alerts (TrayiconPlugin *plugin)
{
	g_autoptr(GString)	names = g_string_new (NULL);
	guint			mask = 0;

	if (!plugin->registered || !feedlist_get_root ())
		return;

	gboolean newOnly = FALSE;

	trayicon_update_checks (plugin);
	conf_get_bool_value (TRAY_ALERT_NEW_ONLY, &newOnly);
	trayicon_collect_alerts (feedlist_get_root (), newOnly, &mask, names);
	if (plugin->lockShown) {
		g_autoptr(GPtrArray) nodes = g_ptr_array_new ();

		trayicon_collect_checks (feedlist_get_root (), nodes);
		g_string_append_printf (names, "%s%s", names->len?"\n":"", _("Site check needed:"));
		for (guint i = 0; i < nodes->len; i++)
			g_string_append_printf (names, "%s %s", i?",":"", node_get_title (g_ptr_array_index (nodes, i)));
	}

	if (!plugin->tooltip || !g_str_equal (plugin->tooltip, names->str)) {
		g_free (plugin->tooltip);
		plugin->tooltip = g_strdup (names->str);
		if (names->len && mask) {
			g_autofree gchar *text = g_strdup_printf (_("Unread items in %s"), names->str);
			stray_set_tooltip (plugin->tray, "Liferea", text);
		} else if (names->len) {
			g_autofree gchar *text = g_strdup (names->str);
			stray_set_tooltip (plugin->tray, "Liferea", text);
		} else {
			stray_set_tooltip (plugin->tray, "Liferea", "");
		}
	}

	if (plugin->lockShown)
		mask |= 1u << TRAYICON_LOCK;

	if (mask == plugin->alertMask)
		return;

	debug (DEBUG_GUI, "tray alert frames changed to 0x%x (%s)", mask, names->str);
	plugin->alertMask = mask;
	trayicon_stop_alert_timers (plugin);

	if (!mask) {
		plugin->shownColor = TRAYICON_PLAIN;
		stray_set_icon (plugin->tray, plugin->iconName);
		return;
	}

	/* stay on the frame shown if it is still on, else the first one */
	if (plugin->shownColor == TRAYICON_PLAIN || !(mask & (1u << plugin->shownColor)))
		plugin->shownColor = trayicon_next_color (mask, TRAYICON_PLAIN);
	trayicon_draw_frames (plugin, plugin->shownColor, plugin->shownColor, 0);

	/* more than one frame on: cycle through them */
	if (mask & (mask - 1))
		plugin->holdId = g_timeout_add_seconds (TRAYICON_HOLD_SECONDS, trayicon_hold_cb, plugin);
}

static gboolean
trayicon_check_cb (gpointer user_data)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)user_data;

	plugin->checkId = 0;
	trayicon_update_alerts (plugin);
	return G_SOURCE_REMOVE;
}

/* The signals come before the unread counters are updated (a feed with
   new items still says 0 unread), so look a moment later */
static void
on_feedlist_changed (GObject *obj, gpointer arg, gpointer user_data)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)user_data;

	if (!plugin->checkId)
		plugin->checkId = g_timeout_add (300, trayicon_check_cb, plugin);
}

static gboolean
TrayEvents(gpointer Data)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *) Data;

	// Process events the tray icon received, this also registers
	// the icon when the panel's tray starts after Liferea
	stray_process_events(plugin->tray);

	/* and every second, for counter changes that come without a signal
	   (e.g. an item marked unread) */
	if (++plugin->ticks % 10 == 0)
		trayicon_update_alerts (plugin);

	/* closing the window only hides it while there is an icon to bring it back */
	if (plugin->inTray != (plugin->tray->registered != 0)) {
		plugin->inTray = (plugin->tray->registered != 0);
		debug (DEBUG_GUI, "tray icon %s", plugin->inTray?"shown by the panel":"not shown");
		g_object_set (plugin->shell, "close-to-tray", plugin->inTray, NULL);
	}

	return G_SOURCE_CONTINUE;
}

/* The application icon: installed with Liferea into the base icon
   set every theme falls back to, so every panel finds it (themed names
   like "emblem-web" are missing from e.g. Breeze, the icon then did
   not show at all) */
static const gchar *
trayicon_pick_icon (void)
{
	return g_application_get_application_id (g_application_get_default ());
}

static void
trayicon_activate (LifereaActivatable *activatable)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)activatable;
	TrayMenu *menu;

	plugin->iconName = trayicon_pick_icon ();
        plugin->tray = stray_create ("Liferea", plugin->iconName, "Liferea");
	stray_set_status (plugin->tray, STRAY_STATUS_ACTIVE);

	// Create tray menu and add to icon
	menu = stray_menu_create ();
	stray_set_menu (plugin->tray, menu);

	/* Without a tray yet (e.g. Liferea started at login before the
	   panel) stray registers as soon as the tray appears, the events
	   below must keep being processed for that */
	debug (DEBUG_GUI, "tray icon \"%s\"", plugin->iconName);
	if (stray_register (plugin->tray) == 0)
		debug (DEBUG_GUI, "no system tray yet, waiting for one");

	/* hidden until feeds wait for a site check */
	for (guint i = 0; i < TRAYICON_CHECK_ITEMS; i++) {
		plugin->checkItems[i] = stray_menu_add_item (menu, _("Pass Site Check"), on_pass_check, plugin);
		stray_menu_set_item_visible (menu, plugin->checkItems[i], FALSE);
	}
	plugin->checkSeparator = stray_menu_add_separator (menu);
	stray_menu_set_item_visible (menu, plugin->checkSeparator, FALSE);

	stray_menu_add_item (menu, _("Show / Hide"), on_toggle_visibility, plugin);
	//stray_menu_add_check_item (menu, _("Minimize to tray on close"), on_close_checkbox_changed, plugin);
	//stray_menu_set_item_checked(menu, id, checked);
	stray_menu_add_item (menu, _("Quit"), on_quit, NULL);
	stray_set_button_callback (plugin->tray, on_button_cb, plugin);

	plugin->timeout_id = g_timeout_add (100, TrayEvents, plugin);

	/* colour alerts follow item and selection changes */
	plugin->registered = TRUE;
	if (feedlist) {
		g_signal_connect (feedlist, "items-updated", G_CALLBACK (on_feedlist_changed), plugin);
		g_signal_connect (feedlist, "node-updated", G_CALLBACK (on_feedlist_changed), plugin);
		g_signal_connect (feedlist, "node-selected", G_CALLBACK (on_feedlist_changed), plugin);
		g_signal_connect (feedlist, "new-items", G_CALLBACK (on_feedlist_changed), plugin);
	}
	trayicon_update_alerts (plugin);

	// Always force show window upon activation to avoid hidden window on startup
	g_object_set (plugin->shell, "visibility", TRUE, NULL);
}

static void
trayicon_deactivate (LifereaActivatable *activatable)
{
	TrayiconPlugin *plugin = (TrayiconPlugin *)activatable;

	trayicon_remove (plugin);
}

static void
trayicon_plugin_iface_init (LifereaActivatableInterface *iface)
{
	iface->activate = trayicon_activate;
	iface->deactivate = trayicon_deactivate;
}

static void
trayicon_plugin_shell_iface_init (LifereaShellActivatableInterface *iface)
{
}

G_MODULE_EXPORT void
_lp_trayicon_register_types (PeasObjectModule *module)
{
        peas_object_module_register_extension_type (module, LIFEREA_TYPE_SHELL_ACTIVATABLE, TRAYICON_TYPE_PLUGIN);
}