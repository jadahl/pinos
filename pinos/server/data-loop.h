/* Pinos
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PINOS_DATA_LOOP_H__
#define __PINOS_DATA_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pinos/client/loop.h>

typedef struct _PinosDataLoop PinosDataLoop;

/**
 * PinosDataLoop:
 *
 * Pinos rt-loop object.
 */
struct _PinosDataLoop {
  PinosLoop *loop;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosDataLoop *loop));
};

PinosDataLoop *     pinos_data_loop_new              (void);
void                pinos_data_loop_destroy          (PinosDataLoop *loop);

SpaResult           pinos_data_loop_start            (PinosDataLoop *loop);
SpaResult           pinos_data_loop_stop             (PinosDataLoop *loop);

bool                pinos_data_loop_in_thread        (PinosDataLoop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_DATA_LOOP_H__ */
