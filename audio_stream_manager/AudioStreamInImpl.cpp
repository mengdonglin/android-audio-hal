/*
 * INTEL CONFIDENTIAL
 * Copyright (c) 2013-2014 Intel
 * Corporation All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to
 * the source code ("Material") are owned by Intel Corporation or its suppliers
 * or licensors. Title to the Material remains with Intel Corporation or its
 * suppliers and licensors. The Material contains trade secrets and proprietary
 * and confidential information of Intel or its suppliers and licensors. The
 * Material is protected by worldwide copyright and trade secret laws and
 * treaty provisions. No part of the Material may be used, copied, reproduced,
 * modified, published, uploaded, posted, transmitted, distributed, or
 * disclosed in any way without Intel's prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery
 * of the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be
 * express and approved by Intel in writing.
 *
 */
#define LOG_TAG "AudioStreamInImpl"

#include "AudioStreamInImpl.hpp"
#include <AudioCommsAssert.hpp>
#include <BitField.hpp>
#include <EffectHelper.hpp>
#include <algorithm>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <media/AudioRecord.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utils/Log.h>
#include <utils/String8.h>


using namespace std;
using audio_comms::utilities::BitField;

namespace android_audio_legacy
{

const std::string AudioStreamInImpl::mHwEffectImplementor = "IntelLPE";

AudioStreamInImpl::AudioStreamInImpl(AudioIntelHal *parent,
                                     AudioSystem::audio_in_acoustics audio_acoustics)
    : AudioStream(parent),
      mFramesLost(0),
      mAcoustics(audio_acoustics),
      mFramesIn(0),
      mProcessingFramesIn(0),
      mProcessingBuffer(NULL),
      mProcessingBufferSizeInFrames(0),
      mReferenceFramesIn(0),
      mReferenceBuffer(NULL),
      mReferenceBufferSizeInFrames(0),
      mPreprocessorsHandlerList(),
      mHwBuffer(NULL)
{
}

AudioStreamInImpl::~AudioStreamInImpl()
{
    freeAllocatedBuffers();
}

status_t AudioStreamInImpl::setGain(float gain)
{
    return NO_ERROR;
}

status_t AudioStreamInImpl::getNextBuffer(AudioBufferProvider::Buffer *buffer,
                                          int64_t pts)
{
    size_t maxFrames = getBufferSizeInFrames();


    ssize_t hwFramesToRead = min(maxFrames, buffer->frameCount);
    ssize_t framesRead;

    framesRead = readHwFrames(mHwBuffer, hwFramesToRead);
    if (framesRead < 0) {

        return NOT_ENOUGH_DATA;
    }
    buffer->raw = mHwBuffer;
    buffer->frameCount = framesRead;

    return NO_ERROR;
}

void AudioStreamInImpl::releaseBuffer(AudioBufferProvider::Buffer *buffer)
{
    // Nothing special to do here...
}

ssize_t AudioStreamInImpl::readHwFrames(void *buffer, size_t frames)
{
    uint32_t retryCount = 0;
    status_t ret = OK;

    do {
        std::string error;

        ret = pcmReadFrames(buffer, frames, error);

        if (ret < 0) {

            ALOGE("%s: read error: %s - requested %d (bytes=%d) frames",
                  __FUNCTION__,
                  error.c_str(),
                  frames,
                  streamSampleSpec().convertFramesToBytes(frames));

            AUDIOCOMMS_ASSERT(++retryCount < mMaxReadWriteRetried,
                              "Hardware not responding, restarting media server");

            // Get the number of microseconds to sleep, inferred from the number of
            // frames to write.
            size_t sleepUSecs = routeSampleSpec().convertFramesToUsec(frames);

            // Go sleeping before trying I/O operation again.
            if (safeSleep(sleepUSecs)) {
                // If some error arises when trying to sleep, try I/O operation anyway.
                // Error counter will provoke the restart of mediaserver.
                ALOGE("%s:  Error while calling nanosleep interface", __FUNCTION__);
            }
        }

    } while (ret < 0);

    // Dump audio input before eventual conversions
    // FOR DEBUG PURPOSE ONLY
    if (getDumpObjectBeforeConv() != NULL) {
        getDumpObjectBeforeConv()->dumpAudioSamples(buffer,
                                                    routeSampleSpec().convertFramesToBytes(frames),
                                                    isOut(),
                                                    routeSampleSpec().getSampleRate(),
                                                    routeSampleSpec().getChannelCount(),
                                                    "before_conversion");
    }

    return frames;
}

ssize_t AudioStreamInImpl::readFrames(void *buffer, size_t frames)
{
    //
    // No conversion required, read HW frames directly
    //
    if (streamSampleSpec() == routeSampleSpec()) {

        return readHwFrames(buffer, frames);
    }

    //
    // Otherwise, request for a converted buffer
    //
    status_t status = getConvertedBuffer(buffer, frames, this);
    if (status != NO_ERROR) {

        return status;
    }

    if (getDumpObjectAfterConv() != NULL) {
        getDumpObjectAfterConv()->dumpAudioSamples(buffer,
                                                   streamSampleSpec().convertFramesToBytes(frames),
                                                   isOut(),
                                                   streamSampleSpec().getSampleRate(),
                                                   streamSampleSpec().getChannelCount(),
                                                   "after_conversion");
    }
    return frames;
}

int AudioStreamInImpl::doProcessFrames(const void *buffer, ssize_t frames,
                                       ssize_t *processedFrames,
                                       ssize_t *processingFramesIn)
{
    int ret = 0;

    audio_buffer_t inBuf;
    audio_buffer_t outBuf;

    while ((*processedFrames < frames) && (*processingFramesIn > 0) && (ret == 0)) {

        Vector<AudioEffectHandle>::const_iterator it;
        for (it = mPreprocessorsHandlerList.begin(); it != mPreprocessorsHandlerList.end(); ++it) {

            if (it->mEchoReference != NULL) {

                pushEchoReference(*processingFramesIn, it->mPreprocessor, it->mEchoReference);
            }
            // in_buf.frameCount and out_buf.frameCount indicate respectively
            // the maximum number of frames to be consumed and produced by process()
            inBuf.frameCount = *processingFramesIn;
            inBuf.s16 = (int16_t *)((char *)mProcessingBuffer +
                                    streamSampleSpec().convertFramesToBytes(*processedFrames));
            outBuf.frameCount = frames - *processedFrames;
            outBuf.s16 = (int16_t *)((char *)buffer +
                                     streamSampleSpec().convertFramesToBytes(*processedFrames));

            ret = (*(it->mPreprocessor))->process(it->mPreprocessor, &inBuf, &outBuf);
            if (ret == 0) {
                // Note: it is useless to recopy the output of effect processing as input
                // for the next effect processing because it is done in webrtc::audio_processing

                // process() has updated the number of frames consumed and produced in
                // in_buf.frameCount and out_buf.frameCount respectively
                *processingFramesIn -= inBuf.frameCount;
                *processedFrames += outBuf.frameCount;
            }
        }
    }
    return ret;
}

ssize_t AudioStreamInImpl::processFrames(void *buffer, ssize_t frames)
{
    // first reload enough frames at the end of process input buffer
    if (mProcessingFramesIn < frames) {

        if (mProcessingBufferSizeInFrames < frames) {

            status_t ret = allocateProcessingMemory(frames);
            if (ret != OK) {

                return ret;
            }
        }

        ssize_t read_frames = readFrames((char *)mProcessingBuffer +
                                         streamSampleSpec().convertFramesToBytes(
                                             mProcessingFramesIn),
                                         frames - mProcessingFramesIn);
        if (read_frames < 0) {

            return read_frames;
        }
        /* OK, we have to process all read frames */
        mProcessingFramesIn += read_frames;
        AUDIOCOMMS_ASSERT(mProcessingFramesIn >= frames, "Not enough frames");

    }

    ssize_t processed_frames = 0;
    ssize_t processingFramesIn = mProcessingFramesIn;
    int processingReturn = 0;

    // Then process the frames
    processingReturn = doProcessFrames(buffer, frames, &processed_frames, &processingFramesIn);
    if (processingReturn != 0) {

        // Effects processing failed
        // at least, it is necessary to return the read HW frames
        ALOGD("%s: unable to apply any effect; returned value is %d", __FUNCTION__,
              processingReturn);
        memcpy(buffer,
               mProcessingBuffer,
               streamSampleSpec().convertFramesToBytes(mProcessingFramesIn));
        processed_frames = mProcessingFramesIn;
    } else {

        // move remaining frames to the beginning of mProccesingBuffer because currently,
        // the configuration imposes working with 160 frames and effects library works
        // with 80 frames per cycle (10 ms), i.e. the processing of 160 read HW frames
        // requests two calls to effects library (which are done by while loop. In future or
        // if configuration changed, effects library processing could be not more multiple of
        // HW read frames, so it is necessary to realign the buffer
        if (processingFramesIn != 0) {

            AUDIOCOMMS_ASSERT(processingFramesIn > 0, "Not enough frames");
            memmove(mProcessingBuffer,
                    (char *)mProcessingBuffer +
                    streamSampleSpec().convertFramesToBytes(mProcessingFramesIn -
                                                            processingFramesIn),
                    streamSampleSpec().convertFramesToBytes(processingFramesIn));
        }
    }
    // at the end, we keep remainder frames not cosumed by effect processor.
    mProcessingFramesIn = processingFramesIn;

    return processed_frames;
}

ssize_t AudioStreamInImpl::read(void *buffer, ssize_t bytes)
{
    setStandby(false);

    AutoR lock(mStreamLock);

    // Check if the audio route is available for this stream
    if (!isRoutedL()) {

        ALOGW("%s(buffer=%p, bytes=%ld) No route available. Generating silence for stream %p.",
              __FUNCTION__, buffer, static_cast<long int>(bytes), this);
        return generateSilence(bytes, buffer);
    }

    ssize_t received_frames = -1;
    ssize_t frames = streamSampleSpec().convertBytesToFrames(bytes);

    // Take the effect lock while processing
    mPreProcEffectLock.readLock();
    if (!mPreprocessorsHandlerList.empty()) {

        received_frames = processFrames(buffer, frames);
    } else {

        received_frames = readFrames(buffer, frames);
    }
    mPreProcEffectLock.unlock();


    if (received_frames < 0) {

        ALOGE("%s(buffer=%p, bytes=%ld) returns %ld. Generating silence for stream %p.",
              __FUNCTION__, buffer, static_cast<long int>(bytes),
              static_cast<long int>(received_frames), this);
        generateSilence(bytes, buffer);
        return received_frames;
    }
    return streamSampleSpec().convertFramesToBytes(received_frames);
}

status_t AudioStreamInImpl::dump(int fd, const Vector<String16> &args)
{
    return NO_ERROR;
}

status_t AudioStreamInImpl::standby()
{
    return setStandby(true);
}

void AudioStreamInImpl::resetFramesLost()
{
    // setVoiceVolume and mixing during voice call cannot happen together
    // need a lock; but deadlock may appear during simultaneous R or W
    // so remove lock and the reset of mFramesLost which is never updated btw
}

unsigned int AudioStreamInImpl::getInputFramesLost() const
{
    unsigned int count = mFramesLost;   // set to 0 during construction

    AudioStreamInImpl *mutable_this = const_cast<AudioStreamInImpl *>(this);
    // Requirement from AudioHardwareInterface.h:
    // Audio driver is expected to reset the value to 0 and restart counting upon
    // returning the current value by this function call.
    mutable_this->resetFramesLost();
    return count;
}

status_t AudioStreamInImpl::setParameters(const String8 &keyValuePairs)
{
    // Give a chance to parent to handle the change
    return mParent->setStreamParameters(this, keyValuePairs);
}

status_t AudioStreamInImpl::allocateHwBuffer()
{
    freeAllocatedBuffers();

    mHwBufferSize = getBufferSizeInBytes();

    mHwBuffer = new char[mHwBufferSize];
    if (!mHwBuffer) {

        ALOGE("%s: cannot allocate resampler Hwbuffer", __FUNCTION__);
        return NO_MEMORY;
    }
    return NO_ERROR;
}

void AudioStreamInImpl::freeAllocatedBuffers()
{
    delete[] mHwBuffer;
    mHwBuffer = NULL;
}


void AudioStreamInImpl::setInputSource(uint32_t inputSource)
{
    setApplicabilityMask(BitField::indexToMask(inputSource));
}

status_t AudioStreamInImpl::attachRouteL()
{
    status_t status = AudioStream::attachRouteL();
    if (status != NO_ERROR) {

        return status;
    }
    return allocateHwBuffer();
}

status_t AudioStreamInImpl::detachRouteL()
{
    freeAllocatedBuffers();
    return AudioStream::detachRouteL();
}

size_t AudioStreamInImpl::bufferSize() const
{
    return getBufferSize();
}

bool AudioStreamInImpl::isHwEffectL(effect_handle_t effect)
{
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");

    string implementor;
    status_t err = getAudioEffectImplementorFromHandle(effect, implementor);
    if (err != NO_ERROR) {

        return false;
    }
    return implementor == mHwEffectImplementor;
}

status_t AudioStreamInImpl::addAudioEffect(effect_handle_t effect)
{
    ALOGD("%s (effect=%p)", __FUNCTION__, effect);
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");

    // Called from different context than the stream,
    // so effect Lock must be held
    AutoW lock(mPreProcEffectLock);

    if (isHwEffectL(effect)) {

        ALOGD("%s HW Effect requested(effect=%p)", __FUNCTION__, effect);
        /**
         * HW Effects management
         */
        string name;
        status_t err = getAudioEffectNameFromHandle(effect, name);
        if (err != NO_ERROR) {

            return BAD_VALUE;
        }
        addRequestedEffect(EffectHelper::convertEffectNameToProcId(name));
        if (isStarted()) {

            ALOGD("%s stream running, reconsider routing", __FUNCTION__);
            // If the stream is routed, force a reconsider routing to take effect into account
            mParent->updateRequestedEffect();
        }
    } else {

        ALOGD("%s SW Effect requested(effect=%p)", __FUNCTION__, effect);
        /**
         * SW Effects management
         */
        if (isAecEffect(effect)) {

            struct echo_reference_itfe *stReference = NULL;
            stReference = mParent->getEchoReference(streamSampleSpec());
            return addSwAudioEffectL(effect, stReference);
        }
        addSwAudioEffectL(effect);
    }

    return NO_ERROR;
}

status_t AudioStreamInImpl::removeAudioEffect(effect_handle_t effect)
{
    ALOGD("%s (effect=%p)", __FUNCTION__, effect);

    // Called from different context than the stream,
    // so effect Lock must be held.
    AutoW lock(mPreProcEffectLock);

    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");

    if (isHwEffectL(effect)) {

        ALOGD("%s HW Effect requested(effect=%p)", __FUNCTION__, effect);
        /**
         * HW Effects management
         */
        string name;
        status_t err = getAudioEffectNameFromHandle(effect, name);
        if (err != NO_ERROR) {

            return BAD_VALUE;
        }
        removeRequestedEffect(EffectHelper::convertEffectNameToProcId(name));
        if (isStarted()) {

            ALOGD("%s stream running, reconsider routing", __FUNCTION__);
            // If the stream is routed,
            // force a reconsider routing to take effect removal into account
            mParent->updateRequestedEffect();
        }
    } else {

        ALOGD("%s SW Effect requested(effect=%p)", __FUNCTION__, effect);
        /**
         * SW Effects management
         */
        removeSwAudioEffectL(effect);
    }
    return NO_ERROR;
}

status_t AudioStreamInImpl::addSwAudioEffectL(effect_handle_t effect,
                                              echo_reference_itfe *reference)
{
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");

    // audio effects processing is very costy in term of CPU,
    // so useless to add the same effect more than one time
    Vector<AudioEffectHandle>::const_iterator it;
    it = std::find_if(mPreprocessorsHandlerList.begin(), mPreprocessorsHandlerList.end(),
                      std::bind2nd(MatchEffect(), effect));
    if (it != mPreprocessorsHandlerList.end()) {

        ALOGW("%s (effect=%p): it is useless to add again the same effect",
              __FUNCTION__, effect);
        return NO_ERROR;
    }

    status_t ret = mPreprocessorsHandlerList.add(AudioEffectHandle(effect, reference));
    if (ret < 0) {

        ALOGE("%s (effect=%p): unable to add effect!", __FUNCTION__, effect);
        return ret;
    }
    ALOGD("%s (effect=%p): effect added. number of stored effects is %d", __FUNCTION__,
          effect, mPreprocessorsHandlerList.size());
    return NO_ERROR;
}

status_t AudioStreamInImpl::removeSwAudioEffectL(effect_handle_t effect)
{
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");

    Vector<AudioEffectHandle>::iterator it;
    it = std::find_if(mPreprocessorsHandlerList.begin(), mPreprocessorsHandlerList.end(),
                      std::bind2nd(MatchEffect(), effect));
    if (it != mPreprocessorsHandlerList.end()) {

        ALOGD("%s (effect=%p): effect has been found. number of effects before erase %d",
              __FUNCTION__, effect, mPreprocessorsHandlerList.size());
        if (it->mEchoReference != NULL) {

            /* stop reading from echo reference */
            it->mEchoReference->read(it->mEchoReference, NULL);
            mParent->resetEchoReference(it->mEchoReference);
            it->mEchoReference = NULL;
        }
        mPreprocessorsHandlerList.erase(it);
        ALOGD("%s (effect=%p): number of effects after erase %d",
              __FUNCTION__, effect, mPreprocessorsHandlerList.size());

        return NO_ERROR;
    }
    return BAD_VALUE;
}

status_t AudioStreamInImpl::getAudioEffectNameFromHandle(effect_handle_t effect,
                                                         string &name) const
{
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");
    effect_descriptor_t desc;
    if ((*effect)->get_descriptor(effect, &desc) != 0) {

        ALOGE("%s: could not get effect descriptor", __FUNCTION__);
        return BAD_VALUE;
    }
    ALOGE("%s: Name=%s", __FUNCTION__, desc.name);
    name = string(desc.name);
    return NO_ERROR;
}

status_t AudioStreamInImpl::getAudioEffectImplementorFromHandle(effect_handle_t effect,
                                                                string &implementor) const
{
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");
    effect_descriptor_t desc;
    if ((*effect)->get_descriptor(effect, &desc) != 0) {

        ALOGE("%s: could not get effect descriptor", __FUNCTION__);
        return BAD_VALUE;
    }
    ALOGE("%s: Name=%s", __FUNCTION__, desc.implementor);
    implementor = string(desc.implementor);
    return NO_ERROR;
}

bool AudioStreamInImpl::isAecEffect(effect_handle_t effect)
{
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");
    effect_descriptor_t desc;
    if ((*effect)->get_descriptor(effect, &desc) != 0) {

        ALOGE("%s: could not get effect descriptor", __FUNCTION__);
        return false;
    }
    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {

        ALOGD("%s effect is AEC", __FUNCTION__);
        return true;
    }
    return false;
}

void AudioStreamInImpl::getCaptureDelay(struct echo_reference_buffer *buffer)
{
    /* read frames available in kernel driver buffer */
    size_t kernel_frames;
    struct timespec tstamp;
    long buf_delay;
    long kernel_delay;
    long delay_ns;

    if (getFramesAvailable(kernel_frames, tstamp) != OK) {

        buffer->time_stamp.tv_sec = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns = 0;
        ALOGW("read get_capture_delay(): pcm_htimestamp error");
        return;
    }
    // read frames available in audio HAL input buffer
    // add number of frames being read as we want the capture time of first sample
    // in current buffer.
    buf_delay = streamSampleSpec().convertFramesToUsec(mFramesIn + mProcessingFramesIn);

    // add delay introduced by kernel
    kernel_delay = routeSampleSpec().convertFramesToUsec(kernel_frames);

    delay_ns = kernel_delay + buf_delay;

    buffer->time_stamp = tstamp;
    buffer->delay_ns = delay_ns;
    ALOGV("get_capture_delay time_stamp = [%ld].[%ld], delay_ns: [%d],"
          " kernel_delay:[%ld], buf_delay:[%ld], kernel_frames:[%d], ",
          buffer->time_stamp.tv_sec, buffer->time_stamp.tv_nsec, buffer->delay_ns,
          kernel_delay, buf_delay, kernel_frames);
}

int32_t AudioStreamInImpl::updateEchoReference(ssize_t frames,
                                               struct echo_reference_itfe *reference)
{
    struct echo_reference_buffer b;

    AUDIOCOMMS_ASSERT(reference != NULL, "Null reference handle");

    b.delay_ns = 0;

    if (mReferenceFramesIn < frames) {

        if (mReferenceBufferSizeInFrames < frames) {

            mReferenceBufferSizeInFrames = frames;
            int16_t *referenceBuffer = (int16_t *)realloc(mReferenceBuffer,
                                                          streamSampleSpec().convertFramesToBytes(
                                                              mReferenceBufferSizeInFrames));
            if (referenceBuffer == NULL) {

                ALOGE(" %s(frames=%ld): realloc failed", __FUNCTION__,
                      static_cast<long int>(frames));
                return NO_MEMORY;
            }
            mReferenceBuffer = referenceBuffer;
        }

        b.frame_count = frames - mReferenceFramesIn;
        b.raw = (void *)((char *)mReferenceBuffer +
                         streamSampleSpec().convertFramesToBytes(mReferenceFramesIn));

        getCaptureDelay(&b);

        if (reference->read(reference, &b) == 0) {

            mReferenceFramesIn += b.frame_count;
        } else {

            ALOGW("%s: NOT enough frames to read ref buffer", __FUNCTION__);
        }
    }
    return b.delay_ns;
}

status_t AudioStreamInImpl::pushEchoReference(ssize_t frames, effect_handle_t preprocessor,
                                              struct echo_reference_itfe *reference)
{
    /* read frames from echo reference buffer and update echo delay
     * mReferenceFramesIn is updated with frames available in mReferenceBuffer */
    int32_t delay_us = updateEchoReference(frames, reference) / 1000;

    AUDIOCOMMS_ASSERT(preprocessor != NULL, "Null preproc pointer");
    AUDIOCOMMS_ASSERT(*preprocessor != NULL, "Null preproc");
    AUDIOCOMMS_ASSERT(reference != NULL, "Null eference");

    if (mReferenceFramesIn < frames) {

        frames = mReferenceFramesIn;
    }

    if ((*preprocessor)->process_reverse == NULL) {

        ALOGW(" %s(frames %ld): process_reverse is NULL", __FUNCTION__,
              static_cast<long int>(frames));
        return BAD_VALUE;
    }

    audio_buffer_t buf;

    buf.frameCount = mReferenceFramesIn;
    buf.s16 = mReferenceBuffer;

    status_t processingReturn = (*preprocessor)->process_reverse(preprocessor,
                                                                 &buf,
                                                                 NULL);
    setPreprocessorEchoDelay(preprocessor, delay_us);
    mReferenceFramesIn -= buf.frameCount;

    if (mReferenceFramesIn > 0) {

        memcpy(mReferenceBuffer,
               (char *)mReferenceBuffer + streamSampleSpec().convertFramesToBytes(buf.frameCount),
               streamSampleSpec().convertFramesToBytes(mReferenceFramesIn));
    }

    return processingReturn;
}

status_t AudioStreamInImpl::setPreprocessorParam(effect_handle_t effect, effect_param_t *param)
{
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");
    AUDIOCOMMS_ASSERT(param != NULL, "Null param");

    status_t ret;
    uint32_t size = sizeof(int);
    AUDIOCOMMS_ASSERT(param->psize >= 1, "Invalid parameter size");
    uint32_t psize = ((param->psize - 1) / sizeof(int) + 1) * sizeof(int) + param->vsize;

    ret = (*effect)->command(effect,
                             EFFECT_CMD_SET_PARAM,
                             sizeof(effect_param_t) + psize,
                             param,
                             &size,
                             &param->status);

    return ret == 0 ? param->status : ret;
}

status_t AudioStreamInImpl::setPreprocessorEchoDelay(effect_handle_t effect, int32_t delayInUs)
{
    AUDIOCOMMS_ASSERT(effect != NULL, "NULL effect context");
    AUDIOCOMMS_ASSERT(*effect != NULL, "NULL effect interface");
    /** effect_param_t contains extensible field "data"
     * in our case, it is necessary to "allocate" memory to store
     * AEC_PARAM_ECHO_DELAY and delay_us as uint32_t
     * so, computation of "allocated" memory is size of
     * effect_param_t in uint32_t + 2
     */
    uint32_t buf[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
    effect_param_t *param = reinterpret_cast<effect_param_t *>(buf);

    param->psize = sizeof(uint32_t);
    param->vsize = sizeof(uint32_t);

    struct delay
    {
        uint32_t aecEchoDelay;
        uint32_t delayUs;
    };
    delay *data = reinterpret_cast<delay *>(param->data);

    data->aecEchoDelay = AEC_PARAM_ECHO_DELAY;
    data->delayUs = delayInUs;

    return setPreprocessorParam(effect, param);
}

status_t AudioStreamInImpl::allocateProcessingMemory(ssize_t frames)
{
    mProcessingBufferSizeInFrames = frames;

    int16_t *processingBuffer = (int16_t *)realloc(mProcessingBuffer,
                                                   streamSampleSpec().convertFramesToBytes(
                                                       mProcessingBufferSizeInFrames));
    if (processingBuffer == NULL) {

        ALOGE(" %s(frames=%ld): realloc failed errno = %s!", __FUNCTION__,
              static_cast<long int>(frames), strerror(errno));
        return NO_MEMORY;
    }
    mProcessingBuffer = processingBuffer;
    ALOGD("%s(frames=%ld): mProcessingBuffer=%p size extended to %ld frames (i.e. %d bytes)",
          __FUNCTION__,
          static_cast<long int>(frames),
          mProcessingBuffer,
          static_cast<long int>(mProcessingBufferSizeInFrames),
          streamSampleSpec().convertFramesToBytes(mProcessingBufferSizeInFrames));

    return NO_ERROR;
}

}       // namespace android
