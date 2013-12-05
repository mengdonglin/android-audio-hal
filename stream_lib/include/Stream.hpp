/*
 * INTEL CONFIDENTIAL
 * Copyright © 2013 Intel
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
 * disclosed in any way without Intel’s prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery
 * of the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be
 * express and approved by Intel in writing.
 *
 */
#pragma once

#include <SampleSpec.hpp>
#include <utils/RWLock.h>
#include <cutils/log.h>
#include <string>

typedef android::RWLock::AutoRLock AutoR;
typedef android::RWLock::AutoWLock AutoW;

class IStreamRoute;
class IAudioDevice;

class Stream
{
public:
    Stream()
        : _currentStreamRoute(NULL),
          _newStreamRoute(NULL),
          _effectsRequestedMask(0),
          _isRouted(false)
    {}

    /**
     * indicates if the stream has been routed (ie audio device available and the routing is done)
     *
     * @return true if stream is routed, false otherwise
     */
    bool isRouted() const;

    /**
     * indicates if the stream has been routed (ie audio device available and the routing is done).
     * Must be called from locked context.
     *
     * @return true if stream is routed, false otherwise
     */
    bool isRoutedL() const;

    /**
     * indicates if the stream has already been attached to a new route.
     * Note that the routing is still not done.
     *
     * @return true if stream has a new route assigned, false otherwise
     */
    bool isNewRouteAvailable() const;

    /**
     * get stream direction.
     * Note that true=playback, false=capture.
     * @return true if playback, false otherwise.
     */
    virtual bool isOut() const = 0;

    /**
     * get stream state.
     * Note that true=playing, false=standby|stopped.
     * @return true if started, false otherwise.
     */
    virtual bool isStarted() const = 0;

    /**
     * Applicability mask.
     * For an input stream, applicability mask is the ID of the input source
     * For an output stream, applicability mask is the output flags
     *
     * @return applicability mask.
     */
    virtual uint32_t getApplicabilityMask() const = 0;

    /**
     * Get output silence to be appended before playing.
     * Some route may require to append silence in the ring buffer as powering on of components
     * involved in this route may take a while, and audio sample might be lost. It will result in
     * loosing the beginning of the audio samples.
     *
     * @return silence to append in milliseconds.
     */
    uint32_t getOutputSilencePrologMs() const;

    /**
     * Adds an effect to the mask of requested effect.
     *
     * @param[in] effectId Id of the requested effect.
     */
    void addRequestedEffect(uint32_t effectId);

    /**
     * Removes an effect from the mask of requested effect.
     *
     * @param[in] effectId Id of the requested effect.
     */
    void removeRequestedEffect(uint32_t effectId);

    /**
     * Get effects requested for this stream.
     * The route manager will select the route that supports all requested effects.
     *
     * @return mask with Id of requested effects
     */
    uint32_t getEffectRequested() const { return _effectsRequestedMask; }

    /**
     * Get the sample specifications of the stream route.
     *
     * @return sample specifications.
     */
    android_audio_legacy::SampleSpec routeSampleSpec() const { return _routeSampleSpec; }

    /**
     * Reset the new stream route.
     */
    void resetNewStreamRoute();

    /**
     * Set a new route for this stream.
     * No need to lock as the newStreamRoute is for unique usage of the route manager,
     * so accessed from atomic context.
     *
     * @param[in] newStreamRoute: stream route to be attached to this stream.
     */
    void setNewStreamRoute(IStreamRoute *newStreamRoute);

    virtual uint32_t getBufferSizeInBytes() const = 0;

    virtual size_t getBufferSizeInFrames() const = 0;

    /**
     * Read frames from audio device.
     *
     * @param[in] buffer: audio samples buffer to fill from audio device.
     * @param[out] frames: number of frames to read.
     *
     * @return number of frames read from audio device.
     */
    virtual ssize_t pcmReadFrames(void *buffer, size_t frames) = 0;

    /**
     * Write frames to audio device.
     *
     * @param[in] buffer: audio samples buffer to render on audio device.
     * @param[out] frames: number of frames to render.
     *
     * @return number of frames rendered to audio device.
     */
    virtual ssize_t pcmWriteFrames(void *buffer, ssize_t frames) = 0;

    virtual android::status_t pcmStop() = 0;

    /**
     * Returns available frames in pcm buffer and corresponding time stamp.
     * For an input stream, frames available are frames ready for the
     * application to read.
     * For an output stream, frames available are the number of empty frames available
     * for the application to write.
     */
    virtual android::status_t getFramesAvailable(uint32_t &avail, struct timespec &tStamp) = 0;

    IStreamRoute *getCurrentStreamRoute() { return _currentStreamRoute; }

    IStreamRoute *getNewStreamRoute() { return _newStreamRoute; }

    /**
     * Attach the stream to its route.
     * Called by the StreamRoute to allow accessing the pcm device.
     * Set the new pcm device and sample spec given by the stream route.
     *
     * @return true if attach successful, false otherwise.
     */
    android::status_t attachRoute();

    /**
     * Detach the stream from its route.
     * Either the stream has been preempted by another stream or the stream has stopped.
     * Called by the StreamRoute to prevent from accessing the device any more.
     *
     * @return true if detach successful, false otherwise.
     */
    android::status_t detachRoute();

protected:
    /**
     * Attach the stream to its route.
     * Set the new pcm device and sample spec given by the stream route.
     * Called from locked context.
     *
     * @return true if attach successful, false otherwise.
     */
    virtual android::status_t attachRouteL();

    /**
     * Detach the stream from its route.
     * Either the stream has been preempted by another stream or the stream has stopped.
     * Called from locked context.
     *
     * @return true if detach successful, false otherwise.
     */
    virtual android::status_t detachRouteL();

    /**
     * Lock to protect not only the access to pcm device but also any access to device dependant
     * parameters as sample specification.
     */
    mutable android::RWLock _streamLock;

    virtual ~Stream() {}

private:
    void setCurrentStreamRouteL(IStreamRoute *currentStreamRoute);

    /**
     * Sets the route sample specification.
     * Must be called with stream lock held.
     *
     * @param[in] sampleSpec specifications of the route attached to the stream.
     */
    void setRouteSampleSpecL(android_audio_legacy::SampleSpec sampleSpec);

    IStreamRoute *_currentStreamRoute; /**< route assigned to the stream (routed yet). */
    IStreamRoute *_newStreamRoute; /**< New route assigned to the stream (not routed yet). */

    /**
     * Sample specifications of the route assigned to the stream.
     */
    android_audio_legacy::SampleSpec _routeSampleSpec;

    uint32_t _effectsRequestedMask; /**< Mask of requested effects. */

    bool _isRouted; /**< flag indicating the stream is routed and device is ready to use. */
};