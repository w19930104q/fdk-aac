/* ------------------------------------------------------------------
 * Copyright (C) 2026 Martin Storsjo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * -------------------------------------------------------------------
 */

/*
 * Acceptance tests for HE-AACv2 (mono core + Parametric Stereo) decoding.
 *
 * 1. Channel account stability: the decoded channel count of an HE-AACv2
 *    stream is determined by the bitstream configuration only and stays
 *    constant for every frame of the stream, including error concealment
 *    frames.
 * 2. IID/ICC boundary robustness: streams that drive the IID/ICC
 *    parameters to their quantization boundaries (hard panned content)
 *    decode to sample values within the declared output range, and the
 *    PS mixing stage itself never produces out-of-range (wrapping)
 *    values, even for out-of-range parameter indices.
 * 3. Energy relation: the same content encoded as HE-AACv2 and as
 *    AAC-LC stereo decodes to signals whose energies relate within the
 *    specified ratio.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "aacenc_lib.h"
#include "aacdecoder_lib.h"

#include "libSBRdec/src/psdec.h"

#define AOT_AAC_LC 2
#define AOT_SBR 5
#define AOT_PS 29

#define SAMPLE_RATE 48000
#define NUM_SECONDS 2

/* Declared decoder output range (16 bit PCM). */
#define PCM_OUT_MIN (-32768)
#define PCM_OUT_MAX (32767)

/* Specified energy ratio bounds between the HE-AACv2 and the AAC-LC
   stereo decodings of the same content. */
#define ENERGY_RATIO_MIN 0.5
#define ENERGY_RATIO_MAX 2.0

/* Largest possible absolute value of the PS mixing coefficients h11..h22:
   the largest entry of the IID dequantization tables ScaleFactors and
   ScaleFactorsFine (the tables store the values shifted right by 1). */
#define H_COEF_MAX ((FIXP_DBL)0x5A825C00)

/* Number of hybrid (sub-)subbands processed by applySlotBasedRotation. */
#define NO_HYBRID_BANDS 71

static int failures = 0;

#define CHECK(cond, msg)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, \
              __LINE__);                                  \
      failures++;                                         \
    }                                                     \
  } while (0)

/* Deterministic pseudo random noise in [-1, 1). */
static uint32_t rng_state;
static void rng_seed(uint32_t seed) { rng_state = seed; }
static double frand(void) {
  rng_state = rng_state * 1664525u + 1013904223u;
  return (double)((rng_state >> 8) & 0xFFFFFF) / 8388608.0 - 1.0;
}

/* Encode interleaved 16 bit PCM (nsamples counts all channels) to an ADTS
   stream. Returns 0 on success. */
static int encode_to_stream(const int16_t *pcm, size_t nsamples, int channels,
                            int aot, int bitrate, std::vector<uint8_t> &out) {
  HANDLE_AACENCODER encoder;
  AACENC_InfoStruct info = {0};
  AACENC_ERROR err;

  if (aacEncOpen(&encoder, 0, channels) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(encoder, AACENC_AOT, aot) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(encoder, AACENC_SAMPLERATE, SAMPLE_RATE) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(encoder, AACENC_CHANNELMODE,
                          channels == 2 ? MODE_2 : MODE_1) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(encoder, AACENC_CHANNELORDER, 1) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(encoder, AACENC_BITRATE, bitrate) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(encoder, AACENC_TRANSMUX, 2) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(encoder, AACENC_AFTERBURNER, 1) != AACENC_OK)
    return 1;
  if (aacEncEncode(encoder, NULL, NULL, NULL, NULL) != AACENC_OK)
    return 1;
  if (aacEncInfo(encoder, &info) != AACENC_OK)
    return 1;

  size_t frame_samples = (size_t)info.frameLength * channels;
  size_t pos = 0;
  int flushing = 0;
  while (1) {
    AACENC_BufDesc in_buf = {0}, out_buf = {0};
    AACENC_InArgs in_args = {0};
    AACENC_OutArgs out_args = {0};
    int in_identifier = IN_AUDIO_DATA;
    int out_identifier = OUT_BITSTREAM_DATA;
    int in_size, in_elem_size, out_size, out_elem_size;
    void *in_ptr, *out_ptr;
    uint8_t outbuf[20480];

    if (!flushing && pos < nsamples) {
      size_t chunk = nsamples - pos;
      if (chunk > frame_samples)
        chunk = frame_samples;
      in_ptr = (void *)&pcm[pos];
      in_size = (int)(chunk * sizeof(int16_t));
      in_elem_size = 2;
      in_args.numInSamples = (int)chunk;
      pos += chunk;
      if (pos >= nsamples)
        flushing = 1;
    } else {
      in_ptr = NULL;
      in_size = 0;
      in_elem_size = 2;
      in_args.numInSamples = -1; /* flush */
    }
    in_buf.numBufs = 1;
    in_buf.bufs = &in_ptr;
    in_buf.bufferIdentifiers = &in_identifier;
    in_buf.bufSizes = &in_size;
    in_buf.bufElSizes = &in_elem_size;

    out_ptr = outbuf;
    out_size = sizeof(outbuf);
    out_elem_size = 1;
    out_buf.numBufs = 1;
    out_buf.bufs = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes = &out_size;
    out_buf.bufElSizes = &out_elem_size;

    err = aacEncEncode(encoder, &in_buf, &out_buf, &in_args, &out_args);
    if (err == AACENC_ENCODE_EOF)
      break;
    if (err != AACENC_OK)
      return 1;
    out.insert(out.end(), outbuf, outbuf + out_args.numOutBytes);
  }
  aacEncClose(&encoder);
  return 0;
}

struct DecodeStats {
  long minv;
  long maxv;
  int frames;
  int bad_channel_frames;
  std::vector<int16_t> samples; /* collected only if collect != 0 */
};

/* Decode a complete ADTS stream. If drop_frame >= 0, that frame is
   replaced by error concealment. Returns 0 on success. */
static int decode_stream(const uint8_t *data, size_t size, int expect_channels,
                         int drop_frame, int collect, DecodeStats &st) {
  HANDLE_AACDECODER decoder;
  AAC_DECODER_ERROR err;
  const int outbuf_size = 8 * 2 * 2048;
  int16_t *outbuf = (int16_t *)malloc(outbuf_size * sizeof(int16_t));
  int frame = 0;

  st.minv = 0;
  st.maxv = 0;
  st.frames = 0;
  st.bad_channel_frames = 0;

  decoder = aacDecoder_Open(TT_MP4_ADTS, 1);
  aacDecoder_SetParam(decoder, AAC_CONCEAL_METHOD, 1);
  aacDecoder_SetParam(decoder, AAC_PCM_LIMITER_ENABLE, 0);

  /* Split the stream into ADTS frames so that frame loss can be
     simulated. */
  size_t pos = 0;
  while (pos < size) {
    UINT valid, packet_size;
    const uint8_t *ptr;

    if (size - pos < 7 || data[pos] != 0xff ||
        (data[pos + 1] & 0xf0) != 0xf0) {
      free(outbuf);
      aacDecoder_Close(decoder);
      return 1;
    }
    packet_size = ((data[pos + 3] & 0x03) << 11) | (data[pos + 4] << 3) |
                  (data[pos + 5] >> 5);
    if (packet_size < 7 || pos + packet_size > size) {
      free(outbuf);
      aacDecoder_Close(decoder);
      return 1;
    }

    if ((int)frame == drop_frame) {
      /* Simulate a lost frame: conceal instead of feeding the data. */
      err = aacDecoder_DecodeFrame(decoder, outbuf, outbuf_size,
                                   AACDEC_CONCEAL);
      if (err != AAC_DEC_OK && !IS_DECODE_ERROR(err)) {
        free(outbuf);
        aacDecoder_Close(decoder);
        return 1;
      }
      CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
      if (info->numChannels != expect_channels)
        st.bad_channel_frames++;
      st.frames++;
      frame++;
      pos += packet_size;
      continue;
    }

    ptr = &data[pos];
    valid = packet_size;
    err = aacDecoder_Fill(decoder, (UCHAR **)&ptr, &packet_size, &valid);
    if (err != AAC_DEC_OK) {
      free(outbuf);
      aacDecoder_Close(decoder);
      return 1;
    }
    while (1) {
      err = aacDecoder_DecodeFrame(decoder, outbuf, outbuf_size, 0);
      if (err == AAC_DEC_NOT_ENOUGH_BITS)
        break;
      if (err != AAC_DEC_OK) {
        free(outbuf);
        aacDecoder_Close(decoder);
        return 1;
      }
      CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
      if (info->numChannels != expect_channels)
        st.bad_channel_frames++;
      st.frames++;
      int n = info->numChannels * info->frameSize;
      for (int i = 0; i < n; i++) {
        if (outbuf[i] < st.minv)
          st.minv = outbuf[i];
        if (outbuf[i] > st.maxv)
          st.maxv = outbuf[i];
        if (collect)
          st.samples.push_back(outbuf[i]);
      }
    }
    frame++;
    pos += packet_size;
  }

  free(outbuf);
  aacDecoder_Close(decoder);
  return 0;
}

/* Generate deterministic stereo test content. kind 0: both channels
   active; kind 1: hard panned left; kind 2: hard panned right. */
static void gen_content(int kind, std::vector<int16_t> &pcm) {
  int total = SAMPLE_RATE * NUM_SECONDS;
  pcm.resize((size_t)total * 2);
  rng_seed(kind * 7919u + 13u);
  for (int i = 0; i < total; i++) {
    double t = (double)i / SAMPLE_RATE;
    double l = 0.6 * sin(2.0 * M_PI * 440.0 * t) +
               0.2 * sin(2.0 * M_PI * 2900.0 * t) + 0.05 * frand();
    double r = 0.6 * sin(2.0 * M_PI * 554.0 * t) +
               0.2 * sin(2.0 * M_PI * 3400.0 * t) + 0.05 * frand();
    if (kind == 1)
      r = 0.0;
    if (kind == 2)
      l = 0.0;
    pcm[2 * i] = (int16_t)(l * 30000.0);
    pcm[2 * i + 1] = (int16_t)(r * 30000.0);
  }
}

/* Test 1: the channel count of an HE-AACv2 stream is constant for every
   frame, including concealed frames. */
static void test_channel_stability(void) {
  static const int bitrates[] = {16000, 24000, 32000};
  std::vector<int16_t> pcm;
  gen_content(0, pcm);

  for (size_t b = 0; b < sizeof(bitrates) / sizeof(bitrates[0]); b++) {
    std::vector<uint8_t> stream;
    DecodeStats st;
    char msg[128];

    snprintf(msg, sizeof(msg), "encode HE-AACv2 @ %d", bitrates[b]);
    CHECK(encode_to_stream(&pcm[0], pcm.size(), 2, AOT_PS, bitrates[b],
                           stream) == 0,
          msg);
    if (stream.empty()) {
      failures++;
      continue;
    }

    snprintf(msg, sizeof(msg), "decode HE-AACv2 @ %d", bitrates[b]);
    CHECK(decode_stream(&stream[0], stream.size(), 2, -1, 0, st) == 0, msg);
    snprintf(msg, sizeof(msg), "frames decoded @ %d", bitrates[b]);
    CHECK(st.frames > 0, msg);
    snprintf(msg, sizeof(msg),
             "channel count constant (2) for all %d frames @ %d bps",
             st.frames, bitrates[b]);
    CHECK(st.bad_channel_frames == 0, msg);

    /* Same, but with frame 5 lost (error concealment): the channel
       account must not change either. */
    DecodeStats stc;
    CHECK(decode_stream(&stream[0], stream.size(), 2, 5, 0, stc) == 0,
          "decode HE-AACv2 with concealed frame");
    CHECK(stc.bad_channel_frames == 0,
          "channel count constant (2) across concealment");
  }
}

/* Test 2, black box part: IID/ICC boundary content (hard panned, near
   full scale) decodes to sample values within the declared range. */
static void test_boundary_output_range(void) {
  for (int kind = 1; kind <= 2; kind++) {
    std::vector<int16_t> pcm;
    std::vector<uint8_t> stream;
    DecodeStats st;
    char msg[128];

    gen_content(kind, pcm);
    snprintf(msg, sizeof(msg), "encode hard panned (kind %d)", kind);
    CHECK(encode_to_stream(&pcm[0], pcm.size(), 2, AOT_PS, 24000, stream) ==
              0,
          msg);
    CHECK(decode_stream(&stream[0], stream.size(), 2, -1, 0, st) == 0,
          "decode hard panned");
    snprintf(msg, sizeof(msg),
             "decoded samples within [%d, %d] (kind %d, got [%ld, %ld])",
             PCM_OUT_MIN, PCM_OUT_MAX, kind, st.minv, st.maxv);
    CHECK(st.minv >= PCM_OUT_MIN && st.maxv <= PCM_OUT_MAX, msg);
    CHECK(st.bad_channel_frames == 0,
          "channel count constant (2) for boundary content");
  }
}

/* Test 2, white box part: the PS mixing coefficients stay bounded for
   boundary and out-of-range IID/ICC indices, and the mixing accumulation
   saturates instead of wrapping around. */
static void test_ps_mixing(void) {
  HANDLE_PS_DEC h = NULL;
  static PS_DEC_COEFFICIENTS coef;
  static const int iid_cases[] = {-31, -15, -7, 0, 7, 15, 31};
  static const int icc_cases[] = {-1, 0, 3, 7, 31};

  CHECK(CreatePsDec(&h, 1024) == 0, "CreatePsDec");
  if (!h) {
    failures++;
    return;
  }
  h->specificTo.mpeg.pCoef = &coef;
  h->processSlot = 0;
  h->bsData[0].mpeg.aEnvStartStop[0] = 0;
  h->bsData[0].mpeg.aEnvStartStop[1] = 32;

  for (int fine = 0; fine <= 1; fine++) {
    h->bsData[0].mpeg.bFineIidQ = fine;
    for (size_t ii = 0; ii < sizeof(iid_cases) / sizeof(iid_cases[0]); ii++) {
      for (size_t ic = 0; ic < sizeof(icc_cases) / sizeof(icc_cases[0]);
           ic++) {
        memset(&coef, 0, sizeof(coef));
        memset(h->specificTo.mpeg.h11rPrev, 0,
               sizeof(h->specificTo.mpeg.h11rPrev));
        memset(h->specificTo.mpeg.h12rPrev, 0,
               sizeof(h->specificTo.mpeg.h12rPrev));
        memset(h->specificTo.mpeg.h21rPrev, 0,
               sizeof(h->specificTo.mpeg.h21rPrev));
        memset(h->specificTo.mpeg.h22rPrev, 0,
               sizeof(h->specificTo.mpeg.h22rPrev));
        for (int bin = 0; bin < NO_HI_RES_IID_BINS; bin++)
          coef.aaIidIndexMapped[0][bin] = (SCHAR)iid_cases[ii];
        for (int bin = 0; bin < NO_HI_RES_ICC_BINS; bin++)
          coef.aaIccIndexMapped[0][bin] = (SCHAR)icc_cases[ic];

        initSlotBasedRotation(h, 0, 32);

        for (int g = 0; g < NO_IID_GROUPS; g++) {
          CHECK(coef.H11r[g] >= -H_COEF_MAX && coef.H11r[g] <= H_COEF_MAX,
                "H11 bounded");
          CHECK(coef.H12r[g] >= -H_COEF_MAX && coef.H12r[g] <= H_COEF_MAX,
                "H12 bounded");
          CHECK(coef.H21r[g] >= -H_COEF_MAX && coef.H21r[g] <= H_COEF_MAX,
                "H21 bounded");
          CHECK(coef.H22r[g] >= -H_COEF_MAX && coef.H22r[g] <= H_COEF_MAX,
                "H22 bounded");
          CHECK(coef.DeltaH11r[g] >= -H_COEF_MAX &&
                    coef.DeltaH11r[g] <= H_COEF_MAX,
                "DeltaH11 bounded");
          CHECK(coef.DeltaH22r[g] >= -H_COEF_MAX &&
                    coef.DeltaH22r[g] <= H_COEF_MAX,
                "DeltaH22 bounded");
        }
      }
    }
  }

  /* Mixing accumulation: worst case coefficients and full scale inputs
     of equal sign must saturate at the FIXP_DBL bounds instead of
     wrapping around to the opposite sign. */
  for (int g = 0; g < NO_IID_GROUPS; g++) {
    coef.H11r[g] = coef.H21r[g] = (FIXP_DBL)0x60000000; /* 0.75 */
    coef.H12r[g] = coef.H22r[g] = (FIXP_DBL)0;
    coef.DeltaH11r[g] = coef.DeltaH12r[g] = coef.DeltaH21r[g] =
        coef.DeltaH22r[g] = (FIXP_DBL)0;
  }
  {
    FIXP_DBL rl[NO_HYBRID_BANDS], il[NO_HYBRID_BANDS], rr[NO_HYBRID_BANDS],
        ir[NO_HYBRID_BANDS];
    for (int i = 0; i < NO_HYBRID_BANDS; i++) {
      rl[i] = rr[i] = (FIXP_DBL)MAXVAL_DBL;
      il[i] = ir[i] = (FIXP_DBL)0;
    }
    applySlotBasedRotation(h, rl, il, rr, ir);
    for (int i = 0; i < NO_HYBRID_BANDS; i++) {
      CHECK(rl[i] == (FIXP_DBL)MAXVAL_DBL,
            "positive overflow saturates at MAXVAL_DBL");
      CHECK(rr[i] == (FIXP_DBL)0, "zero coefficient stays zero");
    }
    for (int i = 0; i < NO_HYBRID_BANDS; i++) {
      rl[i] = rr[i] = (FIXP_DBL)MINVAL_DBL;
    }
    applySlotBasedRotation(h, rl, il, rr, ir);
    for (int i = 0; i < NO_HYBRID_BANDS; i++) {
      CHECK(rl[i] == (FIXP_DBL)MINVAL_DBL,
            "negative overflow saturates at MINVAL_DBL");
    }
  }

  /* For sums that do not overflow, the saturating accumulation must be
     bit exact with the original fMultAdd(fMultDiv2(a, b), c, d). */
  for (int g = 0; g < NO_IID_GROUPS; g++) {
    coef.H11r[g] = (FIXP_DBL)0x30000000;
    coef.H21r[g] = (FIXP_DBL)0x10000000;
    coef.H12r[g] = coef.H22r[g] = (FIXP_DBL)0;
    coef.DeltaH11r[g] = coef.DeltaH12r[g] = coef.DeltaH21r[g] =
        coef.DeltaH22r[g] = (FIXP_DBL)0;
  }
  {
    FIXP_DBL rl[NO_HYBRID_BANDS], il[NO_HYBRID_BANDS], rr[NO_HYBRID_BANDS],
        ir[NO_HYBRID_BANDS];
    FIXP_DBL expect[NO_HYBRID_BANDS];
    for (int i = 0; i < NO_HYBRID_BANDS; i++) {
      rl[i] = (FIXP_DBL)(0x12345670 + i * 0x11111);
      rr[i] = (FIXP_DBL)(0x0ABCDE00 - i * 0x22222);
      il[i] = ir[i] = (FIXP_DBL)0;
      expect[i] = fMultAdd(fMultDiv2((FIXP_DBL)0x30000000, rl[i]),
                           (FIXP_DBL)0x10000000, rr[i]);
    }
    applySlotBasedRotation(h, rl, il, rr, ir);
    for (int i = 0; i < NO_HYBRID_BANDS; i++) {
      CHECK(rl[i] == expect[i], "bit exact with fMultAdd formulation");
    }
  }

  DeletePsDec(&h);
}

/* Test 3: the energies of the HE-AACv2 and AAC-LC stereo decodings of
   the same content relate within the specified ratio. */
static void test_energy_ratio(void) {
  std::vector<int16_t> pcm;
  std::vector<uint8_t> stream_lc, stream_ps;
  DecodeStats st_lc, st_ps;

  gen_content(0, pcm);
  CHECK(encode_to_stream(&pcm[0], pcm.size(), 2, AOT_AAC_LC, 128000,
                         stream_lc) == 0,
        "encode AAC-LC");
  CHECK(encode_to_stream(&pcm[0], pcm.size(), 2, AOT_PS, 24000, stream_ps) ==
            0,
        "encode HE-AACv2");
  CHECK(decode_stream(&stream_lc[0], stream_lc.size(), 2, -1, 1, st_lc) == 0,
        "decode AAC-LC");
  CHECK(decode_stream(&stream_ps[0], stream_ps.size(), 2, -1, 1, st_ps) == 0,
        "decode HE-AACv2");

  /* Compare the energies over the steady state region (skip the codec
     delay and the flush tails). */
  size_t skip = (size_t)(SAMPLE_RATE / 2) * 2; /* 0.5 s, interleaved */
  size_t n = st_lc.samples.size();
  if (st_ps.samples.size() < n)
    n = st_ps.samples.size();
  CHECK(n > 2 * skip, "enough decoded samples for energy comparison");
  if (n <= 2 * skip) {
    failures++;
    return;
  }
  n -= skip;

  double e_lc = 0.0, e_ps = 0.0;
  for (size_t i = skip; i < n; i++) {
    e_lc += (double)st_lc.samples[i] * st_lc.samples[i];
    e_ps += (double)st_ps.samples[i] * st_ps.samples[i];
  }
  double ratio = e_ps / e_lc;
  fprintf(stderr, "energy ratio HE-AACv2/AAC-LC: %.3f (specified [%.1f, "
          "%.1f])\n", ratio, ENERGY_RATIO_MIN, ENERGY_RATIO_MAX);
  CHECK(ratio >= ENERGY_RATIO_MIN && ratio <= ENERGY_RATIO_MAX,
        "energy ratio within specified bounds");
}

int main(void) {
  test_channel_stability();
  test_boundary_output_range();
  test_ps_mixing();
  test_energy_ratio();

  fprintf(stderr, "%d failures\n", failures);
  return failures ? 1 : 0;
}
