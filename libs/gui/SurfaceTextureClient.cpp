/*
 * Copyright (C) 2010 The Android Open Source Project
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

#define LOG_TAG "SurfaceTextureClient"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include <android/native_window.h>

#include <utils/Log.h>
#include <utils/Trace.h>

#ifdef ALLWINNER
#include <hardware/hwcomposer.h>
#endif
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceTexture.h>
#include <gui/SurfaceTextureClient.h>

#include <private/gui/ComposerService.h>
#ifdef QCOM_HARDWARE
#include <gralloc_priv.h>
#endif

namespace android {

SurfaceTextureClient::SurfaceTextureClient(
        const sp<ISurfaceTexture>& surfaceTexture)
{
    SurfaceTextureClient::init();
    SurfaceTextureClient::setISurfaceTexture(surfaceTexture);
}

// see SurfaceTextureClient.h
SurfaceTextureClient::SurfaceTextureClient(const
         sp<SurfaceTexture>& surfaceTexture)
{
    SurfaceTextureClient::init();
    SurfaceTextureClient::setISurfaceTexture(surfaceTexture->getBufferQueue());
}

SurfaceTextureClient::SurfaceTextureClient() {
    SurfaceTextureClient::init();
}

SurfaceTextureClient::~SurfaceTextureClient() {
    if (mConnectedToCpu) {
        SurfaceTextureClient::disconnect(NATIVE_WINDOW_API_CPU);
    }
}

void SurfaceTextureClient::init() {
    // Initialize the ANativeWindow function pointers.
    ANativeWindow::setSwapInterval  = hook_setSwapInterval;
    ANativeWindow::dequeueBuffer    = hook_dequeueBuffer;
    ANativeWindow::cancelBuffer     = hook_cancelBuffer;
    ANativeWindow::lockBuffer       = hook_lockBuffer;
    ANativeWindow::queueBuffer      = hook_queueBuffer;
    ANativeWindow::query            = hook_query;
    ANativeWindow::perform          = hook_perform;

    const_cast<int&>(ANativeWindow::minSwapInterval) = 0;
    const_cast<int&>(ANativeWindow::maxSwapInterval) = 1;

    mReqWidth = 0;
    mReqHeight = 0;
    mReqFormat = 0;
    mReqUsage = 0;
    mReqExtUsage = 0;
    mTimestamp = NATIVE_WINDOW_TIMESTAMP_AUTO;
    mCrop.clear();
    mScalingMode = NATIVE_WINDOW_SCALING_MODE_FREEZE;
    mTransform = 0;
    mDefaultWidth = 0;
    mDefaultHeight = 0;
    mUserWidth = 0;
    mUserHeight = 0;
    mTransformHint = 0;
    mConsumerRunningBehind = false;
    mConnectedToCpu = false;
}

void SurfaceTextureClient::setISurfaceTexture(
        const sp<ISurfaceTexture>& surfaceTexture)
{
    mSurfaceTexture = surfaceTexture;
}

sp<ISurfaceTexture> SurfaceTextureClient::getISurfaceTexture() const {
    return mSurfaceTexture;
}

int SurfaceTextureClient::hook_setSwapInterval(ANativeWindow* window, int interval) {
    SurfaceTextureClient* c = getSelf(window);
    return c->setSwapInterval(interval);
}

int SurfaceTextureClient::hook_dequeueBuffer(ANativeWindow* window,
        ANativeWindowBuffer** buffer) {
    SurfaceTextureClient* c = getSelf(window);
    return c->dequeueBuffer(buffer);
}

int SurfaceTextureClient::hook_cancelBuffer(ANativeWindow* window,
        ANativeWindowBuffer* buffer) {
    SurfaceTextureClient* c = getSelf(window);
    return c->cancelBuffer(buffer);
}

int SurfaceTextureClient::hook_lockBuffer(ANativeWindow* window,
        ANativeWindowBuffer* buffer) {
    SurfaceTextureClient* c = getSelf(window);
    return c->lockBuffer(buffer);
}

int SurfaceTextureClient::hook_queueBuffer(ANativeWindow* window,
        ANativeWindowBuffer* buffer) {
    SurfaceTextureClient* c = getSelf(window);
    return c->queueBuffer(buffer);
}

int SurfaceTextureClient::hook_query(const ANativeWindow* window,
                                int what, int* value) {
    const SurfaceTextureClient* c = getSelf(window);
    return c->query(what, value);
}

int SurfaceTextureClient::hook_perform(ANativeWindow* window, int operation, ...) {
    va_list args;
    va_start(args, operation);
    SurfaceTextureClient* c = getSelf(window);
    return c->perform(operation, args);
}

int SurfaceTextureClient::setSwapInterval(int interval) {
    ATRACE_CALL();
    // EGL specification states:
    //  interval is silently clamped to minimum and maximum implementation
    //  dependent values before being stored.
    // Although we don't have to, we apply the same logic here.

    if (interval < minSwapInterval)
        interval = minSwapInterval;

    if (interval > maxSwapInterval)
        interval = maxSwapInterval;

    status_t res = mSurfaceTexture->setSynchronousMode(interval ? true : false);

    return res;
}

int SurfaceTextureClient::dequeueBuffer(android_native_buffer_t** buffer) {
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::dequeueBuffer");
    Mutex::Autolock lock(mMutex);
    int buf = -1;
    int reqW = mReqWidth ? mReqWidth : mUserWidth;
    int reqH = mReqHeight ? mReqHeight : mUserHeight;
    status_t result = mSurfaceTexture->dequeueBuffer(&buf, reqW, reqH,
            mReqFormat, mReqUsage);
    if (result < 0) {
        ALOGV("dequeueBuffer: ISurfaceTexture::dequeueBuffer(%d, %d, %d, %d)"
             "failed: %d", mReqWidth, mReqHeight, mReqFormat, mReqUsage,
             result);
        return result;
    }
    sp<GraphicBuffer>& gbuf(mSlots[buf].buffer);
    if (result & ISurfaceTexture::RELEASE_ALL_BUFFERS) {
        freeAllBuffers();
    }

    if ((result & ISurfaceTexture::BUFFER_NEEDS_REALLOCATION) || gbuf == 0) {
        result = mSurfaceTexture->requestBuffer(buf, &gbuf);
        if (result != NO_ERROR) {
            ALOGE("dequeueBuffer: ISurfaceTexture::requestBuffer failed: %d",
                    result);
            return result;
        }
    }
    *buffer = gbuf.get();
    return OK;
}

int SurfaceTextureClient::cancelBuffer(android_native_buffer_t* buffer) {
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::cancelBuffer");
    Mutex::Autolock lock(mMutex);
    int i = getSlotFromBufferLocked(buffer);
    if (i < 0) {
        return i;
    }
    mSurfaceTexture->cancelBuffer(i);
    return OK;
}

int SurfaceTextureClient::getSlotFromBufferLocked(
        android_native_buffer_t* buffer) const {
    bool dumpedState = false;
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
        if (mSlots[i].buffer != NULL &&
                mSlots[i].buffer->handle == buffer->handle) {
            return i;
        }
    }
    ALOGE("getSlotFromBufferLocked: unknown buffer: %p", buffer->handle);
    return BAD_VALUE;
}

int SurfaceTextureClient::lockBuffer(android_native_buffer_t* buffer) {
    ALOGV("SurfaceTextureClient::lockBuffer");
    Mutex::Autolock lock(mMutex);
    return OK;
}

int SurfaceTextureClient::queueBuffer(android_native_buffer_t* buffer) {
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::queueBuffer");
    Mutex::Autolock lock(mMutex);
    int64_t timestamp;
    if (mTimestamp == NATIVE_WINDOW_TIMESTAMP_AUTO) {
        timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
        ALOGV("SurfaceTextureClient::queueBuffer making up timestamp: %.2f ms",
             timestamp / 1000000.f);
    } else {
        timestamp = mTimestamp;
    }
    int i = getSlotFromBufferLocked(buffer);
    if (i < 0) {
        return i;
    }

    // Make sure the crop rectangle is entirely inside the buffer.
    Rect crop;
    mCrop.intersect(Rect(buffer->width, buffer->height), &crop);

    ISurfaceTexture::QueueBufferOutput output;
    ISurfaceTexture::QueueBufferInput input(timestamp, crop, mScalingMode,
            mTransform);
    status_t err = mSurfaceTexture->queueBuffer(i, input, &output);
    if (err != OK)  {
        ALOGE("queueBuffer: error queuing buffer to SurfaceTexture, %d", err);
    }
    uint32_t numPendingBuffers = 0;
    output.deflate(&mDefaultWidth, &mDefaultHeight, &mTransformHint,
            &numPendingBuffers);

    mConsumerRunningBehind = (numPendingBuffers >= 2);

    return err;
}

int SurfaceTextureClient::query(int what, int* value) const {
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::query");
    { // scope for the lock
        Mutex::Autolock lock(mMutex);
        switch (what) {
            case NATIVE_WINDOW_FORMAT:
                if (mReqFormat) {
                    *value = mReqFormat;
                    return NO_ERROR;
                }
                break;
            case NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER: {
                sp<ISurfaceComposer> composer(
                        ComposerService::getComposerService());
                if (composer->authenticateSurfaceTexture(mSurfaceTexture)) {
                    *value = 1;
                } else {
                    *value = 0;
                }
                return NO_ERROR;
            }
            case NATIVE_WINDOW_CONCRETE_TYPE:
                *value = NATIVE_WINDOW_SURFACE_TEXTURE_CLIENT;
                return NO_ERROR;
            case NATIVE_WINDOW_DEFAULT_WIDTH:
                *value = mUserWidth ? mUserWidth : mDefaultWidth;
                return NO_ERROR;
            case NATIVE_WINDOW_DEFAULT_HEIGHT:
                *value = mUserHeight ? mUserHeight : mDefaultHeight;
                return NO_ERROR;
            case NATIVE_WINDOW_TRANSFORM_HINT:
                *value = mTransformHint;
                return NO_ERROR;
            case NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND: {
                status_t err = NO_ERROR;
                if (!mConsumerRunningBehind) {
                    *value = 0;
                } else {
                    err = mSurfaceTexture->query(what, value);
                    if (err == NO_ERROR) {
                        mConsumerRunningBehind = *value;
                    }
                }
                return err;
            }
        }
    }
    return mSurfaceTexture->query(what, value);
}

int SurfaceTextureClient::perform(int operation, va_list args)
{
    int res = NO_ERROR;
    switch (operation) {
    case NATIVE_WINDOW_CONNECT:
        // deprecated. must return NO_ERROR.
        break;
    case NATIVE_WINDOW_DISCONNECT:
        // deprecated. must return NO_ERROR.
        break;
    case NATIVE_WINDOW_SET_USAGE:
        res = dispatchSetUsage(args);
        break;
    case NATIVE_WINDOW_SET_CROP:
        res = dispatchSetCrop(args);
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        res = dispatchSetBufferCount(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        res = dispatchSetBuffersGeometry(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
        res = dispatchSetBuffersTransform(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
        res = dispatchSetBuffersTimestamp(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
        res = dispatchSetBuffersDimensions(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        res = dispatchSetBuffersUserDimensions(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
        res = dispatchSetBuffersFormat(args);
        break;
#ifdef QCOM_HARDWARE
    case NATIVE_WINDOW_SET_BUFFERS_SIZE:
        res = dispatchSetBuffersSize(args);
        break;
    case NATIVE_WINDOW_UPDATE_BUFFERS_GEOMETRY:
        res = dispatchUpdateBuffersGeometry(args);
        break;
#endif
    case NATIVE_WINDOW_LOCK:
        res = dispatchLock(args);
        break;
    case NATIVE_WINDOW_UNLOCK_AND_POST:
        res = dispatchUnlockAndPost(args);
        break;
    case NATIVE_WINDOW_SET_SCALING_MODE:
        res = dispatchSetScalingMode(args);
        break;
    case NATIVE_WINDOW_API_CONNECT:
        res = dispatchConnect(args);
        break;
    case NATIVE_WINDOW_API_DISCONNECT:
        res = dispatchDisconnect(args);
        break;
#ifdef ALLWINNER
    case NATIVE_WINDOW_SETPARAMETER:
        res = dispatchSetParameter(args);
        break;
    case NATIVE_WINDOW_GETPARAMETER:
        res = dispatchGetParameter(args);
        break; 
#endif
    default:
        res = NAME_NOT_FOUND;
        break;
    }
    return res;
}

int SurfaceTextureClient::dispatchConnect(va_list args) {
    int api = va_arg(args, int);
    return connect(api);
}

int SurfaceTextureClient::dispatchDisconnect(va_list args) {
    int api = va_arg(args, int);
    return disconnect(api);
}

int SurfaceTextureClient::dispatchSetUsage(va_list args) {
    int usage = va_arg(args, int);
    return setUsage(usage);
}

#ifdef ALLWINNER
int SurfaceTextureClient::dispatchSetParameter(va_list args)
{
    int cmd     = va_arg(args,int);
    int value   = va_arg(args,int);

    return setParameter((uint32_t)cmd,(uint32_t)value);
}

int SurfaceTextureClient::dispatchGetParameter(va_list args)
{
    int cmd = va_arg(args,int);

    return getParameter((uint32_t)cmd);
}
#endif

int SurfaceTextureClient::dispatchSetCrop(va_list args) {
    android_native_rect_t const* rect = va_arg(args, android_native_rect_t*);
    return setCrop(reinterpret_cast<Rect const*>(rect));
}

int SurfaceTextureClient::dispatchSetBufferCount(va_list args) {
    size_t bufferCount = va_arg(args, size_t);
    return setBufferCount(bufferCount);
}

int SurfaceTextureClient::dispatchSetBuffersGeometry(va_list args) {
#ifdef ALLWINNER
    layerinitpara_t  layer_info;
#endif
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    int f = va_arg(args, int);
#ifdef ALLWINNER
    int screenid = va_arg(args, int);
#endif
    int err = setBuffersDimensions(w, h);
    if (err != 0) {
        return err;
    }
#ifdef ALLWINNER
    ALOGD("dispatchSetBuffersGeometry1!\n");
    err = setBuffersFormat(f);
    if (err != 0) 
    {
        return err;
    }

    ALOGD("dispatchSetBuffersGeometry2!\n");
    
    layer_info.w 			= w;
    layer_info.h 			= h;
    layer_info.format 		= f;
    layer_info.screenid		= screenid;
    return setParameter(HWC_LAYER_SETINITPARA,(uint32_t)&layer_info);
#else
    return setBuffersFormat(f);
#endif
}

int SurfaceTextureClient::dispatchSetBuffersDimensions(va_list args) {
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    return setBuffersDimensions(w, h);
}

int SurfaceTextureClient::dispatchSetBuffersUserDimensions(va_list args) {
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    return setBuffersUserDimensions(w, h);
}

int SurfaceTextureClient::dispatchSetBuffersFormat(va_list args) {
    int f = va_arg(args, int);
    return setBuffersFormat(f);
}

#ifdef QCOM_HARDWARE
int SurfaceTextureClient::dispatchSetBuffersSize(va_list args) {
    int size = va_arg(args, int);
    return setBuffersSize(size);
}

int SurfaceTextureClient::dispatchUpdateBuffersGeometry(va_list args) {
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    int f = va_arg(args, int);
    return updateBuffersGeometry(w, h, f);
}
#endif

int SurfaceTextureClient::dispatchSetScalingMode(va_list args) {
    int m = va_arg(args, int);
    return setScalingMode(m);
}

int SurfaceTextureClient::dispatchSetBuffersTransform(va_list args) {
    int transform = va_arg(args, int);
    return setBuffersTransform(transform);
}

int SurfaceTextureClient::dispatchSetBuffersTimestamp(va_list args) {
    int64_t timestamp = va_arg(args, int64_t);
    return setBuffersTimestamp(timestamp);
}

int SurfaceTextureClient::dispatchLock(va_list args) {
    ANativeWindow_Buffer* outBuffer = va_arg(args, ANativeWindow_Buffer*);
    ARect* inOutDirtyBounds = va_arg(args, ARect*);
    return lock(outBuffer, inOutDirtyBounds);
}

int SurfaceTextureClient::dispatchUnlockAndPost(va_list args) {
    return unlockAndPost();
}


int SurfaceTextureClient::connect(int api) {
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::connect");
    Mutex::Autolock lock(mMutex);
    ISurfaceTexture::QueueBufferOutput output;
    int err = mSurfaceTexture->connect(api, &output);
    if (err == NO_ERROR) {
        uint32_t numPendingBuffers = 0;
        output.deflate(&mDefaultWidth, &mDefaultHeight, &mTransformHint,
                &numPendingBuffers);
        mConsumerRunningBehind = (numPendingBuffers >= 2);
    }
    if (!err && api == NATIVE_WINDOW_API_CPU) {
        mConnectedToCpu = true;
    }
    return err;
}

int SurfaceTextureClient::disconnect(int api) {
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::disconnect");
    Mutex::Autolock lock(mMutex);
    freeAllBuffers();
    int err = mSurfaceTexture->disconnect(api);
    if (!err) {
        mReqFormat = 0;
        mReqWidth = 0;
        mReqHeight = 0;
        mReqUsage = 0;
        mCrop.clear();
        mScalingMode = NATIVE_WINDOW_SCALING_MODE_FREEZE;
        mTransform = 0;
        if (api == NATIVE_WINDOW_API_CPU) {
            mConnectedToCpu = false;
        }
    }
    return err;
}

#ifdef ALLWINNER
int SurfaceTextureClient::setParameter(uint32_t cmd,uint32_t value) 
{
    ALOGV("SurfaceTextureClient::setParameter");
    
    return mSurfaceTexture->setParameter(cmd,value);
}

int SurfaceTextureClient::getParameter(uint32_t cmd) 
{
    ALOGV("SurfaceTextureClient::setParameter");
    
    return mSurfaceTexture->getParameter(cmd);
}
#endif

int SurfaceTextureClient::setUsage(uint32_t reqUsage)
{
    ALOGV("SurfaceTextureClient::setUsage");
    Mutex::Autolock lock(mMutex);

#ifdef QCOM_HARDWARE
    if (reqUsage & GRALLOC_USAGE_PRIVATE_EXTERNAL_ONLY) {
        //Set explicitly, since reqUsage may have other values.
        mReqExtUsage = GRALLOC_USAGE_PRIVATE_EXTERNAL_ONLY;
        //This flag is never independent. Always an add-on to
        //GRALLOC_USAGE_EXTERNAL_ONLY
        if(reqUsage & GRALLOC_USAGE_PRIVATE_EXTERNAL_BLOCK) {
            mReqExtUsage |= GRALLOC_USAGE_PRIVATE_EXTERNAL_BLOCK;
        } else if(reqUsage & GRALLOC_USAGE_PRIVATE_EXTERNAL_CC) {
            mReqExtUsage |= GRALLOC_USAGE_PRIVATE_EXTERNAL_CC;
        }
    }
#endif

    // For most cases mReqExtUsage will be 0.
    // reqUsage could come from app or driver. When it comes from app
    // and subsequently from driver, the latter ends up overwriting
    // the existing values. We cache certain values in mReqExtUsage
    // to avoid being overwritten.
    mReqUsage = reqUsage | mReqExtUsage;
    return OK;
}

int SurfaceTextureClient::setCrop(Rect const* rect)
{
    ATRACE_CALL();

    Rect realRect;
    if (rect == NULL || rect->isEmpty()) {
        realRect.clear();
    } else {
        realRect = *rect;
    }

    ALOGV("SurfaceTextureClient::setCrop rect=[%d %d %d %d]",
            realRect.left, realRect.top, realRect.right, realRect.bottom);

    Mutex::Autolock lock(mMutex);
    mCrop = realRect;
    return NO_ERROR;
}

int SurfaceTextureClient::setBufferCount(int bufferCount)
{
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::setBufferCount");
    Mutex::Autolock lock(mMutex);

    status_t err = mSurfaceTexture->setBufferCount(bufferCount);
    ALOGE_IF(err, "ISurfaceTexture::setBufferCount(%d) returned %s",
            bufferCount, strerror(-err));

    if (err == NO_ERROR) {
        freeAllBuffers();
    }

    return err;
}

int SurfaceTextureClient::setBuffersDimensions(int w, int h)
{
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::setBuffersDimensions");

    if (w<0 || h<0)
        return BAD_VALUE;

    if ((w && !h) || (!w && h))
        return BAD_VALUE;

    Mutex::Autolock lock(mMutex);
    mReqWidth = w;
    mReqHeight = h;
    return NO_ERROR;
}

int SurfaceTextureClient::setBuffersUserDimensions(int w, int h)
{
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::setBuffersUserDimensions");

    if (w<0 || h<0)
        return BAD_VALUE;

    if ((w && !h) || (!w && h))
        return BAD_VALUE;

    Mutex::Autolock lock(mMutex);
    mUserWidth = w;
    mUserHeight = h;
    return NO_ERROR;
}

int SurfaceTextureClient::setBuffersFormat(int format)
{
    ALOGV("SurfaceTextureClient::setBuffersFormat");

    if (format<0)
        return BAD_VALUE;

    Mutex::Autolock lock(mMutex);
    mReqFormat = format;
    return NO_ERROR;
}

#ifdef QCOM_HARDWARE
int SurfaceTextureClient::setBuffersSize(int size)
{
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::setBuffersSize");

    if (size<0)
        return BAD_VALUE;

    Mutex::Autolock lock(mMutex);
    status_t err = mSurfaceTexture->setBuffersSize(size);
    return NO_ERROR;
}

int SurfaceTextureClient::updateBuffersGeometry(int w, int h, int f)
{
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::updateBuffersGeometry");

    if (w<0 || h<0 || f<0)
        return BAD_VALUE;

    if ((w && !h) || (!w && h))
        return BAD_VALUE;

    Mutex::Autolock lock(mMutex);
    status_t err = mSurfaceTexture->updateBuffersGeometry(w, h, f);
    return NO_ERROR;
}
#endif

int SurfaceTextureClient::setScalingMode(int mode)
{
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::setScalingMode(%d)", mode);

    switch (mode) {
        case NATIVE_WINDOW_SCALING_MODE_FREEZE:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_CROP:
            break;
        default:
            ALOGE("unknown scaling mode: %d", mode);
            return BAD_VALUE;
    }

    Mutex::Autolock lock(mMutex);
    mScalingMode = mode;
    return NO_ERROR;
}

int SurfaceTextureClient::setBuffersTransform(int transform)
{
    ATRACE_CALL();
    ALOGV("SurfaceTextureClient::setBuffersTransform");
    Mutex::Autolock lock(mMutex);
    mTransform = transform;
    return NO_ERROR;
}

int SurfaceTextureClient::setBuffersTimestamp(int64_t timestamp)
{
    ALOGV("SurfaceTextureClient::setBuffersTimestamp");
    Mutex::Autolock lock(mMutex);
    mTimestamp = timestamp;
    return NO_ERROR;
}

void SurfaceTextureClient::freeAllBuffers() {
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
        mSlots[i].buffer = 0;
    }
}

// ----------------------------------------------------------------------
// the lock/unlock APIs must be used from the same thread

static status_t copyBlt(
        const sp<GraphicBuffer>& dst,
        const sp<GraphicBuffer>& src,
        const Region& reg)
{
    // src and dst with, height and format must be identical. no verification
    // is done here.
    status_t err;
    uint8_t const * src_bits = NULL;
    err = src->lock(GRALLOC_USAGE_SW_READ_OFTEN, reg.bounds(), (void**)&src_bits);
    ALOGE_IF(err, "error locking src buffer %s", strerror(-err));

    uint8_t* dst_bits = NULL;
    err = dst->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, reg.bounds(), (void**)&dst_bits);
    ALOGE_IF(err, "error locking dst buffer %s", strerror(-err));

    Region::const_iterator head(reg.begin());
    Region::const_iterator tail(reg.end());
    if (head != tail && src_bits && dst_bits) {
        const size_t bpp = bytesPerPixel(src->format);
        const size_t dbpr = dst->stride * bpp;
        const size_t sbpr = src->stride * bpp;

        while (head != tail) {
            const Rect& r(*head++);
            ssize_t h = r.height();
            if (h <= 0) continue;
            size_t size = r.width() * bpp;
            uint8_t const * s = src_bits + (r.left + src->stride * r.top) * bpp;
            uint8_t       * d = dst_bits + (r.left + dst->stride * r.top) * bpp;
            if (dbpr==sbpr && size==sbpr) {
                size *= h;
                h = 1;
            }
            do {
                memcpy(d, s, size);
                d += dbpr;
                s += sbpr;
            } while (--h > 0);
        }
    }

    if (src_bits)
        src->unlock();

    if (dst_bits)
        dst->unlock();

    return err;
}

// ----------------------------------------------------------------------------

status_t SurfaceTextureClient::lock(
        ANativeWindow_Buffer* outBuffer, ARect* inOutDirtyBounds)
{
    if (mLockedBuffer != 0) {
        ALOGE("Surface::lock failed, already locked");
        return INVALID_OPERATION;
    }

    if (!mConnectedToCpu) {
        int err = SurfaceTextureClient::connect(NATIVE_WINDOW_API_CPU);
        if (err) {
            return err;
        }
        // we're intending to do software rendering from this point
        setUsage(GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
    }

    ANativeWindowBuffer* out;
    status_t err = dequeueBuffer(&out);
    ALOGE_IF(err, "dequeueBuffer failed (%s)", strerror(-err));
    if (err == NO_ERROR) {
        sp<GraphicBuffer> backBuffer(GraphicBuffer::getSelf(out));
        err = lockBuffer(backBuffer.get());
        ALOGE_IF(err, "lockBuffer (handle=%p) failed (%s)",
                backBuffer->handle, strerror(-err));
        if (err == NO_ERROR) {
            const Rect bounds(backBuffer->width, backBuffer->height);

            Region newDirtyRegion;
            if (inOutDirtyBounds) {
                newDirtyRegion.set(static_cast<Rect const&>(*inOutDirtyBounds));
                newDirtyRegion.andSelf(bounds);
            } else {
                newDirtyRegion.set(bounds);
            }

            // figure out if we can copy the frontbuffer back
            const sp<GraphicBuffer>& frontBuffer(mPostedBuffer);
            const bool canCopyBack = (frontBuffer != 0 &&
                    backBuffer->width  == frontBuffer->width &&
                    backBuffer->height == frontBuffer->height &&
                    backBuffer->format == frontBuffer->format);

#ifdef QCOM_HARDWARE
            int backBufferSlot(getSlotFromBufferLocked(backBuffer.get()));
#endif
            if (canCopyBack) {
                // copy the area that is invalid and not repainted this round
#ifdef QCOM_HARDWARE
                Mutex::Autolock lock(mMutex);
                Region oldDirtyRegion;
                for(int i = 0 ; i < NUM_BUFFER_SLOTS; i++ ) {
                     if(i != backBufferSlot && !mSlots[i].dirtyRegion.isEmpty())
                         oldDirtyRegion.orSelf(mSlots[i].dirtyRegion);
                }
                const Region copyback(oldDirtyRegion.subtract(newDirtyRegion));
#else
                const Region copyback(mDirtyRegion.subtract(newDirtyRegion));
#endif
                if (!copyback.isEmpty())
                    copyBlt(backBuffer, frontBuffer, copyback);
            } else {
                // if we can't copy-back anything, modify the user's dirty
                // region to make sure they redraw the whole buffer
                newDirtyRegion.set(bounds);
#ifndef QCOM_HARDWARE
                mDirtyRegion.clear();
#endif
                Mutex::Autolock lock(mMutex);
                for (size_t i=0 ; i<NUM_BUFFER_SLOTS ; i++) {
                    mSlots[i].dirtyRegion.clear();
                }
            }


            { // scope for the lock
                Mutex::Autolock lock(mMutex);
#ifdef QCOM_HARDWARE
                mSlots[backBufferSlot].dirtyRegion = newDirtyRegion;
#else
                int backBufferSlot(getSlotFromBufferLocked(backBuffer.get()));
                if (backBufferSlot >= 0) {
                    Region& dirtyRegion(mSlots[backBufferSlot].dirtyRegion);
                    mDirtyRegion.subtract(dirtyRegion);
                    dirtyRegion = newDirtyRegion;
                }
#endif
            }

#ifndef QCOM_HARDWARE
           mDirtyRegion.orSelf(newDirtyRegion);
#endif

            if (inOutDirtyBounds) {
                *inOutDirtyBounds = newDirtyRegion.getBounds();
            }

            void* vaddr;
            status_t res = backBuffer->lock(
                    GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                    newDirtyRegion.bounds(), &vaddr);

            ALOGW_IF(res, "failed locking buffer (handle = %p)",
                    backBuffer->handle);

            mLockedBuffer = backBuffer;
            outBuffer->width  = backBuffer->width;
            outBuffer->height = backBuffer->height;
            outBuffer->stride = backBuffer->stride;
            outBuffer->format = backBuffer->format;
            outBuffer->bits   = vaddr;
        }
    }
    return err;
}

status_t SurfaceTextureClient::unlockAndPost()
{
    if (mLockedBuffer == 0) {
        ALOGE("Surface::unlockAndPost failed, no locked buffer");
        return INVALID_OPERATION;
    }

    status_t err = mLockedBuffer->unlock();
    ALOGE_IF(err, "failed unlocking buffer (%p)", mLockedBuffer->handle);

    err = queueBuffer(mLockedBuffer.get());
    ALOGE_IF(err, "queueBuffer (handle=%p) failed (%s)",
            mLockedBuffer->handle, strerror(-err));

    mPostedBuffer = mLockedBuffer;
    mLockedBuffer = 0;
    return err;
}

}; // namespace android
