/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Vincent Untz <vuntz@gnome.org>
 *
 * Based on set-timezone.c from gnome-panel which was GPLv2+ with this
 * copyright:
 *    Copyright (C) 2007 David Zeuthen <david@fubar.dk>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "gconf-policykit.h"

#define CACHE_VALIDITY_SEC 10

static DBusGConnection *
get_system_bus (void)
{
        GError          *error;
        static DBusGConnection *bus = NULL;

        if (bus == NULL) {
                error = NULL;
                bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
                if (bus == NULL) {
                        g_warning ("Couldn't connect to system bus: %s",
                                   error->message);
                        g_error_free (error);
                }
        }

        return bus;
}

static guint
can_set (const gchar *key, gboolean mandatory)
{
        DBusGConnection *bus;
        DBusGProxy *proxy = NULL;
	const gchar *keys[2];
	const gchar *func;
	GError *error = NULL;
        guint res = 0;

        bus = get_system_bus ();
        if (bus == NULL)
                goto out;

        proxy = dbus_g_proxy_new_for_name (bus,
                                           "org.gnome.GConf.Defaults",
                                           "/",
                                           "org.gnome.GConf.Defaults");
        if (proxy == NULL)
                goto out;

	keys[0] = key;
	keys[1] = NULL;
	func = mandatory ? "CanSetMandatory" : "CanSetSystem";

        if (!dbus_g_proxy_call (proxy, func,
                                &error,
                                G_TYPE_STRV, keys,
                                G_TYPE_INVALID,
				G_TYPE_UINT, &res,
                                G_TYPE_INVALID)) {
    		g_warning ("error calling %s: %s\n", func, error->message);
    		g_error_free (error);
  	}

out:
	if (proxy)
		g_object_unref (proxy);

        return res;
}

typedef struct
{
	time_t last_refreshed;
	gint can_set;
} CacheEntry;

static GHashTable *defaults_cache = NULL;
static GHashTable *mandatory_cache = NULL;

gint
gconf_pk_can_set (const gchar *key, gboolean mandatory)
{
        time_t now;
	GHashTable **cache;
	CacheEntry *entry;

        time (&now);
	cache = mandatory ? &mandatory_cache : &defaults_cache;
	if (!*cache)
		*cache = g_hash_table_new (g_str_hash, g_str_equal);
	entry = (CacheEntry *) g_hash_table_lookup (*cache, key);
	if (!entry) {
		entry = g_new0 (CacheEntry, 1);
		g_hash_table_insert (*cache, (char *) key, entry);
	}
        if (ABS (now - entry->last_refreshed) > CACHE_VALIDITY_SEC) {
        	entry->can_set = can_set (key, mandatory);
                entry->last_refreshed = now;
        }

        return entry->can_set;
}

gint
gconf_pk_can_set_default (const gchar *key)
{
	return gconf_pk_can_set (key, FALSE);
}

gint
gconf_pk_can_set_mandatory (const gchar *key)
{
	return gconf_pk_can_set (key, TRUE);
}

static void
gconf_pk_update_can_set_cache (const gchar *key,
                               gboolean     mandatory)
{
        time_t          now;
	GHashTable **cache;
	CacheEntry *entry;

        time (&now);
	cache = mandatory ? &mandatory_cache : &defaults_cache;
	if (!*cache)
		*cache = g_hash_table_new (g_str_hash, g_str_equal);
	entry = (CacheEntry *) g_hash_table_lookup (*cache, key);
	if (!entry) {
		entry = g_new0 (CacheEntry, 1);
		g_hash_table_insert (*cache, (char *) key, entry);
	}
        entry->can_set = 2;
        entry->last_refreshed = now;
}

typedef struct {
        gint            ref_count;
        gboolean        mandatory;
        gchar          *key;
        GFunc           callback;
        gpointer        data;
        GDestroyNotify  notify;
} GConfPKCallbackData;

static gpointer
_gconf_pk_data_ref (gpointer d)
{
        GConfPKCallbackData *data = d;

        data->ref_count++;

        return data;
}

static void
_gconf_pk_data_unref (gpointer d)
{
        GConfPKCallbackData *data = d;

        data->ref_count--;
        if (data->ref_count == 0) {
                if (data->notify)
                        data->notify (data->data);
                g_free (data->key);
                g_slice_free (GConfPKCallbackData, data);
        }
}

static void set_key_async (GConfPKCallbackData *data);

static void
gconf_pk_update_can_set_cache (const gchar *key,
                               gboolean     mandatory);

static void
set_key_notify (DBusGProxy     *proxy,
                DBusGProxyCall *call,
                void           *user_data)
{
        GConfPKCallbackData *data = user_data;
        GError *error = NULL;

        if (dbus_g_proxy_end_call (proxy, call, &error, G_TYPE_INVALID)) {
		gconf_pk_update_can_set_cache (data->key, data->mandatory);
                if (data->callback)
                        data->callback (data->data, NULL);
        }
        else {
                if (error->domain == DBUS_GERROR &&
                    error->code == DBUS_GERROR_NO_REPLY) {
                        /* these errors happen because dbus doesn't
                         * use monotonic clocks
                         */
                        g_warning ("ignoring no-reply error when setting key");
                        g_error_free (error);
                        if (data->callback)
                                data->callback (data->data, NULL);
                }
                else {
                        if (data->callback)
                                data->callback (data->data, error);
                        else
                                g_error_free (error);
                }
        }
}

static void
set_key_async (GConfPKCallbackData *data)
{
        DBusGConnection *bus;
        DBusGProxy      *proxy;
	const gchar     *call;
        gchar           *keys[2] = { data->key, NULL };

        bus = get_system_bus ();
        if (bus == NULL)
                return;

        proxy = dbus_g_proxy_new_for_name (bus,
                                           "org.gnome.GConf.Defaults",
                                           "/",
                                           "org.gnome.GConf.Defaults");

	call = data->mandatory ? "SetMandatory" : "SetSystem";
        dbus_g_proxy_begin_call_with_timeout (proxy,
                                              call,
                                              set_key_notify,
                                              _gconf_pk_data_ref (data),
                                              _gconf_pk_data_unref,
                                              INT_MAX,
                                              /* parameters: */
                                              G_TYPE_STRV, keys,
                                              G_TYPE_STRV, NULL,
                                              G_TYPE_INVALID,
                                              /* return values: */
                                              G_TYPE_INVALID);
}

void
gconf_pk_set_default_async (const gchar    *key,
                            GFunc           callback,
                            gpointer        d,
                            GDestroyNotify  notify)
{
        GConfPKCallbackData *data;

        if (key == NULL)
                return;

        data = g_slice_new0 (GConfPKCallbackData);
        data->ref_count = 1;
        data->mandatory = FALSE;
        data->key = g_strdup (key);
        data->callback = callback;
        data->data = d;
        data->notify = notify;

        set_key_async (data);
        _gconf_pk_data_unref (data);
}

void
gconf_pk_set_mandatory_async (const gchar    *key,
                              GFunc           callback,
                              gpointer        d,
                              GDestroyNotify  notify)
{
        GConfPKCallbackData *data;

        if (key == NULL)
                return;

        data = g_slice_new0 (GConfPKCallbackData);
        data->ref_count = 1;
        data->mandatory = TRUE;
        data->key = g_strdup (key);
        data->callback = callback;
        data->data = d;
        data->notify = notify;

        set_key_async (data);
        _gconf_pk_data_unref (data);
}
