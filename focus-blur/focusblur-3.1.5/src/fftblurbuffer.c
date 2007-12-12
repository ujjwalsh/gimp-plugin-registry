/* Focus Blur -- blur with focus plug-in.
 * Copyright (C) 2002-2007 Kyoichiro Suda
 *
 * The GIMP -- an image manipulation program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#ifdef HAVE_COMPLEX_H
#  include <complex.h>
#  include <math.h>
#endif
#include <fftw3.h>

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

#include "focusblur.h"

#include "focusblurenums.h"
#include "focusblurtypes.h"
#include "focusblurparam.h"

#include "diffusion.h"
#include "depthmap.h"
#include "shine.h"
#include "interface.h"

#include "fftblurbuffer.h"
#include "fftblurproc.h"


/*---- Prototypes ----*/

static gboolean
focusblur_fft_buffer_update_source      (FblurFftBuffer         *fft,
                                         GimpDrawable           *drawable,
                                         GimpPreview            *preview);
static gboolean
focusblur_fft_buffer_update_work        (FblurFftBuffer         *fft,
                                         gint                    radius);
static void
focusblur_fft_buffer_update_depth_division
                                        (FblurFftBuffer         *fft,
                                         FblurQualityType        quality,
                                         gint                    radius);
static void
focusblur_fft_buffer_update_depth_table            (FblurFftBuffer  *fft,
                                                    gint             division);
static void
focusblur_fft_buffer_update_depth_count            (FblurFftBuffer  *fft,
                                                    FblurDepthMap   *map);

static void     focusblur_fft_buffer_clear_source  (FblurFftBuffer  *fft);
static void     focusblur_fft_buffer_clear_work    (FblurFftBuffer  *fft);
static void     focusblur_fft_buffer_clear_depth   (FblurFftBuffer  *fft);


/*---- Functions for structures ----*/

gboolean
focusblur_fft_buffer_update (FblurFftBuffer **fft,
                             FblurParam      *param,
                             GimpPreview     *preview)
{
  FblurQualityType       quality;
  gint                   x, y;
  gint                   width, height;
  gint                   radius, range;

  quality = preview ? param->pref.quality_preview : param->pref.quality;


  /* check condition */
  if (quality == FBLUR_QUALITY_BEST)
    return FALSE;

  focusblur_diffusion_update (&(param->diffusion), *fft, &(param->store));
  radius = ceilf (param->diffusion->model_radius); // without softness
  range = param->diffusion->model_radius_int;

  if (radius < 3 ||
      (quality == FBLUR_QUALITY_NORMAL &&
       param->store.enable_depth_map &&
       radius > 63)) // needs more tune
    return FALSE;

  if (! preview)
    gimp_drawable_mask_intersect (param->drawable_ID, &x, &y, &width, &height);
  else
    gimp_preview_get_size (GIMP_PREVIEW (preview), &width, &height); 

  if (width  <= range ||
      height <= range)
    return FALSE;

  if (! *fft)
    {
      *fft = g_new0 (FblurFftBuffer, 1);

      if (! *fft)
        {
          g_printerr ("%s: %s(): failed to allocate memory.\n",
                      PLUG_IN_BINARY, G_STRFUNC);

          return FALSE;
        }
    }

  //focusblur_source_destroy (&(param->source));

  /* fftsource must be updated before to update work and depth */
  if (! focusblur_fft_buffer_update_source (*fft, param->drawable, preview))
    {
      g_printerr ("%s: %s(): failed to update source buffer.\n",
                  PLUG_IN_BINARY, G_STRFUNC);

      focusblur_fft_buffer_destroy (fft);
      return FALSE;
    }

  if (param->store.enable_depth_map)
    {
      if (! focusblur_depth_map_update (&(param->depth_map), *fft,
                                        &(param->store)))
        {
          g_printerr ("%s: %s(): failed to update depth info, but continue.\n",
                      PLUG_IN_BINARY, G_STRFUNC);

          focusblur_widget_set_enable_depth_map (param, FALSE);
        }
      else
        {
          focusblur_fft_buffer_update_depth_division (*fft, quality, range);

          focusblur_fft_buffer_update_depth_count (*fft, param->depth_map);
        }
    }

  if (! focusblur_fft_buffer_update_work (*fft, range))
    {
      g_printerr ("%s: %s(): failed to update working buffer.\n",
                  PLUG_IN_BINARY, G_STRFUNC);

      focusblur_fft_buffer_destroy (fft);
      return FALSE;
    }

  if (! focusblur_shine_update (&(param->shine), param->drawable,
                                &(param->store)))
    {
      g_printerr ("%s: %s(): failed to update shine data, but continue.\n",
                  PLUG_IN_BINARY, G_STRFUNC);

      focusblur_widget_set_enable_shine (param, FALSE);
    }

  return TRUE;
}


void
focusblur_fft_buffer_destroy (FblurFftBuffer **fft)
{
  if (*fft)
    {
      focusblur_fft_buffer_clear_source (*fft);
      focusblur_fft_buffer_clear_work (*fft);
      focusblur_fft_buffer_clear_depth (*fft);

      g_free (*fft);
      *fft = NULL;
    }
}


void
focusblur_fft_buffer_draw (FblurFftBuffer *fft)
{
  GimpPixelRgn   pr;
  gint x, y;

  if (! fft->source.preview)
    {
      g_assert (fft->source.data);

      gimp_pixel_rgn_init (&pr, fft->source.drawable,
                           fft->source.x1, fft->source.y1,
                           fft->source.width, fft->source.height, TRUE, TRUE);
      gimp_pixel_rgn_set_rect (&pr, fft->source.data,
                               fft->source.x1, fft->source.y1,
                               fft->source.width, fft->source.height);

      gimp_drawable_flush (fft->source.drawable);
      gimp_drawable_merge_shadow (fft->source.drawable->drawable_id, TRUE);
      gimp_drawable_update (fft->source.drawable->drawable_id,
                            fft->source.x1, fft->source.y1,
                            fft->source.width, fft->source.height);

      /* this buffer has been dirty */
      focusblur_fft_buffer_clear_source (fft);
    }
  else
    {
      /* preview area size */
      if (fft->source.data_preview)
        {
          gimp_preview_area_draw
            (GIMP_PREVIEW_AREA (gimp_preview_get_area (fft->source.preview)),
             0, 0, fft->source.width, fft->source.height,
             gimp_drawable_type (fft->source.drawable->drawable_id),
             fft->source.data_preview, fft->source.rowstride);
        }
      /* non-cropped image (not implemented) */
      else
        {
          g_assert (fft->source.data);

          gimp_preview_get_position
            (GIMP_PREVIEW (fft->source.preview), &x, &y);

          x = fft->source.x1 - x;
          y = fft->source.y1 - y;

          gimp_preview_area_draw
            (GIMP_PREVIEW_AREA (gimp_preview_get_area (fft->source.preview)),
             x, y, fft->source.width, fft->source.height,
             gimp_drawable_type (fft->source.drawable->drawable_id),
             fft->source.data, fft->source.rowstride);

          focusblur_fft_buffer_clear_source (fft);
        }
    }
}


void
focusblur_fft_buffer_make_kernel (FblurFftBuffer      *fft,
                                  FblurDiffusionTable *diffusion,
                                  gint                 level)
{
  gfloat        *o0, *o1, *o2, *o3;
  gfloat        *dlp, *dp;
  gint           row, col;
  gint           n, r;
  gint           x, y;
  gfloat         norm;

  if (level == fft->work.level)
    return;

  norm = 1.0f / (fft->work.row * fft->work.col);

  if (level)
    {
      row = fft->work.row;
      col = fft->work.col;
      n = fft->work.col_padded;
      r = MIN (diffusion->model_radius_int, fft->work.space);

      o0 = (gfloat *) fft->work.image;
      o1 = o0 + col - r;
      o2 = o0 + (row - r) * n;
      o3 = o2 + col - r;

      focusblur_fft_work_fill_zero (fft);

      for (x = 0, dlp = o0; x <= r; x ++, dlp += n)
        for (y = 0, dp = dlp; y <= r; y ++, dp ++)
          *dp = norm *
            focusblur_diffusion_get (diffusion, level, 0, 0, x, y);

      for (x = 0, dlp = o1; x <= r; x ++, dlp += n)
        for (y = -r, dp = dlp; y < 0; y ++, dp ++)
          *dp = norm *
            focusblur_diffusion_get (diffusion, level, 0, 0, x, y);

      for (x = -r, dlp = o2; x < 0; x ++, dlp += n)
        for (y = 0, dp = dlp; y <= r; y ++, dp ++)
          *dp = norm *
            focusblur_diffusion_get (diffusion, level, 0, 0, x, y);

      for (x = -r, dlp = o3; x < 0; x ++, dlp += n)
        for (y = -r, dp = dlp; y < 0; y ++, dp ++)
          *dp = norm *
            focusblur_diffusion_get (diffusion, level, 0, 0, x, y);

      fftwf_execute (fft->work.plan_r2c);

      focusblur_fft_work_store_in_kernel (fft);
    }

  fft->work.level = level;
}


void
focusblur_fft_buffer_make_depth_slice (FblurFftBuffer   *fft,
                                       FblurDepthMap    *depth_map,
                                       gint              look)
{
  gfloat        *dlp, *dp;
  gint           orig, x, y;

  focusblur_fft_work_fill_zero (fft);

  dlp = (gfloat *) fft->work.image + fft->work.origin;

  if (fft->depth.quality == FBLUR_QUALITY_NORMAL)
    for (y = fft->source.y1; y < fft->source.y2; y ++, dlp ++)
      for (x = fft->source.x1, dp = dlp; x < fft->source.x2;
           x ++, dp += fft->work.col_padded)
        {
          orig = focusblur_depth_map_get_depth (depth_map, x, y);

          if (fft->depth.fval[orig] == look)
            {
              *dp = 1.0f - fft->depth.dval[orig];
              continue;
            }

          if (fft->depth.cval[orig] == look)
            {
              *dp = fft->depth.dval[orig];
            }
        }

  else
    for (y = fft->source.y1; y < fft->source.y2; y ++, dlp ++)
      for (x = fft->source.x1, dp = dlp; x < fft->source.x2;
           x ++, dp += fft->work.col_padded)
        {
          orig = focusblur_depth_map_get_depth (depth_map, x, y);

          if (fft->depth.rval[orig] == look)
            *dp = 1.0f;
        }
}


void
focusblur_fft_buffer_make_depth_behind (FblurFftBuffer  *fft,
                                       FblurDepthMap    *depth_map)
{
  gfloat        *dlp, *dp;
  gint           orig, x, y;
  gint           look;

  look = focusblur_depth_map_focal_depth (depth_map);

  focusblur_fft_work_fill_zero (fft);

  dlp = (gfloat *) fft->work.image + fft->work.origin;

  for (y = fft->source.y1; y < fft->source.y2; y ++, dlp ++)
    for (x = fft->source.x1, dp = dlp; x < fft->source.x2;
         x ++, dp += fft->work.col_padded)
      {
        orig = focusblur_depth_map_get_depth (depth_map, x, y);

        /* this is imperfect to fill behind,
           because fftblur works in a whole picture,
           it can't work adaptive at each pixels. */
        if (fft->depth.rval[orig] >= look)
          *dp = 1.0f;
      }
}


void
focusblur_fft_buffer_invalidate_depth_map (FblurFftBuffer *fft)
{
  if (fft)
    {
      fft->depth.count = 0;
    }
}


void
focusblur_fft_buffer_invalidate_diffusion (FblurFftBuffer *fft)
{
  if (fft)
    {
      fft->work.level = 0;
    }
}


static gboolean
focusblur_fft_buffer_update_source (FblurFftBuffer *fft,
                                    GimpDrawable   *drawable,
                                    GimpPreview    *preview)
{
  GimpPixelRgn  pr;
  gsize         size;
  gint          x1, x2, y1, y2;
  gint          width, height;
  gint          ntiles;

  fft->source.drawable = drawable;
  fft->source.preview  = preview;
  if (! preview)
    {
      gimp_drawable_mask_intersect
        (drawable->drawable_id, &x1, &y1, &width, &height);
    }
  else
    {
      gimp_preview_get_position (GIMP_PREVIEW (preview), &x1, &y1);
      gimp_preview_get_size (GIMP_PREVIEW (preview), &width, &height); 
    }
  x2 = x1 + width;
  y2 = y1 + height;

  ntiles = 1 + x2 / TILE_WIDTH - x1 / TILE_WIDTH;
  gimp_tile_cache_ntiles (ntiles);

  fft->source.has_alpha = gimp_drawable_has_alpha (drawable->drawable_id);
  fft->source.bpp       = drawable->bpp;
  fft->source.channels  = drawable->bpp - (fft->source.has_alpha ? 1 : 0);
  fft->source.rowstride = drawable->bpp * width;

  size = fft->source.rowstride * height;

  if (fft->source.data_preview)
    {
      if (! preview)
        {
          g_free (fft->source.data_preview);
          fft->source.data_preview = NULL;
        }
      else if (size != fft->source.size)
        {
          g_free (fft->source.data_preview);
          goto allocate2;
        }
    }
  else
    {
    allocate2:
      if (preview)
        {
          fft->source.data_preview = g_malloc (size);
          if (! fft->source.data_preview)
            return FALSE;
        }
    }

  if (fft->source.data)
    {
      if (size != fft->source.size)
        {
          g_free (fft->source.data);
          goto allocate;
        }
    }
  else
    {
    allocate:
      fft->source.size = size;
      fft->source.data = g_malloc (size);
      if (! fft->source.data)
        return FALSE;
      goto reload;
    }

  if (x1 == fft->source.x1 &&
      x2 == fft->source.x2 &&
      y1 == fft->source.y1 &&
      y2 == fft->source.y2)
    return TRUE;

 reload:
  fft->source.x1     = x1;
  fft->source.x2     = x2;
  fft->source.y1     = y1;
  fft->source.y2     = y2;
  fft->source.width  = width;
  fft->source.height = height;

  /* need to recount */
  fft->depth.count = 0;

  /* load */
  gimp_pixel_rgn_init (&pr, drawable, x1, y1, width, height, FALSE, FALSE);
  gimp_pixel_rgn_get_rect (&pr, fft->source.data,
                           fft->source.x1, fft->source.y1,
                           fft->source.width, fft->source.height);

  return TRUE;
}


static gboolean
focusblur_fft_buffer_update_work (FblurFftBuffer *fft,
                                  gint            radius)
{
  gint row, col;

  row = fft->source.width  + 2 * radius;
  col = fft->source.height + 2 * radius;

  if (fft->work.buffers)
    {
      g_warning ("buffer hadn't been cleared.");
      focusblur_fft_work_free_buffers (fft);
    }

  if (fft->work.image &&
      row == fft->work.row &&
      col == fft->work.col)
    {
      if (radius != fft->work.space)
        {
          fft->work.space = radius;
          fft->work.origin = (fft->work.col_padded + 1) * radius;
          fft->work.level = 0;
        }
      return TRUE;
    }

  focusblur_fft_buffer_clear_work (fft);

  fft->work.row = row;
  fft->work.col = col;
  fft->work.col_padded = (col + 2) & ~1;

  fft->work.nelements = row * fft->work.col_padded;
  fft->work.complex_nelements = fft->work.nelements / 2;
  fft->work.size = sizeof (fftwf_complex) * fft->work.complex_nelements;

  fft->work.size += 15;
  fft->work.size &= ~15;

  fft->work.image  = fftwf_malloc (fft->work.size);
  fft->work.kernel = fftwf_malloc (fft->work.size);
  if (! fft->work.image || ! fft->work.kernel)
    {
      focusblur_fft_buffer_clear_work (fft);
      return FALSE;
    }

  fft->work.plan_r2c = fftwf_plan_dft_r2c_2d
    (row, col, (gfloat *) fft->work.image, fft->work.image, FFTW_ESTIMATE);

  fft->work.plan_c2r = fftwf_plan_dft_c2r_2d
    (row, col, fft->work.image, (gfloat *) fft->work.image, FFTW_ESTIMATE);

  if (! fft->work.plan_r2c || ! fft->work.plan_c2r)
    {
      focusblur_fft_buffer_clear_work (fft);
      return FALSE;
    }

  fft->work.space = radius;
  fft->work.origin = (fft->work.col_padded + 1) * radius;
  fft->work.level = 0;

  return TRUE;
}


static void
focusblur_fft_buffer_update_depth_division (FblurFftBuffer   *fft,
                                            FblurQualityType  quality,
                                            gint              radius)
{
  gint division;

  fft->depth.quality = quality;

  division = radius;
  if (quality == FBLUR_QUALITY_LOW)
    division = MIN (division, 15);
  else if (quality == FBLUR_QUALITY_DEFECTIVE)
    division = MIN (division, 7);

  focusblur_fft_buffer_update_depth_table (fft, division);
}


static void
focusblur_fft_buffer_update_depth_table (FblurFftBuffer *fft,
                                         gint            division)
{
  gfloat dfac, fval;
  gfloat r, f, c, d;
  gint i;

  g_assert (division > 0);

  if (division == fft->depth.division)
    return;

  dfac = (gfloat) FBLUR_DEPTH_MAX / division;

  for (i = 0; i <= FBLUR_DEPTH_MAX; i ++)
    {
      fval = (gfloat) i / dfac;

      r = rintf (fval);
      if (fabsf (r - fval) < 0.001f)
        {
          fft->depth.rval[i] = fft->depth.fval[i] = fft->depth.cval[i]
            = rintf (r * dfac);
          fft->depth.dval[i] = 0.0f;
        }
      else
        {
          f = floorf (fval);
          c = ceilf (fval);
          d = (c - f);

          fft->depth.rval[i] = rintf (r * dfac);
          fft->depth.fval[i] = rintf (f * dfac);
          fft->depth.cval[i] = rintf (c * dfac);
          fft->depth.dval[i] = (d > 0.0f) ? ((fval - f) / d) : (0.0f);
        }
    }

  fft->depth.division = division;
  fft->depth.count = 0;
}


static void
focusblur_fft_buffer_update_depth_count (FblurFftBuffer *fft,
                                         FblurDepthMap  *depth_map)
{
  gint   orig, depth;
  gint   x, y;

  if (fft->depth.count)
    return;

  /* count layer of depth */
  memset (fft->depth.check, 0, sizeof fft->depth.check);

  if (fft->depth.quality == FBLUR_QUALITY_NORMAL)
    {
      for (y = fft->source.y1; y < fft->source.y2; y ++)
        for (x = fft->source.x1; x < fft->source.x2; x ++)
          {
            orig = focusblur_depth_map_get_depth (depth_map, x, y);

            depth = fft->depth.fval[orig];

            if (! fft->depth.check[depth])
              {
                fft->depth.check[depth] = TRUE;
                fft->depth.count ++;
              }

            depth = fft->depth.cval[orig];

            if (! fft->depth.check[depth])
              {
                fft->depth.check[depth] = TRUE;
                fft->depth.count ++;
              }
          }
    }
  else
    {
      for (y = fft->source.y1; y < fft->source.y2; y ++)
        for (x = fft->source.x1; x < fft->source.x2; x ++)
          {
            orig = focusblur_depth_map_get_depth (depth_map, x, y);

            depth = fft->depth.rval[orig];

            if (! fft->depth.check[depth])
              {
                fft->depth.check[depth] = TRUE;
                fft->depth.count ++;
              }
          }
    }
}


static void
focusblur_fft_buffer_clear_source (FblurFftBuffer *fft)
{
  g_assert (fft != NULL);

  if (fft->source.data)
    g_free (fft->source.data);

  if (fft->source.data_preview)
    g_free (fft->source.data_preview);

  memset (&(fft->source), 0, sizeof (FblurFftSource));
}


static void
focusblur_fft_buffer_clear_work (FblurFftBuffer *fft)
{
  g_assert (fft != NULL);

  if (fft->work.buffers)
    focusblur_fft_work_free_buffers (fft);

  if (fft->work.plan_r2c)
    fftwf_destroy_plan (fft->work.plan_r2c);

  if (fft->work.plan_c2r)
    fftwf_destroy_plan (fft->work.plan_c2r);

  if (fft->work.image)
    fftwf_free (fft->work.image);

  if (fft->work.kernel)
    fftwf_free (fft->work.kernel);

  memset (&(fft->work), 0, sizeof (FblurFftWork));
}


static void
focusblur_fft_buffer_clear_depth (FblurFftBuffer *fft)
{
  g_assert (fft != NULL);

  memset (&(fft->depth), 0, sizeof (FblurFftDepth));
}