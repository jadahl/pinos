/* Simple Plugin API
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

#ifndef __SPA_LIST_H__
#define __SPA_LIST_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaList SpaList;

#include <spa/defs.h>

struct _SpaList {
  SpaList *next;
  SpaList *prev;
};

static inline void
spa_list_init (SpaList *list)
{
  list->next = list;
  list->prev = list;
}

static inline void
spa_list_insert (SpaList *list,
                 SpaList *elem)
{
  elem->prev = list;
  elem->next = list->next;
  list->next = elem;
  elem->next->prev = elem;
}

static inline void
spa_list_remove (SpaList *elem)
{
  elem->prev->next = elem->next;
  elem->next->prev = elem->prev;
}

#define spa_list_is_empty(l)  ((l)->next == (l))

#define spa_list_first(head, type, member)                                      \
    SPA_CONTAINER_OF((head)->next, type, member)

#define spa_list_last(item, type, member)                                       \
    SPA_CONTAINER_OF((head)->prev, type, member)

#define spa_list_for_each(pos, head, member)                                    \
    for (pos = SPA_CONTAINER_OF((head)->next, __typeof__(*pos), member);        \
         &pos->member != (head);                                                \
         pos = SPA_CONTAINER_OF(pos->member.next, __typeof__(*pos), member))

#define spa_list_for_each_safe(pos, tmp, head, member)                          \
    for (pos = SPA_CONTAINER_OF((head)->next, __typeof__(*pos), member),        \
         tmp = SPA_CONTAINER_OF((pos)->member.next, __typeof__(*tmp), member);  \
         &pos->member != (head);                                                \
         pos = tmp,                                                             \
         tmp = SPA_CONTAINER_OF(pos->member.next, __typeof__(*tmp), member))


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_LIST_H__ */
