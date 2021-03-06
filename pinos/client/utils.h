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

#ifndef __PINOS_UTILS_H__
#define __PINOS_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/pod-utils.h>

const char * pinos_split_walk   (const char  *str,
                                 const char  *delimiter,
                                 size_t      *len,
                                 const char **state);
char **      pinos_split_strv   (const char *str,
                                 const char *delimiter,
                                 int         max_tokens,
                                 int        *n_tokens);
void         pinos_free_strv    (char **str);

char *       pinos_strip        (char       *str,
                                 const char *whitespace);

static inline SpaPOD *
pinos_spa_pod_copy (const SpaPOD *pod)
{
  return pod ? memcpy (malloc (SPA_POD_SIZE (pod)), pod, SPA_POD_SIZE (pod)) : NULL;
}

#define spa_format_copy(f)      ((SpaFormat*)pinos_spa_pod_copy(&(f)->pod))
#define spa_props_copy(p)       ((SpaProps*)pinos_spa_pod_copy(&(p)->pod))
#define spa_alloc_param_copy(p) ((SpaAllocParam*)pinos_spa_pod_copy(&(p)->pod))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_UTILS_H__ */
