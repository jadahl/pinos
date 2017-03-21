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

#ifndef __SPA_FORMAT_H__
#define __SPA_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SPA_FORMAT_URI             "http://spaplug.in/ns/format"
#define SPA_FORMAT_PREFIX          SPA_FORMAT_URI "#"

typedef struct _SpaFormat SpaFormat;

#include <stdarg.h>

#include <spa/defs.h>
#include <spa/pod-utils.h>
#include <spa/id-map.h>

#define SPA_MEDIA_TYPE_URI           "http://spaplug.in/ns/media-type"
#define SPA_MEDIA_TYPE_PREFIX        SPA_MEDIA_TYPE_URI "#"

#define SPA_MEDIA_TYPE__audio        SPA_MEDIA_TYPE_PREFIX "audio"
#define SPA_MEDIA_TYPE__video        SPA_MEDIA_TYPE_PREFIX "video"
#define SPA_MEDIA_TYPE__image        SPA_MEDIA_TYPE_PREFIX "image"

typedef struct {
  uint32_t audio;
  uint32_t video;
  uint32_t image;
} SpaMediaTypes;

static inline void
spa_media_types_fill (SpaMediaTypes *types, SpaIDMap *map)
{
  if (types->audio == 0) {
    types->audio = spa_id_map_get_id (map, SPA_MEDIA_TYPE__audio);
    types->video = spa_id_map_get_id (map, SPA_MEDIA_TYPE__video);
    types->image = spa_id_map_get_id (map, SPA_MEDIA_TYPE__image);
  }
}

#define SPA_MEDIA_SUBTYPE_URI           "http://spaplug.in/ns/media-subtype"
#define SPA_MEDIA_SUBTYPE_PREFIX        SPA_MEDIA_TYPE_URI "#"

#define SPA_MEDIA_SUBTYPE__raw          SPA_MEDIA_TYPE_PREFIX "raw"

typedef struct {
  uint32_t raw;
} SpaMediaSubtypes;

static inline void
spa_media_subtypes_map (SpaIDMap *map, SpaMediaSubtypes *types)
{
  if (types->raw == 0) {
    types->raw = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__raw);
  }
}

#define SPA_MEDIA_SUBTYPE__h264         SPA_MEDIA_SUBTYPE_PREFIX "h264"
#define SPA_MEDIA_SUBTYPE__mjpg         SPA_MEDIA_SUBTYPE_PREFIX "mjpg"
#define SPA_MEDIA_SUBTYPE__dv           SPA_MEDIA_SUBTYPE_PREFIX "dv"
#define SPA_MEDIA_SUBTYPE__mpegts       SPA_MEDIA_SUBTYPE_PREFIX "mpegts"
#define SPA_MEDIA_SUBTYPE__h263         SPA_MEDIA_SUBTYPE_PREFIX "h263"
#define SPA_MEDIA_SUBTYPE__mpeg1        SPA_MEDIA_SUBTYPE_PREFIX "mpeg1"
#define SPA_MEDIA_SUBTYPE__mpeg2        SPA_MEDIA_SUBTYPE_PREFIX "mpeg2"
#define SPA_MEDIA_SUBTYPE__mpeg4        SPA_MEDIA_SUBTYPE_PREFIX "mpeg4"
#define SPA_MEDIA_SUBTYPE__xvid         SPA_MEDIA_SUBTYPE_PREFIX "xvid"
#define SPA_MEDIA_SUBTYPE__vc1          SPA_MEDIA_SUBTYPE_PREFIX "vc1"
#define SPA_MEDIA_SUBTYPE__vp8          SPA_MEDIA_SUBTYPE_PREFIX "vp8"
#define SPA_MEDIA_SUBTYPE__vp9          SPA_MEDIA_SUBTYPE_PREFIX "vp9"
#define SPA_MEDIA_SUBTYPE__jpeg         SPA_MEDIA_SUBTYPE_PREFIX "jpeg"
#define SPA_MEDIA_SUBTYPE__bayer        SPA_MEDIA_SUBTYPE_PREFIX "bayer"

typedef struct {
  uint32_t h264;
  uint32_t mjpg;
  uint32_t dv;
  uint32_t mpegts;
  uint32_t h263;
  uint32_t mpeg1;
  uint32_t mpeg2;
  uint32_t mpeg4;
  uint32_t xvid;
  uint32_t vc1;
  uint32_t vp8;
  uint32_t vp9;
  uint32_t jpeg;
  uint32_t bayer;
} SpaMediaSubtypesVideo;

static inline void
spa_media_subtypes_video_map (SpaIDMap *map, SpaMediaSubtypesVideo *types)
{
  if (types->h264 == 0) {
    types->h264 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__h264);
    types->mjpg = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__mjpg);
    types->dv = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__dv);
    types->mpegts = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__mpegts);
    types->h263 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__h263);
    types->mpeg1 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__mpeg1);
    types->mpeg2 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__mpeg2);
    types->mpeg4 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__mpeg4);
    types->xvid = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__xvid);
    types->vc1 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__vc1);
    types->vp8 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__vp8);
    types->vp9 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__vp9);
    types->jpeg = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__jpeg);
    types->bayer = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__bayer);
  }
}

#define SPA_MEDIA_SUBTYPE__mp3          SPA_MEDIA_SUBTYPE_PREFIX "mp3"
#define SPA_MEDIA_SUBTYPE__aac          SPA_MEDIA_SUBTYPE_PREFIX "aac"
#define SPA_MEDIA_SUBTYPE__vorbis       SPA_MEDIA_SUBTYPE_PREFIX "vorbis"
#define SPA_MEDIA_SUBTYPE__wma          SPA_MEDIA_SUBTYPE_PREFIX "wma"
#define SPA_MEDIA_SUBTYPE__ra           SPA_MEDIA_SUBTYPE_PREFIX "ra"
#define SPA_MEDIA_SUBTYPE__sbc          SPA_MEDIA_SUBTYPE_PREFIX "sbc"
#define SPA_MEDIA_SUBTYPE__adpcm        SPA_MEDIA_SUBTYPE_PREFIX "adpcm"
#define SPA_MEDIA_SUBTYPE__g723         SPA_MEDIA_SUBTYPE_PREFIX "g723"
#define SPA_MEDIA_SUBTYPE__g726         SPA_MEDIA_SUBTYPE_PREFIX "g726"
#define SPA_MEDIA_SUBTYPE__g729         SPA_MEDIA_SUBTYPE_PREFIX "g729"
#define SPA_MEDIA_SUBTYPE__amr          SPA_MEDIA_SUBTYPE_PREFIX "amr"
#define SPA_MEDIA_SUBTYPE__gsm          SPA_MEDIA_SUBTYPE_PREFIX "gsm"

typedef struct {
  uint32_t mp3;
  uint32_t aac;
  uint32_t vorbis;
  uint32_t wma;
  uint32_t ra;
  uint32_t sbc;
  uint32_t adpcm;
  uint32_t g723;
  uint32_t g726;
  uint32_t g729;
  uint32_t amr;
  uint32_t gsm;
} SpaMediaSubtypesAudio;

static inline void
spa_media_subtypes_audio_map (SpaIDMap *map, SpaMediaSubtypesAudio *types)
{
  if (types->mp3 == 0) {
    types->mp3 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__mp3);
    types->aac = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__aac);
    types->vorbis = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__vorbis);
    types->wma = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__wma);
    types->ra = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__ra);
    types->sbc = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__sbc);
    types->adpcm = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__adpcm);
    types->g723 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__g723);
    types->g726 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__g726);
    types->g729 = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__g729);
    types->amr = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__amr);
    types->gsm = spa_id_map_get_id (map, SPA_MEDIA_SUBTYPE__gsm);
  }
}

typedef struct {
  SpaPODObjectBody obj_body;
  SpaPODURI        media_type           SPA_ALIGNED (8);
  SpaPODURI        media_subtype        SPA_ALIGNED (8);
  /* contents follow, series of SpaPODProp */
} SpaFormatBody;

/**
 * SpaFormat:
 * @media_type: media type
 * @media_subtype: subtype
 * @pod: POD object with properties
 */
struct _SpaFormat {
  SpaPOD        pod;
  SpaFormatBody body;
};

#define SPA_FORMAT_INIT(size,type,media_type,media_subtype,...)         \
  { { size, SPA_POD_TYPE_OBJECT },                                      \
    { { 0, type },                                                      \
        SPA_POD_URI_INIT (media_type),                                  \
        SPA_POD_URI_INIT (media_subtype) } }

#define SPA_FORMAT_BODY_FOREACH(body, size, iter) \
  for ((iter) = SPA_MEMBER ((body), sizeof (SpaFormatBody), SpaPODProp); \
       (iter) < SPA_MEMBER ((body), (size), SpaPODProp); \
       (iter) = SPA_MEMBER ((iter), SPA_ROUND_UP_N (SPA_POD_SIZE (iter), 8), SpaPODProp))

#define SPA_FORMAT_FOREACH(format, iter) \
  SPA_FORMAT_BODY_FOREACH(&format->body, SPA_POD_BODY_SIZE(format), iter)

static inline SpaPODProp *
spa_format_find_prop (const SpaFormat *format, uint32_t key)
{
  return spa_pod_contents_find_prop (&format->pod, sizeof (SpaFormat), key);
}

static inline uint32_t
spa_format_query (const SpaFormat *format, uint32_t key, ...)
{
  uint32_t count;
  va_list args;

  va_start (args, key);
  count = spa_pod_contents_queryv (&format->pod, sizeof (SpaFormat), key, args);
  va_end (args);

  return count;
}

static inline SpaResult
spa_format_fixate (SpaFormat *format)
{
  SpaPODProp *prop;

  SPA_FORMAT_FOREACH (format, prop)
    prop->body.flags &= ~SPA_POD_PROP_FLAG_UNSET;

  return SPA_RESULT_OK;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_FORMAT_H__ */
