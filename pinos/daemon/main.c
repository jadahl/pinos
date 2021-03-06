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

#include <pinos/client/pinos.h>
#include <pinos/server/core.h>
#include <pinos/server/module.h>

#include "daemon-config.h"

int
main (int argc, char *argv[])
{
  PinosCore *core;
  PinosMainLoop *loop;
  PinosDaemonConfig *config;
  char *err = NULL;

  pinos_init (&argc, &argv);

  /* parse configuration */
  config = pinos_daemon_config_new ();
  if (!pinos_daemon_config_load (config, &err)) {
    pinos_log_error ("failed to parse config: %s", err);
    free (err);
    return -1;
  }

  loop = pinos_main_loop_new ();
#if 1
  {
    SpaSource *source;
    source = pinos_loop_add_event (loop->loop, NULL, NULL);
    pinos_log_set_trace_event (source);
  }
#endif

  core = pinos_core_new (loop, NULL);

  pinos_daemon_config_run_commands (config, core);

  pinos_main_loop_run (loop);

  pinos_main_loop_destroy (loop);

  pinos_core_destroy (core);

  return 0;
}
