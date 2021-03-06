/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/***
  Copyright 2009 Lennart Poettering
  Copyright 2010 David Henningsson <diwic@ubuntu.com>

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
***/

#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>

#include <dbus/dbus.h>

#include <pinos/client/log.h>

#include "rtkit.h"

struct _PinosRTKitBus {
  DBusConnection *bus;
};

PinosRTKitBus *
pinos_rtkit_bus_get_system (void)
{
  PinosRTKitBus *bus;
  DBusError error;

  dbus_error_init(&error);

  bus = calloc (1, sizeof (PinosRTKitBus));
  if (bus == NULL)
    return NULL;

  bus->bus = dbus_bus_get_private (DBUS_BUS_SYSTEM, &error);
  if (bus->bus == NULL)
    goto error;

  dbus_connection_set_exit_on_disconnect (bus->bus, false);

  return bus;

error:
  pinos_log_error ("Failed to connect to system bus: %s", error.message);
  dbus_error_free (&error);
  return NULL;
}

void
pinos_rtkit_bus_free (PinosRTKitBus *system_bus)
{
  dbus_connection_close (system_bus->bus);
  dbus_connection_unref (system_bus->bus);
  free (system_bus);
}

#if defined(__linux__) && !defined(__ANDROID__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>


static pid_t _gettid(void) {
        return (pid_t) syscall(SYS_gettid);
}

static int translate_error(const char *name) {
        if (0 == strcmp (name, DBUS_ERROR_NO_MEMORY))
                return -ENOMEM;
        if (0 == strcmp (name, DBUS_ERROR_SERVICE_UNKNOWN) ||
            0 == strcmp (name, DBUS_ERROR_NAME_HAS_NO_OWNER))
                return -ENOENT;
        if (0 == strcmp (name, DBUS_ERROR_ACCESS_DENIED) ||
            0 == strcmp (name, DBUS_ERROR_AUTH_FAILED))
                return -EACCES;

        return -EIO;
}

static long long rtkit_get_int_property(PinosRTKitBus *connection, const char* propname, long long* propval) {
        DBusMessage *m = NULL, *r = NULL;
        DBusMessageIter iter, subiter;
        dbus_int64_t i64;
        dbus_int32_t i32;
        DBusError error;
        int current_type;
        long long ret;
        const char * interfacestr = "org.freedesktop.RealtimeKit1";

        dbus_error_init(&error);

        if (!(m = dbus_message_new_method_call(
                              RTKIT_SERVICE_NAME,
                              RTKIT_OBJECT_PATH,
                              "org.freedesktop.DBus.Properties",
                              "Get"))) {
                ret = -ENOMEM;
                goto finish;
        }

        if (!dbus_message_append_args(
                            m,
                            DBUS_TYPE_STRING, &interfacestr,
                            DBUS_TYPE_STRING, &propname,
                            DBUS_TYPE_INVALID)) {
                ret = -ENOMEM;
                goto finish;
        }

        if (!(r = dbus_connection_send_with_reply_and_block(connection->bus, m, -1, &error))) {
                ret = translate_error(error.name);
                goto finish;
        }

        if (dbus_set_error_from_message(&error, r)) {
                ret = translate_error(error.name);
                goto finish;
        }

        ret = -EBADMSG;
        dbus_message_iter_init(r, &iter);
        while ((current_type = dbus_message_iter_get_arg_type (&iter)) != DBUS_TYPE_INVALID) {

                if (current_type == DBUS_TYPE_VARIANT) {
                        dbus_message_iter_recurse(&iter, &subiter);

                        while ((current_type = dbus_message_iter_get_arg_type (&subiter)) != DBUS_TYPE_INVALID) {

                                if (current_type == DBUS_TYPE_INT32) {
                                        dbus_message_iter_get_basic(&subiter, &i32);
                                        *propval = i32;
                                        ret = 0;
                                }

                                if (current_type == DBUS_TYPE_INT64) {
                                        dbus_message_iter_get_basic(&subiter, &i64);
                                        *propval = i64;
                                        ret = 0;
                                }

                                dbus_message_iter_next (&subiter);
                         }
                }
                dbus_message_iter_next (&iter);
        }

finish:

        if (m)
                dbus_message_unref(m);

        if (r)
                dbus_message_unref(r);

        dbus_error_free(&error);

        return ret;
}

int pinos_rtkit_get_max_realtime_priority(PinosRTKitBus *connection) {
        long long retval;
        int err;

        err = rtkit_get_int_property(connection, "MaxRealtimePriority", &retval);
        return err < 0 ? err : retval;
}

int pinos_rtkit_get_min_nice_level(PinosRTKitBus *connection, int* min_nice_level) {
        long long retval;
        int err;

        err = rtkit_get_int_property(connection, "MinNiceLevel", &retval);
        if (err >= 0)
                *min_nice_level = retval;
        return err;
}

long long pinos_rtkit_get_rttime_usec_max(PinosRTKitBus *connection) {
        long long retval;
        int err;

        err = rtkit_get_int_property(connection, "RTTimeUSecMax", &retval);
        return err < 0 ? err : retval;
}

int
pinos_rtkit_make_realtime (PinosRTKitBus       *connection,
                           pid_t                thread,
                           int                  priority)
{
        DBusMessage *m = NULL, *r = NULL;
        dbus_uint64_t u64;
        dbus_uint32_t u32;
        DBusError error;
        int ret;

        dbus_error_init(&error);

        if (thread == 0)
                thread = _gettid();

        if (!(m = dbus_message_new_method_call(
                              RTKIT_SERVICE_NAME,
                              RTKIT_OBJECT_PATH,
                              "org.freedesktop.RealtimeKit1",
                              "MakeThreadRealtime"))) {
                ret = -ENOMEM;
                goto finish;
        }

        u64 = (dbus_uint64_t) thread;
        u32 = (dbus_uint32_t) priority;

        if (!dbus_message_append_args(
                            m,
                            DBUS_TYPE_UINT64, &u64,
                            DBUS_TYPE_UINT32, &u32,
                            DBUS_TYPE_INVALID)) {
                ret = -ENOMEM;
                goto finish;
        }

        if (!(r = dbus_connection_send_with_reply_and_block(connection->bus, m, -1, &error))) {
                ret = translate_error(error.name);
                goto finish;
        }


        if (dbus_set_error_from_message(&error, r)) {
                ret = translate_error(error.name);
                goto finish;
        }

        ret = 0;

finish:

        if (m)
                dbus_message_unref(m);

        if (r)
                dbus_message_unref(r);

        dbus_error_free(&error);

        return ret;
}

int
pinos_rtkit_make_high_priority (PinosRTKitBus       *connection,
                                pid_t                thread,
                                int                  nice_level)
{
        DBusMessage *m = NULL, *r = NULL;
        dbus_uint64_t u64;
        dbus_int32_t s32;
        DBusError error;
        int ret;

        dbus_error_init(&error);

        if (thread == 0)
                thread = _gettid();

        if (!(m = dbus_message_new_method_call(
                              RTKIT_SERVICE_NAME,
                              RTKIT_OBJECT_PATH,
                              "org.freedesktop.RealtimeKit1",
                              "MakeThreadHighPriority"))) {
                ret = -ENOMEM;
                goto finish;
        }

        u64 = (dbus_uint64_t) thread;
        s32 = (dbus_int32_t) nice_level;

        if (!dbus_message_append_args(
                            m,
                            DBUS_TYPE_UINT64, &u64,
                            DBUS_TYPE_INT32, &s32,
                            DBUS_TYPE_INVALID)) {
                ret = -ENOMEM;
                goto finish;
        }



        if (!(r = dbus_connection_send_with_reply_and_block(connection->bus, m, -1, &error))) {
                ret = translate_error(error.name);
                goto finish;
        }


        if (dbus_set_error_from_message(&error, r)) {
                ret = translate_error(error.name);
                goto finish;
        }

        ret = 0;

finish:

        if (m)
                dbus_message_unref(m);

        if (r)
                dbus_message_unref(r);

        dbus_error_free(&error);

        return ret;
}

#else

int pinos_rtkit_make_realtime(PinosRTKitBus *connection, pid_t thread, int priority) {
        return -ENOTSUP;
}

int pinos_rtkit_make_high_priority(PinosRTKitBus *connection, pid_t thread, int nice_level) {
        return -ENOTSUP;
}

int pinos_rtkit_get_max_realtime_priority(PinosRTKitBus *connection) {
        return -ENOTSUP;
}

int pinos_rtkit_get_min_nice_level(PinosRTKitBus *connection, int* min_nice_level) {
        return -ENOTSUP;
}

long long pinos_rtkit_get_rttime_usec_max(PinosRTKitBus *connection) {
        return -ENOTSUP;
}

#endif
