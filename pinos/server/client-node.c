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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>


#include "pinos/client/pinos.h"
#include "pinos/client/interfaces.h"
#include "pinos/client/transport.h"

#include "pinos/server/core.h"
#include "pinos/server/client-node.h"

#include "spa/node.h"
#include "spa/format-builder.h"

#define MAX_INPUTS       64
#define MAX_OUTPUTS      64

#define MAX_BUFFERS      64

#define CHECK_IN_PORT_ID(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) < MAX_INPUTS)
#define CHECK_OUT_PORT_ID(this,d,p)      ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_OUTPUTS)
#define CHECK_PORT_ID(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) || CHECK_OUT_PORT_ID(this,d,p))
#define CHECK_FREE_IN_PORT(this,d,p)     (CHECK_IN_PORT_ID(this,d,p) && !(this)->in_ports[p].valid)
#define CHECK_FREE_OUT_PORT(this,d,p)    (CHECK_OUT_PORT_ID(this,d,p) && !(this)->out_ports[p].valid)
#define CHECK_FREE_PORT(this,d,p)        (CHECK_FREE_IN_PORT (this,d,p) || CHECK_FREE_OUT_PORT (this,d,p))
#define CHECK_IN_PORT(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) && (this)->in_ports[p].valid)
#define CHECK_OUT_PORT(this,d,p)         (CHECK_OUT_PORT_ID(this,d,p) && (this)->out_ports[p].valid)
#define CHECK_PORT(this,d,p)             (CHECK_IN_PORT (this,d,p) || CHECK_OUT_PORT (this,d,p))

#define CHECK_PORT_BUFFER(this,b,p)      (b < p->n_buffers)

typedef struct _SpaProxy SpaProxy;
typedef struct _ProxyBuffer ProxyBuffer;
typedef struct _PinosClientNodeImpl PinosClientNodeImpl;

struct _ProxyBuffer {
  SpaBuffer   *outbuf;
  SpaBuffer    buffer;
  SpaMeta      metas[4];
  SpaData      datas[4];
  off_t        offset;
  size_t       size;
  bool         outstanding;
};

typedef struct {
  bool           valid;
  SpaPortInfo    info;
  SpaFormat     *format;
  uint32_t       n_formats;
  SpaFormat    **formats;
  SpaPortIO     *io;

  uint32_t       n_buffers;
  ProxyBuffer    buffers[MAX_BUFFERS];

  uint32_t       buffer_mem_id;
  PinosMemblock  buffer_mem;
} SpaProxyPort;

struct _SpaProxy
{
  SpaNode    node;

  PinosClientNodeImpl *impl;

  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop *main_loop;
  SpaLoop *data_loop;

  SpaNodeCallbacks callbacks;
  void *user_data;

  PinosResource *resource;

  SpaSource data_source;
  int writefd;

  uint32_t     max_inputs;
  uint32_t     n_inputs;
  uint32_t     max_outputs;
  uint32_t     n_outputs;
  SpaProxyPort in_ports[MAX_INPUTS];
  SpaProxyPort out_ports[MAX_OUTPUTS];

  uint8_t format_buffer[1024];
  uint32_t seq;
};

struct _PinosClientNodeImpl
{
  PinosClientNode this;

  PinosCore *core;

  SpaProxy proxy;

  PinosTransport *transport;

  PinosListener node_free;
  PinosListener initialized;
  PinosListener loop_changed;
  PinosListener global_added;

  int fds[2];
  int other_fds[2];
};

static SpaResult
clear_buffers (SpaProxy *this, SpaProxyPort *port)
{
  if (port->n_buffers) {
    spa_log_info (this->log, "proxy %p: clear buffers", this);

    pinos_memblock_free (&port->buffer_mem);

    port->n_buffers = 0;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_get_props (SpaNode       *node,
                          SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_set_props (SpaNode         *node,
                          const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static inline void
do_flush (SpaProxy *this)
{
  uint64_t cmd = 1;
  write (this->writefd, &cmd, 8);
}

static inline void
send_need_input (SpaProxy *this)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (this, PinosClientNodeImpl, proxy);
  SpaEvent event = SPA_EVENT_INIT (impl->core->type.event_transport.NeedInput);

  pinos_transport_add_event (impl->transport, &event);
  do_flush (this);
}

static inline void
send_have_output (SpaProxy *this)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (this, PinosClientNodeImpl, proxy);
  SpaEvent event = SPA_EVENT_INIT (impl->core->type.event_transport.HaveOutput);

  pinos_transport_add_event (impl->transport, &event);
  do_flush (this);
}

static SpaResult
spa_proxy_node_send_command (SpaNode    *node,
                             SpaCommand *command)
{
  SpaProxy *this;
  SpaResult res = SPA_RESULT_OK;
  PinosCore *core;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (this->resource == NULL)
    return SPA_RESULT_OK;

  core = this->impl->core;

  if (SPA_COMMAND_TYPE (command) == core->type.command_node.ClockUpdate) {
    pinos_client_node_notify_node_command (this->resource,
                                           this->seq++,
                                           command);
  }
  else {
    /* send start */
    pinos_client_node_notify_node_command (this->resource,
                                           this->seq,
                                           command);
    if (SPA_COMMAND_TYPE (command) == core->type.command_node.Start)
      send_need_input (this);

    res = SPA_RESULT_RETURN_ASYNC (this->seq++);
  }
  return res;
}

static SpaResult
spa_proxy_node_set_callbacks (SpaNode                *node,
                              const SpaNodeCallbacks *callbacks,
                              size_t                  callbacks_size,
                              void                   *user_data)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  this->callbacks = *callbacks;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_get_n_ports (SpaNode       *node,
                            uint32_t      *n_input_ports,
                            uint32_t      *max_input_ports,
                            uint32_t      *n_output_ports,
                            uint32_t      *max_output_ports)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (n_input_ports)
    *n_input_ports = this->n_inputs;
  if (max_input_ports)
    *max_input_ports = this->max_inputs;
  if (n_output_ports)
    *n_output_ports = this->n_outputs;
  if (max_output_ports)
    *max_output_ports = this->max_outputs;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_get_port_ids (SpaNode       *node,
                             uint32_t       n_input_ports,
                             uint32_t      *input_ids,
                             uint32_t       n_output_ports,
                             uint32_t      *output_ids)
{
  SpaProxy *this;
  int c, i;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (input_ids) {
    for (c = 0, i = 0; i < MAX_INPUTS && c < n_input_ports; i++) {
      if (this->in_ports[i].valid)
        input_ids[c++] = i;
    }
  }
  if (output_ids) {
    for (c = 0, i = 0; i < MAX_OUTPUTS && c < n_output_ports; i++) {
      if (this->out_ports[i].valid)
        output_ids[c++] = i;
    }
  }
  return SPA_RESULT_OK;
}

static void
do_update_port (SpaProxy          *this,
                SpaDirection       direction,
                uint32_t           port_id,
                uint32_t           change_mask,
                uint32_t           n_possible_formats,
                const SpaFormat  **possible_formats,
                const SpaFormat   *format,
                const SpaProps    *props,
                const SpaPortInfo *info)
{
  SpaProxyPort *port;
  uint32_t i;

  if (direction == SPA_DIRECTION_INPUT) {
    port = &this->in_ports[port_id];
  } else {
    port = &this->out_ports[port_id];
  }

  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS) {
    for (i = 0; i < port->n_formats; i++)
      free (port->formats[i]);
    port->n_formats = n_possible_formats;
    port->formats = realloc (port->formats, port->n_formats * sizeof (SpaFormat *));
    for (i = 0; i < port->n_formats; i++)
      port->formats[i] = spa_format_copy (possible_formats[i]);
  }
  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_FORMAT) {
    if (port->format)
      free (port->format);
    port->format = spa_format_copy (format);
  }

  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_PROPS) {
  }

  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_INFO && info) {
    void *old;
    for (i = 0; i < port->info.n_params; i++)
      free (port->info.params[i]);
    old = port->info.params;
    port->info = *info;
    port->info.params = realloc (old, port->info.n_params * sizeof (SpaAllocParam *));
    for (i = 0; i < port->info.n_params; i++)
      port->info.params[i] = spa_alloc_param_copy (info->params[i]);
    port->info.extra = NULL;
  }

  if (!port->valid) {
    spa_log_info (this->log, "proxy %p: adding port %d", this, port_id);
    port->format = NULL;
    port->valid = true;

    if (direction == SPA_DIRECTION_INPUT)
      this->n_inputs++;
    else
      this->n_outputs++;
  }
}

static void
clear_port (SpaProxy     *this,
            SpaProxyPort *port,
            SpaDirection  direction,
            uint32_t      port_id)
{
  do_update_port (this,
                  direction,
                  port_id,
                  PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS |
                  PINOS_MESSAGE_PORT_UPDATE_FORMAT |
                  PINOS_MESSAGE_PORT_UPDATE_PROPS |
                  PINOS_MESSAGE_PORT_UPDATE_INFO,
                  0,
                  NULL,
                  NULL,
                  NULL,
                  NULL);
  clear_buffers (this, port);
}

static void
do_uninit_port (SpaProxy     *this,
                SpaDirection  direction,
                uint32_t      port_id)
{
  SpaProxyPort *port;

  spa_log_info (this->log, "proxy %p: removing port %d", this, port_id);
  if (direction == SPA_DIRECTION_INPUT) {
    port = &this->in_ports[port_id];
    this->n_inputs--;
  } else {
    port = &this->out_ports[port_id];
    this->n_outputs--;
  }
  clear_port (this, port, direction, port_id);
  port->valid = false;
}

static SpaResult
spa_proxy_node_add_port (SpaNode        *node,
                         SpaDirection    direction,
                         uint32_t        port_id)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_FREE_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  clear_port (this, port, direction, port_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_remove_port (SpaNode        *node,
                            SpaDirection    direction,
                            uint32_t        port_id)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  do_uninit_port (this, direction, port_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_enum_formats (SpaNode          *node,
                                  SpaDirection      direction,
                                  uint32_t          port_id,
                                  SpaFormat       **format,
                                  const SpaFormat  *filter,
                                  uint32_t          index)
{
  SpaProxy *this;
  SpaProxyPort *port;
  SpaFormat *fmt;
  SpaPODBuilder b = { NULL, };
  SpaResult res;
  uint32_t count, match = 0;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  count = match = filter ? 0 : index;

next:
  if (count >= port->n_formats)
    return SPA_RESULT_ENUM_END;

  fmt = port->formats[count++];

  spa_pod_builder_init (&b, this->format_buffer, sizeof (this->format_buffer));

  if ((res = spa_format_filter (fmt, filter, &b)) != SPA_RESULT_OK || match++ != index)
    goto next;

  *format = SPA_POD_BUILDER_DEREF (&b, 0, SpaFormat);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_set_format (SpaNode         *node,
                                SpaDirection     direction,
                                uint32_t         port_id,
                                uint32_t         flags,
                                const SpaFormat *format)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (this->resource == NULL)
    return SPA_RESULT_OK;

  pinos_client_node_notify_set_format (this->resource,
                                       this->seq,
                                       direction,
                                       port_id,
                                       flags,
                                       format);

  return SPA_RESULT_RETURN_ASYNC (this->seq++);
}

static SpaResult
spa_proxy_node_port_get_format (SpaNode          *node,
                                SpaDirection      direction,
                                uint32_t          port_id,
                                const SpaFormat **format)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  *format = port->format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_get_info (SpaNode            *node,
                              SpaDirection        direction,
                              uint32_t            port_id,
                              const SpaPortInfo **info)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_get_props (SpaNode        *node,
                               SpaDirection    direction,
                               uint32_t        port_id,
                               SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_set_props (SpaNode        *node,
                               SpaDirection    direction,
                               uint32_t        port_id,
                               const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_set_io (SpaNode      *node,
                            SpaDirection  direction,
                            uint32_t      port_id,
                            SpaPortIO    *io)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  port->io = io;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_use_buffers (SpaNode         *node,
                                 SpaDirection     direction,
                                 uint32_t         port_id,
                                 SpaBuffer      **buffers,
                                 uint32_t         n_buffers)
{
  SpaProxy *this;
  PinosClientNodeImpl *impl;
  SpaProxyPort *port;
  uint32_t i, j;
  size_t n_mem;
  PinosClientNodeBuffer *mb;
  SpaMetaShared *msh;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  impl = this->impl;
  spa_log_info (this->log, "proxy %p: use buffers %p %u", this, buffers, n_buffers);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this, port);

  if (n_buffers > 0) {
    mb = alloca (n_buffers * sizeof (PinosClientNodeBuffer));
  } else {
    mb = NULL;
  }

  port->n_buffers = n_buffers;

  if (this->resource == NULL)
    return SPA_RESULT_OK;

  n_mem = 0;
  for (i = 0; i < n_buffers; i++) {
    ProxyBuffer *b = &port->buffers[i];

    msh = spa_buffer_find_meta (buffers[i], impl->core->type.meta.Shared);
    if (msh == NULL) {
      spa_log_error (this->log, "missing shared metadata on buffer %d", i);
      return SPA_RESULT_ERROR;
    }

    b->outbuf = buffers[i];
    memcpy (&b->buffer, buffers[i], sizeof (SpaBuffer));
    b->buffer.datas = b->datas;
    b->buffer.metas = b->metas;

    mb[i].buffer = &b->buffer;
    mb[i].mem_id = n_mem++;
    mb[i].offset = 0;
    mb[i].size = msh->size;

    pinos_client_node_notify_add_mem (this->resource,
                                      direction,
                                      port_id,
                                      mb[i].mem_id,
                                      impl->core->type.data.MemFd,
                                      msh->fd,
                                      msh->flags,
                                      msh->offset,
                                      msh->size);

    for (j = 0; j < buffers[i]->n_metas; j++) {
      memcpy (&b->buffer.metas[j], &buffers[i]->metas[j], sizeof (SpaMeta));
    }

    for (j = 0; j < buffers[i]->n_datas; j++) {
      SpaData *d = &buffers[i]->datas[j];

      memcpy (&b->buffer.datas[j], d, sizeof (SpaData));

      if (d->type == impl->core->type.data.DmaBuf ||
          d->type == impl->core->type.data.MemFd) {
        pinos_client_node_notify_add_mem (this->resource,
                                          direction,
                                          port_id,
                                          n_mem,
                                          d->type,
                                          d->fd,
                                          d->flags,
                                          d->mapoffset,
                                          d->maxsize);
        b->buffer.datas[j].type = impl->core->type.data.Id;
        b->buffer.datas[j].data = SPA_UINT32_TO_PTR (n_mem);
        n_mem++;
      }
      else if (d->type == impl->core->type.data.MemPtr) {
        b->buffer.datas[j].data = SPA_INT_TO_PTR (b->size);
        b->size += d->maxsize;
      }
      else {
        b->buffer.datas[j].type = SPA_ID_INVALID;
        b->buffer.datas[j].data = 0;
        spa_log_error (this->log, "invalid memory type %d", d->type);
      }
    }
  }

  pinos_client_node_notify_use_buffers (this->resource,
                                        this->seq,
                                        direction,
                                        port_id,
                                        n_buffers,
                                        mb);

  return SPA_RESULT_RETURN_ASYNC (this->seq++);
}

static SpaResult
spa_proxy_node_port_alloc_buffers (SpaNode         *node,
                                   SpaDirection     direction,
                                   uint32_t         port_id,
                                   SpaAllocParam  **params,
                                   uint32_t         n_params,
                                   SpaBuffer      **buffers,
                                   uint32_t        *n_buffers)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_reuse_buffer (SpaNode         *node,
                                  uint32_t         port_id,
                                  uint32_t         buffer_id)
{
  SpaProxy *this;
  PinosClientNodeImpl *impl;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  impl = this->impl;

  if (!CHECK_OUT_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  spa_log_trace (this->log, "reuse buffer %d", buffer_id);
  {
    PinosEventTransportReuseBuffer rb = PINOS_EVENT_TRANSPORT_REUSE_BUFFER_INIT
                                (impl->core->type.event_transport.ReuseBuffer, port_id, buffer_id);
    pinos_transport_add_event (impl->transport, (SpaEvent *)&rb);
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_send_command (SpaNode        *node,
                                  SpaDirection    direction,
                                  uint32_t        port_id,
                                  SpaCommand     *command)
{
  SpaProxy *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  spa_log_warn (this->log, "unhandled command %d", SPA_COMMAND_TYPE (command));
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_process_input (SpaNode *node)
{
  PinosClientNodeImpl *impl;
  SpaProxy *this;
  int i;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  impl = this->impl;

  for (i = 0; i < MAX_INPUTS; i++) {
    SpaPortIO *io = this->in_ports[i].io;

    if (!io)
      continue;

    pinos_log_trace ("%d %d", io->status, io->buffer_id);

    impl->transport->inputs[i] = *io;
    io->status = SPA_RESULT_OK;
  }
  send_have_output (this);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_process_output (SpaNode *node)
{
  SpaProxy *this;
  PinosClientNodeImpl *impl;
  int i;
  bool send_need = false, flush = false;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  impl = this->impl;

  for (i = 0; i < MAX_OUTPUTS; i++) {
    SpaPortIO *io = this->out_ports[i].io, tmp;

    if (!io)
      continue;

    if (io->buffer_id != SPA_ID_INVALID) {
      PinosEventTransportReuseBuffer rb =
        PINOS_EVENT_TRANSPORT_REUSE_BUFFER_INIT (impl->core->type.event_transport.ReuseBuffer, i, io->buffer_id);

      spa_log_trace (this->log, "reuse buffer %d", io->buffer_id);

      pinos_transport_add_event (impl->transport, (SpaEvent *)&rb);
      io->buffer_id = SPA_ID_INVALID;
      flush = true;
    }

    tmp = impl->transport->outputs[i];
    impl->transport->outputs[i] = *io;

    pinos_log_trace ("%d %d  %d %d", io->status, io->buffer_id, tmp.status, tmp.buffer_id);

    if (io->status == SPA_RESULT_NEED_BUFFER)
      send_need = true;

    *io = tmp;
  }
  if (send_need)
    send_need_input (this);
  else if (flush)
    do_flush (this);

  return SPA_RESULT_HAVE_BUFFER;
}

static SpaResult
handle_node_event (SpaProxy *this,
                   SpaEvent *event)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (this, PinosClientNodeImpl, proxy);
  int i;

  if (SPA_EVENT_TYPE (event) == impl->core->type.event_transport.HaveOutput) {
    for (i = 0; i < MAX_OUTPUTS; i++) {
      SpaPortIO *io = this->out_ports[i].io;

      if (!io)
        continue;

      *io = impl->transport->outputs[i];
      pinos_log_trace ("%d %d", io->status, io->buffer_id);
    }
    this->callbacks.have_output (&this->node, this->user_data);
  }
  else if (SPA_EVENT_TYPE (event) == impl->core->type.event_transport.NeedInput) {
    this->callbacks.need_input (&this->node, this->user_data);
  }
  else if (SPA_EVENT_TYPE (event) == impl->core->type.event_transport.ReuseBuffer) {
    PinosEventTransportReuseBuffer *p = (PinosEventTransportReuseBuffer *) event;
    this->callbacks.reuse_buffer (&this->node, p->body.port_id.value, p->body.buffer_id.value, this->user_data);
  }
  return SPA_RESULT_OK;
}

static void
client_node_update (void           *object,
                    uint32_t        change_mask,
                    uint32_t        max_input_ports,
                    uint32_t        max_output_ports,
                    const SpaProps *props)
{
  PinosResource *resource = object;
  PinosClientNode *node = resource->object;
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (node, PinosClientNodeImpl, this);
  SpaProxy *this = &impl->proxy;

  if (change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS)
    this->max_inputs = max_input_ports;
  if (change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS)
    this->max_outputs = max_output_ports;

  spa_log_info (this->log, "proxy %p: got node update max_in %u, max_out %u", this,
      this->max_inputs, this->max_outputs);
}

static void
client_node_port_update (void              *object,
                         SpaDirection       direction,
                         uint32_t           port_id,
                         uint32_t           change_mask,
                         uint32_t           n_possible_formats,
                         const SpaFormat  **possible_formats,
                         const SpaFormat   *format,
                         const SpaProps    *props,
                         const SpaPortInfo *info)
{
  PinosResource *resource = object;
  PinosClientNode *node = resource->object;
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (node, PinosClientNodeImpl, this);
  SpaProxy *this = &impl->proxy;
  bool remove;

  spa_log_info (this->log, "proxy %p: got port update", this);
  if (!CHECK_PORT_ID (this, direction, port_id))
    return;

  remove = (change_mask == 0);

  if (remove) {
    do_uninit_port (this, direction, port_id);
  } else {
    do_update_port (this,
                    direction,
                    port_id,
                    change_mask,
                    n_possible_formats,
                    possible_formats,
                    format,
                    props,
                    info);
  }
}

static void
client_node_event (void     *object,
                   SpaEvent *event)
{
  PinosResource *resource = object;
  PinosClientNode *node = resource->object;
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (node, PinosClientNodeImpl, this);
  SpaProxy *this = &impl->proxy;

  this->callbacks.event (&this->node, event, this->user_data);
}

static void
client_node_destroy (void              *object)
{
  PinosResource *resource = object;
  PinosClientNode *node = resource->object;
  pinos_client_node_destroy (node);
}

static PinosClientNodeMethods client_node_methods = {
  &client_node_update,
  &client_node_port_update,
  &client_node_event,
  &client_node_destroy,
};

static void
proxy_on_data_fd_events (SpaSource *source)
{
  SpaProxy *this = source->data;
  PinosClientNodeImpl *impl = this->impl;

  if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
    spa_log_warn (this->log, "proxy %p: got error", this);
    return;
  }

  if (source->rmask & SPA_IO_IN) {
    SpaEvent event;
    uint64_t cmd;

    read (this->data_source.fd, &cmd, 8);

    while (pinos_transport_next_event (impl->transport, &event) == SPA_RESULT_OK) {
      SpaEvent *ev = alloca (SPA_POD_SIZE (&event));
      pinos_transport_parse_event (impl->transport, ev);
      handle_node_event (this, ev);
    }
  }
}

static const SpaNode proxy_node = {
  sizeof (SpaNode),
  NULL,
  spa_proxy_node_get_props,
  spa_proxy_node_set_props,
  spa_proxy_node_send_command,
  spa_proxy_node_set_callbacks,
  spa_proxy_node_get_n_ports,
  spa_proxy_node_get_port_ids,
  spa_proxy_node_add_port,
  spa_proxy_node_remove_port,
  spa_proxy_node_port_enum_formats,
  spa_proxy_node_port_set_format,
  spa_proxy_node_port_get_format,
  spa_proxy_node_port_get_info,
  spa_proxy_node_port_get_props,
  spa_proxy_node_port_set_props,
  spa_proxy_node_port_use_buffers,
  spa_proxy_node_port_alloc_buffers,
  spa_proxy_node_port_set_io,
  spa_proxy_node_port_reuse_buffer,
  spa_proxy_node_port_send_command,
  spa_proxy_node_process_input,
  spa_proxy_node_process_output,
};

static SpaResult
proxy_init (SpaProxy         *this,
            SpaDict          *info,
            const SpaSupport *support,
            uint32_t          n_support)
{
  uint32_t i;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
      this->main_loop = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
      this->data_loop = support[i].data;
  }
  if (this->data_loop == NULL) {
    spa_log_error (this->log, "a main-loop is needed");
  }
  if (this->data_loop == NULL) {
    spa_log_error (this->log, "a data-loop is needed");
  }

  this->node = proxy_node;

  this->data_source.func = proxy_on_data_fd_events;
  this->data_source.data = this;
  this->data_source.fd = -1;
  this->data_source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
  this->data_source.rmask = 0;

  return SPA_RESULT_RETURN_ASYNC (this->seq++);
}

static void
on_initialized (PinosListener   *listener,
                PinosNode       *node)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (listener, PinosClientNodeImpl, initialized);
  PinosClientNode *this = &impl->this;
  PinosTransportInfo info;

  if (this->resource == NULL)
    return;

  impl->transport = pinos_transport_new (node->max_input_ports,
                                         node->max_output_ports);
  impl->transport->area->n_inputs = node->n_input_ports;
  impl->transport->area->n_outputs = node->n_output_ports;

  pinos_transport_get_info (impl->transport, &info);
  pinos_client_node_notify_transport (this->resource,
                                      info.memfd,
                                      info.offset,
                                      info.size);
}

static void
on_loop_changed (PinosListener   *listener,
                 PinosNode       *node)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (listener, PinosClientNodeImpl, loop_changed);
  impl->proxy.data_loop = node->data_loop->loop->loop;
}

static void
on_global_added (PinosListener   *listener,
                 PinosCore       *core,
                 PinosGlobal     *global)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (listener, PinosClientNodeImpl, global_added);

  if (global->object == impl->this.node)
    global->owner = impl->this.client;
}

static SpaResult
proxy_clear (SpaProxy *this)
{
  uint32_t i;

  for (i = 0; i < MAX_INPUTS; i++) {
    if (this->in_ports[i].valid)
      clear_port (this, &this->in_ports[i], SPA_DIRECTION_INPUT, i);
  }
  for (i = 0; i < MAX_OUTPUTS; i++) {
    if (this->out_ports[i].valid)
      clear_port (this, &this->out_ports[i], SPA_DIRECTION_OUTPUT, i);
  }

  return SPA_RESULT_OK;
}

static void
client_node_resource_destroy (PinosResource *resource)
{
  PinosClientNode *this = resource->object;
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (this, PinosClientNodeImpl, this);
  SpaProxy *proxy = &impl->proxy;

  pinos_log_debug ("client-node %p: destroy", impl);
  pinos_signal_emit (&this->destroy_signal, this);

  impl->proxy.resource = this->resource = NULL;

  pinos_signal_remove (&impl->global_added);
  pinos_signal_remove (&impl->loop_changed);
  pinos_signal_remove (&impl->initialized);

  if (proxy->data_source.fd != -1)
    spa_loop_remove_source (proxy->data_loop, &proxy->data_source);

  pinos_node_destroy (this->node);
}

static void
on_node_free (PinosListener *listener,
              PinosNode     *node)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (listener, PinosClientNodeImpl, node_free);

  pinos_log_debug ("client-node %p: free", &impl->this);
  proxy_clear (&impl->proxy);

  pinos_signal_remove (&impl->node_free);

  if (impl->transport)
    pinos_transport_destroy (impl->transport);

  if (impl->fds[0] != -1)
    close (impl->fds[0]);
  if (impl->fds[1] != -1)
    close (impl->fds[1]);
  free (impl);
}

/**
 * pinos_client_node_new:
 * @daemon: a #PinosDaemon
 * @name: a name
 * @properties: extra properties
 *
 * Create a new #PinosNode.
 *
 * Returns: a new #PinosNode
 */
PinosClientNode *
pinos_client_node_new (PinosClient     *client,
                       uint32_t         id,
                       const char      *name,
                       PinosProperties *properties)
{
  PinosClientNodeImpl *impl;
  PinosClientNode *this;

  impl = calloc (1, sizeof (PinosClientNodeImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  this->client = client;

  impl->core = client->core;
  impl->fds[0] = impl->fds[1] = -1;
  pinos_log_debug ("client-node %p: new", impl);

  pinos_signal_init (&this->destroy_signal);

  proxy_init (&impl->proxy, NULL, client->core->support, client->core->n_support);

  this->node = pinos_node_new (client->core,
                               client,
                               name,
                               true,
                               &impl->proxy.node,
                               NULL,
                               properties);
  if (this->node == NULL)
    goto error_no_node;

  impl->proxy.impl = impl;

  this->resource = pinos_resource_new (client,
                                       id,
                                       client->core->type.client_node,
                                       this,
                                       (PinosDestroy) client_node_resource_destroy);
  if (this->resource == NULL)
    goto error_no_resource;

  impl->proxy.resource = this->resource;

  pinos_signal_add (&this->node->free_signal,
                    &impl->node_free,
                    on_node_free);

  pinos_signal_add (&this->node->initialized,
                    &impl->initialized,
                    on_initialized);

  pinos_signal_add (&this->node->loop_changed,
                    &impl->loop_changed,
                    on_loop_changed);

  pinos_signal_add (&impl->core->global_added,
                    &impl->global_added,
                    on_global_added);

  this->resource->implementation = &client_node_methods;

  return this;

error_no_resource:
  pinos_node_destroy (this->node);
error_no_node:
  proxy_clear (&impl->proxy);
  free (impl);
  return NULL;
}

void
pinos_client_node_destroy (PinosClientNode * this)
{
  pinos_resource_destroy (this->resource);
}

/**
 * pinos_client_node_get_fds:
 * @node: a #PinosClientNode
 * @readfd: an fd for reading
 * @writefd: an fd for writing
 *
 * Create or return a previously create set of fds for @node.
 *
 * Returns: %SPA_RESULT_OK on success
 */
SpaResult
pinos_client_node_get_fds (PinosClientNode  *this,
                           int              *readfd,
                           int              *writefd)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (this, PinosClientNodeImpl, this);

  if (impl->fds[0] == -1) {
#if 0
    if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, impl->fds) != 0)
      return SPA_RESULT_ERRNO;

    impl->proxy.data_source.fd = impl->fds[0];
    impl->proxy.writefd = impl->fds[0];
    impl->other_fds[0] = impl->fds[1];
    impl->other_fds[1] = impl->fds[1];
#else
    impl->fds[0] = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
    impl->fds[1] = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
    impl->proxy.data_source.fd = impl->fds[0];
    impl->proxy.writefd = impl->fds[1];
    impl->other_fds[0] = impl->fds[1];
    impl->other_fds[1] = impl->fds[0];
#endif

    spa_loop_add_source (impl->proxy.data_loop, &impl->proxy.data_source);
    pinos_log_debug ("client-node %p: add data fd %d", this, impl->proxy.data_source.fd);
  }
  *readfd = impl->other_fds[0];
  *writefd = impl->other_fds[1];

  return SPA_RESULT_OK;
}
