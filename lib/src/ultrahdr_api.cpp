/*
 * Copyright 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdio>
#include <cstring>

#include "ultrahdr_api.h"
#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/gainmapmath.h"
#include "ultrahdr/editorhelper.h"
#include "ultrahdr/jpegr.h"
#include "ultrahdr/jpegrutils.h"

static const uhdr_error_info_t g_no_error = {UHDR_CODEC_OK, 0, ""};

namespace ultrahdr {

uhdr_memory_block::uhdr_memory_block(size_t capacity) {
  m_buffer = std::make_unique<uint8_t[]>(capacity);
  m_capacity = capacity;
}

uhdr_raw_image_ext::uhdr_raw_image_ext(uhdr_img_fmt_t fmt_, uhdr_color_gamut_t cg_,
                                       uhdr_color_transfer_t ct_, uhdr_color_range_t range_,
                                       unsigned w_, unsigned h_, unsigned align_stride_to) {
  this->fmt = fmt_;
  this->cg = cg_;
  this->ct = ct_;
  this->range = range_;

  this->w = w_;
  this->h = h_;

  int aligned_width = ALIGNM(w_, align_stride_to);

  int bpp = 1;
  if (fmt_ == UHDR_IMG_FMT_24bppYCbCrP010) {
    bpp = 2;
  } else if (fmt_ == UHDR_IMG_FMT_32bppRGBA8888 || fmt_ == UHDR_IMG_FMT_32bppRGBA1010102) {
    bpp = 4;
  } else if (fmt_ == UHDR_IMG_FMT_64bppRGBAHalfFloat) {
    bpp = 8;
  }

  size_t plane_1_sz = bpp * aligned_width * h_;
  size_t plane_2_sz;
  size_t plane_3_sz;
  if (fmt_ == UHDR_IMG_FMT_24bppYCbCrP010) {
    plane_2_sz = (2 /* planes */ * ((aligned_width / 2) * (h_ / 2) * bpp));
    plane_3_sz = 0;
  } else if (fmt_ == UHDR_IMG_FMT_12bppYCbCr420) {
    plane_2_sz = (((aligned_width / 2) * (h_ / 2) * bpp));
    plane_3_sz = (((aligned_width / 2) * (h_ / 2) * bpp));
  } else {
    plane_2_sz = 0;
    plane_3_sz = 0;
  }
  size_t total_size = plane_1_sz + plane_2_sz + plane_3_sz;
  this->m_block = std::make_unique<uhdr_memory_block_t>(total_size);

  uint8_t* data = this->m_block->m_buffer.get();
  this->planes[UHDR_PLANE_Y] = data;
  this->stride[UHDR_PLANE_Y] = aligned_width;
  if (fmt_ == UHDR_IMG_FMT_24bppYCbCrP010) {
    this->planes[UHDR_PLANE_UV] = data + plane_1_sz;
    this->stride[UHDR_PLANE_UV] = aligned_width;
    this->planes[UHDR_PLANE_V] = nullptr;
    this->stride[UHDR_PLANE_V] = 0;
  } else if (fmt_ == UHDR_IMG_FMT_12bppYCbCr420) {
    this->planes[UHDR_PLANE_U] = data + plane_1_sz;
    this->stride[UHDR_PLANE_U] = aligned_width / 2;
    this->planes[UHDR_PLANE_V] = data + plane_1_sz + plane_2_sz;
    this->stride[UHDR_PLANE_V] = aligned_width / 2;
  } else {
    this->planes[UHDR_PLANE_U] = nullptr;
    this->stride[UHDR_PLANE_U] = 0;
    this->planes[UHDR_PLANE_V] = nullptr;
    this->stride[UHDR_PLANE_V] = 0;
  }
}

uhdr_compressed_image_ext::uhdr_compressed_image_ext(uhdr_color_gamut_t cg_,
                                                     uhdr_color_transfer_t ct_,
                                                     uhdr_color_range_t range_, unsigned size) {
  this->m_block = std::make_unique<uhdr_memory_block_t>(size);
  this->data = this->m_block->m_buffer.get();
  this->capacity = size;
  this->data_sz = 0;
  this->cg = cg_;
  this->ct = ct_;
  this->range = range_;
}

uhdr_error_info_t apply_effects(uhdr_encoder_private* enc) {
  for (auto& it : enc->m_effects) {
    std::unique_ptr<ultrahdr::uhdr_raw_image_ext_t> hdr_img = nullptr;
    std::unique_ptr<ultrahdr::uhdr_raw_image_ext_t> sdr_img = nullptr;

    if (nullptr != dynamic_cast<uhdr_rotate_effect_t*>(it)) {
      auto& hdr_raw_entry = enc->m_raw_images.find(UHDR_HDR_IMG)->second;
      hdr_img = apply_rotate(dynamic_cast<uhdr_rotate_effect_t*>(it), hdr_raw_entry.get());
      if (enc->m_raw_images.find(UHDR_SDR_IMG) != enc->m_raw_images.end()) {
        auto& sdr_raw_entry = enc->m_raw_images.find(UHDR_SDR_IMG)->second;
        sdr_img = apply_rotate(dynamic_cast<uhdr_rotate_effect_t*>(it), sdr_raw_entry.get());
      }
    } else if (nullptr != dynamic_cast<uhdr_mirror_effect_t*>(it)) {
      auto& hdr_raw_entry = enc->m_raw_images.find(UHDR_HDR_IMG)->second;
      hdr_img = apply_mirror(dynamic_cast<uhdr_mirror_effect_t*>(it), hdr_raw_entry.get());
      if (enc->m_raw_images.find(UHDR_SDR_IMG) != enc->m_raw_images.end()) {
        auto& sdr_raw_entry = enc->m_raw_images.find(UHDR_SDR_IMG)->second;
        sdr_img = apply_mirror(dynamic_cast<uhdr_mirror_effect_t*>(it), sdr_raw_entry.get());
      }
    } else if (nullptr != dynamic_cast<uhdr_crop_effect_t*>(it)) {
      auto crop_effect = dynamic_cast<uhdr_crop_effect_t*>(it);
      auto& hdr_raw_entry = enc->m_raw_images.find(UHDR_HDR_IMG)->second;
      int left = (std::max)(0, crop_effect->m_left);
      int right = (std::min)((int)hdr_raw_entry->w, crop_effect->m_right);
      int crop_width = right - left;
      if (crop_width <= 0 || (crop_width % 2 != 0)) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
                 "unexpected crop dimensions. crop width is expected to be > 0 and even, crop "
                 "width is %d",
                 crop_width);
        return status;
      }

      int top = (std::max)(0, crop_effect->m_top);
      int bottom = (std::min)((int)hdr_raw_entry->h, crop_effect->m_bottom);
      int crop_height = bottom - top;
      if (crop_height <= 0 || (crop_height % 2 != 0)) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
                 "unexpected crop dimensions. crop height is expected to be > 0 and even, crop "
                 "height is %d",
                 crop_height);
        return status;
      }
      apply_crop(hdr_raw_entry.get(), left, top, crop_width, crop_height);
      if (enc->m_raw_images.find(UHDR_SDR_IMG) != enc->m_raw_images.end()) {
        auto& sdr_raw_entry = enc->m_raw_images.find(UHDR_SDR_IMG)->second;
        apply_crop(sdr_raw_entry.get(), left, top, crop_width, crop_height);
      }
      continue;
    } else if (nullptr != dynamic_cast<uhdr_resize_effect_t*>(it)) {
      auto resize_effect = dynamic_cast<uhdr_resize_effect_t*>(it);
      int dst_w = resize_effect->m_width;
      int dst_h = resize_effect->m_height;
      if (dst_w == 0 || dst_h == 0 || dst_w % 2 != 0 || dst_h % 2 != 0) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        snprintf(status.detail, sizeof status.detail,
                 "destination dimension cannot be zero or odd. dest image width is %d, dest image "
                 "height is %d",
                 dst_w, dst_h);
        return status;
      }
      auto& hdr_raw_entry = enc->m_raw_images.find(UHDR_HDR_IMG)->second;
      hdr_img =
          apply_resize(dynamic_cast<uhdr_resize_effect_t*>(it), hdr_raw_entry.get(), dst_w, dst_h);
      if (enc->m_raw_images.find(UHDR_SDR_IMG) != enc->m_raw_images.end()) {
        auto& sdr_raw_entry = enc->m_raw_images.find(UHDR_SDR_IMG)->second;
        sdr_img = apply_resize(dynamic_cast<uhdr_resize_effect_t*>(it), sdr_raw_entry.get(), dst_w,
                               dst_h);
      }
    }

    if (hdr_img == nullptr ||
        (enc->m_raw_images.find(UHDR_SDR_IMG) != enc->m_raw_images.end() && sdr_img == nullptr)) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "encountered unknown error while applying effect %s", it->to_string().c_str());
      return status;
    }
    enc->m_raw_images.insert_or_assign(UHDR_HDR_IMG, std::move(hdr_img));
    if (sdr_img != nullptr) {
      enc->m_raw_images.insert_or_assign(UHDR_SDR_IMG, std::move(sdr_img));
    }
  }
  if (enc->m_effects.size() > 0) {
    auto it = enc->m_effects.back();
    if (nullptr != dynamic_cast<uhdr_crop_effect_t*>(it) &&
        enc->m_raw_images.find(UHDR_SDR_IMG) != enc->m_raw_images.end()) {
      // As cropping is handled via pointer arithmetic as opposed to buffer copy, u and v data of
      // yuv420 inputs are no longer contiguous. As the library does not accept distinct buffer
      // pointers for u and v for 420 input, copy the sdr intent to a contiguous buffer
      auto& sdr_raw_entry = enc->m_raw_images.find(UHDR_SDR_IMG)->second;
      enc->m_raw_images.insert_or_assign(UHDR_SDR_IMG,
                                         convert_raw_input_to_ycbcr(sdr_raw_entry.get()));
    }
  }

  return g_no_error;
}

uhdr_error_info_t apply_effects(uhdr_decoder_private* dec) {
  for (auto& it : dec->m_effects) {
    std::unique_ptr<ultrahdr::uhdr_raw_image_ext_t> disp_img = nullptr;
    std::unique_ptr<ultrahdr::uhdr_raw_image_ext_t> gm_img = nullptr;

    if (nullptr != dynamic_cast<uhdr_rotate_effect_t*>(it)) {
      disp_img =
          apply_rotate(dynamic_cast<uhdr_rotate_effect_t*>(it), dec->m_decoded_img_buffer.get());
      gm_img =
          apply_rotate(dynamic_cast<uhdr_rotate_effect_t*>(it), dec->m_gainmap_img_buffer.get());
    } else if (nullptr != dynamic_cast<uhdr_mirror_effect_t*>(it)) {
      disp_img =
          apply_mirror(dynamic_cast<uhdr_mirror_effect_t*>(it), dec->m_decoded_img_buffer.get());
      gm_img =
          apply_mirror(dynamic_cast<uhdr_mirror_effect_t*>(it), dec->m_gainmap_img_buffer.get());
    } else if (nullptr != dynamic_cast<uhdr_crop_effect_t*>(it)) {
      auto crop_effect = dynamic_cast<uhdr_crop_effect_t*>(it);
      uhdr_raw_image_t* disp = dec->m_decoded_img_buffer.get();
      uhdr_raw_image_t* gm = dec->m_gainmap_img_buffer.get();
      int left = (std::max)(0, crop_effect->m_left);
      int right = (std::min)((int)disp->w, crop_effect->m_right);
      if (right <= left) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(
            status.detail, sizeof status.detail,
            "unexpected crop dimensions. crop right is <= crop left, after crop image width is %d",
            right - left);
        return status;
      }

      int top = (std::max)(0, crop_effect->m_top);
      int bottom = (std::min)((int)disp->h, crop_effect->m_bottom);
      if (bottom <= top) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(
            status.detail, sizeof status.detail,
            "unexpected crop dimensions. crop bottom is <= crop top, after crop image height is %d",
            bottom - top);
        return status;
      }

      float wd_ratio = ((float)disp->w) / gm->w;
      float ht_ratio = ((float)disp->h) / gm->h;
      int gm_left = left / wd_ratio;
      int gm_right = right / wd_ratio;
      if (gm_right <= gm_left) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
                 "unexpected crop dimensions. crop right is <= crop left for gainmap image, after "
                 "crop gainmap image width is %d",
                 gm_right - gm_left);
        return status;
      }

      int gm_top = top / ht_ratio;
      int gm_bottom = bottom / ht_ratio;
      if (gm_bottom <= gm_top) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
                 "unexpected crop dimensions. crop bottom is <= crop top for gainmap image, after "
                 "crop gainmap image height is %d",
                 gm_bottom - gm_top);
        return status;
      }

      apply_crop(disp, left, top, right - left, bottom - top);
      apply_crop(gm, gm_left, gm_top, (gm_right - gm_left), (gm_bottom - gm_top));
      continue;
    } else if (nullptr != dynamic_cast<uhdr_resize_effect_t*>(it)) {
      auto resize_effect = dynamic_cast<uhdr_resize_effect_t*>(it);
      int dst_w = resize_effect->m_width;
      int dst_h = resize_effect->m_height;
      float wd_ratio =
          ((float)dec->m_decoded_img_buffer.get()->w) / dec->m_gainmap_img_buffer.get()->w;
      float ht_ratio =
          ((float)dec->m_decoded_img_buffer.get()->h) / dec->m_gainmap_img_buffer.get()->h;
      int dst_gm_w = dst_w / wd_ratio;
      int dst_gm_h = dst_h / ht_ratio;
      if (dst_w == 0 || dst_h == 0 || dst_gm_w == 0 || dst_gm_h == 0) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        snprintf(status.detail, sizeof status.detail,
                 "destination dimension cannot be zero. dest image width is %d, dest image height "
                 "is %d, dest gainmap width is %d, dest gainmap height is %d",
                 dst_w, dst_h, dst_gm_w, dst_gm_h);
        return status;
      }
      disp_img = apply_resize(dynamic_cast<uhdr_resize_effect_t*>(it),
                              dec->m_decoded_img_buffer.get(), dst_w, dst_h);
      gm_img = apply_resize(dynamic_cast<uhdr_resize_effect_t*>(it),
                            dec->m_gainmap_img_buffer.get(), dst_gm_w, dst_gm_h);
    }

    if (disp_img == nullptr || gm_img == nullptr) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "encountered unknown error while applying effect %s", it->to_string().c_str());
      return status;
    }
    dec->m_decoded_img_buffer = std::move(disp_img);
    dec->m_gainmap_img_buffer = std::move(gm_img);
  }

  return g_no_error;
}

}  // namespace ultrahdr

uhdr_codec_private::~uhdr_codec_private() {
  for (auto it : m_effects) delete it;
  m_effects.clear();
}

ultrahdr::ultrahdr_color_gamut map_cg_to_internal_cg(uhdr_color_gamut_t cg) {
  switch (cg) {
    case UHDR_CG_BT_2100:
      return ultrahdr::ULTRAHDR_COLORGAMUT_BT2100;
    case UHDR_CG_BT_709:
      return ultrahdr::ULTRAHDR_COLORGAMUT_BT709;
    case UHDR_CG_DISPLAY_P3:
      return ultrahdr::ULTRAHDR_COLORGAMUT_P3;
    default:
      return ultrahdr::ULTRAHDR_COLORGAMUT_UNSPECIFIED;
  }
}

uhdr_color_gamut_t map_internal_cg_to_cg(ultrahdr::ultrahdr_color_gamut cg) {
  switch (cg) {
    case ultrahdr::ULTRAHDR_COLORGAMUT_BT2100:
      return UHDR_CG_BT_2100;
    case ultrahdr::ULTRAHDR_COLORGAMUT_BT709:
      return UHDR_CG_BT_709;
    case ultrahdr::ULTRAHDR_COLORGAMUT_P3:
      return UHDR_CG_DISPLAY_P3;
    default:
      return UHDR_CG_UNSPECIFIED;
  }
}

ultrahdr::ultrahdr_transfer_function map_ct_to_internal_ct(uhdr_color_transfer_t ct) {
  switch (ct) {
    case UHDR_CT_HLG:
      return ultrahdr::ULTRAHDR_TF_HLG;
    case UHDR_CT_PQ:
      return ultrahdr::ULTRAHDR_TF_PQ;
    case UHDR_CT_LINEAR:
      return ultrahdr::ULTRAHDR_TF_LINEAR;
    case UHDR_CT_SRGB:
      return ultrahdr::ULTRAHDR_TF_SRGB;
    default:
      return ultrahdr::ULTRAHDR_TF_UNSPECIFIED;
  }
}

ultrahdr::ultrahdr_output_format map_ct_fmt_to_internal_output_fmt(uhdr_color_transfer_t ct,
                                                                   uhdr_img_fmt fmt) {
  if (ct == UHDR_CT_HLG && fmt == UHDR_IMG_FMT_32bppRGBA1010102) {
    return ultrahdr::ULTRAHDR_OUTPUT_HDR_HLG;
  } else if (ct == UHDR_CT_PQ && fmt == UHDR_IMG_FMT_32bppRGBA1010102) {
    return ultrahdr::ULTRAHDR_OUTPUT_HDR_PQ;
  } else if (ct == UHDR_CT_LINEAR && fmt == UHDR_IMG_FMT_64bppRGBAHalfFloat) {
    return ultrahdr::ULTRAHDR_OUTPUT_HDR_LINEAR;
  } else if (ct == UHDR_CT_SRGB && fmt == UHDR_IMG_FMT_32bppRGBA8888) {
    return ultrahdr::ULTRAHDR_OUTPUT_SDR;
  }
  return ultrahdr::ULTRAHDR_OUTPUT_UNSPECIFIED;
}

void map_internal_error_status_to_error_info(ultrahdr::status_t internal_status,
                                             uhdr_error_info_t& status) {
  if (internal_status == ultrahdr::JPEGR_NO_ERROR) {
    status = g_no_error;
  } else {
    status.has_detail = 1;
    if (internal_status == ultrahdr::ERROR_JPEGR_RESOLUTION_MISMATCH) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      snprintf(status.detail, sizeof status.detail,
               "dimensions of sdr intent and hdr intent do not match");
    } else if (internal_status == ultrahdr::ERROR_JPEGR_ENCODE_ERROR) {
      status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
      snprintf(status.detail, sizeof status.detail, "encountered unknown error during encoding");
    } else if (internal_status == ultrahdr::ERROR_JPEGR_DECODE_ERROR) {
      status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
      snprintf(status.detail, sizeof status.detail, "encountered unknown error during decoding");
    } else if (internal_status == ultrahdr::ERROR_JPEGR_NO_IMAGES_FOUND) {
      status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
      snprintf(status.detail, sizeof status.detail, "input uhdr image does not any valid images");
    } else if (internal_status == ultrahdr::ERROR_JPEGR_GAIN_MAP_IMAGE_NOT_FOUND) {
      status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
      snprintf(status.detail, sizeof status.detail,
               "input uhdr image does not contain gainmap image");
    } else if (internal_status == ultrahdr::ERROR_JPEGR_BUFFER_TOO_SMALL) {
      status.error_code = UHDR_CODEC_MEM_ERROR;
      snprintf(status.detail, sizeof status.detail,
               "output buffer to store compressed data is too small");
    } else if (internal_status == ultrahdr::ERROR_JPEGR_MULTIPLE_EXIFS_RECEIVED) {
      status.error_code = UHDR_CODEC_INVALID_OPERATION;
      snprintf(status.detail, sizeof status.detail,
               "received exif from uhdr_enc_set_exif_data() while the base image intent already "
               "contains exif, unsure which one to use");
    } else if (internal_status == ultrahdr::ERROR_JPEGR_UNSUPPORTED_MAP_SCALE_FACTOR) {
      status.error_code = UHDR_CODEC_UNSUPPORTED_FEATURE;
      snprintf(status.detail, sizeof status.detail,
               "say base image wd to gain map image wd ratio is 'k1' and base image ht to gain map "
               "image ht ratio is 'k2', we found k1 != k2.");
    } else {
      status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
      status.has_detail = 0;
    }
  }
}

uhdr_error_info_t uhdr_enc_validate_and_set_compressed_img(uhdr_codec_private_t* enc,
                                                           uhdr_compressed_image_t* img,
                                                           uhdr_img_label_t intent) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_encoder_private*>(enc) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (img == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for compressed image handle");
  } else if (img->data == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received nullptr for compressed img->data field");
  } else if (img->capacity < img->data_sz) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "img->capacity %d is less than img->data_sz %d",
             img->capacity, img->data_sz);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  if (handle->m_sailed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_encode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  auto entry = std::make_unique<ultrahdr::uhdr_compressed_image_ext_t>(img->cg, img->ct, img->range,
                                                                       img->data_sz);
  memcpy(entry->data, img->data, img->data_sz);
  entry->data_sz = img->data_sz;
  handle->m_compressed_images.insert_or_assign(intent, std::move(entry));

  return status;
}

uhdr_codec_private_t* uhdr_create_encoder(void) {
  uhdr_encoder_private* handle = new uhdr_encoder_private();

  if (handle != nullptr) {
    uhdr_reset_encoder(handle);
  }
  return handle;
}

void uhdr_release_encoder(uhdr_codec_private_t* enc) {
  if (dynamic_cast<uhdr_encoder_private*>(enc) != nullptr) {
    uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
    delete handle;
  }
}

UHDR_EXTERN uhdr_error_info_t uhdr_enc_set_using_multi_channel_gainmap(uhdr_codec_private_t* enc,
                                                                       bool use_multi_channel_gainmap) {
  uhdr_error_info_t status = g_no_error;
  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  if (handle == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  handle->m_use_multi_channel_gainmap = use_multi_channel_gainmap;
  return status;
}

UHDR_EXTERN uhdr_error_info_t uhdr_enc_set_gainmap_scale_factor(uhdr_codec_private_t* enc,
                                                                int gainmap_scale_factor) {
  uhdr_error_info_t status = g_no_error;
  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  if (handle == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  handle->m_gainmap_scale_factor = gainmap_scale_factor;
  return status;
}

uhdr_error_info_t uhdr_enc_set_raw_image(uhdr_codec_private_t* enc, uhdr_raw_image_t* img,
                                         uhdr_img_label_t intent) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_encoder_private*>(enc) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (img == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for raw image handle");
  } else if (intent != UHDR_HDR_IMG && intent != UHDR_SDR_IMG) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid intent %d, expects one of {UHDR_HDR_IMG, UHDR_SDR_IMG}", intent);
  } else if (intent == UHDR_HDR_IMG && (img->fmt != UHDR_IMG_FMT_24bppYCbCrP010 &&
                                        img->fmt != UHDR_IMG_FMT_32bppRGBA1010102)) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "unsupported input pixel format for hdr intent %d, expects one of "
             "{UHDR_IMG_FMT_24bppYCbCrP010, UHDR_IMG_FMT_32bppRGBA1010102}",
             img->fmt);
  } else if (intent == UHDR_SDR_IMG &&
             (img->fmt != UHDR_IMG_FMT_12bppYCbCr420 && img->fmt != UHDR_IMG_FMT_32bppRGBA8888)) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "unsupported input pixel format for sdr intent %d, expects one of "
             "{UHDR_IMG_FMT_12bppYCbCr420, UHDR_IMG_FMT_32bppRGBA8888}",
             img->fmt);
  } else if (img->cg != UHDR_CG_BT_2100 && img->cg != UHDR_CG_DISPLAY_P3 &&
             img->cg != UHDR_CG_BT_709) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid input color gamut %d, expects one of {UHDR_CG_BT_2100, UHDR_CG_DISPLAY_P3, "
             "UHDR_CG_BT_709}",
             img->cg);
  } else if (img->fmt == UHDR_IMG_FMT_12bppYCbCr420 && img->ct != UHDR_CT_SRGB) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid input color transfer for sdr intent image %d, expects UHDR_CT_SRGB", img->ct);
  } else if (img->fmt == UHDR_IMG_FMT_24bppYCbCrP010 &&
             (img->ct != UHDR_CT_HLG && img->ct != UHDR_CT_LINEAR && img->ct != UHDR_CT_PQ)) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid input color transfer for hdr intent image %d, expects one of {UHDR_CT_HLG, "
             "UHDR_CT_LINEAR, UHDR_CT_PQ}",
             img->ct);
  } else if (img->w % 2 != 0 || img->h % 2 != 0) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "image dimensions cannot be odd, received image dimensions %dx%d", img->w, img->h);
  } else if (img->w < ultrahdr::kMinWidth || img->h < ultrahdr::kMinHeight) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "image dimensions cannot be less than %dx%d, received image dimensions %dx%d",
             ultrahdr::kMinWidth, ultrahdr::kMinHeight, img->w, img->h);
  } else if (img->w > ultrahdr::kMaxWidth || img->h > ultrahdr::kMaxHeight) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "image dimensions cannot be larger than %dx%d, received image dimensions %dx%d",
             ultrahdr::kMaxWidth, ultrahdr::kMaxHeight, img->w, img->h);
  } else if (img->fmt == UHDR_IMG_FMT_24bppYCbCrP010) {
    if (img->planes[UHDR_PLANE_Y] == nullptr || img->planes[UHDR_PLANE_UV] == nullptr) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "received nullptr for data field(s), luma ptr %p, chroma_uv ptr %p",
               img->planes[UHDR_PLANE_Y], img->planes[UHDR_PLANE_UV]);
    } else if (img->stride[UHDR_PLANE_Y] < img->w) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "luma stride must not be smaller than width, stride=%d, width=%d",
               img->stride[UHDR_PLANE_Y], img->w);
    } else if (img->stride[UHDR_PLANE_UV] < img->w) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "chroma_uv stride must not be smaller than width, stride=%d, width=%d",
               img->stride[UHDR_PLANE_UV], img->w);
    }
  } else if (img->fmt == UHDR_IMG_FMT_12bppYCbCr420) {
    if (img->planes[UHDR_PLANE_Y] == nullptr || img->planes[UHDR_PLANE_U] == nullptr ||
        img->planes[UHDR_PLANE_V] == nullptr) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "received nullptr for data field(s) luma ptr %p, chroma_u ptr %p, chroma_v ptr %p",
               img->planes[UHDR_PLANE_Y], img->planes[UHDR_PLANE_U], img->planes[UHDR_PLANE_V]);
    } else if (img->stride[UHDR_PLANE_Y] < img->w) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "luma stride must not be smaller than width, stride=%d, width=%d",
               img->stride[UHDR_PLANE_Y], img->w);
    } else if (img->stride[UHDR_PLANE_U] < img->w / 2) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "chroma_u stride must not be smaller than width / 2, stride=%d, width=%d",
               img->stride[UHDR_PLANE_U], img->w);
    } else if (img->stride[UHDR_PLANE_V] < img->w / 2) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "chroma_v stride must not be smaller than width / 2, stride=%d, width=%d",
               img->stride[UHDR_PLANE_V], img->w);
    }
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  if (intent == UHDR_HDR_IMG &&
      handle->m_raw_images.find(UHDR_SDR_IMG) != handle->m_raw_images.end()) {
    auto& sdr_raw_entry = handle->m_raw_images.find(UHDR_SDR_IMG)->second;
    if (img->w != sdr_raw_entry->w || img->h != sdr_raw_entry->h) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "image resolutions mismatch: hdr intent: %dx%d, sdr intent: %dx%d", img->w, img->h,
               sdr_raw_entry->w, sdr_raw_entry->h);
      return status;
    }
  }
  if (intent == UHDR_SDR_IMG &&
      handle->m_raw_images.find(UHDR_HDR_IMG) != handle->m_raw_images.end()) {
    auto& hdr_raw_entry = handle->m_raw_images.find(UHDR_HDR_IMG)->second;
    if (img->w != hdr_raw_entry->w || img->h != hdr_raw_entry->h) {
      status.error_code = UHDR_CODEC_INVALID_PARAM;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "image resolutions mismatch: sdr intent: %dx%d, hdr intent: %dx%d", img->w, img->h,
               hdr_raw_entry->w, hdr_raw_entry->h);
      return status;
    }
  }
  if (handle->m_sailed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_encode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  std::unique_ptr<ultrahdr::uhdr_raw_image_ext_t> entry = ultrahdr::convert_raw_input_to_ycbcr(img);
  if (entry == nullptr) {
    status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "encountered unknown error during color space conversion");
    return status;
  }

  handle->m_raw_images.insert_or_assign(intent, std::move(entry));

  return status;
}

uhdr_error_info_t uhdr_enc_set_compressed_image(uhdr_codec_private_t* enc,
                                                uhdr_compressed_image_t* img,
                                                uhdr_img_label_t intent) {
  uhdr_error_info_t status = g_no_error;

  if (intent != UHDR_HDR_IMG && intent != UHDR_SDR_IMG && intent != UHDR_BASE_IMG) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid intent %d, expects one of {UHDR_HDR_IMG, UHDR_SDR_IMG, UHDR_BASE_IMG}",
             intent);
  }

  return uhdr_enc_validate_and_set_compressed_img(enc, img, intent);
}

uhdr_error_info_t uhdr_enc_set_gainmap_image(uhdr_codec_private_t* enc,
                                             uhdr_compressed_image_t* img,
                                             uhdr_gainmap_metadata_t* metadata) {
  uhdr_error_info_t status = g_no_error;

  if (metadata == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received nullptr for gainmap metadata descriptor");
  } else if (metadata->max_content_boost < metadata->min_content_boost) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received bad value for content boost min %f > max %f", metadata->min_content_boost,
             metadata->max_content_boost);
  } else if (metadata->gamma <= 0.0f) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received bad value for gamma %f, expects > 0.0f",
             metadata->gamma);
  } else if (metadata->offset_sdr < 0.0f) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received bad value for offset sdr %f, expects to be >= 0.0f", metadata->offset_sdr);
  } else if (metadata->offset_hdr < 0.0f) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received bad value for offset hdr %f, expects to be >= 0.0f", metadata->offset_hdr);
  } else if (metadata->hdr_capacity_max < metadata->hdr_capacity_min) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received bad value for hdr capacity min %f > max %f", metadata->hdr_capacity_min,
             metadata->hdr_capacity_max);
  } else if (metadata->hdr_capacity_min < 1.0f) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received bad value for hdr capacity min %f, expects to be >= 1.0f",
             metadata->hdr_capacity_min);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  status = uhdr_enc_validate_and_set_compressed_img(enc, img, UHDR_GAIN_MAP_IMG);
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  memcpy(&handle->m_metadata, metadata, sizeof *metadata);

  return status;
}

uhdr_error_info_t uhdr_enc_set_quality(uhdr_codec_private_t* enc, int quality,
                                       uhdr_img_label_t intent) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_encoder_private*>(enc) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (quality < 0 || quality > 100) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid quality factor %d, expects in range [0-100]", quality);
  } else if (intent != UHDR_HDR_IMG && intent != UHDR_SDR_IMG && intent != UHDR_BASE_IMG &&
             intent != UHDR_GAIN_MAP_IMG) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid intent %d, expects one of {UHDR_HDR_IMG, UHDR_SDR_IMG, UHDR_BASE_IMG, "
             "UHDR_GAIN_MAP_IMG}",
             intent);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  if (handle->m_sailed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_encode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  handle->m_quality.insert_or_assign(intent, quality);

  return status;
}

uhdr_error_info_t uhdr_enc_set_exif_data(uhdr_codec_private_t* enc, uhdr_mem_block_t* exif) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_encoder_private*>(enc) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (exif == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for exif image handle");
  } else if (exif->data == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for exif->data field");
  } else if (exif->capacity < exif->data_sz) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "exif->capacity %d is less than exif->data_sz %d",
             exif->capacity, exif->data_sz);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  if (handle->m_sailed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_encode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  uint8_t* data = static_cast<uint8_t*>(exif->data);
  std::vector<uint8_t> entry(data, data + exif->data_sz);
  handle->m_exif = std::move(entry);

  return status;
}

uhdr_error_info_t uhdr_enc_set_output_format(uhdr_codec_private_t* enc, uhdr_codec_t media_type) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_encoder_private*>(enc) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (media_type != UHDR_CODEC_JPG) {
    status.error_code = UHDR_CODEC_UNSUPPORTED_FEATURE;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid output format %d, expects {UHDR_CODEC_JPG}", media_type);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  if (handle->m_sailed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_encode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  handle->m_output_format = media_type;

  return status;
}

uhdr_error_info_t uhdr_encode(uhdr_codec_private_t* enc) {
  if (dynamic_cast<uhdr_encoder_private*>(enc) == nullptr) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);

  if (handle->m_sailed) {
    return handle->m_encode_call_status;
  }

  handle->m_sailed = true;

  uhdr_error_info_t& status = handle->m_encode_call_status;

  if (handle->m_compressed_images.find(UHDR_BASE_IMG) != handle->m_compressed_images.end() &&
      handle->m_compressed_images.find(UHDR_GAIN_MAP_IMG) != handle->m_compressed_images.end()) {
    if (handle->m_effects.size() != 0) {
      status.error_code = UHDR_CODEC_INVALID_OPERATION;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "image effects are not enabled for inputs with compressed intent");
      return status;
    }
  } else if (handle->m_raw_images.find(UHDR_HDR_IMG) != handle->m_raw_images.end()) {
    if (handle->m_compressed_images.find(UHDR_SDR_IMG) == handle->m_compressed_images.end() &&
        handle->m_raw_images.find(UHDR_SDR_IMG) == handle->m_raw_images.end()) {
      // api - 0
      if (handle->m_effects.size() != 0) {
        status = ultrahdr::apply_effects(handle);
        if (status.error_code != UHDR_CODEC_OK) return status;
      }
    } else if (handle->m_compressed_images.find(UHDR_SDR_IMG) !=
                   handle->m_compressed_images.end() &&
               handle->m_raw_images.find(UHDR_SDR_IMG) == handle->m_raw_images.end()) {
      if (handle->m_effects.size() != 0) {
        status.error_code = UHDR_CODEC_INVALID_OPERATION;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
                 "image effects are not enabled for inputs with compressed intent");
        return status;
      }
    } else if (handle->m_raw_images.find(UHDR_SDR_IMG) != handle->m_raw_images.end()) {
      if (handle->m_compressed_images.find(UHDR_SDR_IMG) == handle->m_compressed_images.end()) {
        if (handle->m_effects.size() != 0) {
          status = ultrahdr::apply_effects(handle);
          if (status.error_code != UHDR_CODEC_OK) return status;
        }
      } else {
        if (handle->m_effects.size() != 0) {
          status.error_code = UHDR_CODEC_INVALID_OPERATION;
          status.has_detail = 1;
          snprintf(status.detail, sizeof status.detail,
                   "image effects are not enabled for inputs with compressed intent");
          return status;
        }
      }
    }
  }

  ultrahdr::status_t internal_status = ultrahdr::JPEGR_NO_ERROR;
  if (handle->m_output_format == UHDR_CODEC_JPG) {
    ultrahdr::jpegr_exif_struct exif{};
    if (handle->m_exif.size() > 0) {
      exif.data = handle->m_exif.data();
      exif.length = handle->m_exif.size();
    }

    ultrahdr::JpegR jpegr(handle->m_gainmap_scale_factor,
                          handle->m_quality.find(UHDR_GAIN_MAP_IMG)->second,
                          handle->m_use_multi_channel_gainmap);
    ultrahdr::jpegr_compressed_struct dest{};
    if (handle->m_compressed_images.find(UHDR_BASE_IMG) != handle->m_compressed_images.end() &&
        handle->m_compressed_images.find(UHDR_GAIN_MAP_IMG) != handle->m_compressed_images.end()) {
      auto& base_entry = handle->m_compressed_images.find(UHDR_BASE_IMG)->second;
      ultrahdr::jpegr_compressed_struct primary_image;
      primary_image.data = base_entry->data;
      primary_image.length = primary_image.maxLength = base_entry->data_sz;
      primary_image.colorGamut = map_cg_to_internal_cg(base_entry->cg);

      auto& gainmap_entry = handle->m_compressed_images.find(UHDR_GAIN_MAP_IMG)->second;
      ultrahdr::jpegr_compressed_struct gainmap_image;
      gainmap_image.data = gainmap_entry->data;
      gainmap_image.length = gainmap_image.maxLength = gainmap_entry->data_sz;
      gainmap_image.colorGamut = map_cg_to_internal_cg(gainmap_entry->cg);

      ultrahdr::ultrahdr_metadata_struct metadata;
      metadata.version = ultrahdr::kJpegrVersion;
      metadata.maxContentBoost = handle->m_metadata.max_content_boost;
      metadata.minContentBoost = handle->m_metadata.min_content_boost;
      metadata.gamma = handle->m_metadata.gamma;
      metadata.offsetSdr = handle->m_metadata.offset_sdr;
      metadata.offsetHdr = handle->m_metadata.offset_hdr;
      metadata.hdrCapacityMin = handle->m_metadata.hdr_capacity_min;
      metadata.hdrCapacityMax = handle->m_metadata.hdr_capacity_max;

      size_t size = (std::max)((8 * 1024), 2 * (primary_image.length + gainmap_image.length));
      handle->m_compressed_output_buffer = std::make_unique<ultrahdr::uhdr_compressed_image_ext_t>(
          UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED, size);

      dest.data = handle->m_compressed_output_buffer->data;
      dest.length = 0;
      dest.maxLength = handle->m_compressed_output_buffer->capacity;
      dest.colorGamut = ultrahdr::ULTRAHDR_COLORGAMUT_UNSPECIFIED;

      // api - 4
      internal_status = jpegr.encodeJPEGR(&primary_image, &gainmap_image, &metadata, &dest);
      map_internal_error_status_to_error_info(internal_status, status);
    } else if (handle->m_raw_images.find(UHDR_HDR_IMG) != handle->m_raw_images.end()) {
      auto& hdr_raw_entry = handle->m_raw_images.find(UHDR_HDR_IMG)->second;

      size_t size = (std::max)((8u * 1024), hdr_raw_entry->w * hdr_raw_entry->h * 3 * 2);
      handle->m_compressed_output_buffer = std::make_unique<ultrahdr::uhdr_compressed_image_ext_t>(
          UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED, size);

      dest.data = handle->m_compressed_output_buffer->data;
      dest.length = 0;
      dest.maxLength = handle->m_compressed_output_buffer->capacity;
      dest.colorGamut = ultrahdr::ULTRAHDR_COLORGAMUT_UNSPECIFIED;

      ultrahdr::jpegr_uncompressed_struct p010_image;
      p010_image.data = hdr_raw_entry->planes[UHDR_PLANE_Y];
      p010_image.width = hdr_raw_entry->w;
      p010_image.height = hdr_raw_entry->h;
      p010_image.colorGamut = map_cg_to_internal_cg(hdr_raw_entry->cg);
      p010_image.luma_stride = hdr_raw_entry->stride[UHDR_PLANE_Y];
      p010_image.chroma_data = hdr_raw_entry->planes[UHDR_PLANE_UV];
      p010_image.chroma_stride = hdr_raw_entry->stride[UHDR_PLANE_UV];
      p010_image.colorRange = hdr_raw_entry->range;
      p010_image.pixelFormat = hdr_raw_entry->fmt;

      if (handle->m_compressed_images.find(UHDR_SDR_IMG) == handle->m_compressed_images.end() &&
          handle->m_raw_images.find(UHDR_SDR_IMG) == handle->m_raw_images.end()) {
        // api - 0
        internal_status = jpegr.encodeJPEGR(&p010_image, map_ct_to_internal_ct(hdr_raw_entry->ct),
                                            &dest, handle->m_quality.find(UHDR_BASE_IMG)->second,
                                            handle->m_exif.size() > 0 ? &exif : nullptr);
      } else if (handle->m_compressed_images.find(UHDR_SDR_IMG) !=
                     handle->m_compressed_images.end() &&
                 handle->m_raw_images.find(UHDR_SDR_IMG) == handle->m_raw_images.end()) {
        auto& sdr_compressed_entry = handle->m_compressed_images.find(UHDR_SDR_IMG)->second;
        ultrahdr::jpegr_compressed_struct sdr_compressed_image;
        sdr_compressed_image.data = sdr_compressed_entry->data;
        sdr_compressed_image.length = sdr_compressed_image.maxLength =
            sdr_compressed_entry->data_sz;
        sdr_compressed_image.colorGamut = map_cg_to_internal_cg(sdr_compressed_entry->cg);
        // api - 3
        internal_status = jpegr.encodeJPEGR(&p010_image, &sdr_compressed_image,
                                            map_ct_to_internal_ct(hdr_raw_entry->ct), &dest);
      } else if (handle->m_raw_images.find(UHDR_SDR_IMG) != handle->m_raw_images.end()) {
        auto& sdr_raw_entry = handle->m_raw_images.find(UHDR_SDR_IMG)->second;

        ultrahdr::jpegr_uncompressed_struct yuv420_image;
        yuv420_image.data = sdr_raw_entry->planes[UHDR_PLANE_Y];
        yuv420_image.width = sdr_raw_entry->w;
        yuv420_image.height = sdr_raw_entry->h;
        yuv420_image.colorGamut = map_cg_to_internal_cg(sdr_raw_entry->cg);
        yuv420_image.luma_stride = sdr_raw_entry->stride[UHDR_PLANE_Y];
        yuv420_image.chroma_data = nullptr;
        yuv420_image.chroma_stride = 0;
        yuv420_image.pixelFormat = sdr_raw_entry->fmt;

        if (handle->m_compressed_images.find(UHDR_SDR_IMG) == handle->m_compressed_images.end()) {
          // api - 1
          internal_status = jpegr.encodeJPEGR(&p010_image, &yuv420_image,
                                              map_ct_to_internal_ct(hdr_raw_entry->ct), &dest,
                                              handle->m_quality.find(UHDR_BASE_IMG)->second,
                                              handle->m_exif.size() > 0 ? &exif : nullptr);
        } else {
          auto& sdr_compressed_entry = handle->m_compressed_images.find(UHDR_SDR_IMG)->second;
          ultrahdr::jpegr_compressed_struct sdr_compressed_image;
          sdr_compressed_image.data = sdr_compressed_entry->data;
          sdr_compressed_image.length = sdr_compressed_image.maxLength =
              sdr_compressed_entry->data_sz;
          sdr_compressed_image.colorGamut = map_cg_to_internal_cg(sdr_compressed_entry->cg);

          // api - 2
          internal_status = jpegr.encodeJPEGR(&p010_image, &yuv420_image, &sdr_compressed_image,
                                              map_ct_to_internal_ct(hdr_raw_entry->ct), &dest);
        }
      }
      map_internal_error_status_to_error_info(internal_status, status);
    } else {
      status.error_code = UHDR_CODEC_INVALID_OPERATION;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "resources required for uhdr_encode() operation are not present");
    }
    if (status.error_code == UHDR_CODEC_OK) {
      handle->m_compressed_output_buffer->data_sz = dest.length;
      handle->m_compressed_output_buffer->cg = map_internal_cg_to_cg(dest.colorGamut);
    }
  }

  return status;
}

uhdr_compressed_image_t* uhdr_get_encoded_stream(uhdr_codec_private_t* enc) {
  if (dynamic_cast<uhdr_encoder_private*>(enc) == nullptr) {
    return nullptr;
  }

  uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  if (!handle->m_sailed || handle->m_encode_call_status.error_code != UHDR_CODEC_OK) {
    return nullptr;
  }

  return handle->m_compressed_output_buffer.get();
}

void uhdr_reset_encoder(uhdr_codec_private_t* enc) {
  if (dynamic_cast<uhdr_encoder_private*>(enc) != nullptr) {
    uhdr_encoder_private* handle = dynamic_cast<uhdr_encoder_private*>(enc);

    // clear entries and restore defaults
    for (auto it : handle->m_effects) delete it;
    handle->m_effects.clear();
    handle->m_raw_images.clear();
    handle->m_compressed_images.clear();
    handle->m_quality.clear();
    handle->m_quality.emplace(UHDR_HDR_IMG, 95);
    handle->m_quality.emplace(UHDR_SDR_IMG, 95);
    handle->m_quality.emplace(UHDR_BASE_IMG, 95);
    handle->m_quality.emplace(UHDR_GAIN_MAP_IMG, ultrahdr::kMapCompressQualityDefault);
    handle->m_exif.clear();
    handle->m_output_format = UHDR_CODEC_JPG;
    handle->m_gainmap_scale_factor = ultrahdr::kMapDimensionScaleFactorDefault;
    handle->m_use_multi_channel_gainmap = ultrahdr::kUseMultiChannelGainMapDefault;

    handle->m_sailed = false;
    handle->m_compressed_output_buffer.reset();
    handle->m_encode_call_status = g_no_error;
  }
}

int is_uhdr_image(void* data, int size) {
#define RET_IF_ERR(x)                         \
  {                                           \
    uhdr_error_info_t status = (x);           \
    if (status.error_code != UHDR_CODEC_OK) { \
      uhdr_release_decoder(obj);              \
      return 0;                               \
    }                                         \
  }

  uhdr_codec_private_t* obj = uhdr_create_decoder();
  uhdr_compressed_image_t uhdr_image;
  uhdr_image.data = data;
  uhdr_image.data_sz = size;
  uhdr_image.capacity = size;
  uhdr_image.cg = UHDR_CG_UNSPECIFIED;
  uhdr_image.ct = UHDR_CT_UNSPECIFIED;
  uhdr_image.range = UHDR_CR_UNSPECIFIED;

  RET_IF_ERR(uhdr_dec_set_image(obj, &uhdr_image));
  RET_IF_ERR(uhdr_dec_probe(obj));
#undef RET_IF_ERR

  uhdr_release_decoder(obj);

  return 1;
}

uhdr_codec_private_t* uhdr_create_decoder(void) {
  uhdr_decoder_private* handle = new uhdr_decoder_private();

  if (handle != nullptr) {
    uhdr_reset_decoder(handle);
  }
  return handle;
}

void uhdr_release_decoder(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) != nullptr) {
    uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
    delete handle;
  }
}

uhdr_error_info_t uhdr_dec_set_image(uhdr_codec_private_t* dec, uhdr_compressed_image_t* img) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (img == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for compressed image handle");
  } else if (img->data == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received nullptr for compressed img->data field");
  } else if (img->capacity < img->data_sz) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "img->capacity %d is less than img->data_sz %d",
             img->capacity, img->data_sz);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (handle->m_probed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_decode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  handle->m_uhdr_compressed_img = std::make_unique<ultrahdr::uhdr_compressed_image_ext_t>(
      img->cg, img->ct, img->range, img->data_sz);
  memcpy(handle->m_uhdr_compressed_img->data, img->data, img->data_sz);
  handle->m_uhdr_compressed_img->data_sz = img->data_sz;

  return status;
}

uhdr_error_info_t uhdr_dec_set_out_img_format(uhdr_codec_private_t* dec, uhdr_img_fmt_t fmt) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (fmt != UHDR_IMG_FMT_32bppRGBA8888 && fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat &&
             fmt != UHDR_IMG_FMT_32bppRGBA1010102) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid output format %d, expects one of {UHDR_IMG_FMT_32bppRGBA8888,  "
             "UHDR_IMG_FMT_64bppRGBAHalfFloat, UHDR_IMG_FMT_32bppRGBA1010102}",
             fmt);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (handle->m_probed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_decode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  handle->m_output_fmt = fmt;

  return status;
}

uhdr_error_info_t uhdr_dec_set_out_color_transfer(uhdr_codec_private_t* dec,
                                                  uhdr_color_transfer_t ct) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (ct != UHDR_CT_HLG && ct != UHDR_CT_PQ && ct != UHDR_CT_LINEAR && ct != UHDR_CT_SRGB) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid output color transfer %d, expects one of {UHDR_CT_HLG, UHDR_CT_PQ, "
             "UHDR_CT_LINEAR, UHDR_CT_SRGB}",
             ct);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (handle->m_probed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_decode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  handle->m_output_ct = ct;

  return status;
}

uhdr_error_info_t uhdr_dec_set_out_max_display_boost(uhdr_codec_private_t* dec,
                                                     float display_boost) {
  uhdr_error_info_t status = g_no_error;

  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
  } else if (display_boost < 1.0f) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "invalid display boost %f, expects to be >= 1.0f}", display_boost);
  }
  if (status.error_code != UHDR_CODEC_OK) return status;

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (handle->m_probed) {
    status.error_code = UHDR_CODEC_INVALID_OPERATION;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "An earlier call to uhdr_decode() has switched the context from configurable state to "
             "end state. The context is no longer configurable. To reuse, call reset()");
    return status;
  }

  handle->m_output_max_disp_boost = display_boost;

  return status;
}

uhdr_error_info_t uhdr_dec_probe(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  uhdr_error_info_t& status = handle->m_probe_call_status;

  if (!handle->m_probed) {
    handle->m_probed = true;

    if (handle->m_uhdr_compressed_img.get() == nullptr) {
      status.error_code = UHDR_CODEC_INVALID_OPERATION;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail, "did not receive any image for decoding");
      return status;
    }

    ultrahdr::jpeg_info_struct primary_image;
    ultrahdr::jpeg_info_struct gainmap_image;
    ultrahdr::jpegr_info_struct jpegr_info;
    jpegr_info.primaryImgInfo = &primary_image;
    jpegr_info.gainmapImgInfo = &gainmap_image;

    ultrahdr::jpegr_compressed_struct uhdr_image;
    uhdr_image.data = handle->m_uhdr_compressed_img->data;
    uhdr_image.length = uhdr_image.maxLength = handle->m_uhdr_compressed_img->data_sz;
    uhdr_image.colorGamut = map_cg_to_internal_cg(handle->m_uhdr_compressed_img->cg);

    ultrahdr::JpegR jpegr;
    ultrahdr::status_t internal_status = jpegr.getJPEGRInfo(&uhdr_image, &jpegr_info);
    map_internal_error_status_to_error_info(internal_status, status);
    if (status.error_code != UHDR_CODEC_OK) return status;

    ultrahdr::ultrahdr_metadata_struct metadata;
    if (ultrahdr::getMetadataFromXMP(gainmap_image.xmpData.data(), gainmap_image.xmpData.size(),
                                     &metadata)) {
      handle->m_metadata.max_content_boost = metadata.maxContentBoost;
      handle->m_metadata.min_content_boost = metadata.minContentBoost;
      handle->m_metadata.gamma = metadata.gamma;
      handle->m_metadata.offset_sdr = metadata.offsetSdr;
      handle->m_metadata.offset_hdr = metadata.offsetHdr;
      handle->m_metadata.hdr_capacity_min = metadata.hdrCapacityMin;
      handle->m_metadata.hdr_capacity_max = metadata.hdrCapacityMax;
    } else {
      status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail, "encountered error while parsing metadata");
      return status;
    }

    handle->m_img_wd = primary_image.width;
    handle->m_img_ht = primary_image.height;
    handle->m_gainmap_wd = gainmap_image.width;
    handle->m_gainmap_ht = gainmap_image.height;
    handle->m_exif = std::move(primary_image.exifData);
    handle->m_exif_block.data = handle->m_exif.data();
    handle->m_exif_block.data_sz = handle->m_exif_block.capacity = handle->m_exif.size();
    handle->m_icc = std::move(primary_image.iccData);
    handle->m_icc_block.data = handle->m_icc.data();
    handle->m_icc_block.data_sz = handle->m_icc_block.capacity = handle->m_icc.size();
    handle->m_base_xmp = std::move(primary_image.xmpData);
    handle->m_gainmap_xmp = std::move(gainmap_image.xmpData);
  }

  return status;
}

int uhdr_dec_get_image_width(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return -1;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_probed || handle->m_probe_call_status.error_code != UHDR_CODEC_OK) {
    return -1;
  }

  return handle->m_img_wd;
}

int uhdr_dec_get_image_height(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return -1;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_probed || handle->m_probe_call_status.error_code != UHDR_CODEC_OK) {
    return -1;
  }

  return handle->m_img_ht;
}

int uhdr_dec_get_gainmap_width(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return -1;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_probed || handle->m_probe_call_status.error_code != UHDR_CODEC_OK) {
    return -1;
  }

  return handle->m_gainmap_wd;
}

int uhdr_dec_get_gainmap_height(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return -1;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_probed || handle->m_probe_call_status.error_code != UHDR_CODEC_OK) {
    return -1;
  }

  return handle->m_gainmap_ht;
}

uhdr_mem_block_t* uhdr_dec_get_exif(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return nullptr;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_probed || handle->m_probe_call_status.error_code != UHDR_CODEC_OK) {
    return nullptr;
  }

  return &handle->m_exif_block;
}

uhdr_mem_block_t* uhdr_dec_get_icc(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return nullptr;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_probed || handle->m_probe_call_status.error_code != UHDR_CODEC_OK) {
    return nullptr;
  }

  return &handle->m_icc_block;
}

uhdr_gainmap_metadata_t* uhdr_dec_get_gain_map_metadata(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return nullptr;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_probed || handle->m_probe_call_status.error_code != UHDR_CODEC_OK) {
    return nullptr;
  }

  return &handle->m_metadata;
}

uhdr_error_info_t uhdr_decode(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);

  if (handle->m_sailed) {
    return handle->m_decode_call_status;
  }

  uhdr_error_info_t& status = handle->m_decode_call_status;
  status = uhdr_dec_probe(dec);
  if (status.error_code != UHDR_CODEC_OK) return status;

  handle->m_sailed = true;

  ultrahdr::ultrahdr_output_format outputFormat =
      map_ct_fmt_to_internal_output_fmt(handle->m_output_ct, handle->m_output_fmt);
  if (outputFormat == ultrahdr::ultrahdr_output_format::ULTRAHDR_OUTPUT_UNSPECIFIED) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "unsupported output pixel format and output color transfer pair");
    return status;
  }

  ultrahdr::jpegr_compressed_struct uhdr_image;
  uhdr_image.data = handle->m_uhdr_compressed_img->data;
  uhdr_image.length = uhdr_image.maxLength = handle->m_uhdr_compressed_img->data_sz;
  uhdr_image.colorGamut = map_cg_to_internal_cg(handle->m_uhdr_compressed_img->cg);

  handle->m_decoded_img_buffer = std::make_unique<ultrahdr::uhdr_raw_image_ext_t>(
      handle->m_output_fmt, UHDR_CG_UNSPECIFIED, handle->m_output_ct, UHDR_CR_UNSPECIFIED,
      handle->m_img_wd, handle->m_img_ht, 1);
  // alias
  ultrahdr::jpegr_uncompressed_struct dest;
  dest.data = handle->m_decoded_img_buffer->planes[UHDR_PLANE_PACKED];
  dest.colorGamut = ultrahdr::ULTRAHDR_COLORGAMUT_UNSPECIFIED;

  handle->m_gainmap_img_buffer = std::make_unique<ultrahdr::uhdr_raw_image_ext_t>(
      UHDR_IMG_FMT_8bppYCbCr400, UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED,
      handle->m_gainmap_wd, handle->m_gainmap_ht, 1);
  // alias
  ultrahdr::jpegr_uncompressed_struct dest_gainmap;
  dest_gainmap.data = handle->m_gainmap_img_buffer->planes[UHDR_PLANE_Y];

  ultrahdr::JpegR jpegr;
  ultrahdr::status_t internal_status =
      jpegr.decodeJPEGR(&uhdr_image, &dest, handle->m_output_max_disp_boost, nullptr, outputFormat,
                        &dest_gainmap, nullptr);
  map_internal_error_status_to_error_info(internal_status, status);
  if (status.error_code == UHDR_CODEC_OK) {
    handle->m_decoded_img_buffer->cg = map_internal_cg_to_cg(dest.colorGamut);
  }

  if (status.error_code == UHDR_CODEC_OK && dec->m_effects.size() != 0) {
    status = ultrahdr::apply_effects(handle);
  }

  return status;
}

uhdr_raw_image_t* uhdr_get_decoded_image(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return nullptr;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_sailed || handle->m_decode_call_status.error_code != UHDR_CODEC_OK) {
    return nullptr;
  }

  return handle->m_decoded_img_buffer.get();
}

uhdr_raw_image_t* uhdr_get_gain_map_image(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) == nullptr) {
    return nullptr;
  }

  uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);
  if (!handle->m_sailed || handle->m_decode_call_status.error_code != UHDR_CODEC_OK) {
    return nullptr;
  }

  return handle->m_gainmap_img_buffer.get();
}

void uhdr_reset_decoder(uhdr_codec_private_t* dec) {
  if (dynamic_cast<uhdr_decoder_private*>(dec) != nullptr) {
    uhdr_decoder_private* handle = dynamic_cast<uhdr_decoder_private*>(dec);

    // clear entries and restore defaults
    for (auto it : handle->m_effects) delete it;
    handle->m_effects.clear();
    handle->m_uhdr_compressed_img.reset();
    handle->m_output_fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
    handle->m_output_ct = UHDR_CT_LINEAR;
    handle->m_output_max_disp_boost = FLT_MAX;

    // ready to be configured
    handle->m_probed = false;
    handle->m_sailed = false;
    handle->m_decoded_img_buffer.reset();
    handle->m_gainmap_img_buffer.reset();
    handle->m_img_wd = 0;
    handle->m_img_ht = 0;
    handle->m_gainmap_wd = 0;
    handle->m_gainmap_ht = 0;
    handle->m_exif.clear();
    memset(&handle->m_exif_block, 0, sizeof handle->m_exif_block);
    handle->m_icc.clear();
    memset(&handle->m_icc_block, 0, sizeof handle->m_icc_block);
    handle->m_base_xmp.clear();
    handle->m_gainmap_xmp.clear();
    memset(&handle->m_metadata, 0, sizeof handle->m_metadata);
    handle->m_probe_call_status = g_no_error;
    handle->m_decode_call_status = g_no_error;
  }
}

uhdr_error_info_t uhdr_add_effect_mirror(uhdr_codec_private_t* codec,
                                         uhdr_mirror_direction_t direction) {
  uhdr_error_info_t status = g_no_error;

  if (codec == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  if (direction != UHDR_MIRROR_HORIZONTAL && direction != UHDR_MIRROR_VERTICAL) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(
        status.detail, sizeof status.detail,
        "unsupported direction, expects one of {UHDR_MIRROR_HORIZONTAL, UHDR_MIRROR_VERTICAL}");
    return status;
  }

  codec->m_effects.push_back(new ultrahdr::uhdr_mirror_effect_t(direction));

  return status;
}

uhdr_error_info_t uhdr_add_effect_rotate(uhdr_codec_private_t* codec, int degrees) {
  uhdr_error_info_t status = g_no_error;

  if (codec == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  if (degrees != 90 && degrees != 180 && degrees != 270) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "unsupported degrees, expects one of {90, 180, 270}");
    return status;
  }

  codec->m_effects.push_back(new ultrahdr::uhdr_rotate_effect_t(degrees));

  return status;
}

uhdr_error_info_t uhdr_add_effect_crop(uhdr_codec_private_t* codec, int left, int right, int top,
                                       int bottom) {
  uhdr_error_info_t status = g_no_error;

  if (codec == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  codec->m_effects.push_back(new ultrahdr::uhdr_crop_effect_t(left, right, top, bottom));

  return status;
}

uhdr_error_info_t uhdr_add_effect_resize(uhdr_codec_private_t* codec, int width, int height) {
  uhdr_error_info_t status = g_no_error;

  if (codec == nullptr) {
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "received nullptr for uhdr codec instance");
    return status;
  }

  codec->m_effects.push_back(new ultrahdr::uhdr_resize_effect_t(width, height));

  return status;
}
