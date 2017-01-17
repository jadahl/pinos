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
#include "pinos/client/connection.h"
#include "pinos/client/serialize.h"
#include "pinos/client/transport.h"

#include "pinos/server/core.h"
#include "pinos/server/client-node.h"

#include "spa/include/spa/node.h"

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
  SpaPortInfo   *info;
  SpaFormat     *format;
  unsigned int   n_formats;
  SpaFormat    **formats;
  void          *io;

  unsigned int   n_buffers;
  ProxyBuffer    buffers[MAX_BUFFERS];

  uint32_t       buffer_mem_id;
  PinosMemblock  buffer_mem;
} SpaProxyPort;

struct _SpaProxy
{
  SpaNode    node;

  PinosNode *pnode;

  SpaIDMap *map;
  SpaLog *log;
  SpaLoop *main_loop;
  SpaLoop *data_loop;

  SpaNodeEventCallback event_cb;
  void *user_data;

  PinosResource *resource;

  SpaSource data_source;

  unsigned int max_inputs;
  unsigned int n_inputs;
  unsigned int max_outputs;
  unsigned int n_outputs;
  SpaProxyPort in_ports[MAX_INPUTS];
  SpaProxyPort out_ports[MAX_OUTPUTS];

  uint32_t seq;
};

typedef struct
{
  PinosClientNode this;

  PinosCore *core;

  SpaProxy proxy;

  PinosListener node_free;
  PinosListener transport_changed;
  PinosListener loop_changed;
  PinosListener global_added;

  int data_fd;
} PinosClientNodeImpl;

static void
send_async_complete (SpaProxy *this, uint32_t seq, SpaResult res)
{
  SpaNodeEventAsyncComplete ac;

  ac.event.type = SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE;
  ac.event.size = sizeof (ac);
  ac.seq = seq;
  ac.res = res;
  this->event_cb (&this->node, &ac.event, this->user_data);
}

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

static void
send_need_input (SpaProxy *this)
{
  PinosNode *pnode = this->pnode;
  SpaNodeEventNeedInput ni;
  uint8_t cmd = 0;

  ni.event.type = SPA_NODE_EVENT_TYPE_NEED_INPUT;
  ni.event.size = sizeof (ni);
  ni.port_id = 0;
  pinos_transport_add_event (pnode->transport, &ni.event);
  write (this->data_source.fd, &cmd, 1);
}

static void
send_have_output (SpaProxy *this)
{
  PinosNode *pnode = this->pnode;
  SpaNodeEventHaveOutput ho;
  uint8_t cmd = 0;

  ho.event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
  ho.event.size = sizeof (ho);
  ho.port_id = 0;
  pinos_transport_add_event (pnode->transport, &ho.event);
  write (this->data_source.fd, &cmd, 1);
}

static SpaResult
spa_proxy_node_send_command (SpaNode        *node,
                             SpaNodeCommand *command)
{
  SpaProxy *this;
  SpaResult res = SPA_RESULT_OK;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (this->resource == NULL)
    return SPA_RESULT_OK;

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
    case SPA_NODE_COMMAND_PAUSE:
    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    {
      PinosMessageNodeCommand cnc;

      /* send start */
      cnc.seq = this->seq++;
      cnc.command = command;
      pinos_client_send_message (this->resource->client,
                                 this->resource,
                                 PINOS_MESSAGE_NODE_COMMAND,
                                 &cnc,
                                 true);
      if (command->type == SPA_NODE_COMMAND_START)
        send_need_input (this);

      res = SPA_RESULT_RETURN_ASYNC (cnc.seq);
      break;
    }

    case SPA_NODE_COMMAND_CLOCK_UPDATE:
    {
      PinosMessageNodeCommand cnc;

      /* send start */
      cnc.command = command;
      pinos_client_send_message (this->resource->client,
                                 this->resource,
                                 PINOS_MESSAGE_NODE_COMMAND,
                                 &cnc,
                                 true);
      break;
    }
  }
  return res;
}

static SpaResult
spa_proxy_node_set_event_callback (SpaNode              *node,
                                   SpaNodeEventCallback  event,
                                   void                 *user_data)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_get_n_ports (SpaNode       *node,
                            unsigned int  *n_input_ports,
                            unsigned int  *max_input_ports,
                            unsigned int  *n_output_ports,
                            unsigned int  *max_output_ports)
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
                             unsigned int   n_input_ports,
                             uint32_t      *input_ids,
                             unsigned int   n_output_ports,
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
do_update_port (SpaProxy               *this,
                PinosMessagePortUpdate *pu)
{
  SpaProxyPort *port;
  unsigned int i;
  size_t size;

  if (pu->direction == SPA_DIRECTION_INPUT) {
    port = &this->in_ports[pu->port_id];
  } else {
    port = &this->out_ports[pu->port_id];
  }

  if (pu->change_mask & PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS) {
    for (i = 0; i < port->n_formats; i++)
      free (port->formats[i]);
    port->n_formats = pu->n_possible_formats;
    if (port->n_formats)
      port->formats = realloc (port->formats, port->n_formats * sizeof (SpaFormat *));
    else {
      free (port->formats);
      port->formats = NULL;
    }
    for (i = 0; i < port->n_formats; i++) {
      size = pinos_serialize_format_get_size (pu->possible_formats[i]);
      port->formats[i] = size ? pinos_serialize_format_copy_into (malloc (size), pu->possible_formats[i]) : NULL;
    }
  }
  if (pu->change_mask & PINOS_MESSAGE_PORT_UPDATE_FORMAT) {
    if (port->format)
      free (port->format);
    size = pinos_serialize_format_get_size (pu->format);
    port->format = size ? pinos_serialize_format_copy_into (malloc (size), pu->format) : NULL;
  }

  if (pu->change_mask & PINOS_MESSAGE_PORT_UPDATE_PROPS) {
  }

  if (pu->change_mask & PINOS_MESSAGE_PORT_UPDATE_INFO && pu->info) {
    if (port->info)
      free (port->info);
    size = pinos_serialize_port_info_get_size (pu->info);
    port->info = size ? pinos_serialize_port_info_copy_into (malloc (size), pu->info) : NULL;
  }

  if (!port->valid) {
    spa_log_info (this->log, "proxy %p: adding port %d", this, pu->port_id);
    port->format = NULL;
    port->valid = true;

    if (pu->direction == SPA_DIRECTION_INPUT)
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
  PinosMessagePortUpdate pu;

  pu.change_mask = PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS |
                   PINOS_MESSAGE_PORT_UPDATE_FORMAT |
                   PINOS_MESSAGE_PORT_UPDATE_PROPS |
                   PINOS_MESSAGE_PORT_UPDATE_INFO;
  pu.direction = direction;
  pu.port_id = port_id;
  pu.n_possible_formats = 0;
  pu.possible_formats = NULL;
  pu.format = NULL;
  pu.props = NULL;
  pu.info = NULL;
  do_update_port (this, &pu);
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
                                  void            **state)
{
  SpaProxy *this;
  SpaProxyPort *port;
  int index;

  if (node == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  index = (*state == NULL ? 0 : *(int*)state);

  if (index >= port->n_formats)
    return SPA_RESULT_ENUM_END;

  *format = port->formats[index];
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_set_format (SpaNode            *node,
                                SpaDirection        direction,
                                uint32_t            port_id,
                                SpaPortFormatFlags  flags,
                                const SpaFormat    *format)
{
  SpaProxy *this;
  PinosMessageSetFormat sf;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (this->resource == NULL)
    return SPA_RESULT_OK;

  sf.seq = this->seq++;
  sf.direction = direction;
  sf.port_id = port_id;
  sf.flags = flags;
  sf.format = (SpaFormat *) format;
  pinos_client_send_message (this->resource->client,
                             this->resource,
                             PINOS_MESSAGE_SET_FORMAT,
                             &sf,
                             true);
  return SPA_RESULT_RETURN_ASYNC (sf.seq);
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

  *info = port->info;

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
spa_proxy_node_port_set_input (SpaNode      *node,
                               uint32_t      port_id,
                               SpaPortInput *input)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_INPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].io = input;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_set_output (SpaNode       *node,
                                uint32_t       port_id,
                                SpaPortOutput *output)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->out_ports[port_id].io = output;

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
  SpaProxyPort *port;
  unsigned int i, j;
  PinosMessageAddMem am;
  PinosMessageUseBuffers ub;
  size_t n_mem;
  PinosMessageBuffer *mb;
  SpaMetaShared *msh;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  spa_log_info (this->log, "proxy %p: use buffers %p %u", this, buffers, n_buffers);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this, port);

  if (n_buffers > 0) {
    mb = alloca (n_buffers * sizeof (PinosMessageBuffer));
  } else {
    mb = NULL;
  }

  port->n_buffers = n_buffers;

  if (this->resource == NULL)
    return SPA_RESULT_OK;

  n_mem = 0;
  for (i = 0; i < n_buffers; i++) {
    ProxyBuffer *b = &port->buffers[i];

    msh = spa_buffer_find_meta (buffers[i], SPA_META_TYPE_SHARED);
    if (msh == NULL) {
      spa_log_error (this->log, "missing shared metadata on buffer %d", i);
      return SPA_RESULT_ERROR;
    }

    b->outbuf = buffers[i];
    memcpy (&b->buffer, buffers[i], sizeof (SpaBuffer));
    b->buffer.datas = b->datas;
    b->buffer.metas = b->metas;

    am.direction = direction;
    am.port_id = port_id;
    am.mem_id = n_mem++;
    am.type = SPA_DATA_TYPE_MEMFD;
    am.memfd = msh->fd;
    am.flags = msh->flags;
    am.offset = msh->offset;
    am.size = msh->size;
    pinos_client_send_message (this->resource->client,
                               this->resource,
                               PINOS_MESSAGE_ADD_MEM,
                               &am,
                               false);

    mb[i].buffer = &b->buffer;
    mb[i].mem_id = am.mem_id;
    mb[i].offset = 0;
    mb[i].size = am.size;

    for (j = 0; j < buffers[i]->n_metas; j++) {
      memcpy (&b->buffer.metas[j], &buffers[i]->metas[j], sizeof (SpaMeta));
    }

    for (j = 0; j < buffers[i]->n_datas; j++) {
      SpaData *d = &buffers[i]->datas[j];

      memcpy (&b->buffer.datas[j], d, sizeof (SpaData));

      switch (d->type) {
        case SPA_DATA_TYPE_DMABUF:
        case SPA_DATA_TYPE_MEMFD:
          am.direction = direction;
          am.port_id = port_id;
          am.mem_id = n_mem;
          am.type = d->type;
          am.memfd = d->fd;
          am.flags = d->flags;
          am.offset = d->mapoffset;
          am.size = d->maxsize;
          pinos_client_send_message (this->resource->client,
                                     this->resource,
                                     PINOS_MESSAGE_ADD_MEM,
                                     &am,
                                     false);
          b->buffer.datas[j].type = SPA_DATA_TYPE_ID;
          b->buffer.datas[j].data = SPA_UINT32_TO_PTR (n_mem);
          n_mem++;
          break;
        case SPA_DATA_TYPE_MEMPTR:
          b->buffer.datas[j].data = SPA_INT_TO_PTR (b->size);
          b->size += d->maxsize;
          break;
        default:
          b->buffer.datas[j].type = SPA_DATA_TYPE_INVALID;
          b->buffer.datas[j].data = 0;
          spa_log_error (this->log, "invalid memory type %d", d->type);
          break;
      }
    }
  }

  ub.seq = this->seq++;
  ub.direction = direction;
  ub.port_id = port_id;
  ub.n_buffers = n_buffers;
  ub.buffers = mb;
  pinos_client_send_message (this->resource->client,
                             this->resource,
                             PINOS_MESSAGE_USE_BUFFERS,
                             &ub,
                             true);

  return SPA_RESULT_RETURN_ASYNC (ub.seq);
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

#if 0
static void
copy_meta_in (SpaProxy *this, SpaProxyPort *port, uint32_t buffer_id)
{
  ProxyBuffer *b = &port->buffers[buffer_id];
  unsigned int i;

  for (i = 0; i < b->outbuf->n_metas; i++) {
    SpaMeta *sm = &b->buffer.metas[i];
    SpaMeta *dm = &b->outbuf->metas[i];
    memcpy (dm->data, sm->data, dm->size);
  }
  for (i = 0; i < b->outbuf->n_datas; i++) {
    b->outbuf->datas[i].size = b->buffer.datas[i].size;
    if (b->outbuf->datas[i].type == SPA_DATA_TYPE_MEMPTR) {
      spa_log_info (this->log, "memcpy in %zd", b->buffer.datas[i].size);
      memcpy (b->outbuf->datas[i].data, b->datas[i].data, b->buffer.datas[i].size);
    }
  }
}
#endif

#if 0
static void
copy_meta_out (SpaProxy *this, SpaProxyPort *port, uint32_t buffer_id)
{
  ProxyBuffer *b = &port->buffers[buffer_id];
  unsigned int i;

  for (i = 0; i < b->outbuf->n_metas; i++) {
    SpaMeta *sm = &b->outbuf->metas[i];
    SpaMeta *dm = &b->buffer.metas[i];
    memcpy (dm->data, sm->data, dm->size);
  }
  for (i = 0; i < b->outbuf->n_datas; i++) {
    b->buffer.datas[i].size = b->outbuf->datas[i].size;
    if (b->datas[i].type == SPA_DATA_TYPE_MEMPTR) {
      spa_log_info (this->log, "memcpy out %zd", b->outbuf->datas[i].size);
      memcpy (b->datas[i].data, b->outbuf->datas[i].data, b->outbuf->datas[i].size);
    }
  }
}
#endif

static SpaResult
spa_proxy_node_port_reuse_buffer (SpaNode         *node,
                                  uint32_t         port_id,
                                  uint32_t         buffer_id)
{
  SpaProxy *this;
  SpaNodeEventReuseBuffer rb;
  PinosNode *pnode;
  uint8_t cmd = 0;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  pnode = this->pnode;

  if (!CHECK_OUT_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  rb.event.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
  rb.event.size = sizeof (rb);
  rb.port_id = port_id;
  rb.buffer_id = buffer_id;
  pinos_transport_add_event (pnode->transport, &rb.event);
  write (this->data_source.fd, &cmd, 1);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_send_command (SpaNode        *node,
                                  SpaDirection    direction,
                                  uint32_t        port_id,
                                  SpaNodeCommand *command)
{
  SpaProxy *this;
  SpaResult res = SPA_RESULT_OK;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
    case SPA_NODE_COMMAND_PAUSE:
    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
      break;

    default:
      spa_log_warn (this->log, "unhandled command %d", command->type);
      res = SPA_RESULT_NOT_IMPLEMENTED;
      break;
  }
  return res;
}

static SpaResult
spa_proxy_node_process_input (SpaNode *node)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  send_have_output (this);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_process_output (SpaNode *node)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  send_need_input (this);

  return SPA_RESULT_OK;
}

static SpaResult
handle_node_event (SpaProxy     *this,
                   SpaNodeEvent *event)
{
  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_INVALID:
      break;

    case SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE:
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    case SPA_NODE_EVENT_TYPE_ERROR:
    case SPA_NODE_EVENT_TYPE_BUFFERING:
    case SPA_NODE_EVENT_TYPE_REQUEST_REFRESH:
    case SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE:
      this->event_cb (&this->node, event, this->user_data);
      break;
  }

  return SPA_RESULT_OK;
}

static SpaResult
client_node_dispatch_func (void             *object,
                           PinosMessageType  type,
                           void             *message,
                           void             *data)
{
  PinosClientNode *node = data;
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (node, PinosClientNodeImpl, this);
  SpaProxy *this = &impl->proxy;

  switch (type) {
    default:
      spa_log_error (this->log, "proxy %p: got unexpected command %d", this, type);
      break;

    case PINOS_MESSAGE_NODE_UPDATE:
    {
      PinosMessageNodeUpdate *nu = message;

      if (nu->change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS)
        this->max_inputs = nu->max_input_ports;
      if (nu->change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS)
        this->max_outputs = nu->max_output_ports;

      spa_log_info (this->log, "proxy %p: got node update %d, max_in %u, max_out %u", this, type,
          this->max_inputs, this->max_outputs);

      break;
    }

    case PINOS_MESSAGE_PORT_UPDATE:
    {
      PinosMessagePortUpdate *pu = message;
      bool remove;

      spa_log_info (this->log, "proxy %p: got port update %d", this, type);
      if (!CHECK_PORT_ID (this, pu->direction, pu->port_id))
        break;

      remove = (pu->change_mask == 0);

      if (remove) {
        do_uninit_port (this, pu->direction, pu->port_id);
      } else {
        do_update_port (this, pu);
      }
      break;
    }

    case PINOS_MESSAGE_PORT_STATUS_CHANGE:
    {
      spa_log_warn (this->log, "proxy %p: command not implemented %d", this, type);
      break;
    }

    case PINOS_MESSAGE_NODE_STATE_CHANGE:
    {
      PinosMessageNodeStateChange *sc = message;
      SpaNodeState old = this->node.state;

      spa_log_info (this->log, "proxy %p: got node state change %d -> %d", this, old, sc->state);
      this->node.state = sc->state;
      if (old == SPA_NODE_STATE_INIT)
        send_async_complete (this, 0, SPA_RESULT_OK);

      break;
    }

    case PINOS_MESSAGE_ADD_MEM:
      break;
    case PINOS_MESSAGE_USE_BUFFERS:
      break;

    case PINOS_MESSAGE_NODE_EVENT:
    {
      PinosMessageNodeEvent *cne = message;
      handle_node_event (this, cne->event);
      break;
    }
  }
  return SPA_RESULT_OK;
}

static void
proxy_on_data_fd_events (SpaSource *source)
{
  SpaProxy *this = source->data;
  PinosNode *pnode = this->pnode;

  if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
    spa_log_warn (this->log, "proxy %p: got error", this);
    return;
  }

  if (source->rmask & SPA_IO_IN) {
    SpaNodeEvent event;
    uint8_t cmd;

    read (this->data_source.fd, &cmd, 1);

    while (pinos_transport_next_event (pnode->transport, &event) == SPA_RESULT_OK) {
      SpaNodeEvent *ev = alloca (event.size);
      pinos_transport_parse_event (pnode->transport, ev);
      this->event_cb (&this->node, ev, this->user_data);
    }
  }
}

static const SpaNode proxy_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_proxy_node_get_props,
  spa_proxy_node_set_props,
  spa_proxy_node_send_command,
  spa_proxy_node_set_event_callback,
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
  spa_proxy_node_port_set_input,
  spa_proxy_node_port_set_output,
  spa_proxy_node_port_reuse_buffer,
  spa_proxy_node_port_send_command,
  spa_proxy_node_process_input,
  spa_proxy_node_process_output,
};

static SpaResult
proxy_init (SpaProxy         *this,
            SpaDict          *info,
            const SpaSupport *support,
            unsigned int      n_support)
{
  unsigned int i;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOOP__MainLoop) == 0)
      this->main_loop = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOOP__DataLoop) == 0)
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
on_transport_changed (PinosListener   *listener,
                      PinosNode       *node)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (listener, PinosClientNodeImpl, transport_changed);
  PinosClientNode *this = &impl->this;
  PinosTransportInfo info;
  PinosMessageTransportUpdate tu;

  if (this->resource == NULL)
    return;

  pinos_transport_get_info (node->transport, &info);

  tu.memfd = info.memfd;
  tu.offset = info.offset;
  tu.size = info.size;
  pinos_client_send_message (this->resource->client,
                             this->resource,
                             PINOS_MESSAGE_TRANSPORT_UPDATE,
                             &tu,
                             true);
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
  unsigned int i;

  for (i = 0; i < MAX_INPUTS; i++) {
    if (this->in_ports[i].valid)
      clear_port (this, &this->in_ports[i], SPA_DIRECTION_INPUT, i);
  }
  for (i = 0; i < MAX_OUTPUTS; i++) {
    if (this->out_ports[i].valid)
      clear_port (this, &this->out_ports[i], SPA_DIRECTION_OUTPUT, i);
  }
  if (this->data_source.fd != -1) {
    spa_loop_remove_source (this->data_loop, &this->data_source);
    close (this->data_source.fd);
  }

  return SPA_RESULT_OK;
}

static void
client_node_resource_destroy (PinosResource *resource)
{
  PinosClientNode *this = resource->object;
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (this, PinosClientNodeImpl, this);

  impl->proxy.resource = this->resource = NULL;
  pinos_client_node_destroy (this);
}

static void
on_node_free (PinosListener *listener,
              PinosNode     *node)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (listener, PinosClientNodeImpl, node_free);

  pinos_log_debug ("client-node %p: free", &impl->this);
  proxy_clear (&impl->proxy);

  if (impl->data_fd != -1)
    close (impl->data_fd);
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
  impl->data_fd = -1;
  pinos_log_debug ("client-node %p: new", impl);

  pinos_signal_init (&this->destroy_signal);

  proxy_init (&impl->proxy, NULL, client->core->support, client->core->n_support);

  this->node = pinos_node_new (client->core,
                               name,
                               &impl->proxy.node,
                               NULL,
                               properties);
  if (this->node == NULL)
    goto error_no_node;

  impl->proxy.pnode = this->node;

  this->resource = pinos_resource_new (client,
                                       id,
                                       client->core->uri.client_node,
                                       this,
                                       (PinosDestroy) client_node_resource_destroy);
  if (this->resource == NULL)
    goto error_no_resource;

  impl->proxy.resource = this->resource;

  pinos_signal_add (&this->node->free_signal,
                    &impl->node_free,
                    on_node_free);

  pinos_signal_add (&this->node->transport_changed,
                    &impl->transport_changed,
                    on_transport_changed);

  pinos_signal_add (&this->node->loop_changed,
                    &impl->loop_changed,
                    on_loop_changed);

  pinos_signal_add (&impl->core->global_added,
                    &impl->global_added,
                    on_global_added);

  pinos_resource_set_dispatch (this->resource,
                               client_node_dispatch_func,
                               this);

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
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (this, PinosClientNodeImpl, this);

  pinos_log_debug ("client-node %p: destroy", impl);
  pinos_signal_emit (&this->destroy_signal, this);

  pinos_signal_remove (&impl->global_added);
  pinos_signal_remove (&impl->loop_changed);
  pinos_signal_remove (&impl->transport_changed);

  pinos_node_destroy (this->node);
}

/**
 * pinos_client_node_get_data_socket:
 * @node: a #PinosClientNode
 * @error: a #GError
 *
 * Create or return a previously create socket pair for @node. The
 * Socket for the other end is returned.
 *
 * Returns: %SPA_RESULT_OK on success
 */
SpaResult
pinos_client_node_get_data_socket (PinosClientNode  *this,
                                   int              *fd)
{
  PinosClientNodeImpl *impl = SPA_CONTAINER_OF (this, PinosClientNodeImpl, this);

  if (impl->data_fd == -1) {
    int fd[2];

    if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fd) != 0)
      return SPA_RESULT_ERRNO;

    impl->proxy.data_source.fd = fd[0];
    spa_loop_add_source (impl->proxy.data_loop, &impl->proxy.data_source);
    pinos_log_debug ("client-node %p: add data fd %d", this, fd[0]);
    impl->data_fd = fd[1];
  }
  *fd = impl->data_fd;
  return SPA_RESULT_OK;
}
