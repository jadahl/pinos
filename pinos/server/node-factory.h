/* Pinos
 * Copyright (C) 2016 Axis Communications AB
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

#ifndef __PINOS_NODE_FACTORY_H__
#define __PINOS_NODE_FACTORY_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_TYPE__NodeFactory                            "Pinos:Object:NodeFactory"
#define PINOS_TYPE_NODE_FACTORY_BASE                       PINOS_TYPE__NodeFactory ":"

typedef struct _PinosNodeFactory PinosNodeFactory;

#include <pinos/server/core.h>
#include <pinos/server/client.h>

/**
 * PinosNodeFactory:
 *
 * Pinos node factory interface.
 */
struct _PinosNodeFactory {
  PinosCore   *core;
  SpaList      link;
  PinosGlobal *global;

  const char *name;

  PinosNode *      (*create_node) (PinosNodeFactory *factory,
                                   PinosClient      *client,
                                   const char       *name,
                                   PinosProperties  *properties);
};

#define pinos_node_factory_create_node(f,...)          (f)->create_node((f),__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_NODE_FACTORY_H__ */
