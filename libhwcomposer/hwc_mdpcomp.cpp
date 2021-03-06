/*
 * Copyright (C) 2012-2013, The Linux Foundation. All rights reserved.
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
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

#include "hwc_mdpcomp.h"
#include <sys/ioctl.h>
#include "external.h"
#include "mdp_version.h"
#include "qdMetaData.h"

namespace qhwc {

namespace ovutils = overlay::utils;

//==============MDPComp========================================================

IdleInvalidator *MDPComp::idleInvalidator = NULL;
bool MDPComp::sIdleFallBack = false;
bool MDPComp::sDebugLogs = false;
bool MDPComp::sEnabled = false;

MDPComp* MDPComp::getObject(const int& width) {
    if(width <= MAX_DISPLAY_DIM) {
        return new MDPCompLowRes();
    } else {
        return new MDPCompHighRes();
    }
}

void MDPComp::dump(android::String8& buf)
{
    dumpsys_log(buf, "  MDP Composition: ");
    dumpsys_log(buf, "MDPCompState=%d\n", mState);
    //XXX: Log more info
}

bool MDPComp::init(hwc_context_t *ctx) {

    if(!ctx) {
        ALOGE("%s: Invalid hwc context!!",__FUNCTION__);
        return false;
    }

    if((qdutils::MDPVersion::getInstance().getMDPVersion() >= qdutils::MDP_V4_2) &&
            !setupBasePipe(ctx)) {
        ALOGE("%s: Failed to setup primary base pipe", __FUNCTION__);
        return false;
    }

    char property[PROPERTY_VALUE_MAX];

    sEnabled = false;
    if((property_get("persist.hwc.mdpcomp.enable", property, NULL) > 0) &&
            (!strncmp(property, "1", PROPERTY_VALUE_MAX ) ||
             (!strncasecmp(property,"true", PROPERTY_VALUE_MAX )))) {
        sEnabled = true;
    }

    sDebugLogs = false;
    if(property_get("debug.mdpcomp.logs", property, NULL) > 0) {
        if(atoi(property) != 0)
            sDebugLogs = true;
    }

    unsigned long idle_timeout = DEFAULT_IDLE_TIME;
    if(property_get("debug.mdpcomp.idletime", property, NULL) > 0) {
        if(atoi(property) != 0)
            idle_timeout = atoi(property);
    }

    //create Idle Invalidator
    idleInvalidator = IdleInvalidator::getInstance();

    if(idleInvalidator == NULL) {
        ALOGE("%s: failed to instantiate idleInvalidator  object", __FUNCTION__);
    } else {
        idleInvalidator->init(timeout_handler, ctx, idle_timeout);
    }
    return true;
}

void MDPComp::timeout_handler(void *udata) {
    struct hwc_context_t* ctx = (struct hwc_context_t*)(udata);

    if(!ctx) {
        ALOGE("%s: received empty data in timer callback", __FUNCTION__);
        return;
    }

    if(!ctx->proc) {
        ALOGE("%s: HWC proc not registered", __FUNCTION__);
        return;
    }
    sIdleFallBack = true;
    /* Trigger SF to redraw the current frame */
    ctx->proc->invalidate(ctx->proc);
}

void MDPComp::setMDPCompLayerFlags(hwc_context_t *ctx,
        hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    LayerProp *layerProp = ctx->layerProp[dpy];

    for(int index = 0; index < ctx->listStats[dpy].numAppLayers; index++ ) {
        hwc_layer_1_t* layer = &(list->hwLayers[index]);
        layerProp[index].mFlags |= HWC_MDPCOMP;
        layer->compositionType = HWC_OVERLAY;
        layer->hints |= HWC_HINT_CLEAR_FB;
    }
}

void MDPComp::unsetMDPCompLayerFlags(hwc_context_t* ctx,
        hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    LayerProp *layerProp = ctx->layerProp[dpy];

    for (int index = 0 ;
            index < ctx->listStats[dpy].numAppLayers; index++) {
        if(layerProp[index].mFlags & HWC_MDPCOMP) {
            layerProp[index].mFlags &= ~HWC_MDPCOMP;
        }

        if(list->hwLayers[index].compositionType == HWC_OVERLAY) {
            list->hwLayers[index].compositionType = HWC_FRAMEBUFFER;
        }
    }
}

/*
 * Sets up BORDERFILL as default base pipe and detaches RGB0.
 * Framebuffer is always updated using PLAY ioctl.
 */
bool MDPComp::setupBasePipe(hwc_context_t *ctx) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    int fb_stride = ctx->dpyAttr[dpy].stride;
    int fb_width = ctx->dpyAttr[dpy].xres;
    int fb_height = ctx->dpyAttr[dpy].yres;
    int fb_fd = ctx->dpyAttr[dpy].fd;

    mdp_overlay ovInfo;
    msmfb_overlay_data ovData;
    memset(&ovInfo, 0, sizeof(mdp_overlay));
    memset(&ovData, 0, sizeof(msmfb_overlay_data));

    int mdpVersion = qdutils::MDPVersion::getInstance().getMDPVersion();
    if (mdpVersion >= qdutils::MDP_V4_2)
        ovInfo.src.format = MDP_RGB_BORDERFILL;
    else
        ovInfo.src.format = MDP_RGBA_8888;
    ovInfo.src.width  = fb_width;
    ovInfo.src.height = fb_height;
    ovInfo.src_rect.w = fb_width;
    ovInfo.src_rect.h = fb_height;
    ovInfo.dst_rect.w = fb_width;
    ovInfo.dst_rect.h = fb_height;
    ovInfo.id = MSMFB_NEW_REQUEST;

    if (ioctl(fb_fd, MSMFB_OVERLAY_SET, &ovInfo) < 0) {
        ALOGE("Failed to call ioctl MSMFB_OVERLAY_SET err=%s",
                strerror(errno));
        return false;
    }

    ovData.id = ovInfo.id;
    if (ioctl(fb_fd, MSMFB_OVERLAY_PLAY, &ovData) < 0) {
        ALOGE("Failed to call ioctl MSMFB_OVERLAY_PLAY err=%s",
                strerror(errno));
        return false;
    }
    return true;
}

void MDPComp::reset(hwc_context_t *ctx,
        hwc_display_contents_1_t* list ) {
    //Reset flags and states
    unsetMDPCompLayerFlags(ctx, list);
    if(mCurrentFrame.pipeLayer) {
        for(int i = 0 ; i < mCurrentFrame.count; i++ ) {
            if(mCurrentFrame.pipeLayer[i].pipeInfo) {
                delete mCurrentFrame.pipeLayer[i].pipeInfo;
                mCurrentFrame.pipeLayer[i].pipeInfo = NULL;
            }
        }
        free(mCurrentFrame.pipeLayer);
        mCurrentFrame.pipeLayer = NULL;
    }
    mCurrentFrame.count = 0;
}

void MDPComp::setVidInfo(hwc_layer_1_t *layer,
        ovutils::eMdpFlags &mdpFlags) {
    private_handle_t *hnd = (private_handle_t *)layer->handle;
#ifdef QCOM_BSP
    MetaData_t *metadata = (MetaData_t *)hnd->base_metadata;
#endif
    if(isSecureBuffer(hnd)) {
        ovutils::setMdpFlags(mdpFlags, ovutils::OV_MDP_SECURE_OVERLAY_SESSION);
    }
#ifdef QCOM_BSP
    if((metadata->operation & PP_PARAM_INTERLACED) && metadata->interlaced) {
        ovutils::setMdpFlags(mdpFlags, ovutils::OV_MDP_DEINTERLACE);
    }
#endif
}

bool MDPComp::isWidthValid(hwc_context_t *ctx, hwc_layer_1_t *layer) {

    const int dpy = HWC_DISPLAY_PRIMARY;
    private_handle_t *hnd = (private_handle_t *)layer->handle;

    if(!hnd) {
        ALOGE("%s: layer handle is NULL", __FUNCTION__);
        return false;
    }

    int hw_w = ctx->dpyAttr[dpy].xres;
    int hw_h = ctx->dpyAttr[dpy].yres;

    hwc_rect_t sourceCrop = layer->sourceCrop;
    hwc_rect_t displayFrame = layer->displayFrame;

    hwc_rect_t crop =  sourceCrop;
    int crop_w = crop.right - crop.left;
    int crop_h = crop.bottom - crop.top;

    hwc_rect_t dst = displayFrame;
    int dst_w = dst.right - dst.left;
    int dst_h = dst.bottom - dst.top;

    if(dst.left < 0 || dst.top < 0 || dst.right > hw_w || dst.bottom > hw_h) {
       hwc_rect_t scissor = {0, 0, hw_w, hw_h };
       qhwc::calculate_crop_rects(crop, dst, scissor, layer->transform);
       crop_w = crop.right - crop.left;
       crop_h = crop.bottom - crop.top;
    }

    //Workaround for MDP HW limitation in DSI command mode panels where
    //FPS will not go beyond 30 if buffers on RGB pipes are of width < 5

    if(crop_w < 5)
        return false;

    return true;
}

ovutils::eDest MDPComp::getMdpPipe(hwc_context_t *ctx, ePipeType type) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    ovutils::eDest mdp_pipe = ovutils::OV_INVALID;

    switch(type) {
        case MDPCOMP_OV_DMA:
            mdp_pipe = ov.nextPipe(ovutils::OV_MDP_PIPE_DMA, dpy);
            if(mdp_pipe != ovutils::OV_INVALID) {
                return mdp_pipe;
            }
        case MDPCOMP_OV_ANY:
        case MDPCOMP_OV_RGB:
            mdp_pipe = ov.nextPipe(ovutils::OV_MDP_PIPE_RGB, dpy);
            if(mdp_pipe != ovutils::OV_INVALID) {
                return mdp_pipe;
            }

            if(type == MDPCOMP_OV_RGB) {
                //Requested only for RGB pipe
                break;
            }
        case  MDPCOMP_OV_VG:
            return ov.nextPipe(ovutils::OV_MDP_PIPE_VG, dpy);
        default:
            ALOGE("%s: Invalid pipe type",__FUNCTION__);
            return ovutils::OV_INVALID;
    };
    return ovutils::OV_INVALID;
}

bool MDPComp::isDoable(hwc_context_t *ctx,
        hwc_display_contents_1_t* list) {
    //Number of layers
    const int dpy = HWC_DISPLAY_PRIMARY;
    int numAppLayers = ctx->listStats[dpy].numAppLayers;

    overlay::Overlay& ov = *ctx->mOverlay;
    int availablePipes = ov.availablePipes(dpy);

    if(numAppLayers < 1 || numAppLayers > MAX_PIPES_PER_MIXER ||
                           pipesNeeded(ctx, list) > availablePipes) {
        ALOGD_IF(isDebug(), "%s: Unsupported number of layers",__FUNCTION__);
        return false;
    }

    if(ctx->mExtDispConfiguring) {
        ALOGD_IF( isDebug(),"%s: External Display connection is pending",
                __FUNCTION__);
        return false;
    }

    if(isSecuring(ctx)) {
        ALOGD_IF(isDebug(), "%s: MDP securing is active", __FUNCTION__);
        return false;
    }

    if(ctx->mSecureMode)
        return false;

    //Check for skip layers
    if(isSkipPresent(ctx, dpy)) {
        ALOGD_IF(isDebug(), "%s: Skip layers are present",__FUNCTION__);
        return false;
    }

    if(ctx->listStats[dpy].needsAlphaScale
                     && ctx->mMDP.version < qdutils::MDSS_V5) {
        ALOGD_IF(isDebug(), "%s: frame needs alpha downscaling",__FUNCTION__);
        return false;
    }

    //FB composition on idle timeout
    if(sIdleFallBack) {
        sIdleFallBack = false;
        ALOGD_IF(isDebug(), "%s: idle fallback",__FUNCTION__);
        return false;
    }

    //MDP composition is not efficient if layer needs rotator.
    for(int i = 0; i < numAppLayers; ++i) {
        // As MDP h/w supports flip operation, use MDP comp only for
        // 180 transforms. Fail for any transform involving 90 (90, 270).
        hwc_layer_1_t* layer = &list->hwLayers[i];
        private_handle_t *hnd = (private_handle_t *)layer->handle;
        if((layer->transform & HWC_TRANSFORM_ROT_90)  && (!isYuvBuffer(hnd)
                                                            || !canRotate())) {
            ALOGD_IF(isDebug(), "%s: orientation involved",__FUNCTION__);
            return false;
        }

        if(!isYuvBuffer(hnd) && !isWidthValid(ctx,layer)) {
            ALOGD_IF(isDebug(), "%s: Buffer is of invalid width",__FUNCTION__);
            return false;
        }
    }
    return true;
}

bool MDPComp::setup(hwc_context_t* ctx, hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    if(!ctx) {
        ALOGE("%s: invalid context", __FUNCTION__);
        return -1;
    }

    if(!allocLayerPipes(ctx, list, mCurrentFrame)) {
        ALOGD_IF(isDebug(), "%s: Falling back to FB", __FUNCTION__);
        return false;
    }

    for (int index = 0 ; index < mCurrentFrame.count; index++) {
        hwc_layer_1_t* layer = &list->hwLayers[index];
        MdpPipeInfo* cur_pipe = mCurrentFrame.pipeLayer[index].pipeInfo;

        if(configure(ctx, layer, cur_pipe) != 0 ) {
            ALOGD_IF(isDebug(), "%s: MDPComp failed to configure overlay for \
                    layer %d",__FUNCTION__, index);
            return false;
        }
    }
    return true;
}

bool MDPComp::prepare(hwc_context_t *ctx,
        hwc_display_contents_1_t* list) {
    if(!isEnabled()) {
        ALOGE_IF(isDebug(),"%s: MDP Comp. not enabled.", __FUNCTION__);
        return false;
    }

    overlay::Overlay& ov = *ctx->mOverlay;
    bool isMDPCompUsed = true;

    //reset old data
    reset(ctx, list);

    bool doable = isDoable(ctx, list);
    if(doable) {
        if(setup(ctx, list)) {
            setMDPCompLayerFlags(ctx, list);
        } else {
            ALOGD_IF(isDebug(),"%s: MDP Comp Failed",__FUNCTION__);
            isMDPCompUsed = false;
        }
    } else {
        ALOGD_IF( isDebug(),"%s: MDP Comp not possible[%d]",__FUNCTION__,
                doable);
        isMDPCompUsed = false;
    }

    //Reset states
    if(!isMDPCompUsed) {
        //Reset current frame
        reset(ctx, list);
    }

    mState = isMDPCompUsed ? MDPCOMP_ON : MDPCOMP_OFF;
    return isMDPCompUsed;
}

//=============MDPCompLowRes===================================================

/*
 * Configures pipe(s) for MDP composition
 */
int MDPCompLowRes::configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
        MdpPipeInfo* mdpInfo) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    private_handle_t *hnd = (private_handle_t *)layer->handle;
    overlay::Overlay& ov = *ctx->mOverlay;

    if(!hnd) {
        ALOGE("%s: layer handle is NULL", __FUNCTION__);
        return -1;
    }

    MdpPipeInfoLowRes& mdp_info = *(MdpPipeInfoLowRes*)mdpInfo;

    int hw_w = ctx->dpyAttr[dpy].xres;
    int hw_h = ctx->dpyAttr[dpy].yres;

    hwc_rect_t crop = layer->sourceCrop;
    hwc_rect_t dst = layer->displayFrame;

    int crop_w = crop.right - crop.left;
    int crop_h = crop.bottom - crop.top;

    int dst_w = dst.right - dst.left;
    int dst_h = dst.bottom - dst.top;

    if(dst.left < 0 || dst.top < 0 ||
            dst.right > hw_w || dst.bottom > hw_h) {
        ALOGD_IF(isDebug(),"%s: Destination has negative coordinates",
                __FUNCTION__);
        hwc_rect_t scissor = {0, 0, hw_w, hw_h };
        qhwc::calculate_crop_rects(crop, dst, scissor, layer->transform);

        //Update calulated width and height
        crop_w = crop.right - crop.left;
        crop_h = crop.bottom - crop.top;

        dst_w = dst.right - dst.left;
        dst_h = dst.bottom - dst.top;
    }

    if( (dst_w > hw_w)|| (dst_h > hw_h)) {
        ALOGD_IF(isDebug(),"%s: Dest rect exceeds FB", __FUNCTION__);
        dst_w = hw_w;
        dst_h = hw_h;
    }

    // Determine pipe to set based on pipe index
    ovutils::eDest dest = mdp_info.index;

    ovutils::eZorder zOrder = ovutils::ZORDER_0;

    if(mdp_info.zOrder == 0 ) {
        zOrder = ovutils::ZORDER_0;
    } else if(mdp_info.zOrder == 1 ) {
        zOrder = ovutils::ZORDER_1;
    } else if(mdp_info.zOrder == 2 ) {
        zOrder = ovutils::ZORDER_2;
    } else if(mdp_info.zOrder == 3) {
        zOrder = ovutils::ZORDER_3;
    }

    // Order order order
    // setSource - just setting source
    // setParameter - changes src w/h/f accordingly
    // setCrop - ROI - src_rect
    // setPosition - dst_rect
    // commit - commit changes to mdp driver
    // queueBuffer - not here, happens when draw is called

    ovutils::Whf info(hnd->width, hnd->height, hnd->format, hnd->size);

    ovutils::eMdpFlags mdpFlags = ovutils::OV_MDP_FLAGS_NONE;

    if(isYuvBuffer(hnd))
        setVidInfo(layer, mdpFlags);

    if (!(isYuvBuffer(hnd) &&
                (qdutils::MDPVersion::getInstance().getMDPVersion() < qdutils::MDP_V4_2)))
        ovutils::setMdpFlags(mdpFlags,ovutils::OV_MDP_BACKEND_COMPOSITION);

    if(layer->blending == HWC_BLENDING_PREMULT) {
        ovutils::setMdpFlags(mdpFlags,
                ovutils::OV_MDP_BLEND_FG_PREMULT);
    }

    ovutils::eTransform orient = overlay::utils::OVERLAY_TRANSFORM_0 ;

    if(!(layer->transform & HWC_TRANSFORM_ROT_90)) {
        if(layer->transform & HWC_TRANSFORM_FLIP_H) {
            ovutils::setMdpFlags(mdpFlags, ovutils::OV_MDP_FLIP_H);
        }

        if(layer->transform & HWC_TRANSFORM_FLIP_V) {
            ovutils::setMdpFlags(mdpFlags,  ovutils::OV_MDP_FLIP_V);
        }
    } else {
        orient = static_cast<ovutils::eTransform>(layer->transform);
    }

    ovutils::eRotFlags rotFlags = ovutils::ROT_FLAGS_NONE;
    if(isYuvBuffer(hnd) && (ctx->mMDP.version >= qdutils::MDP_V4_2 &&
                           ctx->mMDP.version < qdutils::MDSS_V5)) {
        rotFlags = ovutils::ROT_DOWNSCALE_ENABLED;
    }

    ovutils::PipeArgs parg(mdpFlags,
            info,
            zOrder,
            ovutils::IS_FG_OFF,
            rotFlags);

    ov.setSource(parg, dest);

    ov.setTransform(orient, dest);

    ovutils::Dim dcrop(crop.left, crop.top, crop_w, crop_h);
    ov.setCrop(dcrop, dest);

    ovutils::Dim dim(dst.left, dst.top, dst_w, dst_h);
    ov.setPosition(dim, dest);

    ALOGD_IF(isDebug(),"%s: MDP set: crop[%d,%d,%d,%d] dst[%d,%d,%d,%d] \
            nPipe: %d zorder: %d",__FUNCTION__, dcrop.x,
            dcrop.y,dcrop.w, dcrop.h, dim.x, dim.y, dim.w, dim.h,
            mdp_info.index, mdp_info.zOrder);

    if (!ov.commit(dest)) {
        ALOGE("%s: commit failed", __FUNCTION__);
        return -1;
    }
    return 0;
}

int MDPCompLowRes::pipesNeeded(hwc_context_t *ctx,
                        hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    return ctx->listStats[dpy].numAppLayers;
}

bool MDPCompLowRes::allocLayerPipes(hwc_context_t *ctx,
        hwc_display_contents_1_t* list,
        FrameInfo& currentFrame) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    int layer_count = ctx->listStats[dpy].numAppLayers;

    currentFrame.count = layer_count;
    currentFrame.pipeLayer = (PipeLayerPair*)
            malloc(sizeof(PipeLayerPair) * currentFrame.count);

    if(isYuvPresent(ctx, dpy)) {
        int nYuvCount = ctx->listStats[dpy].yuvCount;

        for(int index = 0; index < nYuvCount; index ++) {
            int nYuvIndex = ctx->listStats[dpy].yuvIndices[index];
            hwc_layer_1_t* layer = &list->hwLayers[nYuvIndex];
            PipeLayerPair& info = currentFrame.pipeLayer[nYuvIndex];
            info.pipeInfo = new MdpPipeInfoLowRes;
            MdpPipeInfoLowRes& pipe_info = *(MdpPipeInfoLowRes*)info.pipeInfo;
            pipe_info.index = getMdpPipe(ctx, MDPCOMP_OV_VG);
            if(pipe_info.index == ovutils::OV_INVALID) {
                ALOGD_IF(isDebug(), "%s: Unable to get pipe for Videos",
                        __FUNCTION__);
                return false;
            }
            pipe_info.zOrder = nYuvIndex;
        }
    }

    for(int index = 0 ; index < layer_count ; index++ ) {
        hwc_layer_1_t* layer = &list->hwLayers[index];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        if(isYuvBuffer(hnd))
            continue;

        PipeLayerPair& info = currentFrame.pipeLayer[index];
        info.pipeInfo = new MdpPipeInfoLowRes;
        MdpPipeInfoLowRes& pipe_info = *(MdpPipeInfoLowRes*)info.pipeInfo;

        pipe_info.index = getMdpPipe(ctx, MDPCOMP_OV_ANY);
        if(pipe_info.index == ovutils::OV_INVALID) {
            ALOGD_IF(isDebug(), "%s: Unable to get pipe for UI", __FUNCTION__);
            return false;
        }
        pipe_info.zOrder = index;
    }
    return true;
}

bool MDPCompLowRes::draw(hwc_context_t *ctx, hwc_display_contents_1_t* list) {

    if(!isEnabled() || !isUsed()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp not configured", __FUNCTION__);
        return true;
    }

    if(!ctx || !list) {
        ALOGE("%s: invalid contxt or list",__FUNCTION__);
        return false;
    }

    /* reset Invalidator */
    if(idleInvalidator)
        idleInvalidator->markForSleep();

    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    LayerProp *layerProp = ctx->layerProp[dpy];

    int numHwLayers = ctx->listStats[dpy].numAppLayers;
    for(int i = 0; i < numHwLayers; i++ )
    {
        hwc_layer_1_t *layer = &list->hwLayers[i];

        if(!(layerProp[i].mFlags & HWC_MDPCOMP)) {
            continue;
        }

        MdpPipeInfoLowRes& pipe_info =
                *(MdpPipeInfoLowRes*)mCurrentFrame.pipeLayer[i].pipeInfo;
        ovutils::eDest dest = pipe_info.index;

        if(dest == ovutils::OV_INVALID) {
            ALOGE("%s: Invalid pipe index (%d)", __FUNCTION__, dest);
            return false;
        }

        if (ctx ) {
            private_handle_t *hnd = (private_handle_t *)layer->handle;
            if(!hnd) {
                ALOGE("%s handle null", __FUNCTION__);
                return false;
            }

            ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                    using  pipe: %d", __FUNCTION__, layer,
                    hnd, dest );

            if (!ov.queueBuffer(hnd->fd, hnd->offset, dest)) {
                ALOGE("%s: queueBuffer failed for external", __FUNCTION__);
                return false;
            }
        }

        layerProp[i].mFlags &= ~HWC_MDPCOMP;
    }
    return true;
}

//=============MDPCompHighRes===================================================

int MDPCompHighRes::pipesNeeded(hwc_context_t *ctx,
                        hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    int numAppLayers = ctx->listStats[dpy].numAppLayers;
    int pipesNeeded = 0;

    int hw_w = ctx->dpyAttr[dpy].xres;

    for(int i = 0; i < numAppLayers; ++i) {
        hwc_layer_1_t* layer = &list->hwLayers[i];
        hwc_rect_t dst = layer->displayFrame;
      if(dst.left > hw_w/2) {
          pipesNeeded++;
      } else if(dst.right <= hw_w/2) {
          pipesNeeded++;
      } else {
          pipesNeeded += 2;
      }
    }
    return pipesNeeded;
}

bool MDPCompHighRes::acquireMDPPipes(hwc_context_t *ctx, hwc_layer_1_t* layer,
                        MdpPipeInfoHighRes& pipe_info, ePipeType type) {
     const int dpy = HWC_DISPLAY_PRIMARY;
     int hw_w = ctx->dpyAttr[dpy].xres;

     hwc_rect_t dst = layer->displayFrame;
     if(dst.left > hw_w/2) {
         pipe_info.lIndex = ovutils::OV_INVALID;
         pipe_info.rIndex = getMdpPipe(ctx, type);
         if(pipe_info.rIndex == ovutils::OV_INVALID)
             return false;
     } else if (dst.right <= hw_w/2) {
         pipe_info.rIndex = ovutils::OV_INVALID;
         pipe_info.lIndex = getMdpPipe(ctx, type);
         if(pipe_info.lIndex == ovutils::OV_INVALID)
             return false;
     } else {
         pipe_info.rIndex = getMdpPipe(ctx, type);
         pipe_info.lIndex = getMdpPipe(ctx, type);
         if(pipe_info.rIndex == ovutils::OV_INVALID ||
            pipe_info.lIndex == ovutils::OV_INVALID)
             return false;
     }
     return true;
}

bool MDPCompHighRes::allocLayerPipes(hwc_context_t *ctx,
        hwc_display_contents_1_t* list,
        FrameInfo& currentFrame) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    int layer_count = ctx->listStats[dpy].numAppLayers;

    currentFrame.count = layer_count;
    currentFrame.pipeLayer = (PipeLayerPair*)
            malloc(sizeof(PipeLayerPair) * currentFrame.count);

    if(isYuvPresent(ctx, dpy)) {
        int nYuvCount = ctx->listStats[dpy].yuvCount;

        for(int index = 0; index < nYuvCount; index ++) {
            int nYuvIndex = ctx->listStats[dpy].yuvIndices[index];
            hwc_layer_1_t* layer = &list->hwLayers[nYuvIndex];
            PipeLayerPair& info = currentFrame.pipeLayer[nYuvIndex];
            info.pipeInfo = new MdpPipeInfoHighRes;
            MdpPipeInfoHighRes& pipe_info = *(MdpPipeInfoHighRes*)info.pipeInfo;
            if(!acquireMDPPipes(ctx, layer, pipe_info,MDPCOMP_OV_VG)) {
                ALOGD_IF(isDebug(),"%s: Unable to get pipe for videos",
                                                            __FUNCTION__);
                //TODO: windback pipebook data on fail
                return false;
            }
            pipe_info.zOrder = nYuvIndex;
        }
    }

    for(int index = 0 ; index < layer_count ; index++ ) {
        hwc_layer_1_t* layer = &list->hwLayers[index];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        if(isYuvBuffer(hnd))
            continue;

        PipeLayerPair& info = currentFrame.pipeLayer[index];
        info.pipeInfo = new MdpPipeInfoHighRes;
        MdpPipeInfoHighRes& pipe_info = *(MdpPipeInfoHighRes*)info.pipeInfo;

        ePipeType type = MDPCOMP_OV_ANY;

        if(!qhwc::needsScaling(layer) && !ctx->mDMAInUse
                             && ctx->mMDP.version >= qdutils::MDSS_V5)
            type = MDPCOMP_OV_DMA;

        if(!acquireMDPPipes(ctx, layer, pipe_info, type)) {
            ALOGD_IF(isDebug(), "%s: Unable to get pipe for UI", __FUNCTION__);
            //TODO: windback pipebook data on fail
            return false;
        }
        pipe_info.zOrder = index;
    }
    return true;
}
/*
 * Configures pipe(s) for MDP composition
 */
int MDPCompHighRes::configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
        MdpPipeInfo* mdpInfo) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    private_handle_t *hnd = (private_handle_t *)layer->handle;
    overlay::Overlay& ov = *ctx->mOverlay;

    if(!hnd) {
        ALOGE("%s: layer handle is NULL", __FUNCTION__);
        return -1;
    }

    MdpPipeInfoHighRes& mdp_info = *(MdpPipeInfoHighRes*)mdpInfo;

    int hw_w = ctx->dpyAttr[dpy].xres;
    int hw_h = ctx->dpyAttr[dpy].yres;

    hwc_rect_t crop = layer->sourceCrop;
    hwc_rect_t dst = layer->displayFrame;

    int crop_w = crop.right - crop.left;
    int crop_h = crop.bottom - crop.top;

    int dst_w = dst.right - dst.left;
    int dst_h = dst.bottom - dst.top;

    if(dst.left < 0 || dst.top < 0 ||
            dst.right > hw_w || dst.bottom > hw_h) {
        ALOGD_IF(isDebug(),"%s: Destination has negative coordinates",
                __FUNCTION__);
        hwc_rect_t scissor = {0, 0, hw_w, hw_h };
        qhwc::calculate_crop_rects(crop, dst, scissor, 0);

        //Update calulated width and height
        crop_w = crop.right - crop.left;
        crop_h = crop.bottom - crop.top;

        dst_w = dst.right - dst.left;
        dst_h = dst.bottom - dst.top;
    }

    if( (dst_w > hw_w)|| (dst_h > hw_h)) {
        ALOGD_IF(isDebug(),"%s: Dest rect exceeds FB", __FUNCTION__);
        dst_w = hw_w;
        dst_h = hw_h;
    }

    // Determine pipe to set based on pipe index
    ovutils::eDest l_dest = mdp_info.lIndex;
    ovutils::eDest r_dest = mdp_info.rIndex;

    ovutils::eZorder zOrder = ovutils::ZORDER_0;

    if(mdp_info.zOrder == 0 ) {
        zOrder = ovutils::ZORDER_0;
    } else if(mdp_info.zOrder == 1 ) {
        zOrder = ovutils::ZORDER_1;
    } else if(mdp_info.zOrder == 2 ) {
        zOrder = ovutils::ZORDER_2;
    } else if(mdp_info.zOrder == 3) {
        zOrder = ovutils::ZORDER_3;
    }

    // Order order order
    // setSource - just setting source
    // setParameter - changes src w/h/f accordingly
    // setCrop - ROI - src_rect
    // setPosition - dst_rect
    // commit - commit changes to mdp driver
    // queueBuffer - not here, happens when draw is called

    ovutils::Whf info(hnd->width, hnd->height, hnd->format, hnd->size);

    ovutils::eMdpFlags mdpFlagsL = ovutils::OV_MDP_FLAGS_NONE;

    if(isYuvBuffer(hnd))
        setVidInfo(layer, mdpFlagsL);

    ovutils::setMdpFlags(mdpFlagsL,ovutils::OV_MDP_BACKEND_COMPOSITION);

    if(layer->blending == HWC_BLENDING_PREMULT) {
        ovutils::setMdpFlags(mdpFlagsL,
                ovutils::OV_MDP_BLEND_FG_PREMULT);
    }

    ovutils::eTransform orient = overlay::utils::OVERLAY_TRANSFORM_0 ;

    if(!(layer->transform & HWC_TRANSFORM_ROT_90)) {
        if(layer->transform & HWC_TRANSFORM_FLIP_H) {
            ovutils::setMdpFlags(mdpFlagsL, ovutils::OV_MDP_FLIP_H);
        }

        if(layer->transform & HWC_TRANSFORM_FLIP_V) {
            ovutils::setMdpFlags(mdpFlagsL,  ovutils::OV_MDP_FLIP_V);
        }
    } else {
        orient = static_cast<ovutils::eTransform>(layer->transform);
    }

    ovutils::eMdpFlags mdpFlagsR = mdpFlagsL;
    ovutils::setMdpFlags(mdpFlagsR, ovutils::OV_MDSS_MDP_RIGHT_MIXER);

    hwc_rect_t tmp_cropL, tmp_dstL;
    hwc_rect_t tmp_cropR, tmp_dstR;

    if(l_dest != ovutils::OV_INVALID) {
        tmp_cropL = crop;
        tmp_dstL = dst;
        hwc_rect_t scissor = {0, 0, hw_w/2, hw_h };
        qhwc::calculate_crop_rects(tmp_cropL, tmp_dstL, scissor, 0);
    }
    if(r_dest != ovutils::OV_INVALID) {
        tmp_cropR = crop;
        tmp_dstR = dst;
        hwc_rect_t scissor = {hw_w/2, 0, hw_w, hw_h };
        qhwc::calculate_crop_rects(tmp_cropR, tmp_dstR, scissor, 0);
    }

    //When buffer is flipped, contents of mixer config also needs to swapped.
    //Not needed if the layer is confined to one half of the screen.
    if(layer->transform & HWC_TRANSFORM_FLIP_V &&
       l_dest != ovutils::OV_INVALID  && r_dest != ovutils::OV_INVALID ) {
        hwc_rect_t new_cropR;
        new_cropR.left = tmp_cropL.left;
        new_cropR.right = new_cropR.left + (tmp_cropR.right - tmp_cropR.left);

        hwc_rect_t new_cropL;
        new_cropL.left  = new_cropR.right;
        new_cropL.right = tmp_cropR.right;

        tmp_cropL.left =  new_cropL.left;
        tmp_cropL.right =  new_cropL.right;

        tmp_cropR.left = new_cropR.left;
        tmp_cropR.right =  new_cropR.right;

        ALOGD_IF(isDebug(),"rects on V flip: \
                 cropL(%d,%d,%d,%d) dstL(%d,%d,%d,%d) \
                 cropR(%d,%d,%d,%d) dstR(%d,%d,%d,%d)",
          tmp_cropL.left, tmp_cropL.top, tmp_cropL.right, tmp_cropL.bottom,
          tmp_dstL.left, tmp_dstL.top, tmp_dstL.right, tmp_dstL.bottom,
          tmp_cropR.left, tmp_cropR.top, tmp_cropR.right, tmp_cropR.bottom,
          tmp_dstR.left, tmp_dstR.top, tmp_dstR.right, tmp_dstR.bottom);
    }

    //**** configure left mixer ****
    if(l_dest != ovutils::OV_INVALID) {
        ovutils::PipeArgs pargL(mdpFlagsL,
                                info,
                                zOrder,
                                ovutils::IS_FG_OFF,
                                ovutils::ROT_FLAGS_NONE);

        ov.setSource(pargL, l_dest);
        ov.setTransform(orient, l_dest);
        ovutils::Dim dcropL(tmp_cropL.left, tmp_cropL.top,
                            tmp_cropL.right - tmp_cropL.left,
                            tmp_cropL.bottom - tmp_cropL.top);
        ov.setCrop(dcropL, l_dest);
        ovutils::Dim dimL(tmp_dstL.left , tmp_dstL.top,
                          tmp_dstL.right - tmp_dstL.left,
                          tmp_dstL.bottom - tmp_dstL.top);
        ov.setPosition(dimL, l_dest);

        ALOGD_IF(isDebug(),"%s: MDP set: LEFT: \
                 crop[%d,%d,%d,%d] dst[%d,%d,%d,%d] pipeIndexL: %d  zorder: %d",
                 __FUNCTION__, dcropL.x, dcropL.y,dcropL.w, dcropL.h,
                 dimL.x, dimL.y, dimL.w, dimL.h,
                 mdp_info.lIndex, mdp_info.zOrder);

        if (!ov.commit(l_dest)) {
            ALOGE("%s: commit failed for left mixer config", __FUNCTION__);
            return -1;
        }
    }

    //**** configure right mixer ****
    if(r_dest != ovutils::OV_INVALID) {
        ovutils::PipeArgs pargR(mdpFlagsR,
                                info,
                                zOrder,
                                ovutils::IS_FG_OFF,
                                ovutils::ROT_FLAGS_NONE);

        ov.setSource(pargR, r_dest);
        ov.setTransform(orient, r_dest);
        ovutils::Dim dcropR(tmp_cropR.left, tmp_cropR.top,
                            tmp_cropR.right - tmp_cropR.left,
                            tmp_cropR.bottom - tmp_cropR.top);
        ov.setCrop(dcropR, r_dest);
        ovutils::Dim dimR(tmp_dstR.left - hw_w/2, tmp_dstR.top,
                          tmp_dstR.right - tmp_dstR.left,
                          tmp_dstR.bottom - tmp_dstR.top);
        ov.setPosition(dimR, r_dest);

        ALOGD_IF(isDebug(),"%s: MDP set: RIGHT: \
                 crop[%d,%d,%d,%d] dst[%d,%d,%d,%d] pipeIndexR: %d zorder: %d",
                 __FUNCTION__, dcropR.x, dcropR.y,dcropR.w, dcropR.h,
                 dimR.x, dimR.y, dimR.w, dimR.h,
                 mdp_info.rIndex, mdp_info.zOrder);

        if (!ov.commit(r_dest)) {
            ALOGE("%s: commit failed for right mixer config", __FUNCTION__);
            return -1;
        }
    }

    return 0;
}

bool MDPCompHighRes::draw(hwc_context_t *ctx, hwc_display_contents_1_t* list) {

    if(!isEnabled() || !isUsed()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp not configured", __FUNCTION__);
        return true;
    }

    if(!ctx || !list) {
        ALOGE("%s: invalid contxt or list",__FUNCTION__);
        return false;
    }

    /* reset Invalidator */
    if(idleInvalidator)
        idleInvalidator->markForSleep();

    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    LayerProp *layerProp = ctx->layerProp[dpy];

    int numHwLayers = ctx->listStats[dpy].numAppLayers;
    for(int i = 0; i < numHwLayers; i++ )
    {
        hwc_layer_1_t *layer = &list->hwLayers[i];

        if(!(layerProp[i].mFlags & HWC_MDPCOMP)) {
            continue;
        }

        MdpPipeInfoHighRes& pipe_info =
                *(MdpPipeInfoHighRes*)mCurrentFrame.pipeLayer[i].pipeInfo;
        ovutils::eDest indexL = pipe_info.lIndex;
        ovutils::eDest indexR = pipe_info.rIndex;

        //************* play left mixer **********
        if(indexL != ovutils::OV_INVALID) {
            ovutils::eDest destL = (ovutils::eDest)indexL;
            if (ctx ) {
                private_handle_t *hnd = (private_handle_t *)layer->handle;
                if(!hnd) {
                    ALOGE("%s handle null", __FUNCTION__);
                    return false;
                }

                ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                        using  pipe: %d", __FUNCTION__, layer, hnd, indexL );

                if (!ov.queueBuffer(hnd->fd, hnd->offset, destL)) {
                    ALOGE("%s: queueBuffer failed for external", __FUNCTION__);
                    return false;
                }
            }
        }

        //************* play right mixer **********
        if(indexR != ovutils::OV_INVALID) {
            ovutils::eDest destR = (ovutils::eDest)indexR;
            if (ctx ) {
                private_handle_t *hnd = (private_handle_t *)layer->handle;
                if(!hnd) {
                    ALOGE("%s handle null", __FUNCTION__);
                    return false;
                }

                ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                        using  pipe: %d", __FUNCTION__, layer, hnd, indexR );

                if (!ov.queueBuffer(hnd->fd, hnd->offset, destR)) {
                    ALOGE("%s: queueBuffer failed for external", __FUNCTION__);
                    return false;
                }
            }
        }
        layerProp[i].mFlags &= ~HWC_MDPCOMP;
    }
    return true;
}
}; //namespace

