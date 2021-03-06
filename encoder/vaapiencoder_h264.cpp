/*
 *  vaapiencoder_h264.cpp - h264 encoder for va
 *
 *  Copyright (C) 2013-2014 Intel Corporation
 *    Author: Wind Yuan <feng.yuan@intel.com>
 *    Author: Xu Guangxin <guangxin.xu@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "vaapiencoder_h264.h"
#include <assert.h>
#include "bitwriter.h"
#include "scopedlogger.h"
#include "vaapi/vaapicontext.h"
#include "vaapi/vaapidisplay.h"
#include "vaapicodedbuffer.h"
#include "vaapiencpicture.h"
#include <algorithm>
namespace YamiMediaCodec{
//shortcuts
typedef VaapiEncoderH264::PicturePtr PicturePtr;
typedef VaapiEncoderH264::ReferencePtr ReferencePtr;

using std::list;
using std::vector;


/* Define the maximum IDR period */
#define MAX_IDR_PERIOD 512


#define VAAPI_ENCODER_H264_NAL_REF_IDC_NONE        0
#define VAAPI_ENCODER_H264_NAL_REF_IDC_LOW         1
#define VAAPI_ENCODER_H264_NAL_REF_IDC_MEDIUM      2
#define VAAPI_ENCODER_H264_NAL_REF_IDC_HIGH        3

typedef enum {
  VAAPI_ENCODER_H264_NAL_UNKNOWN     = 0,
  VAAPI_ENCODER_H264_NAL_NON_IDR     = 1,
  VAAPI_ENCODER_H264_NAL_IDR         = 5,    /* ref_idc != 0 */
  VAAPI_ENCODER_H264_NAL_SEI         = 6,    /* ref_idc == 0 */
  VAAPI_ENCODER_H264_NAL_SPS         = 7,
  VAAPI_ENCODER_H264_NAL_PPS         = 8
} GstVaapiEncoderH264NalType;

static inline bool
_poc_greater_than (uint32_t poc1, uint32_t poc2, uint32_t max_poc)
{
    return (((poc1 - poc2) & (max_poc - 1)) < max_poc / 2);
}

/* Get slice_type value for H.264 specification */
static uint8_t
h264_get_slice_type (VaapiPictureType type)
{
    switch (type) {
    case VAAPI_PICTURE_TYPE_I:
        return 2;
    case VAAPI_PICTURE_TYPE_P:
        return 0;
    case VAAPI_PICTURE_TYPE_B:
        return 1;
    default:
        return -1;
    }
    return -1;
}

/* Get log2_max_frame_num value for H.264 specification */
static uint32_t
h264_get_log2_max_frame_num (uint32_t num)
{
    uint32_t ret = 0;

    while (num) {
        ++ret;
        num >>= 1;
    }
    if (ret <= 4)
        ret = 4;
    else if (ret > 10)
        ret = 10;
    /* must be greater than 4 */
    return ret;
}

/* Determines the cpbBrNalFactor based on the supplied profile */
static uint32_t
h264_get_cpb_nal_factor (VaapiProfile profile)
{
    uint32_t f;

    /* Table A-2 */
    switch (profile) {
    case VAAPI_PROFILE_H264_HIGH:
        f = 1500;
        break;
    case VAAPI_PROFILE_H264_HIGH10:
        f = 3600;
        break;
    case VAAPI_PROFILE_H264_HIGH_422:
    case VAAPI_PROFILE_H264_HIGH_444:
        f = 4800;
        break;
    default:
        f = 1200;
        break;
    }
    return f;
}

static uint8_t h264_get_profile_idc (VaapiProfile profile)
{
    uint8_t idc;
    switch (profile) {
    case VAAPI_PROFILE_H264_MAIN:
        idc =  77;
        break;
    default:
        assert(0);
    }
    return idc;

}

BOOL
bit_writer_put_ue(BitWriter *bitwriter, uint32_t value)
{
    uint32_t  size_in_bits = 0;
    uint32_t  tmp_value = ++value;

    while (tmp_value) {
        ++size_in_bits;
        tmp_value >>= 1;
    }
    if (size_in_bits > 1
        && !bit_writer_put_bits_uint32(bitwriter, 0, size_in_bits-1))
        return FALSE;
    if (!bit_writer_put_bits_uint32(bitwriter, value, size_in_bits))
        return FALSE;
    return TRUE;
}

BOOL
bit_writer_put_se(BitWriter *bitwriter, int32_t value)
{
    uint32_t new_val;

    if (value <= 0)
        new_val = -(value<<1);
    else
        new_val = (value<<1) - 1;

    if (!bit_writer_put_ue(bitwriter, new_val))
        return FALSE;
    return TRUE;
}


static BOOL
bit_writer_write_nal_header(
    BitWriter *bitwriter,
    uint32_t nal_ref_idc,
    uint32_t nal_unit_type
)
{
    bit_writer_put_bits_uint32(bitwriter, 0, 1);
    bit_writer_put_bits_uint32(bitwriter, nal_ref_idc, 2);
    bit_writer_put_bits_uint32(bitwriter, nal_unit_type, 5);
    return TRUE;
}

static BOOL
bit_writer_write_trailing_bits(BitWriter *bitwriter)
{
    bit_writer_put_bits_uint32(bitwriter, 1, 1);
    bit_writer_align_bytes_unchecked(bitwriter, 0);
    return TRUE;
}

static BOOL
bit_writer_write_sps(
    BitWriter *bitwriter,
    const VAEncSequenceParameterBufferH264* const seq,
    VaapiProfile profile
)
{
    uint32_t constraint_set0_flag, constraint_set1_flag;
    uint32_t constraint_set2_flag, constraint_set3_flag;
    uint32_t gaps_in_frame_num_value_allowed_flag = 0; // ??
    BOOL nal_hrd_parameters_present_flag;

    uint32_t b_qpprime_y_zero_transform_bypass = 0;
    uint32_t residual_color_transform_flag = 0;
    uint32_t pic_height_in_map_units =
        (seq->seq_fields.bits.frame_mbs_only_flag ?
         seq->picture_height_in_mbs : seq->picture_height_in_mbs/2);
    uint32_t mb_adaptive_frame_field = !seq->seq_fields.bits.frame_mbs_only_flag;
    uint32_t i = 0;

    constraint_set0_flag = profile == VAAPI_PROFILE_H264_BASELINE;
    constraint_set1_flag = profile <= VAAPI_PROFILE_H264_MAIN;
    constraint_set2_flag = 0;
    constraint_set3_flag = 0;

    bit_writer_put_bits_uint32 (bitwriter, 0x00000001, 32);   /* start code */
    bit_writer_write_nal_header (bitwriter,
                         VAAPI_ENCODER_H264_NAL_REF_IDC_HIGH, VAAPI_ENCODER_H264_NAL_SPS);
    /* profile_idc */
    bit_writer_put_bits_uint32(bitwriter, h264_get_profile_idc(profile), 8);
    /* constraint_set0_flag */
    bit_writer_put_bits_uint32(bitwriter, constraint_set0_flag, 1);
    /* constraint_set1_flag */
    bit_writer_put_bits_uint32(bitwriter, constraint_set1_flag, 1);
    /* constraint_set2_flag */
    bit_writer_put_bits_uint32(bitwriter, constraint_set2_flag, 1);
    /* constraint_set3_flag */
    bit_writer_put_bits_uint32(bitwriter, constraint_set3_flag, 1);
    /* reserved_zero_4bits */
    bit_writer_put_bits_uint32(bitwriter, 0, 4);
    /* level_idc */
    bit_writer_put_bits_uint32(bitwriter, seq->level_idc, 8);
    /* seq_parameter_set_id */
    bit_writer_put_ue(bitwriter, seq->seq_parameter_set_id);

    if (profile == VAAPI_PROFILE_H264_HIGH) {
        /* for high profile */
        /* chroma_format_idc  = 1, 4:2:0*/
        bit_writer_put_ue(bitwriter, seq->seq_fields.bits.chroma_format_idc);
        if (3 == seq->seq_fields.bits.chroma_format_idc) {
          bit_writer_put_bits_uint32(bitwriter, residual_color_transform_flag, 1);
        }
        /* bit_depth_luma_minus8 */
        bit_writer_put_ue(bitwriter, seq->bit_depth_luma_minus8);
        /* bit_depth_chroma_minus8 */
        bit_writer_put_ue(bitwriter, seq->bit_depth_chroma_minus8);
        /* b_qpprime_y_zero_transform_bypass */
        bit_writer_put_bits_uint32(bitwriter, b_qpprime_y_zero_transform_bypass, 1);
        assert(seq->seq_fields.bits.seq_scaling_matrix_present_flag == 0);
        /*seq_scaling_matrix_present_flag  */
        bit_writer_put_bits_uint32(bitwriter,
            seq->seq_fields.bits.seq_scaling_matrix_present_flag, 1);

    #if 0
        if (seq->seq_fields.bits.seq_scaling_matrix_present_flag) {
          for (i = 0; i < (seq->seq_fields.bits.chroma_format_idc != 3 ? 8 : 12); i++) {
            bit_writer_put_bits_uint8(bitwriter, seq->seq_fields.bits.seq_scaling_list_present_flag, 1);
            if (seq->seq_fields.bits.seq_scaling_list_present_flag) {
              assert(0);
              /* FIXME, need write scaling list if seq_scaling_matrix_present_flag ==1*/
            }
          }
        }
    #endif
    }

    /* log2_max_frame_num_minus4 */
    bit_writer_put_ue(bitwriter,
        seq->seq_fields.bits.log2_max_frame_num_minus4);
    /* pic_order_cnt_type */
    bit_writer_put_ue(bitwriter, seq->seq_fields.bits.pic_order_cnt_type);

    if (seq->seq_fields.bits.pic_order_cnt_type == 0) {
        /* log2_max_pic_order_cnt_lsb_minus4 */
        bit_writer_put_ue(bitwriter,
            seq->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (seq->seq_fields.bits.pic_order_cnt_type == 1) {
        assert(0);
        bit_writer_put_bits_uint32(bitwriter,
            seq->seq_fields.bits.delta_pic_order_always_zero_flag, 1);
        bit_writer_put_se(bitwriter, seq->offset_for_non_ref_pic);
        bit_writer_put_se(bitwriter, seq->offset_for_top_to_bottom_field);
        bit_writer_put_ue(bitwriter,
            seq->num_ref_frames_in_pic_order_cnt_cycle);
        for ( i = 0; i < seq->num_ref_frames_in_pic_order_cnt_cycle; i++) {
            bit_writer_put_se(bitwriter, seq->offset_for_ref_frame[i]);
        }
    }

    /* num_ref_frames */
    bit_writer_put_ue(bitwriter, seq->max_num_ref_frames);
    /* gaps_in_frame_num_value_allowed_flag */
    bit_writer_put_bits_uint32(bitwriter,
        gaps_in_frame_num_value_allowed_flag, 1);

    /* pic_width_in_mbs_minus1 */
    bit_writer_put_ue(bitwriter, seq->picture_width_in_mbs - 1);
    /* pic_height_in_map_units_minus1 */
    bit_writer_put_ue(bitwriter, pic_height_in_map_units - 1);
    /* frame_mbs_only_flag */
    bit_writer_put_bits_uint32(bitwriter,
        seq->seq_fields.bits.frame_mbs_only_flag, 1);

    if (!seq->seq_fields.bits.frame_mbs_only_flag) { //ONLY mbs
        assert(0);
        bit_writer_put_bits_uint32(bitwriter, mb_adaptive_frame_field, 1);
    }

    /* direct_8x8_inference_flag */
    bit_writer_put_bits_uint32(bitwriter, 0, 1);
    /* frame_cropping_flag */
    bit_writer_put_bits_uint32(bitwriter, seq->frame_cropping_flag, 1);

    if (seq->frame_cropping_flag) {
        /* frame_crop_left_offset */
        bit_writer_put_ue(bitwriter, seq->frame_crop_left_offset);
        /* frame_crop_right_offset */
        bit_writer_put_ue(bitwriter, seq->frame_crop_right_offset);
        /* frame_crop_top_offset */
        bit_writer_put_ue(bitwriter, seq->frame_crop_top_offset);
        /* frame_crop_bottom_offset */
        bit_writer_put_ue(bitwriter, seq->frame_crop_bottom_offset);
    }

    /* vui_parameters_present_flag */
    bit_writer_put_bits_uint32(bitwriter, seq->vui_parameters_present_flag, 1);
    if (seq->vui_parameters_present_flag) {
        /* aspect_ratio_info_present_flag */
        bit_writer_put_bits_uint32(bitwriter,
                                  seq->vui_fields.bits.aspect_ratio_info_present_flag,
                                  1);
        if (seq->vui_fields.bits.aspect_ratio_info_present_flag) {
            bit_writer_put_bits_uint32(bitwriter, seq->aspect_ratio_idc, 8);
            if (seq->aspect_ratio_idc == 0xFF) {
                bit_writer_put_bits_uint32(bitwriter, seq->sar_width, 16);
                bit_writer_put_bits_uint32(bitwriter, seq->sar_height, 16);
            }
        }

        /* overscan_info_present_flag */
        bit_writer_put_bits_uint32(bitwriter, 0, 1);
        /* video_signal_type_present_flag */
        bit_writer_put_bits_uint32(bitwriter, 0, 1);
        /* chroma_loc_info_present_flag */
        bit_writer_put_bits_uint32(bitwriter, 0, 1);

        /* timing_info_present_flag */
        bit_writer_put_bits_uint32(bitwriter,
            seq->vui_fields.bits.timing_info_present_flag, 1);
        if (seq->vui_fields.bits.timing_info_present_flag) {
            bit_writer_put_bits_uint32(bitwriter, seq->num_units_in_tick, 32);
            bit_writer_put_bits_uint32(bitwriter, seq->time_scale, 32);
            bit_writer_put_bits_uint32(bitwriter, 1, 1); /* fixed_frame_rate_flag */
        }

        nal_hrd_parameters_present_flag = (seq->bits_per_second > 0 ? TRUE : FALSE);
        /* nal_hrd_parameters_present_flag */
        bit_writer_put_bits_uint32(bitwriter, nal_hrd_parameters_present_flag, 1);
        if (nal_hrd_parameters_present_flag) {
            /* hrd_parameters */
            /* cpb_cnt_minus1 */
            bit_writer_put_ue(bitwriter, 0);
            bit_writer_put_bits_uint32(bitwriter, 4, 4); /* bit_rate_scale */
            bit_writer_put_bits_uint32(bitwriter, 6, 4); /* cpb_size_scale */

            for (i = 0; i < 1; ++i) {
                /* bit_rate_value_minus1[0] */
                bit_writer_put_ue(bitwriter, seq->bits_per_second/1024- 1);
                /* cpb_size_value_minus1[0] */
                bit_writer_put_ue(bitwriter, seq->bits_per_second/1024*8 - 1);
                /* cbr_flag[0] */
                bit_writer_put_bits_uint32(bitwriter, 1, 1);
            }
            /* initial_cpb_removal_delay_length_minus1 */
            bit_writer_put_bits_uint32(bitwriter, 23, 5);
            /* cpb_removal_delay_length_minus1 */
            bit_writer_put_bits_uint32(bitwriter, 23, 5);
            /* dpb_output_delay_length_minus1 */
            bit_writer_put_bits_uint32(bitwriter, 23, 5);
            /* time_offset_length  */
            bit_writer_put_bits_uint32(bitwriter, 23, 5);
        }
        /* vcl_hrd_parameters_present_flag */
        bit_writer_put_bits_uint32(bitwriter, 0, 1);
        if (nal_hrd_parameters_present_flag || 0/*vcl_hrd_parameters_present_flag*/) {
            /* low_delay_hrd_flag */
            bit_writer_put_bits_uint32(bitwriter, 0, 1);
        }
        /* pic_struct_present_flag */
        bit_writer_put_bits_uint32(bitwriter, 0, 1);
        /* bitwriter_restriction_flag */
        bit_writer_put_bits_uint32(bitwriter, 0, 1);
    }

    /* rbsp_trailing_bits */
    bit_writer_write_trailing_bits(bitwriter);
    return TRUE;
}

static BOOL
bit_writer_write_pps(
    BitWriter *bitwriter,
    const VAEncPictureParameterBufferH264* const pic
)
{
    uint32_t num_slice_groups_minus1 = 0;
    uint32_t pic_init_qs_minus26 = 0;
    uint32_t redundant_pic_cnt_present_flag = 0;

    bit_writer_put_bits_uint32 (bitwriter, 0x00000001, 32);   /* start code */
    bit_writer_write_nal_header (bitwriter,
                         VAAPI_ENCODER_H264_NAL_REF_IDC_HIGH, VAAPI_ENCODER_H264_NAL_PPS);
    /* pic_parameter_set_id */
    bit_writer_put_ue(bitwriter, pic->pic_parameter_set_id);
    /* seq_parameter_set_id */
    bit_writer_put_ue(bitwriter, pic->seq_parameter_set_id);
    /* entropy_coding_mode_flag */
    bit_writer_put_bits_uint32(bitwriter,
        pic->pic_fields.bits.entropy_coding_mode_flag, 1);
    /* pic_order_present_flag */
    bit_writer_put_bits_uint32(bitwriter,
        pic->pic_fields.bits.pic_order_present_flag, 1);
    /*slice_groups-1*/
    bit_writer_put_ue(bitwriter, num_slice_groups_minus1);

    if (num_slice_groups_minus1 > 0) {
        /*FIXME*/
        assert(0);
    }
    bit_writer_put_ue(bitwriter, pic->num_ref_idx_l0_active_minus1);
    bit_writer_put_ue(bitwriter, pic->num_ref_idx_l1_active_minus1);
    bit_writer_put_bits_uint32(bitwriter,
        pic->pic_fields.bits.weighted_pred_flag, 1);
    bit_writer_put_bits_uint32(bitwriter,
        pic->pic_fields.bits.weighted_bipred_idc, 2);
    /* pic_init_qp_minus26 */
    bit_writer_put_se(bitwriter, pic->pic_init_qp-26);
    /* pic_init_qs_minus26 */
    bit_writer_put_se(bitwriter, pic_init_qs_minus26);
    /*chroma_qp_index_offset*/
    bit_writer_put_se(bitwriter, pic->chroma_qp_index_offset);

    bit_writer_put_bits_uint32(bitwriter,
        pic->pic_fields.bits.deblocking_filter_control_present_flag, 1);
    bit_writer_put_bits_uint32(bitwriter,
        pic->pic_fields.bits.constrained_intra_pred_flag, 1);
    bit_writer_put_bits_uint32(bitwriter, redundant_pic_cnt_present_flag, 1);

    /*more_rbsp_data*/
    bit_writer_put_bits_uint32(bitwriter,
        pic->pic_fields.bits.transform_8x8_mode_flag, 1);
    bit_writer_put_bits_uint32(bitwriter,
        pic->pic_fields.bits.pic_scaling_matrix_present_flag, 1);
    if (pic->pic_fields.bits.pic_scaling_matrix_present_flag) {
        assert(0);
        /* FIXME */
        /*
        for (i = 0; i <
            (6+(-( (chroma_format_idc ! = 3) ? 2 : 6) * -pic->pic_fields.bits.transform_8x8_mode_flag));
            i++) {
            bit_writer_put_bits_uint8(bitwriter, pic->pic_fields.bits.pic_scaling_list_present_flag, 1);
        }
        */
    }

    bit_writer_put_se(bitwriter, pic->second_chroma_qp_index_offset);
    bit_writer_write_trailing_bits(bitwriter);

    return TRUE;
}

class VaapiEncStreamHeaderH264
{
  public:
    void setParamSet(uint8_t* data, uint32_t size)
    {
        ASSERT(m_raw.empty());
        if (m_raw.size())
            return;

        ASSERT(size > 4);
        ASSERT(data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
        m_raw.insert(m_raw.end(), data+4, data+size); // skip start code
    }

    bool generateByteStreamWithEmulation()
    {
        int i, j;
        int size;

        if(m_emulation.size())
            return true;

        ASSERT(m_raw.size());
        size = m_raw.size();
        m_emulation.resize(4+size*3/2);

        // start code
        m_emulation[0] = m_emulation[1] = m_emulation[2] = 0;
        m_emulation[3] = 1;

        for (i=0, j=4; i<size; i++, j++) {
            m_emulation[j] = m_raw[i];
            if (!m_raw[i-1] && !m_raw[i]) {
                m_emulation[++j] = 0x03;

                if (i < size-1)
                    m_emulation[++j] = m_raw[++i]; // deal with more than 3 zeros
            }
        }

        m_emulation.resize(j);
        return true;
    }

  public:
    std::vector<uint8_t> m_raw; // neither start code nor nal size
    std::vector<uint8_t> m_emulation; // prefix with start code, insert emulation byte

};
typedef std::tr1::shared_ptr <VaapiEncStreamHeaderH264> StreamHeaderPtr;

class VaapiEncPictureH264:public VaapiEncPicture
{
    friend class VaapiEncoderH264;
    friend class VaapiEncoderH264Ref;

public:
    virtual ~VaapiEncPictureH264() {}
private:
    VaapiEncPictureH264(const ContextPtr& context, const SurfacePtr& surface, int64_t timeStamp):
        VaapiEncPicture(context, surface, timeStamp),
        m_frameNum(0),
        m_poc(0)
    {
    }

    bool isIdr() const {
        return m_type == VAAPI_PICTURE_TYPE_I && !m_frameNum;
    }
    uint32_t m_frameNum;
    uint32_t m_poc;
    StreamHeaderPtr m_sps;
    StreamHeaderPtr m_pps;
};

class VaapiEncoderH264Ref
{
public:
    VaapiEncoderH264Ref(const PicturePtr& picture, const SurfacePtr& surface):
        m_frameNum(picture->m_frameNum),
        m_poc(picture->m_poc),
        m_pic(surface)
    {
    }
    SurfacePtr m_pic;
    uint32_t m_frameNum;
    uint32_t m_poc;
};

VaapiEncoderH264::VaapiEncoderH264():
    m_useCabac(false),
    m_useDct8x8(false),
    m_reorderState(VAAPI_ENC_REORD_WAIT_FRAMES)
{
    m_videoParamCommon.profile = VAProfileH264Main;
    m_videoParamCommon.level = 40;
    m_videoParamCommon.rcParams.initQP = 26;
    m_videoParamCommon.rcParams.minQP = 1;

    m_videoParamAVC.idrInterval = 30;
    pthread_mutex_init(&m_outputQueueMutex, NULL);
}

VaapiEncoderH264::~VaapiEncoderH264()
{
    FUNC_ENTER();
    pthread_mutex_destroy(&m_outputQueueMutex);
}

bool VaapiEncoderH264::ensureCodedBufferSize()
{
    AutoLock locker(m_paramLock);
    uint32_t mbSize;

    FUNC_ENTER();

    if (m_maxCodedbufSize)
        return true;

    if (!width() || !height()) {
        return false;
    }

    m_mbWidth = (width() + 15) / 16;
    m_mbHeight = (height() + 15)/ 16;
    //FIXME:
    m_numSlices = 1;
    mbSize = m_mbWidth * m_mbHeight;
    if (m_numSlices > (mbSize + 1) / 2)
        m_numSlices = (mbSize + 1) / 2;
    ASSERT (m_numSlices);

    /* Maximum sizes for common headers (in bits) */
    enum
    {
      MAX_SPS_HDR_SIZE = 16473,
      MAX_VUI_PARAMS_SIZE = 210,
      MAX_HRD_PARAMS_SIZE = 4103,
      MAX_PPS_HDR_SIZE = 101,
      MAX_SLICE_HDR_SIZE = 397 + 2572 + 6670 + 2402,
    };

    /* Only YUV 4:2:0 formats are supported for now. This means that we
       have a limit of 3200 bits per macroblock. */
    /* XXX: check profile and compute RawMbBits */
    m_maxCodedbufSize = m_mbWidth * m_mbHeight * 400;

    /* Account for SPS header */
    /* XXX: exclude scaling lists, MVC/SVC extensions */
    m_maxCodedbufSize += 4 + (MAX_SPS_HDR_SIZE +
        MAX_VUI_PARAMS_SIZE + 2 * MAX_HRD_PARAMS_SIZE + 7)/8;

    /* Account for PPS header */
    /* XXX: exclude slice groups, scaling lists, MVC/SVC extensions */
    m_maxCodedbufSize += 4 + (MAX_PPS_HDR_SIZE + 7) / 8;

    /* Account for slice header */
    m_maxCodedbufSize += m_numSlices * (4 +
        (MAX_SLICE_HDR_SIZE + 7) / 8);
    DEBUG("m_maxCodedbufSize: %u", m_maxCodedbufSize);

    return true;
}

void VaapiEncoderH264::resetParams ()
{

    m_levelIdc = level();

    DEBUG("resetParams, ensureCodedBufferSize");
    ensureCodedBufferSize();

    //FIXME:
    m_numBFrames = 0;

    if (keyFramePeriod() < intraPeriod())
        keyFramePeriod() = intraPeriod();
    if (keyFramePeriod() > MAX_IDR_PERIOD)
        keyFramePeriod() = MAX_IDR_PERIOD;

    if (minQP() > initQP() ||
            (rateControlMode()== RATE_CONTROL_CQP && minQP() < initQP()))
        minQP() = initQP();

    if (m_numBFrames > (intraPeriod() + 1) / 2)
        m_numBFrames = (intraPeriod() + 1) / 2;

    /* init m_maxFrameNum, max_poc */
    m_log2MaxFrameNum =
        h264_get_log2_max_frame_num (keyFramePeriod());
    assert (m_log2MaxFrameNum >= 4);
    m_maxFrameNum = (1 << m_log2MaxFrameNum);
    m_log2MaxPicOrderCnt = m_log2MaxFrameNum + 1;
    m_maxPicOrderCnt = (1 << m_log2MaxPicOrderCnt);

    m_maxRefList0Count = 1;
    m_maxRefList1Count = m_numBFrames > 0;
    m_maxRefFrames =
        m_maxRefList0Count + m_maxRefList1Count;

    INFO("m_maxRefFrames: %d", m_maxRefFrames);


    resetGopStart();
}

Encode_Status VaapiEncoderH264::getMaxOutSize(uint32_t *maxSize)
{
    FUNC_ENTER();

    if (ensureCodedBufferSize())
        *maxSize = m_maxCodedbufSize;
    else
        *maxSize = 0;

    return ENCODE_SUCCESS;
}

Encode_Status VaapiEncoderH264::start()
{
    FUNC_ENTER();
    resetParams();
    return VaapiEncoderBase::start();
}

void VaapiEncoderH264::flush()
{
    FUNC_ENTER();
    resetGopStart();
    m_reorderFrameList.clear();
    m_refList.clear();

    INFO("output queue size: %ld", m_outputQueue.size());
    pthread_mutex_lock(&m_outputQueueMutex);
    while (!m_outputQueue.empty())
        m_outputQueue.pop();
    pthread_mutex_unlock(&m_outputQueueMutex);
}

Encode_Status VaapiEncoderH264::stop()
{
    flush();
    return VaapiEncoderBase::stop();
}

Encode_Status VaapiEncoderH264::setParameters(VideoParamConfigSet *videoEncParams)
{
    Encode_Status status = ENCODE_SUCCESS;
    AutoLock locker(m_paramLock);

    FUNC_ENTER();
    if (!videoEncParams)
        return ENCODE_INVALID_PARAMS;

    switch (videoEncParams->type) {
    case VideoParamsTypeAVC: {
            VideoParamsAVC* avc = (VideoParamsAVC*)videoEncParams;
            m_videoParamAVC = *avc;
        }
        break;
    case VideoConfigTypeAVCIntraPeriod: {
            VideoConfigAVCIntraPeriod* intraPeriod = (VideoConfigAVCIntraPeriod*)videoEncParams;
            m_videoParamAVC.idrInterval = intraPeriod->idrInterval;
            m_videoParamCommon.intraPeriod = intraPeriod->intraPeriod;
        }
        break;
    default:
        status = VaapiEncoderBase::setParameters(videoEncParams);
        break;
    }
    return status;
}

Encode_Status VaapiEncoderH264::getParameters(VideoParamConfigSet *videoEncParams)
{
    AutoLock locker(m_paramLock);

    FUNC_ENTER();
    if (!videoEncParams)
        return ENCODE_INVALID_PARAMS;
    if (videoEncParams->type == VideoParamsTypeAVC) {
        VideoParamsAVC* avc = (VideoParamsAVC*)videoEncParams;
        *avc = m_videoParamAVC;
        return ENCODE_SUCCESS;
    }

    // TODO, update video resolution basing on hw requirement
    return VaapiEncoderBase::getParameters(videoEncParams);
}

Encode_Status VaapiEncoderH264::reorder(const SurfacePtr& surface, uint64_t timeStamp, bool forceKeyFrame)
{
    if (!surface)
        return ENCODE_INVALID_PARAMS;

    ++m_curPresentIndex;
    PicturePtr picture(new VaapiEncPictureH264(m_context, surface, timeStamp));
    picture->m_poc = ((m_curPresentIndex * 2) % m_maxPicOrderCnt);

    bool isIdr = (m_frameIndex == 0 ||m_frameIndex >= keyFramePeriod() || forceKeyFrame);

    /* check key frames */
    if (isIdr || (m_frameIndex % intraPeriod() == 0)) {
        ++m_curFrameNum;
        ++m_frameIndex;
        /* b frame enabled,  check queue of reorder_frame_list */
        if (m_numBFrames
                && (m_reorderFrameList.size() > 0)) {
            assert(0);

        }
        setIntraFrame (picture, isIdr);
        m_reorderFrameList.push_back(picture);
        m_reorderState = VAAPI_ENC_REORD_DUMP_FRAMES;
        return ENCODE_SUCCESS;
    }
    /* new p/b frames coming */
    ++m_frameIndex;
    if (m_reorderFrameList.size() < m_numBFrames) {
        assert(0);
        m_reorderFrameList.push_back(picture);
        return ENCODE_SUCCESS;
    }
    ++m_curFrameNum;
    setPFrame (picture);
    m_reorderFrameList.push_front(picture);
    m_reorderState = VAAPI_ENC_REORD_DUMP_FRAMES;
    return ENCODE_SUCCESS;
}

Encode_Status VaapiEncoderH264::getStreamHeader(VideoEncOutputBuffer *outBuffer, PicturePtr picture)
{
    uint8_t *data = outBuffer->data;
    uint32_t dataSize = 0;
    AutoLock locker(m_paramLock);
    StreamHeaderPtr header[2]; // sps/pps
    int i;

    if (picture) {
        if (picture->m_type == VAAPI_PICTURE_TYPE_I) {
            // XXX, when we distinguish IDR from I frame, update here
            header[0] = picture->m_sps;
            header[1] = picture->m_pps;
            ASSERT(header[0]);
            ASSERT(header[1]);
        }
    } else {
        // when picture is not valid (no output frame available), use sps/pps from decoder
        header[0] = m_sps;
        header[1] = m_pps;

        if (!header[0] || ! header[1])
            return ENCODE_NO_REQUEST_DATA;
    }

    for (i=0; i<2; i++) {
        if (!header[i] || !header[i]->m_raw.size()) // it is P or B frame
            continue;
        if (!header[i]->m_emulation.size())
            header[i]->generateByteStreamWithEmulation();

        dataSize += header[i]->m_emulation.size();
        if (outBuffer->bufferSize < dataSize)
            return ENCODE_BUFFER_TOO_SMALL;
        memcpy(data, &header[i]->m_emulation[0], header[i]->m_emulation.size());
        data += header[i]->m_emulation.size();
    }

    outBuffer->dataSize = dataSize;
    return ENCODE_SUCCESS;
}
Encode_Status VaapiEncoderH264::getCodecCofnig(VideoEncOutputBuffer *outBuffer, PicturePtr picture)
{
    const uint32_t configurationVersion = 0x01;
    const uint32_t nalLengthSize = 4;
    uint8_t profileIdc, profileComp, levelIdc;
    BitWriter bs;
    AutoLock locker(m_paramLock);
    StreamHeaderPtr sps, pps;

    if (picture) {
        sps = picture->m_sps;
        pps = picture->m_pps;
    } else {
        // when picture is not valid (no output frame available), use sps/pps from decoder
        sps = m_sps;
        pps = m_pps;

        if (!sps || ! pps || sps->m_raw.size() < 4)
            return ENCODE_NO_REQUEST_DATA;
    }

    ASSERT(sps);
    ASSERT(pps);
    if (outBuffer->bufferSize < sps->m_raw.size() + pps->m_raw.size() + 64)
        return ENCODE_BUFFER_TOO_SMALL;

    /* skip sps->m_raw[0], which is the nal_unit_type */
    profileIdc = sps->m_raw[1];
    profileComp = sps->m_raw[2];
    levelIdc = sps->m_raw[3];
    /* Header */
    bit_writer_init (&bs, (sps->m_raw.size() + pps->m_raw.size() + 64) * 8);
    bit_writer_put_bits_uint32 (&bs, configurationVersion, 8);
    bit_writer_put_bits_uint32 (&bs, profileIdc, 8);
    bit_writer_put_bits_uint32 (&bs, profileComp, 8);
    bit_writer_put_bits_uint32 (&bs, levelIdc, 8);
    bit_writer_put_bits_uint32 (&bs, 0x3f, 6);  /* 111111 */
    bit_writer_put_bits_uint32 (&bs, nalLengthSize - 1, 2);
    bit_writer_put_bits_uint32 (&bs, 0x07, 3);  /* 111 */

    /* Write SPS */
    bit_writer_put_bits_uint32 (&bs, 1, 5);     /* SPS count = 1 */
    assert (BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
    bit_writer_put_bits_uint32 (&bs, sps->m_raw.size(), 16);
    bit_writer_put_bytes (&bs, &sps->m_raw[0], sps->m_raw.size());
    /* Write PPS */
    bit_writer_put_bits_uint32 (&bs, 1, 8);     /* PPS count = 1 */
    bit_writer_put_bits_uint32 (&bs, pps->m_raw.size(), 16);
    bit_writer_put_bytes (&bs, &pps->m_raw[0], pps->m_raw.size());

    outBuffer->dataSize = BIT_WRITER_BIT_SIZE (&bs) / 8;
    memcpy(outBuffer->data, BIT_WRITER_DATA (&bs),outBuffer->dataSize);

    bit_writer_clear (&bs, FALSE);
    return ENCODE_SUCCESS;
    /* ERRORS */
bs_error:
    {
        ERROR ("failed to write codec-data");
        bit_writer_clear (&bs, TRUE);
        return ENCODE_FAIL;
    }
}

// calls immediately after reorder,
// it makes sure I frame are encoded immediately, so P frames can be pushed to the front of the m_reorderFrameList.
// it also makes sure input thread and output thread runs in parallel
Encode_Status VaapiEncoderH264::submitEncode()
{
    FUNC_ENTER();
    Encode_Status ret;
    if (m_reorderState == VAAPI_ENC_REORD_DUMP_FRAMES) {
        if (!m_maxCodedbufSize)
            ensureCodedBufferSize();
        CodedBufferPtr codedBuffer = VaapiCodedBuffer::create(m_context, m_maxCodedbufSize);
        PicturePtr picture = m_reorderFrameList.front();
        m_reorderFrameList.pop_front();
        if (m_reorderFrameList.empty())
            m_reorderState = VAAPI_ENC_REORD_WAIT_FRAMES;

        ret =  encodePicture(picture, codedBuffer);
        if (ret != ENCODE_SUCCESS) {
            return ret;
        }
        codedBuffer->setFlag(ENCODE_BUFFERFLAG_ENDOFFRAME);
        INFO("picture->m_type: 0x%x", picture->m_type);
        if (picture->m_type == VAAPI_PICTURE_TYPE_I) {
            codedBuffer->setFlag(ENCODE_BUFFERFLAG_SYNCFRAME);
        }

        pthread_mutex_lock(&m_outputQueueMutex);
        m_outputQueue.push(std::make_pair(picture, codedBuffer));
        pthread_mutex_unlock(&m_outputQueueMutex);
    }

    INFO();
    return ENCODE_SUCCESS;
}
/** getOutput suppose to run in a separated thread with other functions, be carefull
 * 1) it update m_outputQueue only for now, we use m_outputQueueMutex for it
 * 2) the rest which are not protected by mutex is generally fine
 *   a). getCodecCofnig() read some sps/pps parameter only, we suppose client doesn't change sps/pps at the same time.
 *   b). read/peek of the m_outputQueue is fine since getOutput is the only place to erase element
 *   c). picture->sync and copyCodecBuffer are fine as well
*/
Encode_Status VaapiEncoderH264::getOutput(VideoEncOutputBuffer * outBuffer, bool withWait) const
{
    PicturePtr picture;
    CodedBufferPtr codedBuffer;
    Encode_Status ret;
    bool isEmpty;
    uint32_t headerSize = 0;

    FUNC_ENTER();
    if (!outBuffer)
        return ENCODE_INVALID_PARAMS;

    ASSERT(outBuffer->format == OUTPUT_CODEC_DATA || outBuffer->format == OUTPUT_STREAM_HEADER
        || outBuffer->format == OUTPUT_EVERYTHING || outBuffer->format == OUTPUT_FRAME_DATA);

    pthread_mutex_lock(&m_outputQueueMutex);
    isEmpty = m_outputQueue.empty();
    pthread_mutex_unlock(&m_outputQueueMutex);

    if (!isEmpty) {
        picture = m_outputQueue.front().first;
        codedBuffer = m_outputQueue.front().second;
    }

    if (outBuffer->format & OUTPUT_CODEC_DATA)
        return getCodecCofnig(outBuffer, picture);
    if (outBuffer->format & OUTPUT_STREAM_HEADER)
        return getStreamHeader(outBuffer, picture);

    if (isEmpty) {
        return ENCODE_BUFFER_NO_MORE;
    }

    if (outBuffer->format == OUTPUT_EVERYTHING) {
        // fill stream header first
        ret = getStreamHeader(outBuffer, picture);
        if (ret != ENCODE_SUCCESS)
            return ret;

        headerSize = outBuffer->dataSize;
        outBuffer->data += headerSize;
        outBuffer->bufferSize -= headerSize;
    }

    picture->sync();
    ret = copyCodedBuffer(outBuffer, codedBuffer);
    if (outBuffer->format == OUTPUT_EVERYTHING) {
        outBuffer->data -= headerSize;
        outBuffer->bufferSize += headerSize;
    }

    if (ret != ENCODE_SUCCESS)
        return ret;

    pthread_mutex_lock(&m_outputQueueMutex);
    m_outputQueue.pop();
    pthread_mutex_unlock(&m_outputQueueMutex);

    return ENCODE_SUCCESS;
}

/* Handle new GOP starts */
void VaapiEncoderH264::resetGopStart ()
{
    m_idrNum = 0;
    m_frameIndex = 0;
    m_curFrameNum = 0;
    m_curPresentIndex = 0;
}

/* Marks the supplied picture as a B-frame */
void VaapiEncoderH264::setBFrame (const PicturePtr& pic)
{
    pic->m_type = VAAPI_PICTURE_TYPE_B;
    pic->m_frameNum = (m_curFrameNum % m_maxFrameNum);
}

/* Marks the supplied picture as a P-frame */
void VaapiEncoderH264::setPFrame (const PicturePtr& pic)
{
    pic->m_type = VAAPI_PICTURE_TYPE_P;
    pic->m_frameNum = (m_curFrameNum % m_maxFrameNum);
}

/* Marks the supplied picture as an I-frame */
void VaapiEncoderH264::setIFrame (const PicturePtr& pic)
{
    pic->m_type = VAAPI_PICTURE_TYPE_I;
    pic->m_frameNum = (m_curFrameNum % m_maxFrameNum);
}

/* Marks the supplied picture as an IDR frame */
void VaapiEncoderH264::setIdrFrame (const PicturePtr& pic)
{
    pic->m_type = VAAPI_PICTURE_TYPE_I;
    pic->m_frameNum = 0;
    pic->m_poc = 0;
}

/* Marks the supplied picture a a key-frame */
void VaapiEncoderH264::setIntraFrame (const PicturePtr& picture,bool idIdr)
{
    if (idIdr) {
        resetGopStart();
        setIdrFrame(picture);
        //+1 for next frame
        m_frameIndex++;
    } else
        setIFrame(picture);
}

bool VaapiEncoderH264::
referenceListUpdate (const PicturePtr& picture, const SurfacePtr& surface)
{
    if (VAAPI_PICTURE_TYPE_B == picture->m_type) {
        return true;
    }
    if (picture->isIdr()) {
        referenceListFree();
    } else if (m_refList.size() >= m_maxRefFrames) {
        m_refList.pop_front();
    }
    ReferencePtr ref(new VaapiEncoderH264Ref(picture, surface));
    m_refList.push_front(ref); // recent first
    assert (m_refList.size() <= m_maxRefFrames);
    return true;
}

bool  VaapiEncoderH264::referenceListInit (
    const PicturePtr& picture,
    vector<ReferencePtr>& refList0,
    vector<ReferencePtr>& refList1) const
{
    assert(picture->m_type == VAAPI_PICTURE_TYPE_P);
    refList0.reserve(m_refList.size());
    refList0.insert(refList0.end(), m_refList.begin(), m_refList.end());

    assert (refList0.size() + refList1.size() <= m_maxRefFrames);
    if (refList0.size() > m_maxRefList0Count)
        refList0.resize(m_maxRefList0Count);
    if (refList1.size() > m_maxRefList1Count)
        refList1.resize(m_maxRefList1Count);

    return true;
}

void VaapiEncoderH264::referenceListFree()
{
    m_refList.clear();
}

bool VaapiEncoderH264::fill(VAEncSequenceParameterBufferH264* seqParam) const
{
    seqParam->seq_parameter_set_id = 0;
    seqParam->level_idc = m_levelIdc;
    seqParam->intra_period = intraPeriod();
    seqParam->intra_idr_period = seqParam->intra_period;
    seqParam->ip_period = 1 + m_numBFrames;
    seqParam->bits_per_second = bitRate();

    seqParam->max_num_ref_frames = m_maxRefFrames;
    seqParam->picture_width_in_mbs = m_mbWidth;
    seqParam->picture_height_in_mbs = m_mbHeight;

    /*sequence field values */
    seqParam->seq_fields.value = 0;
    seqParam->seq_fields.bits.chroma_format_idc = 1;
    seqParam->seq_fields.bits.frame_mbs_only_flag = 1;
    assert (m_log2MaxFrameNum >= 4);
    seqParam->seq_fields.bits.log2_max_frame_num_minus4 =
        m_log2MaxFrameNum - 4;
    /* picture order count */
    seqParam->seq_fields.bits.pic_order_cnt_type = 0;
    assert (m_log2MaxPicOrderCnt >= 4);
    seqParam->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 =
        m_log2MaxPicOrderCnt - 4;


    /* not used if pic_order_cnt_type == 0 */
    if (seqParam->seq_fields.bits.pic_order_cnt_type == 1) {
        seqParam->seq_fields.bits.delta_pic_order_always_zero_flag = TRUE;
    }

    /* frame_cropping_flag */
    const int cropRight = (16 * m_mbWidth - width());
    const int cropBottom = (16 * m_mbHeight - height());
    if (cropRight || cropBottom) {
        const int CHROMA_420 = 1;
        const int CHROMA_422 = 2;
        int chroma = seqParam->seq_fields.bits.chroma_format_idc;
        static const uint32_t subWidthC = (chroma == CHROMA_420 || chroma == CHROMA_422) ? 2 : 1;
        static const uint32_t subHeightC = chroma == CHROMA_420 ? 2 : 1;
        const uint32_t cropUnitX = subWidthC;
        const uint32_t cropUnitY = subHeightC * (2 - seqParam->seq_fields.bits.frame_mbs_only_flag);

        seqParam->frame_cropping_flag = 1;
        seqParam->frame_crop_right_offset = cropRight / cropUnitX;
        seqParam->frame_crop_bottom_offset = cropBottom / cropUnitY;
    }

    /* VUI parameters are always set, at least for timing_info (framerate) */
    seqParam->vui_parameters_present_flag = TRUE;
    if (seqParam->vui_parameters_present_flag) {
        seqParam->vui_fields.bits.aspect_ratio_info_present_flag = FALSE;
        seqParam->vui_fields.bits.bitstream_restriction_flag = FALSE;
        seqParam->vui_fields.bits.timing_info_present_flag = TRUE;
        if (seqParam->vui_fields.bits.timing_info_present_flag) {
            seqParam->num_units_in_tick = frameRateDenom();
            seqParam->time_scale = frameRateNum() * 2;
        }
    }
    return true;
}

/* Fills in VA picture parameter buffer */
bool VaapiEncoderH264::fill(VAEncPictureParameterBufferH264* picParam, const PicturePtr& picture,
                            const CodedBufferPtr& codedbuf  , const SurfacePtr& surface) const
{
    VaapiEncoderH264Ref *ref_pic;
    uint32_t i = 0;

    /* reference list,  */
    picParam->CurrPic.picture_id = surface->getID();
    picParam->CurrPic.TopFieldOrderCnt = picture->m_poc;

    if (picture->m_type != VAAPI_PICTURE_TYPE_I) {
        list<ReferencePtr>::const_iterator it;
        for (it = m_refList.begin(); it != m_refList.end(); ++it) {
            assert(*it && (*it)->m_pic && ((*it)->m_pic->getID() != VA_INVALID_ID));
            picParam->ReferenceFrames[i].picture_id = (*it)->m_pic->getID();
            ++i;
        }
    }
    for (; i < 16; ++i) {
        picParam->ReferenceFrames[i].picture_id = VA_INVALID_ID;
    }
    picParam->coded_buf = codedbuf->getID();

    picParam->pic_parameter_set_id = 0;
    picParam->seq_parameter_set_id = 0;
    picParam->last_picture = 0;  /* means last encoding picture */
    picParam->frame_num = picture->m_frameNum;
    picParam->pic_init_qp = initQP();
    picParam->num_ref_idx_l0_active_minus1 =
        (m_maxRefList0Count ? (m_maxRefList0Count - 1) : 0);
    picParam->num_ref_idx_l1_active_minus1 =
        (m_maxRefList1Count ? (m_maxRefList1Count - 1) : 0);
    picParam->chroma_qp_index_offset = 0;
    picParam->second_chroma_qp_index_offset = 0;

    /* set picture fields */
    picParam->pic_fields.bits.idr_pic_flag = picture->isIdr();
    picParam->pic_fields.bits.reference_pic_flag = (picture->m_type != VAAPI_PICTURE_TYPE_B);
    picParam->pic_fields.bits.entropy_coding_mode_flag = m_useCabac;
    picParam->pic_fields.bits.transform_8x8_mode_flag = m_useDct8x8;
    /* enable debloking */
    picParam->pic_fields.bits.deblocking_filter_control_present_flag = TRUE;

    return TRUE;
}

bool VaapiEncoderH264::ensureSequenceHeader(const PicturePtr& picture,const VAEncSequenceParameterBufferH264* const sequence)
{
    BitWriter bs;
    uint32_t dataBitSize;
    uint8_t *data;
    StreamHeaderPtr sps(new VaapiEncStreamHeaderH264);
    AutoLock locker(m_paramLock);

    bit_writer_init (&bs, 128 * 8);
    bit_writer_write_sps (&bs, sequence, profile());
    assert (BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
    dataBitSize = BIT_WRITER_BIT_SIZE (&bs);
    data = BIT_WRITER_DATA (&bs);

    /* store sps data */
    sps->setParamSet(data, dataBitSize/8);
    // we don't depend on driver to insert sps/pps in coded buffer now
    // if (!picture->addPackedHeader(VAEncPackedHeaderSequence, data, dataBitSize)) goto add_error;

    bit_writer_clear (&bs, TRUE);

    m_sps = sps;

    return true;
    /* ERRORS */
bs_error:
    {
        WARNING ("failed to write SPS NAL unit");
        bit_writer_clear (&bs, TRUE);
        return false;
    }
add_error:
    {
        WARNING ("failed to add sequence header");
        bit_writer_clear (&bs, TRUE);
        return false;
    }
}

bool VaapiEncoderH264::ensurePictureHeader(const PicturePtr& picture, const VAEncPictureParameterBufferH264* const picParam)
{
    BitWriter bs;
    uint32_t dataBitSize;
    uint8_t *data;
    AutoLock locker(m_paramLock);
    StreamHeaderPtr pps(new VaapiEncStreamHeaderH264);

    bit_writer_init (&bs, 128 * 8);
    bit_writer_write_pps (&bs, picParam);
    assert (BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
    dataBitSize = BIT_WRITER_BIT_SIZE (&bs);
    data = BIT_WRITER_DATA (&bs);

    pps->setParamSet(data, dataBitSize/8);
    // we don't depend on driver to insert sps/pps in coded buffer now
    // if (!picture->addPackedHeader(VAEncPackedHeaderPicture,data, dataBitSize)) goto add_error;
    bit_writer_clear (&bs, TRUE);

    m_pps = pps;
    return true;

    /* ERRORS */
bs_error:
    {
        WARNING ("failed to write PPS NAL unit");
        bit_writer_clear (&bs, TRUE);
        return false;
    }

add_error:
    {
        WARNING ("failed to add picture param");
        bit_writer_clear (&bs, TRUE);
        return false;
    }
}

static void fillReferenceList(VAEncSliceParameterBufferH264* slice, const vector<ReferencePtr>& refList, uint32_t index)
{
    VAPictureH264* picList;
    int total;
    if (!index) {
        picList = slice->RefPicList0;
        total = N_ELEMENTS(slice->RefPicList0);
    }
    else {
        picList = slice->RefPicList1;
        total = N_ELEMENTS(slice->RefPicList1);
    }
    int i = 0;
    for (; i < refList.size(); i++)
        picList[i].picture_id = refList[i]->m_pic->getID();
    for (; i <total; i++)
        picList[i].picture_id = VA_INVALID_SURFACE;
}

/* Adds slice headers to picture */
bool VaapiEncoderH264::addSliceHeaders (const PicturePtr& picture,
                                        const vector<ReferencePtr>& refList0,
                                        const vector<ReferencePtr>& refList1) const
{
    VAEncSliceParameterBufferH264 *sliceParam;
    VaapiBufObject *slice;
    uint32_t sliceOfMbs, sliceModMbs, curSliceMbs;
    uint32_t mbSize;
    uint32_t lastMbIndex;

    assert (picture);
    /*one reference frame supported */
    if (picture->m_type == VAAPI_PICTURE_TYPE_I) {
        assert(!refList0.size() && !refList1.size());
    }
    else {
        assert(refList0.size() == 1);
        if (picture->m_type == VAAPI_PICTURE_TYPE_B)
            assert(refList1.size() == 1);
    }

    mbSize = m_mbWidth * m_mbHeight;

    assert (m_numSlices && m_numSlices < mbSize);
    sliceOfMbs = mbSize / m_numSlices;
    sliceModMbs = mbSize % m_numSlices;
    lastMbIndex = 0;
    for (int i = 0; i < m_numSlices; ++i) {
        curSliceMbs = sliceOfMbs;
        if (sliceModMbs) {
            ++curSliceMbs;
            --sliceModMbs;
        }
        if (!picture->newSlice(sliceParam))
            return false;

        sliceParam->macroblock_address = lastMbIndex;
        sliceParam->num_macroblocks = curSliceMbs;
        sliceParam->macroblock_info = VA_INVALID_ID;
        sliceParam->slice_type = h264_get_slice_type (picture->m_type);
        assert (sliceParam->slice_type != -1);
        sliceParam->idr_pic_id = m_idrNum;
        sliceParam->pic_order_cnt_lsb = picture->m_poc;

        if (picture->m_type != VAAPI_PICTURE_TYPE_I && refList0.size() > 0)
            sliceParam->num_ref_idx_l0_active_minus1 = refList0.size() - 1;
        if (picture->m_type == VAAPI_PICTURE_TYPE_B && refList1.size() > 0)
            sliceParam->num_ref_idx_l1_active_minus1 = refList1.size() - 1;

        fillReferenceList(sliceParam, refList0, 0);
        fillReferenceList(sliceParam, refList1, 1);


        sliceParam->slice_qp_delta = initQP() - minQP();
        if (sliceParam->slice_qp_delta > 4)
            sliceParam->slice_qp_delta = 4;
        sliceParam->slice_alpha_c0_offset_div2 = 2;
        sliceParam->slice_beta_offset_div2 = 2;

        /* set calculation for next slice */
        lastMbIndex += curSliceMbs;
    }
    assert (lastMbIndex == mbSize);
    return true;
}

bool VaapiEncoderH264::ensureSequence(const PicturePtr& picture)
{
    if (picture->m_type != VAAPI_PICTURE_TYPE_I) {
        picture->m_sps = m_sps;
        return true;
    }

    VAEncSequenceParameterBufferH264* seqParam;

    if (!picture->editSequence(seqParam) || !fill(seqParam)) {
        ERROR("failed to create sequence parameter buffer (SPS)");
        return false;
    }

    if (!ensureSequenceHeader(picture, seqParam)) {
        ERROR ("failed to create packed sequence header buffer");
        return false;
    }

    picture->m_sps = m_sps;
    return true;
}

bool VaapiEncoderH264::ensurePicture (const PicturePtr& picture,
                                      const CodedBufferPtr& codedBuf, const SurfacePtr& surface)
{
    VAEncPictureParameterBufferH264 *picParam;

    if (!picture->editPicture(picParam) || !fill(picParam, picture, codedBuf, surface)) {
        ERROR("failed to create picture parameter buffer (PPS)");
        return false;
    }

    if (picture->m_type == VAAPI_PICTURE_TYPE_I && !ensurePictureHeader (picture, picParam)) {
            ERROR ("set picture packed header failed");
            return false;
    }

    picture->m_pps = m_pps;
    return true;
}

bool VaapiEncoderH264::ensureSlices(const PicturePtr& picture)
{
    assert (picture);

    vector<ReferencePtr> refList0;
    vector<ReferencePtr> refList1;

    if (picture->m_type != VAAPI_PICTURE_TYPE_I &&
            !referenceListInit(picture, refList0, refList1)) {
        ERROR ("reference list reorder failed");
        return false;
    }
    if (!addSliceHeaders (picture, refList0, refList1))
        return false;
    return true;
}

Encode_Status VaapiEncoderH264::encodePicture(const PicturePtr& picture,const CodedBufferPtr& codedBuf)
{
    Encode_Status ret = ENCODE_FAIL;
    SurfacePtr reconstruct = createSurface();
    if (!reconstruct)
        return ret;

    if (!ensureSequence (picture))
        return ret;
    if (!ensureMiscParams (picture.get()))
        return ret;
    if (!ensurePicture(picture, codedBuf, reconstruct))
        return ret;
    if (!ensureSlices (picture))
        return ret;
    if (!picture->encode())
        return ret;

    if (!referenceListUpdate (picture, reconstruct))
        return ret;

    return ENCODE_SUCCESS;
}
}
