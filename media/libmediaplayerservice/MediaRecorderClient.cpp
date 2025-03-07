/*
 ** Copyright 2008, The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

/* Copyright (C) 2009-2013, 2015 Freescale Semiconductor, Inc. */

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaRecorderService"
#include <utils/Log.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <cutils/atomic.h>
#include <cutils/properties.h> // for property_get
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryHeapBase.h>
#include <binder/MemoryBase.h>

#include <utils/String16.h>

#include <system/audio.h>

#include "MediaRecorderClient.h"
#include "MediaPlayerService.h"

#include "StagefrightRecorder.h"
#include <gui/IGraphicBufferProducer.h>
#include <camera/ICamera.h>

#ifdef FSL_GM_PLAYER
#include "media/OMXMediaRecorder.h"
#endif

namespace android {

const char* cameraPermission = "android.permission.CAMERA";
const char* recordAudioPermission = "android.permission.RECORD_AUDIO";

#define INVALID_AUDIO_SOURCE (-1)
#define INVALID_VIDEO_SOURCE (-1)


static bool checkPermission(const char* permissionString) {
#ifndef HAVE_ANDROID_OS
    return true;
#endif
    if (getpid() == IPCThreadState::self()->getCallingPid()) return true;
    bool ok = checkCallingPermission(String16(permissionString));
    if (!ok) ALOGE("Request requires %s", permissionString);
    return ok;
}


sp<IGraphicBufferProducer> MediaRecorderClient::querySurfaceMediaSource()
{
    ALOGV("Query SurfaceMediaSource");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NULL;
    }
    return mRecorder->querySurfaceMediaSource();
}



status_t MediaRecorderClient::setCamera(const sp<ICamera>& camera,
                                        const sp<ICameraRecordingProxy>& proxy)
{
    ALOGV("setCamera");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    mCamera = camera;
    mProxy = proxy;
    return mRecorder->setCamera(camera, proxy);
}

status_t MediaRecorderClient::setPreviewSurface(const sp<IGraphicBufferProducer>& surface)
{
    ALOGV("setPreviewSurface");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setPreviewSurface(surface);
}

status_t MediaRecorderClient::setVideoSource(int vs)
{
    ALOGV("setVideoSource(%d)", vs);
    // Check camera permission for sources other than SURFACE
    if (vs != VIDEO_SOURCE_SURFACE && !checkPermission(cameraPermission)) {
        return PERMISSION_DENIED;
    }
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL)     {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
#ifdef FSL_GM_PLAYER

    bool omxgm_enable_record = false;
    char value[PROPERTY_VALUE_MAX];
    if (property_get("media.omxgm.enable-record", value, NULL)
                && ( !strcmp(value, "1") || !strcasecmp(value, "true"))
        )
        omxgm_enable_record = true;

    if((vs != VIDEO_SOURCE_CAMERA && mRecorderType == OMX_RECORDER)
        ||
        (vs == VIDEO_SOURCE_CAMERA && mRecorderType != OMX_RECORDER && omxgm_enable_record))
    {
        ALOGV("need to switch between omx and stagefright");

        if(mRecorder != NULL){
            delete mRecorder;
            mRecorder = NULL;
        }
        CreateRecorder(vs);

        if(mClientNameSet){
            ALOGV("call setClientName");
            mRecorder->setClientName(mClientName);
        }

        if(mCamera != NULL && mProxy != NULL){
            ALOGV("call setCamera");
            mRecorder->setCamera(mCamera, mProxy);
        }

        if(mInited){
            ALOGV("call init");
            mRecorder->init();
        }

        if(mListener != NULL){
            ALOGV("call setListener");
            mRecorder->setListener(mListener);
        }

        if(mAudioSource != INVALID_AUDIO_SOURCE){
            ALOGV("call setAudioSource %d", mAudioSource);
            mRecorder->setAudioSource((audio_source_t)mAudioSource);
        }
    }

#endif

    return mRecorder->setVideoSource((video_source)vs);
}

status_t MediaRecorderClient::setAudioSource(int as)
{
    ALOGV("setAudioSource(%d)", as);
    if (!checkPermission(recordAudioPermission)) {
        return PERMISSION_DENIED;
    }
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL)  {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }

    mAudioSource = as;
    return mRecorder->setAudioSource((audio_source_t)as);
}

status_t MediaRecorderClient::setOutputFormat(int of)
{
    ALOGV("setOutputFormat(%d)", of);
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setOutputFormat((output_format)of);
}

status_t MediaRecorderClient::setVideoEncoder(int ve)
{
    ALOGV("setVideoEncoder(%d)", ve);
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setVideoEncoder((video_encoder)ve);
}

status_t MediaRecorderClient::setAudioEncoder(int ae)
{
    ALOGV("setAudioEncoder(%d)", ae);
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setAudioEncoder((audio_encoder)ae);
}

status_t MediaRecorderClient::setOutputFile(const char* path)
{
    ALOGV("setOutputFile(%s)", path);
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setOutputFile(path);
}

status_t MediaRecorderClient::setOutputFile(int fd, int64_t offset, int64_t length)
{
    ALOGV("setOutputFile(%d, %lld, %lld)", fd, offset, length);
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setOutputFile(fd, offset, length);
}

status_t MediaRecorderClient::setVideoSize(int width, int height)
{
    ALOGV("setVideoSize(%dx%d)", width, height);
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setVideoSize(width, height);
}

status_t MediaRecorderClient::setVideoFrameRate(int frames_per_second)
{
    ALOGV("setVideoFrameRate(%d)", frames_per_second);
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setVideoFrameRate(frames_per_second);
}

status_t MediaRecorderClient::setParameters(const String8& params) {
    ALOGV("setParameters(%s)", params.string());
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->setParameters(params);
}

status_t MediaRecorderClient::prepare()
{
    ALOGV("prepare");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->prepare();
}


status_t MediaRecorderClient::getMaxAmplitude(int* max)
{
    ALOGV("getMaxAmplitude");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->getMaxAmplitude(max);
}

status_t MediaRecorderClient::start()
{
    ALOGV("start");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->start();

}

status_t MediaRecorderClient::stop()
{
    ALOGV("stop");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->stop();
}

status_t MediaRecorderClient::init()
{
    ALOGV("init");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    mInited = true;
    return mRecorder->init();
}

status_t MediaRecorderClient::close()
{
    ALOGV("close");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    return mRecorder->close();
}


status_t MediaRecorderClient::reset()
{
    ALOGV("reset");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    mAudioSource = INVALID_AUDIO_SOURCE;
    mInited = false;
    mListener = NULL;
    mCamera = NULL;
    mProxy = NULL;
    mClientNameSet = false;
    return mRecorder->reset();
}

status_t MediaRecorderClient::release()
{
    ALOGV("release");
    Mutex::Autolock lock(mLock);
    if (mRecorder != NULL) {
        delete mRecorder;
        mRecorder = NULL;
        wp<MediaRecorderClient> client(this);
        mMediaPlayerService->removeMediaRecorderClient(client);
    }
    return NO_ERROR;
}

MediaRecorderClient::MediaRecorderClient(const sp<MediaPlayerService>& service, pid_t pid)
{
    ALOGV("Client constructor");
    mPid = pid;
    mAudioSource = INVALID_AUDIO_SOURCE;
    mInited = false;
    mListener = NULL;
    mCamera = NULL;
    mProxy = NULL;
    mClientNameSet = false;

    CreateRecorder(INVALID_VIDEO_SOURCE);

    mMediaPlayerService = service;
}


MediaRecorderClient::~MediaRecorderClient()
{
    ALOGV("Client destructor");
    release();
}

status_t MediaRecorderClient::setListener(const sp<IMediaRecorderClient>& listener)
{
    ALOGV("setListener");
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    mListener = listener;
    return mRecorder->setListener(listener);
}

status_t MediaRecorderClient::setClientName(const String16& clientName) {
    ALOGV("setClientName(%s)", String8(clientName).string());
    Mutex::Autolock lock(mLock);
    if (mRecorder == NULL) {
        ALOGE("recorder is not initialized");
        return NO_INIT;
    }
    mClientName = clientName;
    mClientNameSet = true;
    return mRecorder->setClientName(clientName);
}

status_t MediaRecorderClient::dump(int fd, const Vector<String16>& args) const {
    if (mRecorder != NULL) {
        return mRecorder->dump(fd, args);
    }
    return OK;
}

void MediaRecorderClient::CreateRecorder(int vs)
{

#ifdef FSL_GM_PLAYER
    char value[PROPERTY_VALUE_MAX];
    if (property_get("media.omxgm.enable-record", value, NULL)
            && ( !strcmp(value, "1") || !strcasecmp(value, "true"))
            && (vs == VIDEO_SOURCE_CAMERA || vs == INVALID_VIDEO_SOURCE)
    ) {
        mRecorder = new OMXRecorder;
        mRecorderType = OMX_RECORDER;
        return;
    }
#endif

    mRecorder = new StagefrightRecorder;
    mRecorderType = STAGEFRIGHT_RECORDER;

}


}; // namespace android
