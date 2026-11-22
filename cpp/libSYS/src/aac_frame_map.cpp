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

#include "aac_frame_map.h"

#include "genericStds.h"

struct AAC_FRAME_MAP_INSTANCE {
  UINT frameLength;    /* Codec frame length, samples per channel. */
  UINT64 samplesFed;   /* Input samples per channel registered so far. */
  UINT64 framesMapped; /* Frames handed out via aacFrameMap_NextFrame(). */
  INT validateInput;   /* Check fed input against mapped frames. */
  INT finalized;       /* End of input signaled; count padded to a whole
                          number of frames. */
};

UINT aacFrameMap_GetAotFrameLength(INT aot, UINT granuleLength) {
  UINT baseLength, altLength;

  switch (aot) {
    case 2: /* AOT_AAC_LC: 1024 (960) samples per frame, ISO/IEC 14496-3 */
      baseLength = 1024;
      altLength = 960;
      break;
    case 23: /* AOT_ER_AAC_LD: 512 (480) samples per frame */
    case 39: /* AOT_ER_AAC_ELD: 512 (480) samples per frame */
      baseLength = 512;
      altLength = 480;
      break;
    default:
      /* Other AOTs (notably SBR configurations, where the encoder input
         frame length is doubled) must be queried from a running instance
         via aacFrameMap_OpenForEncoder()/aacFrameMap_OpenForDecoder(). */
      return 0;
  }

  if (granuleLength == 0 || granuleLength == baseLength) return baseLength;
  if (granuleLength == altLength) return altLength;
  return 0;
}

AAC_FRAME_MAP_ERROR aacFrameMap_Open(HANDLE_AAC_FRAME_MAP *ph,
                                     UINT frameLength) {
  HANDLE_AAC_FRAME_MAP h;

  if (ph == NULL) return AAC_FRAME_MAP_INVALID_HANDLE;
  *ph = NULL;
  if (frameLength == 0) return AAC_FRAME_MAP_INVALID_ARGUMENT;

  h = (HANDLE_AAC_FRAME_MAP)FDKcalloc(1, sizeof(struct AAC_FRAME_MAP_INSTANCE));
  if (h == NULL) return AAC_FRAME_MAP_INVALID_HANDLE;

  h->frameLength = frameLength;
  *ph = h;
  return AAC_FRAME_MAP_OK;
}

AAC_FRAME_MAP_ERROR aacFrameMap_OpenForEncoder(HANDLE_AAC_FRAME_MAP *ph,
                                               HANDLE_AACENCODER hEncoder) {
  AACENC_InfoStruct info = {0};
  AAC_FRAME_MAP_ERROR err;

  if (ph == NULL) return AAC_FRAME_MAP_INVALID_HANDLE;
  *ph = NULL;
  if (hEncoder == NULL) return AAC_FRAME_MAP_INVALID_ARGUMENT;
  if (aacEncInfo(hEncoder, &info) != AACENC_OK || info.frameLength == 0)
    return AAC_FRAME_MAP_INVALID_ARGUMENT;

  err = aacFrameMap_Open(ph, info.frameLength);
  if (err == AAC_FRAME_MAP_OK) (*ph)->validateInput = 1;
  return err;
}

AAC_FRAME_MAP_ERROR aacFrameMap_OpenForDecoder(HANDLE_AAC_FRAME_MAP *ph,
                                               HANDLE_AACDECODER hDecoder) {
  CStreamInfo *pInfo;

  if (ph == NULL) return AAC_FRAME_MAP_INVALID_HANDLE;
  *ph = NULL;
  if (hDecoder == NULL) return AAC_FRAME_MAP_INVALID_ARGUMENT;

  pInfo = aacDecoder_GetStreamInfo(hDecoder);
  if (pInfo == NULL || pInfo->frameSize <= 0)
    return AAC_FRAME_MAP_UNSUPPORTED;

  return aacFrameMap_Open(ph, (UINT)pInfo->frameSize);
}

void aacFrameMap_Close(HANDLE_AAC_FRAME_MAP *ph) {
  if (ph != NULL && *ph != NULL) {
    FDKfree(*ph);
    *ph = NULL;
  }
}

AAC_FRAME_MAP_ERROR aacFrameMap_FeedSamples(HANDLE_AAC_FRAME_MAP h,
                                            UINT numSamples) {
  if (h == NULL) return AAC_FRAME_MAP_INVALID_HANDLE;
  if (h->finalized) return AAC_FRAME_MAP_INVALID_CALL;
  h->samplesFed += numSamples;
  return AAC_FRAME_MAP_OK;
}

AAC_FRAME_MAP_ERROR aacFrameMap_Finalize(HANDLE_AAC_FRAME_MAP h) {
  if (h == NULL) return AAC_FRAME_MAP_INVALID_HANDLE;
  /* After end of input the encoder zero-pads (up to the codec delay) when
     flushing, so frames may legitimately extend beyond the registered
     input samples; only the flag is latched here. */
  h->finalized = 1;
  return AAC_FRAME_MAP_OK;
}

AAC_FRAME_MAP_ERROR aacFrameMap_NextFrame(HANDLE_AAC_FRAME_MAP h,
                                          AAC_FRAME_INTERVAL *pInterval) {
  AAC_FRAME_MAP_ERROR err;

  if (h == NULL || pInterval == NULL) return AAC_FRAME_MAP_INVALID_HANDLE;

  if (h->validateInput && !h->finalized &&
      h->samplesFed < (h->framesMapped + 1) * h->frameLength)
    return AAC_FRAME_MAP_INSUFFICIENT_INPUT;

  err = aacFrameMap_GetInterval(h, h->framesMapped, pInterval);
  if (err != AAC_FRAME_MAP_OK) return err;
  h->framesMapped++;
  return AAC_FRAME_MAP_OK;
}

AAC_FRAME_MAP_ERROR aacFrameMap_GetInterval(HANDLE_AAC_FRAME_MAP h,
                                            UINT64 frameIndex,
                                            AAC_FRAME_INTERVAL *pInterval) {
  if (h == NULL || pInterval == NULL) return AAC_FRAME_MAP_INVALID_HANDLE;

  pInterval->firstSample = frameIndex * h->frameLength;
  pInterval->frameLength = h->frameLength;
  return AAC_FRAME_MAP_OK;
}

UINT aacFrameMap_GetFrameLength(HANDLE_AAC_FRAME_MAP h) {
  if (h == NULL) return 0;
  return h->frameLength;
}

UINT64 aacFrameMap_GetSamplesFed(HANDLE_AAC_FRAME_MAP h) {
  if (h == NULL) return 0;
  return h->samplesFed;
}

UINT64 aacFrameMap_GetFramesMapped(HANDLE_AAC_FRAME_MAP h) {
  if (h == NULL) return 0;
  return h->framesMapped;
}
