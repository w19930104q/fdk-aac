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
 * Encoder delay / trimming consistency test.
 *
 * This test pins down the unified delay/trim contract of the encoder API,
 * identical for all audio object types (AAC-LC, HE-AAC, AAC-LD, AAC-ELD):
 *
 *  - AACENC_InfoStruct::nDelay is the number of leading decoder output
 *    PCM samples per channel that must be discarded to align the decoded
 *    stream with the encoder input timeline (the trim recommendation).
 *  - The reported values are deterministic: two identically configured
 *    encoder instances report the same values, and they match the pinned
 *    reference values in the table below.
 *  - After encoding a fixed sine burst, decoding and trimming nDelay
 *    samples per channel, the burst position deviates from its position
 *    in the original input by at most one frame.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aacenc_lib.h"
#include "aacdecoder_lib.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Fixed test signal: a windowed sine burst at a hardcoded position. */
#define SAMPLE_RATE 48000
#define TOTAL_SAMPLES 24000
#define BURST_POS 10000 /* hardcoded burst position, deliberately not
                           frame aligned for any of the tested AOTs */
#define BURST_LEN 128
#define BURST_AMP 12000

typedef struct {
  int aot;          /* audio object type */
  int eld_sbr;      /* enable SBR for AAC-ELD */
  int bitrate;      /* bitrate in bps */
  UINT frameLength; /* pinned expected values, must be deterministic */
  UINT nDelay;
  UINT nDelayCore;
  const char *name;
} TestConfig;

static const TestConfig configs[] = {
    {2, 0, 64000, 1024, 2048, 2048, "AAC-LC"},       /* AAC-LC */
    {5, 0, 24000, 2048, 5058, 4096, "HE-AAC"},       /* HE-AAC */
    {23, 0, 64000, 512, 512, 512, "AAC-LD"},         /* AAC-LD */
    {39, 0, 64000, 512, 256, 256, "AAC-ELD"},        /* AAC-ELD */
    {39, 1, 32000, 512, 288, 256, "AAC-ELD + SBR"},  /* AAC-ELD with SBR */
};

static void generate_input(int16_t *input) {
  int i;
  for (i = 0; i < TOTAL_SAMPLES; i++)
    input[i] = 0;
  for (i = 0; i < BURST_LEN && BURST_POS + i < TOTAL_SAMPLES; i++) {
    double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (BURST_LEN - 1));
    input[BURST_POS + i] =
        (int16_t)(BURST_AMP * w * sin(2.0 * M_PI * 1000.0 * i / SAMPLE_RATE));
  }
}

/* Open and initialize an encoder with the given fixed configuration and
 * read back the reported info values. */
static int open_and_query(const TestConfig *cfg, HANDLE_AACENCODER *encoder,
                          AACENC_InfoStruct *info) {
  memset(info, 0, sizeof(*info));
  if (aacEncOpen(encoder, 0, 1) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(*encoder, AACENC_AOT, cfg->aot) != AACENC_OK)
    return 1;
  if (cfg->aot == 39 && cfg->eld_sbr) {
    if (aacEncoder_SetParam(*encoder, AACENC_SBR_MODE, 1) != AACENC_OK)
      return 1;
  }
  if (aacEncoder_SetParam(*encoder, AACENC_SAMPLERATE, SAMPLE_RATE) !=
      AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(*encoder, AACENC_CHANNELMODE, MODE_1) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(*encoder, AACENC_CHANNELORDER, 1) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(*encoder, AACENC_BITRATE, cfg->bitrate) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(*encoder, AACENC_TRANSMUX, 0) != AACENC_OK)
    return 1;
  if (aacEncoder_SetParam(*encoder, AACENC_AFTERBURNER, 1) != AACENC_OK)
    return 1;
  if (aacEncEncode(*encoder, NULL, NULL, NULL, NULL) != AACENC_OK)
    return 1;
  if (aacEncInfo(*encoder, info) != AACENC_OK)
    return 1;
  return 0;
}

/* Encode the fixed input signal, decode the bitstream and return the
 * decoded PCM (interleaved). *out_channels and *out_samples (per channel)
 * are set on success. */
static int encode_decode_roundtrip(HANDLE_AACENCODER encoder,
                                   const AACENC_InfoStruct *info,
                                   const int16_t *input, int16_t *decoded,
                                   int *out_channels, int *out_samples) {
  HANDLE_AACDECODER decoder;
  int pos = 0, flushing = 0;
  int decoded_n = 0, channels = 0;
  int ret = 1;

  decoder = aacDecoder_Open(TT_MP4_RAW, 1);
  if (!decoder)
    return 1;
  {
    UCHAR confBuf[64];
    UCHAR *bufArray[] = {confBuf};
    UINT confSize = info->confSize;
    memcpy(confBuf, info->confBuf, sizeof(confBuf));
    if (aacDecoder_ConfigRaw(decoder, bufArray, &confSize) != AAC_DEC_OK)
      goto end;
  }
  aacDecoder_SetParam(decoder, AAC_CONCEAL_METHOD, 1);
  aacDecoder_SetParam(decoder, AAC_PCM_LIMITER_ENABLE, 0);

  while (1) {
    AACENC_BufDesc in_buf = {0}, out_buf = {0};
    AACENC_InArgs in_args = {0};
    AACENC_OutArgs out_args = {0};
    int in_identifier = IN_AUDIO_DATA;
    int in_size, in_elem_size = 2;
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_size, out_elem_size = 1;
    void *in_ptr, *out_ptr;
    uint8_t outbuf[20480];
    int16_t decoder_buffer[2048 * 8];
    AACENC_ERROR err;

    if (!flushing) {
      int chunk = info->frameLength;
      if (pos + chunk > TOTAL_SAMPLES)
        chunk = TOTAL_SAMPLES - pos;
      in_ptr = (void *)(input + pos);
      in_size = chunk * 2;
      in_args.numInSamples = chunk;
      pos += chunk;
      if (pos >= TOTAL_SAMPLES)
        flushing = 1;
    } else {
      in_ptr = NULL;
      in_size = 0;
      in_args.numInSamples = -1;
    }
    in_buf.numBufs = 1;
    in_buf.bufs = &in_ptr;
    in_buf.bufferIdentifiers = &in_identifier;
    in_buf.bufSizes = &in_size;
    in_buf.bufElSizes = &in_elem_size;

    out_ptr = outbuf;
    out_size = sizeof(outbuf);
    out_buf.numBufs = 1;
    out_buf.bufs = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes = &out_size;
    out_buf.bufElSizes = &out_elem_size;

    err = aacEncEncode(encoder, &in_buf, &out_buf, &in_args, &out_args);
    if (err == AACENC_ENCODE_EOF)
      break;
    if (err != AACENC_OK) {
      fprintf(stderr, "Encoding failed\n");
      goto end;
    }
    if (out_args.numOutBytes == 0)
      continue;

    {
      UCHAR *ptr = outbuf;
      UINT size = out_args.numOutBytes;
      AAC_DECODER_ERROR derr;
      do {
        UINT valid, buffer_size;
        valid = buffer_size = size;
        derr = aacDecoder_Fill(decoder, &ptr, &buffer_size, &valid);
        ptr += buffer_size - valid;
        size -= buffer_size - valid;
        if (derr == AAC_DEC_NOT_ENOUGH_BITS)
          continue;
        if (derr != AAC_DEC_OK)
          break;
        derr = aacDecoder_DecodeFrame(decoder, (INT_PCM *)decoder_buffer,
                                      sizeof(decoder_buffer) /
                                          sizeof(INT_PCM),
                                      0);
        if (derr == AAC_DEC_NOT_ENOUGH_BITS)
          continue;
        if (derr != AAC_DEC_OK) {
          fprintf(stderr, "Decoding failed\n");
          goto end;
        }
        {
          CStreamInfo *stream_info = aacDecoder_GetStreamInfo(decoder);
          int i, ch;
          channels = stream_info->numChannels;
          for (i = 0; i < stream_info->frameSize; i++)
            for (ch = 0; ch < channels; ch++)
              decoded[(size_t)(decoded_n + i) * channels + ch] =
                  decoder_buffer[i * channels + ch];
          decoded_n += stream_info->frameSize;
        }
      } while (size > 0);
    }
  }

  *out_channels = channels;
  *out_samples = decoded_n;
  ret = 0;
end:
  aacDecoder_Close(decoder);
  return ret;
}

/* Locate the burst in the trimmed decoder output (channel 0) by
 * correlating with the burst template from the input signal. */
static int find_burst(const int16_t *decoded, int decoded_n, int channels,
                      const int16_t *input) {
  int best_pos = -1;
  double best = -1;
  int i, j;
  for (i = 0; i + BURST_LEN <= decoded_n; i++) {
    double corr = 0;
    for (j = 0; j < BURST_LEN; j++)
      corr += (double)decoded[(size_t)(i + j) * channels] *
              input[BURST_POS + j];
    if (corr > best) {
      best = corr;
      best_pos = i;
    }
  }
  return best_pos;
}

static int test_aot(const TestConfig *cfg) {
  HANDLE_AACENCODER encoder = NULL, encoder2 = NULL;
  AACENC_InfoStruct info, info2;
  int16_t *input = NULL, *decoded = NULL;
  int failures = 0;
  int channels = 0, decoded_n = 0;
  int burst_pos, error_samples;

  input = malloc(sizeof(*input) * TOTAL_SAMPLES);
  decoded = malloc(sizeof(*decoded) * (TOTAL_SAMPLES + 65536) * 8);
  if (!input || !decoded) {
    fprintf(stderr, "Out of memory\n");
    failures++;
    goto end;
  }
  generate_input(input);

  /* The reported delay values must be deterministic: two identically
   * configured encoder instances must report identical values. */
  if (open_and_query(cfg, &encoder, &info)) {
    fprintf(stderr, "%s: unable to initialize the encoder\n", cfg->name);
    failures++;
    goto end;
  }
  if (open_and_query(cfg, &encoder2, &info2)) {
    fprintf(stderr, "%s: unable to initialize the second encoder\n",
            cfg->name);
    failures++;
    goto end;
  }
  if (info.frameLength != info2.frameLength || info.nDelay != info2.nDelay ||
      info.nDelayCore != info2.nDelayCore) {
    fprintf(stderr,
            "%s: nondeterministic delay values: "
            "frameLength %u vs %u, nDelay %u vs %u, nDelayCore %u vs %u\n",
            cfg->name, info.frameLength, info2.frameLength, info.nDelay,
            info2.nDelay, info.nDelayCore, info2.nDelayCore);
    failures++;
  }

  /* The reported values must match the pinned reference values. */
  if (info.frameLength != cfg->frameLength || info.nDelay != cfg->nDelay ||
      info.nDelayCore != cfg->nDelayCore) {
    fprintf(stderr,
            "%s: unexpected delay values: frameLength %u (want %u), "
            "nDelay %u (want %u), nDelayCore %u (want %u)\n",
            cfg->name, info.frameLength, cfg->frameLength, info.nDelay,
            cfg->nDelay, info.nDelayCore, cfg->nDelayCore);
    failures++;
  }

  /* Encode the fixed burst signal, decode, and trim exactly nDelay
   * samples per channel from the decoded output. */
  if (encode_decode_roundtrip(encoder, &info, input, decoded, &channels,
                              &decoded_n)) {
    fprintf(stderr, "%s: encode/decode roundtrip failed\n", cfg->name);
    failures++;
    goto end;
  }
  if (decoded_n <= (int)info.nDelay) {
    fprintf(stderr, "%s: not enough decoded samples to trim\n", cfg->name);
    failures++;
    goto end;
  }
  burst_pos = find_burst(decoded + (size_t)info.nDelay * channels,
                         decoded_n - info.nDelay, channels, input);
  error_samples = burst_pos - BURST_POS;
  if (burst_pos < 0 || abs(error_samples) > (int)info.frameLength) {
    fprintf(stderr,
            "%s: burst position off by %d samples after trimming "
            "(more than one frame of %u samples)\n",
            cfg->name, error_samples, info.frameLength);
    failures++;
  }

  printf("%-14s aot %2d: frameLength %4u, nDelay %5u (%5.2f frames), "
         "nDelayCore %5u, burst error %+d samples -> %s\n",
         cfg->name, cfg->aot, info.frameLength, info.nDelay,
         (double)info.nDelay / info.frameLength, info.nDelayCore,
         error_samples, failures ? "FAIL" : "ok");

end:
  if (encoder)
    aacEncClose(&encoder);
  if (encoder2)
    aacEncClose(&encoder2);
  free(input);
  free(decoded);
  return failures;
}

int main(void) {
  int failures = 0;
  size_t i;
  for (i = 0; i < sizeof(configs) / sizeof(configs[0]); i++)
    failures += test_aot(&configs[i]);
  fprintf(stderr, "%d failures\n", failures);
  return failures;
}
