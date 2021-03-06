/* Pinos
 * Copyright (C) 2016 Axis Communications <dev-gstreamer@axis.com>
 * @author Linus Svensson <linus.svensson@axis.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <errno.h>

#include <pinos/client/pinos.h>
#include <pinos/server/command.h>

#include "pinos/daemon/daemon-config.h"

#define DEFAULT_CONFIG_FILE PINOS_CONFIG_DIR "/pinos.conf"

static bool
parse_line (PinosDaemonConfig  *config,
            const char         *filename,
            char               *line,
            unsigned int        lineno,
            char              **err)
{
  PinosCommand *command = NULL;
  char *p;
  bool ret = true;
  char *local_err = NULL;

  /* search for comments */
  if ((p = strchr (line, '#')))
    *p = '\0';

  /* remove whitespaces */
  pinos_strip (line, "\n\r \t");

  if (*line == '\0') /* empty line */
    return true;

  if ((command = pinos_command_parse (line, &local_err)) == NULL) {
    asprintf (err, "%s:%u: %s", filename, lineno, local_err);
    free (local_err);
    ret = false;
  } else {
    spa_list_insert (config->commands.prev, &command->link);
  }

  return ret;
}

/**
 * pinos_daemon_config_new:
 *
 * Returns a new empty #PinosDaemonConfig.
 */
PinosDaemonConfig *
pinos_daemon_config_new (void)
{
  PinosDaemonConfig *config;

  config = calloc (1, sizeof (PinosDaemonConfig));
  spa_list_init (&config->commands);

  return config;
}

/**
 * pinos_daemon_config_free:
 * @config: A #PinosDaemonConfig
 *
 * Free all resources associated to @config.
 */
void
pinos_daemon_config_free (PinosDaemonConfig * config)
{
  PinosCommand *cmd, *tmp;

  spa_list_for_each_safe (cmd, tmp, &config->commands, link)
    pinos_command_free (cmd);

  free (config);
}

/**
 * pinos_daemon_config_load_file:
 * @config: A #PinosDaemonConfig
 * @filename: A filename
 * @err: Return location for an error string
 *
 * Loads pinos config from @filename.
 *
 * Returns: %true on success, otherwise %false and @err is set.
 */
bool
pinos_daemon_config_load_file (PinosDaemonConfig *config,
                               const char        *filename,
                               char             **err)
{
  unsigned int line;
  FILE *f;
  char buf[4096];

  pinos_log_debug ("deamon-config %p: loading configuration file '%s'", config, filename);

  if ((f = fopen (filename, "r")) == NULL) {
    asprintf (err, "failed to open configuration file '%s': %s", filename, strerror (errno));
    goto open_error;
  }

  line = 0;

  while (!feof(f)) {
    if (!fgets(buf, sizeof (buf), f)) {
      if (feof(f))
        break;

      asprintf (err, "failed to read configuration file '%s': %s", filename, strerror (errno));
      goto read_error;
    }

    line++;

    if (!parse_line (config, filename, buf, line, err))
      goto parse_failed;
  }
  fclose (f);

  return true;

parse_failed:
read_error:
  fclose (f);
open_error:
  return false;
}

/**
 * pinos_daemon_config_load:
 * @config: A #PinosDaemonConfig
 * @err: Return location for a #GError, or %NULL
 *
 * Loads the default config file for pinos. The filename can be overridden with
 * an evironment variable PINOS_CONFIG_FILE.
 *
 * Return: %true on success, otherwise %false and @err is set.
 */
bool
pinos_daemon_config_load (PinosDaemonConfig  *config,
                          char              **err)
{
  const char *filename;

  filename = getenv ("PINOS_CONFIG_FILE");
  if (filename != NULL && *filename != '\0') {
    pinos_log_debug ("PINOS_CONFIG_FILE set to: %s", filename);
  } else {
    filename = DEFAULT_CONFIG_FILE;
  }
  return pinos_daemon_config_load_file (config, filename, err);
}

/**
 * pinos_daemon_config_run_commands:
 * @config: A #PinosDaemonConfig
 * @core: A #PinosCore
 *
 * Run all commands that have been parsed. The list of commands will be cleared
 * when this function has been called.
 *
 * Returns: %true if all commands where executed with success, otherwise %false.
 */
bool
pinos_daemon_config_run_commands (PinosDaemonConfig  *config,
                                  PinosCore          *core)
{
  char *err = NULL;
  bool ret = true;
  PinosCommand *command, *tmp;

  spa_list_for_each (command, &config->commands, link) {
    if (!pinos_command_run (command, core, &err)) {
      pinos_log_warn ("could not run command %s: %s", command->name, err);
      free (err);
      ret = false;
    }
  }

  spa_list_for_each_safe (command, tmp, &config->commands, link) {
    pinos_command_free (command);
  }

  return ret;
}
