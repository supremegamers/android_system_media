/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_AUDIO_FIFO_H
#define ANDROID_AUDIO_FIFO_H

#include <atomic>
#include <stdlib.h>

#ifndef __cplusplus
#error C API is no longer supported
#endif

/** An index that may optionally be placed in shared memory.
 *  Must be Plain Old Data (POD), so no virtual methods are allowed.
 *  If in shared memory, exactly one process must explicitly call the constructor via placement new.
 */
struct audio_utils_fifo_index {
    friend class audio_utils_fifo_reader;
    friend class audio_utils_fifo_writer;

public:
    audio_utils_fifo_index() : mIndex(0) { }

private:
    // Linux futex is 32 bits regardless of platform.
    // It would make more sense to declare this as atomic_uint32_t, but there is no such type name.
    std::atomic_uint_least32_t  mIndex; // accessed by both sides using atomic operations
    static_assert(sizeof(mIndex) == sizeof(uint32_t), "mIndex must be 32 bits");

    // TODO Abstract out atomic operations to here
    // TODO Replace friend by setter and getter, and abstract the futex
};

// Base class for single writer, single-reader or multi-reader non-blocking FIFO.
// The base class manipulates frame indices only, and has no knowledge of frame sizes or the buffer.

class audio_utils_fifo_base {

protected:

/* Construct FIFO base class
 *
 *  \param sharedRear  Writer's rear index in shared memory.
 *  \param throttleFront Pointer to the front index of at most one reader that throttles the
 *                       writer, or NULL for no throttling.
 */
    audio_utils_fifo_base(uint32_t frameCount, audio_utils_fifo_index& sharedRear,
            // TODO inconsistent & vs *
            audio_utils_fifo_index *throttleFront = NULL);
    /*virtual*/ ~audio_utils_fifo_base();

/** Return a new index as the sum of a validated index and a specified increment.
 *
 * \param index     Caller should supply a validated mFront or mRear.
 * \param increment Value to be added to the index <= mFrameCount.
 *
 * \return the sum of index plus increment.
 */
    uint32_t sum(uint32_t index, uint32_t increment);

/** Return the difference between two indices: rear - front.
 *
 * \param rear     Caller should supply an unvalidated mRear.
 * \param front    Caller should supply an unvalidated mFront.
 * \param lost     If non-NULL, set to the approximate number of lost frames.
 *
 * \return the zero or positive difference <= mFrameCount, or a negative error code.
 */
    int32_t diff(uint32_t rear, uint32_t front, size_t *lost);

    // These fields are const after initialization
    const uint32_t mFrameCount;   // max number of significant frames to be stored in the FIFO > 0
    const uint32_t mFrameCountP2; // roundup(mFrameCount)
    const uint32_t mFudgeFactor;  // mFrameCountP2 - mFrameCount, the number of "wasted" frames
                                  // after the end of mBuffer.  Only the indices are wasted, not any
                                  // memory.

    // TODO always true for now, will be extended later to support false
    const bool mIsPrivate;        // whether reader and writer virtual address spaces are the same

    audio_utils_fifo_index&     mSharedRear;

    // Pointer to the front index of at most one reader that throttles the writer,
    // or NULL for no throttling
    audio_utils_fifo_index*     mThrottleFront;
};

////////////////////////////////////////////////////////////////////////////////

// Same as above, but understands frame sizes and knows about the buffer but does not own it.
// Writer and reader must be in same process.

class audio_utils_fifo : audio_utils_fifo_base {

    friend class audio_utils_fifo_reader;
    friend class audio_utils_fifo_writer;

public:

/**
 * Construct a FIFO object: multi-process.
 *
 *  \param frameCount  Max number of significant frames to be stored in the FIFO > 0.
 *                     If writes and reads always use the same count, and that count is a divisor of
 *                     frameCount, then the writes and reads will never do a partial transfer.
 *  \param frameSize   Size of each frame in bytes > 0, and frameSize * frameCount <= INT_MAX.
 *  \param buffer      Pointer to a caller-allocated buffer of frameCount frames.
 *  \param sharedRear  Writer's rear index in shared memory.
 *  \param throttleFront Pointer to the front index of at most one reader that throttles the
 *                       writer, or NULL for no throttling.
 */
    audio_utils_fifo(uint32_t frameCount, uint32_t frameSize, void *buffer,
            // TODO inconsistent & vs *
            audio_utils_fifo_index& sharedRear, audio_utils_fifo_index *throttleFront = NULL);

/**
 * Construct a FIFO object: single-process.
 *  \param throttlesWriter Whether there is a reader that throttles the writer.
 */
    audio_utils_fifo(uint32_t frameCount, uint32_t frameSize, void *buffer,
            bool throttlesWriter = false);

    /*virtual*/ ~audio_utils_fifo();

private:

    // These fields are const after initialization
    const uint32_t mFrameSize;  // size of each frame in bytes
    void * const   mBuffer;     // pointer to caller-allocated buffer of size mFrameCount frames

    // only used for single-process constructor
    audio_utils_fifo_index      mSingleProcessSharedRear;

    // only used for single-process constructor when throttlesWriter == true
    audio_utils_fifo_index      mSingleProcessSharedFront;
};

// Describes one virtually contiguous fragment of a logically contiguous slice.
// Compare to struct iovec for readv(2) and writev(2).
struct audio_utils_iovec {
    uint32_t    mOffset;    // in frames, relative to mBuffer, undefined if mLength == 0
    uint32_t    mLength;    // in frames
};

////////////////////////////////////////////////////////////////////////////////

// Based on frameworks/av/include/media/AudioBufferProvider.h
class audio_utils_fifo_provider {
public:
    audio_utils_fifo_provider();
    virtual ~audio_utils_fifo_provider();

// The count is the maximum number of desired frames, not the minimum number of desired frames.
// See the high/low setpoints for something which is close to, but not the same as, a true minimum.

// The timeout indicates the maximum time to wait for at least one frame, not for all frames.
// NULL is equivalent to non-blocking.
// FIXME specify timebase, relative/absolute etc

// Error codes for ssize_t return value:
//  -EIO        corrupted indices (reader or writer)
//  -EOVERFLOW  reader is not keeping up with writer (reader only)
    virtual ssize_t obtain(audio_utils_iovec iovec[2], size_t count, struct timespec *timeout) = 0;

    virtual void release(size_t count) = 0;

protected:
    // Number of frames obtained at most recent obtain(), less number of frames released
    uint32_t    mObtained;
};

////////////////////////////////////////////////////////////////////////////////

class audio_utils_fifo_writer : public audio_utils_fifo_provider {

public:
    // Single-process and multi-process use same constructor here, but different 'fifo' constructors
    audio_utils_fifo_writer(audio_utils_fifo& fifo);
    virtual ~audio_utils_fifo_writer();

/**
 * Write to FIFO.
 *
 * \param buffer    Pointer to source buffer containing 'count' frames of data.
 * \param count     Desired number of frames to write.
 * \param timeout   NULL and zero fields are both non-blocking.
 *
 * \return actual number of frames written <= count.
 *
 * The actual transfer count may be zero if the FIFO is full,
 * or partial if the FIFO was almost full.
 * A negative return value indicates an error.
 */
    ssize_t write(const void *buffer, size_t count, struct timespec *timeout = NULL);

    // Implement audio_utils_fifo_provider
    virtual ssize_t obtain(audio_utils_iovec iovec[2], size_t count, struct timespec *timeout);
    virtual void release(size_t count);

    // TODO add error checks and getters
    void setHighLevelTrigger(uint32_t level) { mHighLevelTrigger = level; }
    void setEffectiveFrames(uint32_t effectiveFrames) { mEffectiveFrames = effectiveFrames; }

private:
    audio_utils_fifo&   mFifo;

    // Accessed by writer only using ordinary operations
    uint32_t    mLocalRear; // frame index of next frame slot available to write, or write index

    // TODO needs a state transition diagram for threshold and arming process
    uint32_t    mLowLevelArm;       // arm if filled <= threshold
    uint32_t    mHighLevelTrigger;  // trigger reader if armed and filled >= threshold
    bool        mArmed;

    uint32_t    mEffectiveFrames;   // current effective buffer size, <= mFifo.mFrameCount
};

////////////////////////////////////////////////////////////////////////////////

class audio_utils_fifo_reader : public audio_utils_fifo_provider {

public:
    // At most one reader can specify throttlesWriter == true
    audio_utils_fifo_reader(audio_utils_fifo& fifo, bool throttlesWriter = true);
    virtual ~audio_utils_fifo_reader();

/** Read from FIFO.
 *
 * \param buffer    Pointer to destination buffer to be filled with up to 'count' frames of data.
 * \param count     Desired number of frames to read.
 * \param timeout   NULL and zero fields are both non-blocking.
 * \param lost      If non-NULL, set to the approximate number of lost frames before re-sync.
 *
 * \return actual number of frames read <= count.
 *
 * The actual transfer count may be zero if the FIFO is empty,
 * or partial if the FIFO was almost empty.
 * A negative return value indicates an error.
 */
    ssize_t read(void *buffer, size_t count, struct timespec *timeout = NULL, size_t *lost = NULL);

    // Implement audio_utils_fifo_provider
    virtual ssize_t obtain(audio_utils_iovec iovec[2], size_t count, struct timespec *timeout);
    virtual void release(size_t count);

    // Extended parameter list for reader only
    ssize_t obtain(audio_utils_iovec iovec[2], size_t count, struct timespec *timeout,
            size_t *lost);

private:
    audio_utils_fifo&   mFifo;

    // Accessed by reader only using ordinary operations
    uint32_t     mLocalFront;   // frame index of first frame slot available to read, or read index

    // Points to shared front index if this reader throttles writer, or NULL if we don't throttle
    audio_utils_fifo_index*     mThrottleFront;

    // TODO not used yet
    uint32_t    mHighLevelArm;      // arm if filled >= threshold
    uint32_t    mLowLevelTrigger;   // trigger writer if armed and filled <= threshold
    bool        mArmed;

};

#endif  // !ANDROID_AUDIO_FIFO_H
