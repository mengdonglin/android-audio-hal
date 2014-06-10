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
#pragma once


#include "SampleSpec.hpp"
#include <NonCopyable.hpp>
#include <Direction.hpp>
#include <TinyAlsaStream.hpp>
#include <media/AudioBufferProvider.h>
#include <utils/String8.h>
#include <utils/RWLock.h>

/**
 * For debug purposes only, property-driven (dynamic)
 */
#include <HalAudioDump.hpp>

namespace android_audio_legacy
{

class AudioIntelHal;
class AudioConversion;


class AudioStream : public TinyAlsaStream,
                    private audio_comms::utilities::NonCopyable
{
public:
    virtual ~AudioStream();

    /**
     * Sets the sample specifications of the stream.
     *
     * @param[in|out] format: format of the samples of the playback client.
     *                        If not set, stream returns format it supports.
     * @param[in|out] channels: mask of channels of the samples of the playback client.
     *                          If not set, stream returns channels it supports.
     * @param[in|out] sampleRate: sample rate of the samples of the playback client.
     *                            If not set, stream returns sample rate it supports.
     *
     * @return status: error code to set if parameters given by playback client not supported.
     */
    android::status_t set(int *format, uint32_t *channels, uint32_t *rate);

    /**
     * Get the parameters of the stream.
     *
     * @param[out] keys: one or more value pair "name:value", coma separated
     *
     * @return OK if set is successfull, error code otherwise.
     */
    android::String8 getParameters(const android::String8 &keys);

    /**
     * Get the sample rate of the stream.
     *
     * @return sample rate of the stream.
     */
    inline uint32_t sampleRate() const
    {
        return mSampleSpec.getSampleRate();
    }

    /**
     * Get the format of the stream.
     *
     * @return format of the stream.
     */
    inline int format() const
    {
        return mSampleSpec.getFormat();
    }

    /**
     * Get the channel count of the stream.
     *
     * @return channel count of the stream.
     */
    inline uint32_t channelCount() const
    {
        return mSampleSpec.getChannelCount();
    }

    /**
     * Get the channels of the stream.
     * Channels is a mask, each bit represents a specific channel.
     *
     * @return channel mask of the stream.
     */
    inline uint32_t channels() const
    {
        return mSampleSpec.getChannelMask();
    }

    /**
     * Get the size of the buffer.
     * It calibrates the transfert size between audio flinger and the stream.
     *
     * @return size of the buffer in bytes
     */
    size_t getBufferSize() const;

    /**
     * Check the stream status.
     * Inherited from Stream class
     *
     * @return true if stream is started, false if stream is in standby.
     */
    virtual bool isStarted() const;

    /**
     * Get the stream direction.
     * Inherited from Stream class
     *
     * @return true if output, false if input.
     */
    virtual bool isOut() const = 0;

    /**
     * Set the stream state.
     *
     * @param[in] isSet: true if the client requests the stream to enter standby, false to start
     *
     * @return OK if stream started/standbyed successfully, error code otherwise.
     */
    android::status_t setStandby(bool isSet);

    /**
     * Get the stream devices mask.
     * Stream Sample specification is the sample spec in which the client gives/receives samples
     *
     * @return _devices specifications.
     */
    uint32_t getDevices() const
    {
        return mDevices;
    }

    /**
     * Set the stream devices.
     *
     * @param[in] devices: mask in which each bit represents a device.
     */
    void setDevices(uint32_t devices);

    /**
     * Applicability mask.
     * From Stream class
     *
     * @return ID of input source if input, stream flags if output
     */
    virtual uint32_t getApplicabilityMask() const
    {
        AutoR lock(mStreamLock);
        return mApplicabilityMask;
    }

    /**
     * Get the stream sample specification.
     * Stream Sample specification is the sample spec in which the client gives/receives samples
     *
     * @return sample specifications.
     */
    SampleSpec streamSampleSpec() const
    {
        return mSampleSpec;
    }

protected:
    AudioStream(AudioIntelHal *parent);

    /**
     * Set the Applicability mask.
     * This function is non-reetrant.
     *
     * @param[in] applicabilityMask: ID of input source if input, stream flags if output.
     */
    void setApplicabilityMask(uint32_t applicabilityMask);

    /**
     * Callback of route attachement called by the stream lib. (and so route manager)
     * Inherited from Stream class
     * Set the new pcm device and sample spec given by the audio stream route.
     *
     * @return OK if streams attached successfully to the route, error code otherwise.
     */
    virtual android::status_t attachRouteL();

    /**
     * Callback of route detachement called by the stream lib. (and so route manager)
     * Inherited from Stream class
     *
     * @return OK if streams detached successfully from the route, error code otherwise.
     */
    virtual android::status_t detachRouteL();

    /**
     * Apply audio conversion.
     * Stream is attached to an audio route. Sample specification of streams and routes might
     * differ. This function will adapt if needed the samples between the 2 sample specifications.
     *
     * @param[in] src the source buffer.
     * @param[out] dst the destination buffer.
     *                  Note that if the memory is allocated by the converted, it is freed upon next
     *                  call of configure or upon deletion of the converter.
     * @param[in] inFrames number of input frames.
     * @param[out] outFrames pointer on output frames processed.
     *
     * @return status OK is conversion is successfull, error code otherwise.
     */
    android::status_t applyAudioConversion(const void *src, void **dst,
                                           uint32_t inFrames, uint32_t *outFrames);

    /**
     * Converts audio samples and output an exact number of output frames.
     * The caller must give an AudioBufferProvider object that may implement getNextBuffer API
     * to feed the conversion chain.
     * The caller must allocate itself the destination buffer and garantee overflow will not happen.
     *
     * @param[out] dst pointer on the caller destination buffer.
     * @param[in] outFrames frames in the destination sample specification requested to be outputed.
     * @param[in:out] bufferProvider object that will provide source buffer.
     *
     * @return status OK, error code otherwise.
     */
    android::status_t getConvertedBuffer(void *dst, const uint32_t outFrames,
                                         android::AudioBufferProvider *bufferProvider);

    /**
     * Generate silence.
     * According to the direction, the meaning is different. For an output stream, it means
     * trashing audio samples, while for an input stream, it means providing zeroed samples.
     * To emulate the behavior of the HW and to keep time sync, this function will sleep the time
     * the HW would have used to read/write the amount of requested bytes.
     *
     * @param[in] bytes amount of byte to set to 0 within the buffer.
     * @param[in|out] buffer: if provided, need to fill with 0 (expected for input)
     *
     * @return size of the sample trashed / filled with 0.
     */
    size_t generateSilence(size_t bytes, void *buffer = NULL);

    /**
     * Get the latency of the stream.
     * Latency returns the worst case, ie the latency introduced by the alsa ring buffer.
     *
     * @return latency in milliseconds.
     */
    uint32_t latencyMs() const;

    /**
     * Update the latency according to the flag.
     * Request will be done to the route manager to informs the latency introduced by the route
     * supporting this stream flags.
     *
     */
    void updateLatency();

    /**
     * Sets the state of the status.
     *
     * @param[in] isStarted true if start request, false if standby request.
     */
    void setStarted(bool isStarted);

    /**
     * Get audio dump object before conversion for debug purposes
     *
     * @return a HALAudioDump object before conversion
     */
    HalAudioDump *getDumpObjectBeforeConv() const
    {
        return mDumpBeforeConv;
    }


    /**
     * Get audio dump objects after conversion for debug purposes
     *
     * @return a HALAudioDump object after conversion
     */
    HalAudioDump *getDumpObjectAfterConv() const
    {
        return mDumpAfterConv;
    }

    /**
     * Used to sleep on the current thread.
     *
     * This function is used to get a POSIX-compliant way
     * to accurately sleep the current thread.
     *
     * If function is successful, zero is returned
     * and request has been honored, if function fails,
     * EINTR has been raised by the system and -1 is returned.
     *
     * The other two errors considered by standard
     * are not applicable in our context (EINVAL, ENOSYS)
     *
     * @param[in] sleepTimeUs: desired to sleep, in microseconds.
     *
     * @return on success true is returned, false otherwise.
     */
    bool safeSleep(uint32_t sleepTimeUs);

    AudioIntelHal *mParent; /**< Audio HAL singleton handler. */

    /**
     * Lock to protect preprocessing effects accessed from multiple contexts.
     * For output streams, variable protected by the lock is the echo reference, populated by the
     * output stream and accessed by the input stream.
     * For input streams, variable protected by the lock is the list of pre processing effects
     * pushed by Audio Flinger and hooked by the stream in the context of the record thread.
     */
    android::RWLock mPreProcEffectLock;

    /**
     * maximum number of read/write retries.
     *
     * This constant is used to set maximum number of retries to do
     * on write/read operations before stating that error is not
     * recoverable and reset media server.
     */
    static const uint32_t mMaxReadWriteRetried = 50;

private:
    /**
     * Configures the conversion chain.
     * It configures the conversion chain that may be used to convert samples from the source
     * to destination sample specification. This configuration tries to order the list of converters
     * so that it minimizes the number of samples on which the resampling is done.
     *
     * @param[in] ssSrc source sample specifications.
     * @param[in] ssDst destination sample specifications.
     *
     * @return status OK, error code otherwise.
     */
    android::status_t configureAudioConversion(const SampleSpec &ssSrc, const SampleSpec &ssDst);

    /**
     * Init audio dump if dump properties are activated to create the dump object(s).
     * Triggered when the stream is started.
     */
    void initAudioDump();


    bool mStandby; /**< state of the stream, true if standby, false if started. */

    uint32_t mDevices; /**< devices mask selected by the policy for this stream.*/

    SampleSpec mSampleSpec; /**< stream sample specifications. */

    AudioConversion *mAudioConversion; /**< Audio Conversion utility class. */

    uint32_t mLatencyMs; /**< Latency associated with the current flag of the stream. */

    /**
     * Applicability mask is either:
     *  -for output streams: stream flags, from audio_output_flags_t in audio.h file.
     *                       Note that the stream flags are given at output creation and will not
     *                       changed until output is destroyed.
     *  -for input streams: input source (bitfield done from audio_source_t in audio.h file.
     *          Note that 0 will be taken as none.
     */
    uint32_t mApplicabilityMask;

    static const uint32_t mDefaultSampleRate = 48000; /**< Default HAL sample rate. */
    static const uint32_t mDefaultChannelCount = 2; /**< Default HAL nb of channels. */
    static const uint32_t mDefaultFormat = AUDIO_FORMAT_PCM_16_BIT; /**< Default HAL format. */

    /**
     * Audio dump object used if one of the dump property before
     * conversion is true (check init.rc file)
     */
    HalAudioDump *mDumpBeforeConv;

    /**
     * Audio dump object used if one of the dump property after
     * conversion is true (check init.rc file)
     */
    HalAudioDump *mDumpAfterConv;

    /**
     * Array of property names before conversion
     */
    static const std::string dumpBeforeConvProps[audio_comms::utilities::Direction::_nbDirections];

    /**
     * Array of property names after conversion
     */
    static const std::string dumpAfterConvProps[audio_comms::utilities::Direction::_nbDirections];

    /** maximum sleep time to be allowed by HAL, in microseconds. */
    static const uint32_t mMaxSleepTime = 1000000UL;

    /** Ratio between nanoseconds and microseconds */
    static const uint32_t mNsecPerUsec = 1000;
};
}         // namespace android
