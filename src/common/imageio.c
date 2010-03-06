/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
  #include "../config.h"
#endif
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_png.h"
#include "common/imageio_pfm.h"
#include "common/imageio_rgbe.h"
#include "common/image_compression.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "iop/colorout.h"
#include "libraw/libraw.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <strings.h>


int dt_imageio_preview_write(dt_image_t *img, dt_image_buffer_t mip)
{
  sqlite3_stmt *stmt;
  int rc, wd, ht;
  if(mip == DT_IMAGE_NONE || mip == DT_IMAGE_FULL) return 1; 
  dt_image_get_mip_size(img, mip, &wd, &ht);
  // check if resolution is still up-to-date:
  if(img->mip_buf_size[mip] != (mip == DT_IMAGE_MIPF?3*sizeof(float):4*sizeof(uint8_t))*wd*ht)
  {
    printf("[imageio_preview_write] rejecting old resolution\n");
    return 0;
  }

  rc = sqlite3_prepare_v2(darktable.db, "delete from mipmap_timestamps where imgid = ?1 and level = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  rc = sqlite3_bind_int (stmt, 2, mip);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "insert into mipmap_timestamps (imgid, level) values (?1, ?2)", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  rc = sqlite3_bind_int (stmt, 2, mip);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1 and level = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  rc = sqlite3_bind_int (stmt, 2, mip);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "insert into mipmaps (imgid, level) values (?1, ?2)", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  rc = sqlite3_bind_int (stmt, 2, mip);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  if(mip == DT_IMAGE_MIPF)
  {
    dt_image_check_buffer(img, DT_IMAGE_MIPF, 3*wd*ht*sizeof(float));
    uint8_t *buf = (uint8_t *)malloc(sizeof(uint8_t)*wd*ht);
    dt_image_compress(img->mipf, buf, wd, ht);
    rc = sqlite3_prepare_v2(darktable.db, "update mipmaps set data = ?1 where imgid = ?2 and level = ?3", -1, &stmt, NULL);
    rc = sqlite3_bind_blob(stmt, 1, buf, sizeof(uint8_t)*wd*ht, free);
    rc = sqlite3_bind_int (stmt, 2, img->id);
    rc = sqlite3_bind_int (stmt, 3, mip);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
    return 0;
  }

  dt_image_check_buffer(img, mip, 4*wd*ht*sizeof(uint8_t));
  uint8_t *blob = (uint8_t *)malloc(4*sizeof(uint8_t)*wd*ht);
  int length = dt_imageio_jpeg_compress(img->mip[mip], blob, wd, ht, 97);
  rc = sqlite3_prepare_v2(darktable.db, "update mipmaps set data = ?1 where imgid = ?2 and level = ?3", -1, &stmt, NULL);
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_blob(stmt, 1, blob, length, free);
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_int (stmt, 2, img->id);
  rc = sqlite3_bind_int (stmt, 3, mip);
  rc = sqlite3_step(stmt);
  if(rc != SQLITE_DONE) fprintf(stderr, "[preview_write] update mipmap failed: %s\n", sqlite3_errmsg(darktable.db));
  rc = sqlite3_finalize(stmt);

  return 0;
}

int dt_imageio_preview_read(dt_image_t *img, dt_image_buffer_t mip)
{
  if(mip == DT_IMAGE_NONE || mip == DT_IMAGE_FULL) return 1;
  if(img->mip[mip])
  { // already loaded?
    dt_image_buffer_t mip2 = dt_image_get(img, mip, 'r');
    if(mip2 != mip) dt_image_release(img, mip2, 'r');
    else return 0;
  }
  sqlite3_stmt *stmt;
  int rc, wd, ht;
  size_t length = 0;
  const void *blob = NULL;
  dt_image_get_mip_size(img, mip, &wd, &ht);
  rc = sqlite3_prepare_v2(darktable.db, "delete from mipmap_timestamps where imgid = ?1 and level = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  rc = sqlite3_bind_int (stmt, 2, mip);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "insert into mipmap_timestamps (imgid, level) values (?1, ?2)", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  rc = sqlite3_bind_int (stmt, 2, mip);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "select data from mipmaps where imgid = ?1 and level = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  rc = sqlite3_bind_int (stmt, 2, mip);
  HANDLE_SQLITE_ERR(rc);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    blob = sqlite3_column_blob(stmt, 0);
    length = sqlite3_column_bytes(stmt, 0);
  }
  if(!blob || dt_image_alloc(img, mip))
  {
    rc = sqlite3_finalize(stmt);
    return 1;
  }

  if(mip == DT_IMAGE_MIPF)
  {
    if(length != sizeof(uint8_t)*wd*ht) goto err_wrong_res;
    dt_image_check_buffer(img, mip, 3*wd*ht*sizeof(float));
    dt_image_uncompress((uint8_t *)blob, img->mipf, wd, ht);
  }
  else
  {
    dt_image_check_buffer(img, mip, 4*wd*ht*sizeof(uint8_t));
    dt_imageio_jpeg_t jpg;
    if(dt_imageio_jpeg_decompress_header(blob, length, &jpg) || 
      (jpg.width != wd || jpg.height != ht) ||
       dt_imageio_jpeg_decompress(&jpg, img->mip[mip])) goto err_wrong_res;
  }
  rc = sqlite3_finalize(stmt);
  dt_image_release(img, mip, 'w');
  return 0;
err_wrong_res: // remove obsoleted thumbnail from db
  sqlite3_finalize(stmt);
  dt_image_release(img, mip, 'w');
  dt_image_release(img, mip, 'r');
  sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1 and level = ?2", -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, img->id);
  sqlite3_bind_int (stmt, 2, mip);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return 1;
}

void dt_imageio_preview_f_to_8(int32_t p_wd, int32_t p_ht, const float *f, uint8_t *p8)
{
  for(int idx=0;idx < p_wd*p_ht; idx++)
    for(int k=0;k<3;k++) p8[4*idx+2-k] = dt_dev_default_gamma[(int)CLAMP(0xffff*f[3*idx+k], 0, 0xffff)];
}

void dt_imageio_preview_8_to_f(int32_t p_wd, int32_t p_ht, const uint8_t *p8, float *f)
{
  for(int idx=0;idx < p_wd*p_ht; idx++)
    for(int k=0;k<3;k++) f[3*idx+2-k] = dt_dev_de_gamma[p8[4*idx+k]];
}


// =================================================
//   begin libraw wrapper functions:
// =================================================

#define HANDLE_ERRORS(ret, verb) {                                 \
  if(ret)                                                     \
  {                                                       \
    if(verb) fprintf(stderr,"[imageio] %s: %s\n", filename, libraw_strerror(ret)); \
    libraw_close(raw);                         \
    raw = NULL; \
    return DT_IMAGEIO_FILE_CORRUPTED;                                   \
  }                                                       \
}

int dt_imageio_write_pos(int i, int j, int wd, int ht, float fwd, float fht, int orientation)
{
  int ii = i, jj = j, w = wd, h = ht, fw = fwd, fh = fht;
  if(orientation & 4)
  {
    w = ht; h = wd;
    ii = j; jj = i;
    fw = fht; fh = fwd;
  }
  if(orientation & 2) ii = (int)fw - ii - 1;
  if(orientation & 1) jj = (int)fh - jj - 1;
  return jj*w + ii;
}

dt_imageio_retval_t dt_imageio_open_hdr_preview(dt_image_t *img, const char *filename)
{
  dt_imageio_retval_t ret = dt_imageio_open_hdr(img, filename);
  if(ret != DT_IMAGEIO_OK) return ret;
  ret = dt_image_raw_to_preview(img);
  dt_image_release(img, DT_IMAGE_FULL, 'r');  // drop open_raw lock on full buffer.
  // this updates mipf/mip4..0 from raw pixels.
  int p_wd, p_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIPF, &p_wd, &p_ht);
  if(dt_image_alloc(img, DT_IMAGE_MIP4)) return DT_IMAGEIO_CACHE_FULL;
  dt_image_get(img, DT_IMAGE_MIPF, 'r');
  dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
  dt_image_check_buffer(img, DT_IMAGE_MIPF, 3*p_wd*p_ht*sizeof(float));
  ret = DT_IMAGEIO_OK;
  dt_imageio_preview_f_to_8(p_wd, p_ht, img->mipf, img->mip[DT_IMAGE_MIP4]);
  dt_imageio_preview_write(img, DT_IMAGE_MIP4);
  dt_image_release(img, DT_IMAGE_MIP4, 'w');
  ret = dt_image_update_mipmaps(img);
  dt_image_release(img, DT_IMAGE_MIPF, 'r');
  dt_image_release(img, DT_IMAGE_MIP4, 'r');
  return ret;
}

dt_imageio_retval_t dt_imageio_open_hdr(dt_image_t *img, const char *filename)
{
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_rgbe(img, filename);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) return ret;
  ret = dt_imageio_open_pfm(img, filename);
  return ret;
}

// only set mip4..0.
dt_imageio_retval_t dt_imageio_open_raw_preview(dt_image_t *img, const char *filename)
{
  (void) dt_exif_read(img, filename);
  // init libraw stuff
  // img = dt_image_cache_use(img->id, 'r');
  dt_imageio_retval_t retval = DT_IMAGEIO_OK;
  int ret;
  libraw_data_t *raw = libraw_init(0);
  libraw_processed_image_t *image = NULL;
  raw->params.half_size = 0; /* dcraw -h */
  raw->params.use_camera_wb = img->raw_params.wb_cam;
  raw->params.use_auto_wb = img->raw_params.wb_auto;
  raw->params.med_passes = img->raw_params.med_passes;
  raw->params.no_auto_bright = img->raw_params.no_auto_bright;
  raw->params.output_bps = 16;
  raw->params.user_flip = img->raw_params.user_flip;
  raw->params.gamm[0] = 1.0;
  raw->params.gamm[1] = 1.0;
  raw->params.user_qual = 0; // linear
  raw->params.four_color_rgb = img->raw_params.four_color_rgb;
  raw->params.use_camera_matrix = 0;//img->raw_params.cmatrix;
  raw->params.highlight = img->raw_params.highlight; //0 clip, 1 unclip, 2 blend, 3+ rebuild
  raw->params.threshold = img->raw_denoise_threshold;
  raw->params.auto_bright_thr = img->raw_auto_bright_threshold;
  ret = libraw_open_file(raw, filename);
  HANDLE_ERRORS(ret, 0);
  if(raw->idata.dng_version || (raw->sizes.width <= 1200 && raw->sizes.height <= 800))
  { // FIXME: this is a temporary bugfix avoiding segfaults for dng images. (and to avoid shrinking on small images).
    raw->params.user_qual = 0;
    raw->params.half_size = 0;
  }

  // get thumbnail
  ret = libraw_unpack_thumb(raw);

  if(!ret)
  {
    ret = libraw_adjust_sizes_info_only(raw);
    ret = 0;
    img->orientation = raw->sizes.flip;
    img->width  = raw->sizes.iwidth;
    img->height = raw->sizes.iheight;
    img->exif_iso = raw->other.iso_speed;
    img->exif_exposure = raw->other.shutter;
    img->exif_aperture = raw->other.aperture;
    img->exif_focal_length = raw->other.focal_len;
    strncpy(img->exif_maker, raw->idata.make, 20);
    strncpy(img->exif_model, raw->idata.model, 20);
    dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);
    image = libraw_dcraw_make_mem_thumb(raw, &ret);
    if(!image) goto try_full_raw;
    int p_wd, p_ht;
    float f_wd, f_ht;
    dt_image_get_mip_size(img, DT_IMAGE_MIP4, &p_wd, &p_ht);
    dt_image_get_exact_mip_size(img, DT_IMAGE_MIP4, &f_wd, &f_ht);
    if(image && image->type == LIBRAW_IMAGE_JPEG)
    {
      // JPEG: decode with magick (directly rescaled to mip4)
      const int orientation = img->orientation;// & 4 ? img->orientation : img->orientation ^ 1;
      dt_imageio_jpeg_t jpg;
      if(dt_imageio_jpeg_decompress_header(image->data, image->data_size, &jpg)) goto error_raw_corrupted;
      if(orientation & 4)
      {
        image->width  = jpg.height;
        image->height = jpg.width;
      }
      else
      {
        image->width  = jpg.width;
        image->height = jpg.height;
      }
      uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t)*jpg.width*jpg.height*4);
      if(dt_imageio_jpeg_decompress(&jpg, tmp))
      {
        free(tmp);
        goto error_raw_corrupted;
      }
      if(dt_image_alloc(img, DT_IMAGE_MIP4))
      {
        free(tmp);
        fprintf(stderr, "[raw_preview] could not alloc mip4!\n");
        goto error_raw_cache_full;
      }
      dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
      const int p_ht2 = orientation & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
      const int p_wd2 = orientation & 4 ? p_ht : p_wd;
      const int f_ht2 = MIN(p_ht2, (orientation & 4 ? f_wd : f_ht) + 1.0);
      const int f_wd2 = MIN(p_wd2, (orientation & 4 ? f_ht : f_wd) + 1.0);

      if(image->width == p_wd && image->height == p_ht)
      { // use 1:1
        for (int j=0; j < jpg.height; j++)
          for (int i=0; i < jpg.width; i++)
            for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+2-k] = tmp[4*jpg.width*j+4*i+k];
      }
      else
      { // scale to fit
        bzero(img->mip[DT_IMAGE_MIP4], 4*p_wd*p_ht*sizeof(uint8_t));
        const float scale = fmaxf(image->width/f_wd, image->height/f_ht);
        for(int j=0;j<p_ht2 && scale*j<jpg.height;j++) for(int i=0;i<p_wd2 && scale*i < jpg.width;i++)
        {
          uint8_t *cam = tmp + 4*((int)(scale*j)*jpg.width + (int)(scale*i));
          for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+2-k] = cam[k];
        }
      }
      free(tmp);
      dt_image_release(img, DT_IMAGE_MIP4, 'w');
      // store in db.
      dt_imageio_preview_write(img, DT_IMAGE_MIP4);
      retval = dt_image_update_mipmaps(img);

      // clean up raw stuff.
      libraw_recycle(raw);
      libraw_close(raw);
      free(image);
      if(retval == DT_IMAGEIO_OK)
        retval = dt_image_preview_to_raw(img);
      dt_image_release(img, DT_IMAGE_MIP4, 'r');
      // dt_image_cache_release(img, 'r');
      return retval;
    }
    else
    {
      // BMP: directly to mip4
      if (dt_image_alloc(img, DT_IMAGE_MIP4)) goto error_raw_cache_full;
      dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
      const int p_ht2 = raw->sizes.flip & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
      const int p_wd2 = raw->sizes.flip & 4 ? p_ht : p_wd;
      const int f_ht2 = MIN(p_ht2, (raw->sizes.flip & 4 ? f_wd : f_ht) + 1.0);
      const int f_wd2 = MIN(p_wd2, (raw->sizes.flip & 4 ? f_ht : f_wd) + 1.0);
      if(image->width == p_wd2 && image->height == p_ht2)
      { // use 1:1
        for(int j=0;j<image->height;j++) for(int i=0;i<image->width;i++)
        {
          uint8_t *cam = image->data + 3*(j*image->width + i);
          for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, raw->sizes.flip) + 2-k] = cam[k];
        }
      }
      else
      { // scale to fit
        bzero(img->mip[DT_IMAGE_MIP4], 4*p_wd*p_ht*sizeof(uint8_t));
        const float scale = fmaxf(image->width/f_wd, image->height/f_ht);
        for(int j=0;j<p_ht2 && scale*j<image->height;j++) for(int i=0;i<p_wd2 && scale*i < image->width;i++)
        {
          uint8_t *cam = image->data + 3*((int)(scale*j)*image->width + (int)(scale*i));
          for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, raw->sizes.flip) + 2-k] = cam[k];
        }
      }
      retval = dt_image_preview_to_raw(img);
      // store in db.
      dt_image_release(img, DT_IMAGE_MIP4, 'w');
      dt_imageio_preview_write(img, DT_IMAGE_MIP4);
      if(retval == DT_IMAGEIO_OK)
        retval = dt_image_update_mipmaps(img);
      // clean up raw stuff.
      libraw_recycle(raw);
      libraw_close(raw);
      free(image);
      dt_image_release(img, DT_IMAGE_MIP4, 'r');
      // dt_image_cache_release(img, 'r');
      return retval;
    }
  }

  // if no thumbnail: load shrinked raw to tmp buffer (use dt_imageio_load_raw)
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
try_full_raw:
  retval = dt_imageio_open_raw(img, filename);
  if(retval != DT_IMAGEIO_OK) return retval;
  retval = dt_image_raw_to_preview(img);
  dt_image_release(img, DT_IMAGE_FULL, 'r');  // drop open_raw lock on full buffer.
  // this updates mipf/mip4..0 from raw pixels.
  int p_wd, p_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIPF, &p_wd, &p_ht);
  if(dt_image_alloc(img, DT_IMAGE_MIP4)) return DT_IMAGEIO_CACHE_FULL;
  dt_image_get(img, DT_IMAGE_MIPF, 'r');
  dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
  dt_image_check_buffer(img, DT_IMAGE_MIPF, 3*p_wd*p_ht*sizeof(float));
  dt_imageio_preview_f_to_8(p_wd, p_ht, img->mipf, img->mip[DT_IMAGE_MIP4]);
  dt_imageio_preview_write(img, DT_IMAGE_MIP4);
  dt_image_release(img, DT_IMAGE_MIP4, 'w');
  retval = dt_image_update_mipmaps(img);
  dt_image_release(img, DT_IMAGE_MIPF, 'r');
  dt_image_release(img, DT_IMAGE_MIP4, 'r');
  return retval;

error_raw_cache_full:
  fprintf(stderr, "[imageio_open_raw_preview] could not get image from thumbnail!\n");
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  // dt_image_cache_release(img, 'r');
  return DT_IMAGEIO_CACHE_FULL;

error_raw_corrupted:
  fprintf(stderr, "[imageio_open_raw_preview] could not get image from thumbnail!\n");
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  // dt_image_cache_release(img, 'r');
  return DT_IMAGEIO_FILE_CORRUPTED;
}

dt_imageio_retval_t dt_imageio_open_raw(dt_image_t *img, const char *filename)
{
  (void) dt_exif_read(img, filename);
  // img = dt_image_cache_use(img->id, 'r');
  int ret;
  libraw_data_t *raw = libraw_init(0);
  libraw_processed_image_t *image = NULL;
  raw->params.half_size = 0; /* dcraw -h */
  raw->params.use_camera_wb = 0;//img->raw_params.wb_cam;
  raw->params.use_auto_wb = 0;//img->raw_params.wb_auto;
  raw->params.med_passes = img->raw_params.med_passes;
  raw->params.no_auto_bright = 1;//img->raw_params.no_auto_bright;
  // raw->params.filtering_mode |= LIBRAW_FILTERING_NOBLACKS;
  // raw->params.document_mode = 2; // no color scaling, no black, no max, no wb..?
  raw->params.dont_scale = 1;
  raw->params.output_color = 0;
  raw->params.output_bps = 16;
  raw->params.user_flip = img->raw_params.user_flip;
  raw->params.gamm[0] = 1.0;
  raw->params.gamm[1] = 1.0;
  raw->params.user_qual = img->raw_params.demosaic_method; // 3: AHD, 2: PPG, 1: VNG
  raw->params.four_color_rgb = img->raw_params.four_color_rgb;
  raw->params.use_camera_matrix = 0;//img->raw_params.cmatrix;
  raw->params.highlight = img->raw_params.highlight; //0 clip, 1 unclip, 2 blend, 3+ rebuild
  raw->params.threshold = img->raw_denoise_threshold;
  raw->params.auto_bright_thr = img->raw_auto_bright_threshold;
  ret = libraw_open_file(raw, filename);
  HANDLE_ERRORS(ret, 0);
  if(raw->idata.dng_version || (raw->sizes.width <= 1200 && raw->sizes.height <= 800))
  { // FIXME: this is a temporary bugfix avoiding segfaults for dng images. (and to avoid shrinking on small images).
    raw->params.user_qual = 0;
    raw->params.half_size = 0;
  }

  ret = libraw_unpack(raw);
  img->black   = raw->color.black/65535.0;
  img->maximum = raw->color.maximum/65535.0;
  HANDLE_ERRORS(ret, 1);
  ret = libraw_dcraw_process(raw);
  HANDLE_ERRORS(ret, 1);
  image = libraw_dcraw_make_mem_image(raw, &ret);
  HANDLE_ERRORS(ret, 1);

  img->orientation = raw->sizes.flip;
  img->width  = (img->orientation & 4) ? raw->sizes.height : raw->sizes.width;
  img->height = (img->orientation & 4) ? raw->sizes.width  : raw->sizes.height;
  img->exif_iso = raw->other.iso_speed;
  img->exif_exposure = raw->other.shutter;
  img->exif_aperture = raw->other.aperture;
  img->exif_focal_length = raw->other.focal_len;
  strncpy(img->exif_maker, raw->idata.make, 20);
  strncpy(img->exif_model, raw->idata.model, 20);
  dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);

  if(dt_image_alloc(img, DT_IMAGE_FULL))
  {
    libraw_recycle(raw);
    libraw_close(raw);
    free(image);
    // dt_image_cache_release(img, 'r');
    return DT_IMAGEIO_CACHE_FULL;
  }
  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*(img->width)*(img->height)*sizeof(float));
  const float m = 1./0xffff;
// #pragma omp parallel for schedule(static) shared(img, image)
  for(int k=0;k<3*(img->width)*(img->height);k++) img->pixels[k] = ((uint16_t *)(image->data))[k]*m;
  // clean up raw stuff.
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  raw = NULL;
  image = NULL;
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  // dt_image_cache_release(img, 'r');
  return DT_IMAGEIO_OK;
}

#if 0
static void MaxMidMin(int64_t p[3], int *maxc, int *midc, int *minc)
{
  if (p[0] > p[1] && p[0] > p[2]) {
    *maxc = 0;
    if (p[1] > p[2]) { *midc = 1; *minc = 2; }
    else { *midc = 2; *minc = 1; }
  } else if (p[1] > p[2]) {
    *maxc = 1;
    if (p[0] > p[2]) { *midc = 0; *minc = 2; }
    else { *midc = 2; *minc = 0; }
  } else {
    *maxc = 2;
    if (p[0] > p[1]) { *midc = 0; *minc = 1; }
    else { *midc = 1; *minc = 0; }
  }
}

// this is stolen from develop_linear (ufraw_developer.c), because it does a great job ;)
void dt_raw_develop(uint16_t *in, uint16_t *out, dt_image_t *img)
{
  int64_t tmppix[4];//, tmp;
  int64_t exposure = pow(2, img->exposure) * 0x10000;
  int clipped = 0;
  for(int c=0;c<img->raw->idata.colors;c++)
  {
    tmppix[c] = (uint64_t)in[c] * img->raw->color.cam_mul[c]/0x10000 - img->raw->color.black;
    if(tmppix[c] > img->raw->color.maximum) clipped = 1;
    tmppix[c] = tmppix[c] * exposure / img->raw->color.maximum;
    // tmppix[c] = (tmppix[c] * 0x10000) / img->raw.color.maximum;
  }
  if(clipped)
  {
    int64_t unclipped[3], clipped[3];
    for(int cc=0;cc<3;cc++)
    {
      // for(int c=0,tmp=0;c<img->raw->idata.colors;c++)
      //   tmp += tmppix[c] * img->raw->color.cmatrix[cc][c];
      // unclipped[cc] = MAX(tmp/0x10000, 0);
      unclipped[cc] = tmppix[cc];
    }
    for(int c=0; c<3; c++) tmppix[c] = MIN(tmppix[c], 0xFFFF);
    // for(int c=0; c<3; c++) tmppix[c] = MIN(tmppix[c], exposure);
    for(int cc=0; cc<3; cc++)
    {
      // for(int c=0, tmp=0; c<img->raw->idata.colors; c++)
      //   tmp += tmppix[c] * img->raw->color.cmatrix[cc][c];
      // clipped[cc] = MAX(tmp/0x10000, 0);
      clipped[cc] = tmppix[cc];
    }
    int maxc, midc, minc;
    MaxMidMin(unclipped, &maxc, &midc, &minc);
    int64_t unclippedLum = unclipped[maxc];
    int64_t clippedLum = clipped[maxc];
    int64_t clippedSat;
    if(clipped[maxc] < clipped[minc] || clipped[maxc] == 0)
      clippedSat = 0;
    else
      clippedSat = 0x10000 - clipped[minc] * 0x10000 / clipped[maxc];
    int64_t clippedHue;
    if(clipped[maxc] == clipped[minc]) clippedHue = 0;
    else clippedHue =
      (clipped[midc]-clipped[minc])*0x10000 /
        (clipped[maxc]-clipped[minc]);
    int64_t unclippedHue;
    if(unclipped[maxc] == unclipped[minc])
      unclippedHue = clippedHue;
    else
      unclippedHue =
        (unclipped[midc]-unclipped[minc])*0x10000 /
        (unclipped[maxc]-unclipped[minc]);
    int64_t lum = clippedLum + (unclippedLum - clippedLum) * 1/2;
    int64_t sat = clippedSat;
    int64_t hue = unclippedHue;

    tmppix[maxc] = lum;
    tmppix[minc] = lum * (0x10000-sat) / 0x10000;
    tmppix[midc] = lum * (0x10000-sat + sat*hue/0x10000) / 0x10000;
  }
  for(int c=0; c<3; c++)
    out[c] = MIN(MAX(tmppix[c], 0), 0xFFFF);
}
#endif

// =================================================
//   begin magickcore wrapper functions:
// =================================================

dt_imageio_retval_t dt_imageio_open_ldr_preview(dt_image_t *img, const char *filename)
{
  (void) dt_exif_read(img, filename);
  const int orientation = img->orientation == -1 ? 0 : (img->orientation & 4 ? img->orientation : img->orientation ^ 1);

  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_read_header(filename, &jpg)) return DT_IMAGEIO_FILE_CORRUPTED;
  if(orientation & 4)
  {
    img->width  = jpg.height;
    img->height = jpg.width;
  }
  else
  {
    img->width  = jpg.width;
    img->height = jpg.height;
  }
  uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t)*jpg.width*jpg.height*4);
  if(dt_imageio_jpeg_read(&jpg, tmp))
  {
    free(tmp);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  if(dt_image_alloc(img, DT_IMAGE_MIP4))
  {
    free(tmp);
    return DT_IMAGEIO_CACHE_FULL;
  }

  int p_wd, p_ht;
  float f_wd, f_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIP4, &p_wd, &p_ht);
  dt_image_get_exact_mip_size(img, DT_IMAGE_MIP4, &f_wd, &f_ht);

  // printf("mip sizes: %d %d -- %f %f\n", p_wd, p_ht, f_wd, f_ht);
  // FIXME: there is a black border on the left side of a portrait image!

  dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
  const int p_ht2 = orientation & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
  const int p_wd2 = orientation & 4 ? p_ht : p_wd;
  const int f_ht2 = MIN(p_ht2, (orientation & 4 ? f_wd : f_ht) + 1.0);
  const int f_wd2 = MIN(p_wd2, (orientation & 4 ? f_ht : f_wd) + 1.0);

  if(img->width == p_wd && img->height == p_ht)
  { // use 1:1
    for (int j=0; j < jpg.height; j++)
      for (int i=0; i < jpg.width; i++)
        for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+2-k] = tmp[4*jpg.width*j+4*i+k];
  }
  else
  { // scale to fit
    bzero(img->mip[DT_IMAGE_MIP4], 4*p_wd*p_ht*sizeof(uint8_t));
    const float scale = fmaxf(img->width/f_wd, img->height/f_ht);
    for(int j=0;j<p_ht2 && scale*j<jpg.height;j++) for(int i=0;i<p_wd2 && scale*i < jpg.width;i++)
    {
      uint8_t *cam = tmp + 4*((int)(scale*j)*jpg.width + (int)(scale*i));
      for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+2-k] = cam[k];
    }
  }
  free(tmp);
  dt_imageio_retval_t retval = dt_image_preview_to_raw(img);
  dt_image_release(img, DT_IMAGE_MIP4, 'w');
  // store in db.
  dt_imageio_preview_write(img, DT_IMAGE_MIP4);
  if(retval == DT_IMAGEIO_OK)
    retval = dt_image_update_mipmaps(img);
  dt_image_release(img, DT_IMAGE_MIP4, 'r');
  return retval;
}

// transparent read method to load ldr image to dt_raw_image_t with exif and so on.
dt_imageio_retval_t dt_imageio_open_ldr(dt_image_t *img, const char *filename)
{
  (void) dt_exif_read(img, filename);
  const int orientation = img->orientation == -1 ? 0 : (img->orientation & 4 ? img->orientation : img->orientation ^ 1);

  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_read_header(filename, &jpg)) return DT_IMAGEIO_FILE_CORRUPTED;
  if(orientation & 4)
  {
    img->width  = jpg.height;
    img->height = jpg.width;
  }
  else
  {
    img->width  = jpg.width;
    img->height = jpg.height;
  }
  uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t)*jpg.width*jpg.height*4);
  if(dt_imageio_jpeg_read(&jpg, tmp))
  {
    free(tmp);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  if(dt_image_alloc(img, DT_IMAGE_FULL))
  {
    free(tmp);
    return DT_IMAGEIO_CACHE_FULL;
  }
 
  const int ht2 = orientation & 4 ? img->width  : img->height; // pretend unrotated, rotate in write_pos
  const int wd2 = orientation & 4 ? img->height : img->width;
  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*img->width*img->height*sizeof(uint8_t));

  for(int j=0; j < jpg.height; j++)
    for(int i=0; i < jpg.width; i++)
      for(int k=0;k<3;k++) img->pixels[3*dt_imageio_write_pos(i, j, wd2, ht2, wd2, ht2, orientation)+k] = (1.0/255.0)*tmp[4*jpg.width*j+4*i+k];

  free(tmp);
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return DT_IMAGEIO_OK;
}

// batch-processing enabled write method: history stack, raw image, custom copy of gamma/tonecurves
int dt_imageio_export_f(dt_image_t *img, const char *filename)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));

  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_init_export(&pipe, wd, ht);
  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width, &pipe.processed_height);
  const int width  = dt_conf_get_int ("plugins/lighttable/export/width");
  const int height = dt_conf_get_int ("plugins/lighttable/export/height");
  const float scale = width > 0 && height > 0 ? fminf(1.0, fminf(width/(float)pipe.processed_width, height/(float)pipe.processed_height)) : 1.0f;
  const int processed_width  = scale*pipe.processed_width  + .5f;
  const int processed_height = scale*pipe.processed_height + .5f;
  dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, processed_width, processed_height, scale);
  float *buf = (float *)pipe.backbuf;

  int status = 1;
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "PF\n%d %d\n-1.0\n", processed_width, processed_height);
    for(int j=processed_height-1;j>=0;j--)
    {
      int cnt = fwrite(buf + 3*processed_width*j, sizeof(float)*3, processed_width, f);
      if(cnt != processed_width) status = 1;
      else status = 0;
    }
    fclose(f);
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  return status;
}

int dt_imageio_export_16(dt_image_t *img, const char *filename)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));

  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_init_export(&pipe, wd, ht);
  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width, &pipe.processed_height);
  const int width  = dt_conf_get_int ("plugins/lighttable/export/width");
  const int height = dt_conf_get_int ("plugins/lighttable/export/height");
  const float scale = width > 0 && height > 0 ? fminf(1.0, fminf(width/(float)pipe.processed_width, height/(float)pipe.processed_height)) : 1.0f;
  const int processed_width  = scale*pipe.processed_width  + .5f;
  const int processed_height = scale*pipe.processed_height + .5f;
  dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, processed_width,   processed_height, scale);
  float *buf = (float *)pipe.backbuf;

  int status = 1;
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "P6\n%d %d\n65535\n", processed_width, processed_height);
    for(int y=0;y<processed_height;y++)
    for(int x=0;x<processed_width ;x++)
    {
      const int k = x + processed_width*y;
      uint16_t tmp[3];
      for(int i=0;i<3;i++) tmp[i] = CLAMP(buf[3*k+i]*0x10000, 0, 0xffff);
      for(int i=0;i<3;i++) tmp[i] = (0xff00 & (tmp[i]<<8))|(tmp[i]>>8);
      int cnt = fwrite(tmp, sizeof(uint16_t)*3, 1, f);
      if(cnt != 1) break;
    }
    fclose(f);
    status = 0;
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  return status;
}

void dt_imageio_to_fractional(float in, uint32_t *num, uint32_t *den)
{
  if(!(in >= 0))
  {
    *num = *den = 0;
    return;
  }
  *den = 1;
  *num = (int)(in**den + .5f);
  while(fabsf(*num/(float)*den - in) > 0.001f)
  {
    *den *= 10;
    *num = (int)(in**den + .5f);
  }
}

int dt_imageio_export_8(dt_image_t *img, const char *filename)
{
#ifdef HAVE_MAGICK
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));

  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_init_export(&pipe, wd, ht);
  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_process(&pipe, &dev, 0, 0, wd, ht, 1.0);
  uint8_t *buf8 = pipe.backbuf;
  for(int k=0;k<wd*ht;k++) for(int i=0;i<3;i++) buf8[3*k+i] = buf8[4*k+2-i];

  // MagickCore write
  ImageInfo *image_info;
  ExceptionInfo *exception;
  Image *image;
  exception = AcquireExceptionInfo();
  image_info = CloneImageInfo((ImageInfo *) NULL);
  image = ConstituteImage(wd, ht, "RGB", CharPixel, buf8, exception);
  if (image == (Image *) NULL)
  {
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
    dt_dev_cleanup(&dev);
    free(buf8);
    return 1;
  }

  // set image properties!
  ExifRational rat;
  int length;
  uint8_t *exif_profile;
  ExifData *exif_data = exif_data_new();
  exif_data_set_byte_order(exif_data, EXIF_BYTE_ORDER_INTEL);
  ExifContent *exif_content = exif_data->ifd[0];
  ExifEntry *entry;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_MAKE);
  entry->components = strlen (img->exif_maker) + 1;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)entry->data, img->exif_maker, entry->components);

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_MODEL);
  entry->components = strlen (img->exif_model) + 1;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)entry->data, img->exif_model, entry->components);

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_DATE_TIME_ORIGINAL);
  entry->components = 20;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)(entry->data), img->exif_datetime_taken, entry->components);

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_ISO_SPEED_RATINGS);
  exif_set_short(entry->data, EXIF_BYTE_ORDER_INTEL, (int16_t)(img->exif_iso));
  entry->size = 2;
  entry->components = 1;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_FNUMBER);
  dt_imageio_to_fractional(img->exif_aperture, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_EXPOSURE_TIME);
  dt_imageio_to_fractional(img->exif_exposure, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_FOCAL_LENGTH);
  dt_imageio_to_fractional(img->exif_focal_length, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;

  exif_data_save_data(exif_data, &exif_profile, (uint32_t *)&length);

  exif_data_free(exif_data);
  StringInfo *profile = AcquireStringInfo(length);
  SetStringInfoDatum(profile, exif_profile);
  (void)SetImageProfile(image, "exif", profile);
  profile = DestroyStringInfo(profile);
  free(exif_profile);

  image_info->quality = 97;
  (void) strcpy(image->filename, filename);
  WriteImage(image_info, image);

  image = DestroyImage(image);
  image_info = DestroyImageInfo(image_info);
  exception = DestroyExceptionInfo(exception);

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  return 0;
#else
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));

  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_init_export(&pipe, wd, ht);
  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width, &pipe.processed_height);

  // find output color profile for this image:
  int sRGB = 1;
  gchar *overprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  if(!strcmp(overprofile, "sRGB"))
  {
    sRGB = 1;
  }
  else if(!strcmp(overprofile, "image"))
  {
    GList *modules = dev.iop;
    dt_iop_module_t *colorout = NULL;
    while (modules)
    {
      colorout = (dt_iop_module_t *)modules->data;
      if (strcmp(colorout->op, "colorout") == 0)
      {
        dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)colorout->params;
        if(!strcmp(p->iccprofile, "sRGB")) sRGB = 1;
        else sRGB = 0;
      }
      modules = g_list_next(modules);
    }
  }
  else
  {
    sRGB = 0;
  }
  g_free(overprofile);

  const int width  = dt_conf_get_int ("plugins/lighttable/export/width");
  const int height = dt_conf_get_int ("plugins/lighttable/export/height");
  const float scale = width > 0 && height > 0 ? fminf(1.0, fminf(width/(float)pipe.processed_width, height/(float)pipe.processed_height)) : 1.0f;
  const int processed_width  = scale*pipe.processed_width  + .5f;
  const int processed_height = scale*pipe.processed_height + .5f;
  dt_dev_pixelpipe_process(&pipe, &dev, 0, 0, processed_width,   processed_height, scale);
  char pathname[1024];
  dt_image_full_path(img, pathname, 1024);
  const char *suffix = filename + strlen(filename) - 3;
  int export_png = 0;
  if(suffix > filename && strncmp(suffix, "png", 3) == 0) export_png = 1;
  uint8_t *buf8 = pipe.backbuf;
  for(int y=0;y<processed_height;y++)
  for(int x=0;x<processed_width ;x++)
  {
    const int k = x + processed_width*y;
    uint8_t tmp = buf8[4*k+0];
    buf8[4*k+0] = buf8[4*k+2];
    buf8[4*k+2] = tmp;
  }

  int length;
  uint8_t exif_profile[65535]; // C++ alloc'ed buffer is uncool, so we waste some bits here.
  length = dt_exif_read_blob(exif_profile, pathname, sRGB);

  int quality = dt_conf_get_int ("plugins/lighttable/export/quality");
  if(quality <= 0 || quality > 100) quality = 100;
  if((!export_png && dt_imageio_jpeg_write(filename, buf8,             processed_width, processed_height, quality, exif_profile, length)) ||
     ( export_png && dt_imageio_png_write (filename, buf8,             processed_width, processed_height)))
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_dev_cleanup(&dev);
    return 1;
  }
  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  return 0;
#endif
}

// =================================================
//   combined reading
// =================================================

dt_imageio_retval_t dt_imageio_open(dt_image_t *img, const char *filename)
{ // first try hdr and raw loading
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_hdr(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_raw(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_ldr(img, filename);
  if(ret == DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL) dt_image_cache_flush(img);
  img->flags &= ~DT_IMAGE_THUMBNAIL;
  return ret;
}

dt_imageio_retval_t dt_imageio_open_preview(dt_image_t *img, const char *filename)
{ // first try hdr and raw loading
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_hdr_preview(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_raw_preview(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_ldr_preview(img, filename);
  if(ret == DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL) dt_image_cache_flush(img);
  return ret;
}

// =================================================
//   dt-file synching
// =================================================

int dt_imageio_dt_write (const int imgid, const char *filename)
{
  FILE *f = NULL;
  // read history from db
  sqlite3_stmt *stmt;
  int rc;
  size_t rd;
  dt_dev_operation_t op;
  rc = sqlite3_prepare_v2(darktable.db, "select * from history where imgid = ?1 order by num", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(!f) f = fopen(filename, "wb");
    if(!f) break;
    int32_t enabled = sqlite3_column_int(stmt, 5);
    rd = fwrite(&enabled, sizeof(int32_t), 1, f);
    snprintf(op, 20, "%s", (const char *)sqlite3_column_text(stmt, 3));
    rd = fwrite(op, 1, sizeof(op), f);
    int32_t len = sqlite3_column_bytes(stmt, 4);
    rd = fwrite(&len, sizeof(int32_t), 1, f);
    rd = fwrite(sqlite3_column_blob(stmt, 4), len, 1, f);
  }
  rc = sqlite3_finalize (stmt);
  if(f) fclose(f);
  else return 1;
  return 0;
}

int dt_imageio_dt_read (const int imgid, const char *filename)
{
  FILE *f = fopen(filename, "rb");
  if(!f) return 1;

  sqlite3_stmt *stmt;
  int rc, num = 0;
  size_t rd;
  rc = sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, imgid);
  sqlite3_step(stmt);
  rc = sqlite3_finalize (stmt);

  while(!feof(f))
  {
    int32_t enabled, len;
    dt_dev_operation_t op;
    rd = fread(&enabled, 1, sizeof(int32_t), f);
    if(rd < sizeof(int32_t)) break;
    rd = fread(op, 1, sizeof(dt_dev_operation_t), f);
    if(rd < sizeof(dt_dev_operation_t)) break;
    rd = fread(&len, 1, sizeof(int32_t), f);
    if(rd < sizeof(int32_t)) break;
    char *params = (char *)malloc(len);
    rd = fread(params, 1, len, f);
    if(rd < len) { free(params); break; }
    rc = sqlite3_prepare_v2(darktable.db, "select num from history where imgid = ?1 and num = ?2", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, imgid);
    rc = sqlite3_bind_int (stmt, 2, num);
    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
      rc = sqlite3_finalize(stmt);
      rc = sqlite3_prepare_v2(darktable.db, "insert into history (imgid, num) values (?1, ?2)", -1, &stmt, NULL);
      rc = sqlite3_bind_int (stmt, 1, imgid);
      rc = sqlite3_bind_int (stmt, 2, num);
      rc = sqlite3_step (stmt);
    }
    rc = sqlite3_finalize (stmt);
    rc = sqlite3_prepare_v2(darktable.db, "update history set operation = ?1, op_params = ?2, module = ?3, enabled = ?4 where imgid = ?5 and num = ?6", -1, &stmt, NULL);
    rc = sqlite3_bind_text(stmt, 1, op, strlen(op), SQLITE_TRANSIENT);
    rc = sqlite3_bind_blob(stmt, 2, params, len, SQLITE_TRANSIENT);
    rc = sqlite3_bind_int (stmt, 3, 666);
    rc = sqlite3_bind_int (stmt, 4, enabled);
    rc = sqlite3_bind_int (stmt, 5, imgid);
    rc = sqlite3_bind_int (stmt, 6, num);
    rc = sqlite3_step (stmt);
    rc = sqlite3_finalize (stmt);
    free(params);
    num ++;
  }
  return 0;
  fclose(f);
}


// =================================================
// tags synching
// =================================================

int dt_imageio_dttags_write (const int imgid, const char *filename)
{ // write out human-readable file containing images stars and tags.
  FILE *f = fopen(filename, "wb");
  if(!f) return 1;
  int stars = 1, rc = 1, raw_params = 0;
  float denoise = 0.0f, bright = 0.01f;
  // get stars from db
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select flags, raw_denoise_threshold, raw_auto_bright_threshold, raw_parameters from images where id = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    stars      = sqlite3_column_int(stmt, 0);
    denoise    = sqlite3_column_int(stmt, 1);
    bright     = sqlite3_column_int(stmt, 2);
    raw_params = sqlite3_column_int(stmt, 3);
  }
  rc = sqlite3_finalize(stmt);
  fprintf(f, "stars: %d\n", stars & 0x7);
  fprintf(f, "rawimport: %f %f %d\n", denoise, bright, raw_params);
  fprintf(f, "tags:\n");
  // print each tag in one line.
  rc = sqlite3_prepare_v2(darktable.db, "select name from tags join tagged_images on tagged_images.tagid = tags.id where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    fprintf(f, "%s\n", (char *)sqlite3_column_text(stmt, 0));
  rc = sqlite3_finalize(stmt);
  fclose(f);
  return 0;
}

int dt_imageio_dttags_read (dt_image_t *img, const char *filename)
{
  int stars = 1, rd = -1;
  int rc;
  sqlite3_stmt *stmt;
  char line[512];
  FILE *f = fopen(filename, "rb");
  if(!f) return 1;
  rd = fscanf(f, "stars: %d\n", &stars);
  if(rd != 1) return 2;
  // dt_image_t *img = dt_image_cache_get(imgid, 'w');
  img->flags |= 0x7 & stars;
  rd = fscanf(f, "rawimport: %f %f %d\n", &img->raw_denoise_threshold, &img->raw_auto_bright_threshold, (int32_t *)&img->raw_params);
  rd = fscanf(f, "%[^\n]\n", line);

  // consistency: strip all tags from image (tagged_image, tagxtag)
  rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count - 1 where "
      "(id2 in (select tagid from tagged_images where imgid = ?2)) or "
      "(id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, img->id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);

  // remove from tagged_images
  rc = sqlite3_prepare_v2(darktable.db, "delete from tagged_images where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, img->id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);

  if(!strcmp(line, "tags:"))
  {
    // while read line, add tag to db.
    while(fscanf(f, "%[^\n]\n", line) != EOF)
    {
      int tagid = -1;
      pthread_mutex_lock(&darktable.db_insert);
      // check if tag is available, get its id:
      for(int k=0;k<2;k++)
      {
        rc = sqlite3_prepare_v2(darktable.db, "select id from tags where name = ?1", -1, &stmt, NULL);
        rc = sqlite3_bind_text (stmt, 1, line, strlen(line), SQLITE_TRANSIENT);
        if(sqlite3_step(stmt) == SQLITE_ROW)
          tagid = sqlite3_column_int(stmt, 0);
        rc = sqlite3_finalize(stmt);
        if(tagid > 0)
        {
          if(k == 1)
          {
            rc = sqlite3_prepare_v2(darktable.db, "insert into tagxtag select id, ?1, 0 from tags", -1, &stmt, NULL);
            rc = sqlite3_bind_int(stmt, 1, tagid);
            rc = sqlite3_step(stmt);
            rc = sqlite3_finalize(stmt);
            rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = 1000000 where id1 = ?1 and id2 = ?1", -1, &stmt, NULL);
            rc = sqlite3_bind_int(stmt, 1, tagid);
            rc = sqlite3_step(stmt);
            rc = sqlite3_finalize(stmt);
          }
          break;
        }
        // create this tag (increment id, leave icon empty), retry.
        rc = sqlite3_prepare_v2(darktable.db, "insert into tags (id, name) values (null, ?1)", -1, &stmt, NULL);
        rc = sqlite3_bind_text (stmt, 1, line, strlen(line), SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        rc = sqlite3_finalize(stmt);
      }
      pthread_mutex_unlock(&darktable.db_insert);
      // associate image and tag.
      rc = sqlite3_prepare_v2(darktable.db, "insert into tagged_images (tagid, imgid) values (?1, ?2)", -1, &stmt, NULL);
      rc = sqlite3_bind_int (stmt, 1, tagid);
      rc = sqlite3_bind_int (stmt, 2, img->id);
      rc = sqlite3_step(stmt);
      rc = sqlite3_finalize(stmt);
      rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count + 1 where "
          "(id1 = ?1 and id2 in (select tagid from tagged_images where imgid = ?2)) or "
          "(id2 = ?1 and id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt, NULL);
      rc = sqlite3_bind_int(stmt, 1, tagid);
      rc = sqlite3_bind_int(stmt, 2, img->id);
      rc = sqlite3_step(stmt);
      rc = sqlite3_finalize(stmt);
    }
  }
  fclose(f);
  dt_image_cache_flush(img);
  // dt_image_cache_release(img, 'w');
  return 0;
}

