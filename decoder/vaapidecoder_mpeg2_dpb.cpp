/*
 *  vaapidecoder_h264_dpb.cpp - DPB manager for h264 decoder
 *
 *  Copyright (C) 2012-2014 Intel Corporation
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
 *    Author: Xiaowei Li <xiaowei.li@intel.com>
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

#include <assert.h>
#include <stdlib.h>
#include "vaapidecoder_mpeg2.h"

namespace YamiMediaCodec{


struct _VaapiMiniObject {
    /*< private >*/
    const void *         object_class;
    volatile int32_t       ref_count;
    uint32_t               flags;
};
struct _VaapiDpb {
    /*< private >*/
    VaapiMiniObject   parent_instance;

    /*< protected >*/
    VaapiPicture   **pictures;
    uint64_t               num_pictures;
    uint64_t               max_pictures;
};
struct _VaapiMiniObjectClass {
    uint32_t               size;
    GDestroyNotify      finalize;  
};

struct _VaapiDpbClass {
    /*< private >*/
    VaapiMiniObjectClass parent_class;

    /*< protected >*/
    void      (*flush)          (GstVaapiDpb *dpb);
    gboolean  (*add)            (GstVaapiDpb *dpb, GstVaapiPicture *picture);
    void      (*get_neighbours) (GstVaapiDpb *dpb, GstVaapiPicture *picture,
     VaapiPicture **prev_picture_ptr, GstVaapiPicture **next_picture_ptr);
};

VaapiMPEG2DPB::VaapiMPEG2DPB(VaapiDecoderMPEG2* decoder, uint32_t DPBSize)
    :m_decoder(decoder)
{
    DPBLayer.reset(new VaapiDecPicBufLayer(DPBSize));
}

VaapiMPEG2DPB::~VaapiMPEG2DPB()
{
}
static const GstVaapiMiniObjectClass *
gst_vaapi_dpb_class(void);

static const GstVaapiMiniObjectClass *
gst_vaapi_dpb2_class(void);

static inline VaapiDpb * dpb_new(uint64_t max_pictures)
{
    VaapiDpb *dpb;

    g_return_val_if_fail(max_pictures > 0, NULL);

    dpb = (VaapiDpb *)gst_vaapi_mini_object_new(
        max_pictures == 2 ? gst_vaapi_dpb2_class() : gst_vaapi_dpb_class());
    if (!dpb)
        return NULL;

    dpb->num_pictures = 0;
    dpb->max_pictures = max_pictures;

    dpb->pictures = g_new0(GstVaapiPicture *, max_pictures);
    if (!dpb->pictures)
        goto error;
    return dpb;

error:
    gst_vaapi_dpb_unref(dpb);
    return NULL;
}

static gint
dpb_get_oldest(VaapiDpb *dpb, bool output)
{
    int64_t i, lowest_pts_index;

    for (i = 0; i < dpb->num_pictures; i++) {
        if ((VAAPI_PICTURE_IS_OUTPUT(dpb->pictures[i]) ^ output) == 0)
            break;
    }
    if (i == dpb->num_pictures)
        return -1;

    lowest_pts_index = i++;
    for (; i < dpb->num_pictures; i++) {
        VaapiPicture * const picture = dpb->pictures[i];
        if ((VAAPI_PICTURE_IS_OUTPUT(picture) ^ output) != 0)
            continue;
        if (picture->poc < dpb->pictures[lowest_pts_index]->poc)
            lowest_pts_index = i;
    }
    return lowest_pts_index;
}
static void
dpb_remove_index(VaapiDpb *dpb, uint64_t index)
{
    VaapiPicture ** const pictures = dpb->pictures;
    uint64_t num_pictures = --dpb->num_pictures;

    if (index != num_pictures)
        gst_vaapi_picture_replace(&pictures[index], pictures[num_pictures]);
    gst_vaapi_picture_replace(&pictures[num_pictures], NULL);
}

static inline bool
dpb_output(VaapiDpb *dpb, VaapiPicture *picture)
{
    return gst_vaapi_picture_output(picture);
}
static bool
dpb_bump(VaapiDpb *dpb)
{
    int64_t index;
    bool success;

    index = dpb_get_oldest(dpb, FALSE);
    if (index < 0)
        return FALSE;

    success = dpb_output(dpb, dpb->pictures[index]);
    if (!VAAPI_PICTURE_IS_REFERENCE(dpb->pictures[index]))
        dpb_remove_index(dpb, index);
    return success;
}

static void
dpb_clear(VaapiDpb *dpb)
{
    uint64_t i;

    for (i = 0; i < dpb->num_pictures; i++)
        gst_vaapi_picture_replace(&dpb->pictures[i], NULL);
    dpb->num_pictures = 0;
}

static void
dpb_flush(VaapiDpb *dpb)
{
    while (dpb_bump(dpb))
        ;
    dpb_clear(dpb);
}

static bool
dpb_add(VaapiDpb *dpb, VaapiPicture *picture)
{
    uint64_t i;

    // Remove all unused pictures
    i = 0;
    while (i < dpb->num_pictures) {
        VaapiPicture * const picture = dpb->pictures[i];
        if (VAAPI_PICTURE_IS_OUTPUT(picture) &&
            !VAAPI_PICTURE_IS_REFERENCE(picture))
            dpb_remove_index(dpb, i);
        else
            i++;
    }

    // Store reference decoded picture into the DPB
    if (VAAPI_PICTURE_IS_REFERENCE(picture)) {
        while (dpb->num_pictures == dpb->max_pictures) {
            if (!dpb_bump(dpb))
                return FALSE;
        }
    }

    // Store non-reference decoded picture into the DPB
    else {
        if (VAAPI_PICTURE_IS_SKIPPED(picture))
            return TRUE;
        while (dpb->num_pictures == dpb->max_pictures) {
            for (i = 0; i < dpb->num_pictures; i++) {
                if (!VAAPI_PICTURE_IS_OUTPUT(picture) &&
                    dpb->pictures[i]->poc < picture->poc)
                    break;
            }
            if (i == dpb->num_pictures)
                return dpb_output(dpb, picture);
            if (!dpb_bump(dpb))
                return FALSE;
        }
    }
    gst_vaapi_picture_replace(&dpb->pictures[dpb->num_pictures++], picture);
    return TRUE;
}

static void
dpb_get_neighbours(VaapiDpb *dpb, VaapiPicture *picture,
    VaapiPicture **prev_picture_ptr, VaapiPicture **next_picture_ptr)
{
    VaapiPicture *prev_picture = NULL;
    VaapiPicture *next_picture = NULL;
    uint64_t i;

    /* Find the first picture with POC > specified picture POC */
    for (i = 0; i < dpb->num_pictures; i++) {
        VaapiPicture * const ref_picture = dpb->pictures[i];
        if (ref_picture->poc == picture->poc) {
            if (i > 0)
                prev_picture = dpb->pictures[i - 1];
            if (i + 1 < dpb->num_pictures)
                next_picture = dpb->pictures[i + 1];
            break;
        }
        else if (ref_picture->poc > picture->poc) {
            next_picture = ref_picture;
            if (i > 0)
                prev_picture = dpb->pictures[i - 1];
            break;
        }
    }

    assert(next_picture ? next_picture->poc > picture->poc : TRUE);
    assert(prev_picture ? prev_picture->poc < picture->poc : TRUE);

    if (prev_picture_ptr)
        *prev_picture_ptr = prev_picture;
    if (next_picture_ptr)
        *next_picture_ptr = next_picture;
}

static bool
dpb2_add(VaapiDpb *dpb, VaapiPicture *picture)
{
    VaapiPicture *ref_picture;
    int64_t index = -1;

    g_return_val_if_fail(VAAPI_IS_DPB(dpb), FALSE);
    g_return_val_if_fail(dpb->max_pictures == 2, FALSE);

    /*
     * Purpose: only store reference decoded pictures into the DPB
     *
     * This means:
     * - non-reference decoded pictures are output immediately
     * - ... thus causing older reference pictures to be output, if not already
     * - the oldest reference picture is replaced with the new reference picture
     */
    if (G_LIKELY(dpb->num_pictures == 2)) {
        index = (dpb->pictures[0]->poc > dpb->pictures[1]->poc);
        ref_picture = dpb->pictures[index];
        if (!VAAPI_PICTURE_IS_OUTPUT(ref_picture)) {
            if (!dpb_output(dpb, ref_picture))
                return FALSE;
        }
    }

    if (!VAAPI_PICTURE_IS_REFERENCE(picture))
        return dpb_output(dpb, picture);

    if (index < 0)
        index = dpb->num_pictures++;
    gst_vaapi_picture_replace(&dpb->pictures[index], picture);
    return TRUE;
}

static void
dpb2_get_neighbours(VaapiDpb *dpb, VaapiPicture *picture,
    VaapiPicture **prev_picture_ptr, VaapiPicture **next_picture_ptr)
{
    VaapiPicture *ref_picture, *ref_pictures[2];
    VaapiPicture **picture_ptr;
    uint64_t i, index;

    g_return_if_fail(GST_VAAPI_IS_DPB(dpb));
    g_return_if_fail(dpb->max_pictures == 2);
    g_return_if_fail(GST_VAAPI_IS_PICTURE(picture));

    ref_pictures[0] = NULL;
    ref_pictures[1] = NULL;
    for (i = 0; i < dpb->num_pictures; i++) {
        ref_picture = dpb->pictures[i];
        index       = ref_picture->poc > picture->poc;
        picture_ptr = &ref_pictures[index];
        if (!*picture_ptr || ((*picture_ptr)->poc > ref_picture->poc) == index)
            *picture_ptr = ref_picture;
    }

    if (prev_picture_ptr)
        *prev_picture_ptr = ref_pictures[0];
    if (next_picture_ptr)
        *next_picture_ptr = ref_pictures[1];
}

static const VaapiMiniObjectClass *
vaapi_dpb_class(void)
{
    static const VaapiDpbClass VaapiDpbClass = {
        { sizeof(VaapiDpb),
          (GDestroyNotify)gst_vaapi_dpb_finalize },

        dpb_flush,
        dpb_add,
        dpb_get_neighbours
    };
    return &VaapiDpbClass.parent_class;
}

static const VaapiMiniObjectClass *
vaapi_dpb2_class(void)
{
    static const VaapiDpbClass VaapiDpb2Class = {
        { sizeof(VaapiDpb),
          (GDestroyNotify)gst_vaapi_dpb_finalize },

        dpb_flush,
        dpb2_add,
        dpb2_get_neighbours
    };
    return &VaapiDpb2Class.parent_class;
}
 void VaapiMPEG2DPB::
vaapi_dpb_finalize(VaapiDpb *dpb)
{
    dpb_clear(dpb);
    g_free(dpb->pictures);
}




VaapiDpb * VaapiMPEG2DPB::
vaapi_dpb_new(uint64_t max_pictures)
{
    return dpb_new(max_pictures);
}

void VaapiMPEG2DPB::vaapi_dpb_flush(VaapiDpb *dpb)
{
    const VaapiDpbClass *klass;

    g_return_if_fail(VAAPI_IS_DPB(dpb));

    klass = VAAPI_DPB_GET_CLASS(dpb);
    if (G_UNLIKELY(!klass || !klass->add))
        return;
    klass->flush(dpb);
}

bool VaapiMPEG2DPB::
vaapi_dpb_add(VaapiDpb *dpb, VaapiPicture *picture)
{
    const VaapiDpbClass *klass;

    g_return_val_if_fail(VAAPI_IS_DPB(dpb), FALSE);
    g_return_val_if_fail(VAAPI_IS_PICTURE(picture), FALSE);

    klass = VAAPI_DPB_GET_CLASS(dpb);
    if (G_UNLIKELY(!klass || !klass->add))
        return FALSE;
    return klass->add(dpb, picture);
}

uint64_t
vaapi_dpb_size(VaapiDpb *dpb)
{
    g_return_val_if_fail(VAAPI_IS_DPB(dpb), 0);

    return dpb->num_pictures;
}

void VaapiMPEG2DPB::
vaapi_dpb_get_neighbours(VaapiDpb *dpb, VaapiPicture *picture,
    VaapiPicture **prev_picture_ptr, VaapiPicture **next_picture_ptr)
{
    const VaapiDpbClass *klass;

    g_return_if_fail(VAAPI_IS_DPB(dpb));
    g_return_if_fail(VAAPI_IS_PICTURE(picture));

    klass = VAAPI_DPB_GET_CLASS(dpb);
    if (G_UNLIKELY(!klass || !klass->get_neighbours))
        return;
    klass->get_neighbours(dpb, picture, prev_picture_ptr, next_picture_ptr);
}

}
