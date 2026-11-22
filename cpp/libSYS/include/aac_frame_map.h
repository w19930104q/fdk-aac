/* ------------------------------------------------------------------
 * Copyright (C) 2026 Martin Storsjo
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
 * -------------------------------------------------------------------
 */

#ifndef AAC_FRAME_MAP_H
#define AAC_FRAME_MAP_H

/**
 * \file aac_frame_map.h
 * \brief Optional per-frame metadata channel: map each AAC access unit to the
 *        interval of input PCM samples it was encoded from.
 *
 * This helper module is intended for low-delay streaming use cases (e.g.
 * AAC-LD, AOT 23) where the receiver needs to know exactly which input
 * samples - and thereby which capture instant - each encoded frame
 * corresponds to, instead of assuming a uniform playout grid.
 *
 * The FDK encoder consumes input PCM samples strictly in order and emits one
 * access unit per `frameLength` input samples (per channel), independent of
 * how the input is chunked across aacEncEncode() calls.  Likewise, the
 * decoder produces one frame of `frameSize` output samples per access unit,
 * in the same order.  Frame `k` on both sides therefore corresponds to the
 * input sample interval
 *
 *     [ k * frameLength , (k+1) * frameLength )
 *
 * counted in input PCM samples per channel from the first sample fed to the
 * encoder.  This module makes that mapping explicit, queryable and
 * verifiable on both ends, so the end-to-end frame mapping can be
 * recomputed by either side.
 *
 * The frame length itself follows ISO/IEC 14496-3 (MPEG-4 Audio):
 *  - AAC-LC  (AOT 2):        1024 samples (960 with the 960-sample granule)
 *  - ER AAC LD  (AOT 23):    512 samples  (480 with the 480-sample granule)
 *  - ER AAC ELD (AOT 39):    512 samples  (480 with the 480-sample granule)
 *
 * Use aacFrameMap_GetAotFrameLength() to compute these values from the AOT,
 * aacFrameMap_OpenForEncoder() / aacFrameMap_OpenForDecoder() to adopt the
 * frame length actually configured in a running encoder/decoder instance
 * (which also accounts for SBR upsampling etc.), or aacFrameMap_Open() to
 * set an explicitly known frame length.
 *
 * Typical encoder-side usage:
 * \code
 *   HANDLE_AAC_FRAME_MAP map;
 *   aacFrameMap_OpenForEncoder(&map, encoder);   // after encoder init
 *
 *   // whenever input was consumed by aacEncEncode():
 *   aacFrameMap_FeedSamples(map, out_args.numInSamples / channels);
 *   // whenever aacEncEncode() produced an access unit (numOutBytes > 0):
 *   AAC_FRAME_INTERVAL iv;
 *   if (aacFrameMap_NextFrame(map, &iv) == AAC_FRAME_MAP_OK) {
 *     // iv.firstSample / iv.frameLength locate this frame's input samples;
 *     // attach them (or the capture timestamp of iv.firstSample) to the
 *     // frame as out-of-band metadata for the receiver.
 *   }
 *   // at end of stream, before flushing the encoder with numInSamples=-1:
 *   aacFrameMap_Finalize(map);
 * \endcode
 *
 * Typical decoder-side usage:
 * \code
 *   HANDLE_AAC_FRAME_MAP map;
 *   aacFrameMap_OpenForDecoder(&map, decoder);  // after first decoded frame
 *   // per decoded frame, in decode order:
 *   AAC_FRAME_INTERVAL iv;
 *   aacFrameMap_NextFrame(map, &iv);  // same intervals as on the encoder side
 * \endcode
 *
 * All sample counts and indices are per channel, in the encoder's input
 * sample grid (which equals the decoder's output sample grid when no
 * sample-rate conversion is configured).
 */

#include "machine_type.h"
#include "aacenc_lib.h"
#include "aacdecoder_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Error codes returned by the aacFrameMap_* functions. */
typedef enum {
  AAC_FRAME_MAP_OK = 0,               /*!< Success. */
  AAC_FRAME_MAP_INVALID_HANDLE,       /*!< NULL or invalid handle/pointer. */
  AAC_FRAME_MAP_INVALID_ARGUMENT,     /*!< Invalid argument (e.g. zero frame
                                           length, encoder/decoder not
                                           initialized). */
  AAC_FRAME_MAP_INVALID_CALL,         /*!< Call not allowed in the current
                                           state (e.g. feeding input after
                                           aacFrameMap_Finalize()). */
  AAC_FRAME_MAP_INSUFFICIENT_INPUT,   /*!< Fewer input samples were registered
                                           via aacFrameMap_FeedSamples() than
                                           the frame being mapped requires;
                                           the metadata channel and the
                                           encoder are out of sync. */
  AAC_FRAME_MAP_UNSUPPORTED           /*!< Unsupported AOT or unavailable
                                           frame length information. */
} AAC_FRAME_MAP_ERROR;

/** Input sample interval covered by one encoded/decoded frame. */
typedef struct {
  UINT64 firstSample; /*!< Per-channel index of the first input sample this
                           frame was encoded from (0-based, counted from the
                           first sample fed to the encoder). */
  UINT frameLength;   /*!< Number of input samples per channel covered by
                           this frame (the codec frame length). */
} AAC_FRAME_INTERVAL;

/** Opaque frame map instance handle. */
typedef struct AAC_FRAME_MAP_INSTANCE *HANDLE_AAC_FRAME_MAP;

/**
 * \brief Compute the codec frame length (input samples per frame and
 *        channel) for an audio object type, per ISO/IEC 14496-3.
 *
 * Returns the core frame length without any SBR upsampling factor:
 *  - AOT 2  (AAC-LC):    1024, or 960 if granuleLength is 960
 *  - AOT 23 (ER AAC LD): 512,  or 480 if granuleLength is 480
 *  - AOT 39 (ER AAC ELD): 512, or 480 if granuleLength is 480
 *
 * For configurations with SBR (AOT 5, 29, or ELD+SBR) the encoder input
 * frame length is doubled; query the running instance with
 * aacFrameMap_OpenForEncoder() / aacFrameMap_OpenForDecoder() instead.
 *
 * \param aot            Audio object type (e.g. 23 for ER AAC LD).
 * \param granuleLength  Configured granule length (AACENC_GRANULE_LENGTH),
 *                       or 0 for the default frame length of the AOT.
 * \return Frame length in samples per channel, or 0 if the AOT/granule
 *         combination is not supported by this function.
 */
UINT aacFrameMap_GetAotFrameLength(INT aot, UINT granuleLength);

/**
 * \brief Open a frame map with an explicitly known frame length.
 * \param ph           Pointer to a handle to be filled.
 * \param frameLength  Codec frame length in samples per channel (> 0).
 * \return AAC_FRAME_MAP_OK on success.
 */
AAC_FRAME_MAP_ERROR aacFrameMap_Open(HANDLE_AAC_FRAME_MAP *ph,
                                     UINT frameLength);

/**
 * \brief Open a frame map for an initialized encoder, adopting its
 *        configured frame length (AACENC_InfoStruct::frameLength).
 *
 * The encoder must have been initialized (aacEncEncode() with NULL
 * arguments) before this call.  A map opened this way validates on
 * aacFrameMap_NextFrame() that enough input samples were registered with
 * aacFrameMap_FeedSamples(), catching synchronization errors between the
 * metadata channel and the encoder.
 *
 * \param ph        Pointer to a handle to be filled.
 * \param hEncoder  Encoder handle.
 * \return AAC_FRAME_MAP_OK on success.
 */
AAC_FRAME_MAP_ERROR aacFrameMap_OpenForEncoder(HANDLE_AAC_FRAME_MAP *ph,
                                               HANDLE_AACENCODER hEncoder);

/**
 * \brief Open a frame map for a decoder, adopting its stream frame length
 *        (CStreamInfo::frameSize).
 *
 * The decoder must have decoded at least one frame (or otherwise know the
 * stream configuration) before this call.  The resulting map yields the
 * same per-frame intervals as the encoder-side map, so the end-to-end
 * frame mapping can be recomputed at the receiver.
 *
 * \param ph        Pointer to a handle to be filled.
 * \param hDecoder  Decoder handle.
 * \return AAC_FRAME_MAP_OK on success.
 */
AAC_FRAME_MAP_ERROR aacFrameMap_OpenForDecoder(HANDLE_AAC_FRAME_MAP *ph,
                                               HANDLE_AACDECODER hDecoder);

/**
 * \brief Close a frame map and free its resources.
 * \param ph  Pointer to the handle; set to NULL on return.
 */
void aacFrameMap_Close(HANDLE_AAC_FRAME_MAP *ph);

/**
 * \brief Register input samples consumed by the encoder (encoder side only).
 *
 * Call this with the number of samples per channel the encoder consumed in
 * each aacEncEncode() call (AACENC_OutArgs::numInSamples divided by the
 * number of input channels).
 *
 * \param h           Frame map handle.
 * \param numSamples  Number of consumed input samples per channel.
 * \return AAC_FRAME_MAP_OK on success, AAC_FRAME_MAP_INVALID_CALL if called
 *         after aacFrameMap_Finalize().
 */
AAC_FRAME_MAP_ERROR aacFrameMap_FeedSamples(HANDLE_AAC_FRAME_MAP h,
                                            UINT numSamples);

/**
 * \brief Signal end of input (encoder side only).
 *
 * After this call no further aacFrameMap_FeedSamples() calls are allowed,
 * and aacFrameMap_NextFrame() no longer requires the registered input
 * samples to cover the mapped frame.  This mirrors the encoder flushing
 * with numInSamples = -1, which zero-pads the final frame(s) (up to the
 * codec delay); such trailing frames still map to consecutive intervals
 * following the last real input sample.
 *
 * \param h  Frame map handle.
 * \return AAC_FRAME_MAP_OK on success.
 */
AAC_FRAME_MAP_ERROR aacFrameMap_Finalize(HANDLE_AAC_FRAME_MAP h);

/**
 * \brief Retrieve the input sample interval of the next frame, in
 *        encode/decode order.
 *
 * Call once per produced access unit (encoder side) or per decoded frame
 * (decoder side).  For a map opened with aacFrameMap_OpenForEncoder(), the
 * call fails with AAC_FRAME_MAP_INSUFFICIENT_INPUT if the samples
 * registered so far do not cover the whole frame (unless
 * aacFrameMap_Finalize() was called).
 *
 * \param h          Frame map handle.
 * \param pInterval  Output: interval of input samples for this frame.
 * \return AAC_FRAME_MAP_OK on success.
 */
AAC_FRAME_MAP_ERROR aacFrameMap_NextFrame(HANDLE_AAC_FRAME_MAP h,
                                          AAC_FRAME_INTERVAL *pInterval);

/**
 * \brief Retrieve the input sample interval of an arbitrary frame index.
 *
 * Pure function of the frame index; does not affect the sequential state
 * used by aacFrameMap_NextFrame() and performs no input accounting.
 *
 * \param h           Frame map handle.
 * \param frameIndex  0-based frame index.
 * \param pInterval   Output: interval of input samples for this frame.
 * \return AAC_FRAME_MAP_OK on success.
 */
AAC_FRAME_MAP_ERROR aacFrameMap_GetInterval(HANDLE_AAC_FRAME_MAP h,
                                            UINT64 frameIndex,
                                            AAC_FRAME_INTERVAL *pInterval);

/** \return The frame length in samples per channel, or 0 on invalid handle. */
UINT aacFrameMap_GetFrameLength(HANDLE_AAC_FRAME_MAP h);

/** \return Input samples per channel registered so far. */
UINT64 aacFrameMap_GetSamplesFed(HANDLE_AAC_FRAME_MAP h);

/** \return Number of frames mapped so far via aacFrameMap_NextFrame(). */
UINT64 aacFrameMap_GetFramesMapped(HANDLE_AAC_FRAME_MAP h);

#ifdef __cplusplus
}
#endif

#endif /* AAC_FRAME_MAP_H */
