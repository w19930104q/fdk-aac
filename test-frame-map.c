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
 * Test for the aac_frame_map per-frame metadata channel (aac_frame_map.h).
 *
 * Verifies, for AAC-LD (AOT 23):
 *  a) with a fixed, hardcoded non-uniform input chunking, the input sample
 *     interval reported for every encoded frame matches the expectation
 *     computed independently from the ISO/IEC 14496-3 frame length formula
 *     (512 samples per frame for AOT 23);
 *  b) the frame mapping queried on the decoder side matches the encoder
 *     side, frame for frame;
 *  c) the end-of-stream flush path (aacFrameMap_Finalize) maps trailing
 *     zero-padded frames to consecutive intervals;
 *  d) the AOT frame length formula and error paths of the module itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "aacenc_lib.h"
#include "aacdecoder_lib.h"
#include "aac_frame_map.h"

#define SAMPLE_RATE 44100
#define CHANNELS 1
#define BITRATE 64000
#define AOT 23 /* ER AAC LD */

#define MAX_FRAMES 32
#define MAX_FRAME_BYTES 8192

static int failures = 0;

#define CHECK(cond, msg)                                  \
	do {                                                  \
		if (!(cond)) {                                    \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", msg,    \
					__FILE__, __LINE__);                  \
			failures++;                                   \
		}                                                 \
	} while (0)

/* Hardcoded non-uniform input chunk sizes (samples per channel).
 * Sum: 4096 = 8 * 512. */
static const int chunks_main[] = {1, 511, 1024, 13, 499, 512, 700, 324, 256, 256};
#define NUM_MAIN_CHUNKS (sizeof(chunks_main) / sizeof(chunks_main[0]))
#define MAIN_TOTAL_SAMPLES 4096
#define MAIN_FRAMES 8

/* Extra tail for the flush test: 100 samples -> 4196 total, i.e. 8 full
 * frames plus a partial one. */
#define TAIL_SAMPLES 100

struct encoded_stream {
	uint8_t data[MAX_FRAMES][MAX_FRAME_BYTES];
	int size[MAX_FRAMES];
	int num_frames;
};

static int16_t gen_sample(int index) {
	/* Deterministic sawtooth-ish test signal. */
	return (int16_t)(((index * 265) % 20000) - 10000);
}

static int set_param(HANDLE_AACENCODER enc, AACENC_PARAM param, UINT value, const char *name) {
	if (aacEncoder_SetParam(enc, param, value) != AACENC_OK) {
		fprintf(stderr, "FAIL: unable to set %s\n", name);
		failures++;
		return 0;
	}
	return 1;
}

static int open_ld_encoder(HANDLE_AACENCODER *enc, AACENC_InfoStruct *info) {
	if (aacEncOpen(enc, 0, CHANNELS) != AACENC_OK) {
		fprintf(stderr, "FAIL: unable to open encoder\n");
		failures++;
		return 0;
	}
	if (!set_param(*enc, AACENC_AOT, AOT, "AOT") ||
		!set_param(*enc, AACENC_SAMPLERATE, SAMPLE_RATE, "samplerate") ||
		!set_param(*enc, AACENC_CHANNELMODE, MODE_1, "channel mode") ||
		!set_param(*enc, AACENC_CHANNELORDER, 1, "channel order") ||
		!set_param(*enc, AACENC_BITRATE, BITRATE, "bitrate") ||
		!set_param(*enc, AACENC_TRANSMUX, 0, "raw transport") ||
		!set_param(*enc, AACENC_AFTERBURNER, 1, "afterburner")) {
		aacEncClose(enc);
		return 0;
	}
	if (aacEncEncode(*enc, NULL, NULL, NULL, NULL) != AACENC_OK) {
		fprintf(stderr, "FAIL: unable to initialize the encoder\n");
		failures++;
		aacEncClose(enc);
		return 0;
	}
	if (aacEncInfo(*enc, info) != AACENC_OK) {
		fprintf(stderr, "FAIL: unable to get encoder info\n");
		failures++;
		aacEncClose(enc);
		return 0;
	}
	return 1;
}

/*
 * Feed the given chunks to the encoder, honoring partial consumption, until
 * all chunks are consumed. If flush is set, continue with numInSamples=-1
 * until the encoder signals EOF. Each produced access unit is mapped
 * through the frame map and stored in *stream. Returns the number of
 * encoded frames, or -1 on error.
 */
static int encode_chunks(HANDLE_AACENCODER enc, HANDLE_AAC_FRAME_MAP map,
						 const int *chunks, int num_chunks, int flush,
						 AAC_FRAME_INTERVAL *intervals,
						 struct encoded_stream *stream) {
	int chunk_idx = 0, chunk_off = 0;
	int fed_total = 0;
	int num_frames = 0;
	int guard = 0;
	int16_t in_pcm[4096];
	uint8_t outbuf[MAX_FRAME_BYTES];

	stream->num_frames = 0;

	while (guard++ < 10000) {
		AACENC_BufDesc in_buf = {0}, out_buf = {0};
		AACENC_InArgs in_args = {0};
		AACENC_OutArgs out_args = {0};
		int in_identifier = IN_AUDIO_DATA;
		int out_identifier = OUT_BITSTREAM_DATA;
		int in_size, in_elem_size, out_size, out_elem_size;
		void *in_ptr, *out_ptr;
		int offer = 0, i;
		AACENC_ERROR err;

		if (chunk_idx < num_chunks) {
			offer = chunks[chunk_idx] - chunk_off;
			for (i = 0; i < offer; i++)
				in_pcm[i] = gen_sample(fed_total + i);
			in_args.numInSamples = offer * CHANNELS;
		} else if (flush) {
			in_args.numInSamples = -1;
		} else {
			break;
		}

		in_ptr = in_pcm;
		in_size = offer * CHANNELS * (int)sizeof(int16_t);
		in_elem_size = sizeof(int16_t);
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

		err = aacEncEncode(enc, &in_buf, &out_buf, &in_args, &out_args);
		if (err == AACENC_ENCODE_EOF)
			break;
		if (err != AACENC_OK) {
			fprintf(stderr, "FAIL: encoding failed with error %d\n", err);
			failures++;
			return -1;
		}

		if (out_args.numInSamples > 0) {
			int consumed = out_args.numInSamples / CHANNELS;
			if (aacFrameMap_FeedSamples(map, consumed) != AAC_FRAME_MAP_OK) {
				fprintf(stderr, "FAIL: aacFrameMap_FeedSamples\n");
				failures++;
				return -1;
			}
			fed_total += consumed;
			chunk_off += consumed;
			if (chunk_off == chunks[chunk_idx]) {
				chunk_idx++;
				chunk_off = 0;
			}
		}

		if (out_args.numOutBytes > 0) {
			if (num_frames >= MAX_FRAMES) {
				fprintf(stderr, "FAIL: too many frames\n");
				failures++;
				return -1;
			}
			if (aacFrameMap_NextFrame(map, &intervals[num_frames]) != AAC_FRAME_MAP_OK) {
				fprintf(stderr, "FAIL: aacFrameMap_NextFrame for frame %d\n", num_frames);
				failures++;
				return -1;
			}
			if (out_args.numOutBytes > MAX_FRAME_BYTES) {
				fprintf(stderr, "FAIL: oversized frame\n");
				failures++;
				return -1;
			}
			memcpy(stream->data[num_frames], outbuf, out_args.numOutBytes);
			stream->size[num_frames] = out_args.numOutBytes;
			num_frames++;
		}
	}
	stream->num_frames = num_frames;
	return num_frames;
}

/* Acceptance (a): non-uniform input chunking; per-frame intervals must
 * match the AOT 23 frame length formula applied independently. */
static void test_encoder_intervals(AAC_FRAME_INTERVAL *intervals,
								   struct encoded_stream *stream,
								   AACENC_InfoStruct *enc_info) {
	HANDLE_AACENCODER enc;
	HANDLE_AAC_FRAME_MAP map;
	UINT formula_len = aacFrameMap_GetAotFrameLength(AOT, 0);
	/* Independent expectation: ISO/IEC 14496-3 defines 512 samples per
	 * frame for ER AAC LD (AOT 23). */
	const UINT expected_len = 512;
	int num_frames, k;
	AAC_FRAME_INTERVAL iv;

	fprintf(stderr, "Test: encoder-side frame intervals, non-uniform chunks\n");

	CHECK(formula_len == expected_len,
		  "AOT 23 frame length formula must yield 512");
	if (!open_ld_encoder(&enc, enc_info))
		return;
	CHECK(enc_info->frameLength == expected_len,
		  "encoder frameLength must be 512 for AOT 23");

	if (aacFrameMap_OpenForEncoder(&map, enc) != AAC_FRAME_MAP_OK) {
		fprintf(stderr, "FAIL: aacFrameMap_OpenForEncoder\n");
		failures++;
		aacEncClose(&enc);
		return;
	}
	CHECK(aacFrameMap_GetFrameLength(map) == expected_len,
		  "frame map must adopt the encoder frame length");

	/* Before any input is registered, mapping a frame must fail. */
	CHECK(aacFrameMap_NextFrame(map, &iv) == AAC_FRAME_MAP_INSUFFICIENT_INPUT,
		  "NextFrame without fed input must report INSUFFICIENT_INPUT");
	CHECK(aacFrameMap_GetFramesMapped(map) == 0,
		  "failed NextFrame must not advance the frame counter");

	num_frames = encode_chunks(enc, map, chunks_main, NUM_MAIN_CHUNKS, 0,
							   intervals, stream);
	CHECK(num_frames == MAIN_FRAMES, "unexpected number of encoded frames");
	CHECK(aacFrameMap_GetSamplesFed(map) == MAIN_TOTAL_SAMPLES,
		  "all fed samples must be registered");
	CHECK(aacFrameMap_GetFramesMapped(map) == (UINT64)num_frames,
		  "frame counter must match the number of encoded frames");

	for (k = 0; k < num_frames; k++) {
		/* Independent expectation per ISO/IEC 14496-3: frame k covers
		 * [k*512, (k+1)*512) input samples per channel. */
		UINT64 expect_first = (UINT64)k * expected_len;
		if (intervals[k].firstSample != expect_first ||
			intervals[k].frameLength != expected_len) {
			fprintf(stderr,
					"FAIL: frame %d interval {%llu, %u}, expected {%llu, %u}\n",
					k, (unsigned long long)intervals[k].firstSample,
					intervals[k].frameLength,
					(unsigned long long)expect_first, expected_len);
			failures++;
		}
		/* Random-access query must agree with the sequential one. */
		if (aacFrameMap_GetInterval(map, k, &iv) != AAC_FRAME_MAP_OK ||
			iv.firstSample != intervals[k].firstSample ||
			iv.frameLength != intervals[k].frameLength) {
			fprintf(stderr, "FAIL: GetInterval mismatch for frame %d\n", k);
			failures++;
		}
	}

	aacFrameMap_Close(&map);
	aacEncClose(&enc);
}

/* Acceptance (b): the mapping queried on the decoder side must match the
 * encoder side frame for frame. */
static void test_decoder_intervals(const AAC_FRAME_INTERVAL *enc_intervals,
								   const struct encoded_stream *stream,
								   const AACENC_InfoStruct *enc_info) {
	HANDLE_AACDECODER dec;
	HANDLE_AAC_FRAME_MAP map = NULL;
	CStreamInfo *si;
	int k;
	int16_t dec_pcm[2048 * 8];

	fprintf(stderr, "Test: decoder-side frame intervals match encoder\n");

	dec = aacDecoder_Open(TT_MP4_RAW, 1);
	if (!dec) {
		fprintf(stderr, "FAIL: unable to open decoder\n");
		failures++;
		return;
	}
	{
		UCHAR *conf[] = {enc_info->confBuf};
		if (aacDecoder_ConfigRaw(dec, conf, &enc_info->confSize) != AAC_DEC_OK) {
			fprintf(stderr, "FAIL: unable to configure decoder\n");
			failures++;
			aacDecoder_Close(dec);
			return;
		}
	}

	for (k = 0; k < stream->num_frames; k++) {
		UCHAR *ptr = (UCHAR *)stream->data[k];
		UINT bytes = stream->size[k];
		UINT valid = bytes;
		AAC_DECODER_ERROR err;
		AAC_FRAME_INTERVAL iv;

		err = aacDecoder_Fill(dec, &ptr, &bytes, &valid);
		if (err != AAC_DEC_OK) {
			fprintf(stderr, "FAIL: aacDecoder_Fill frame %d\n", k);
			failures++;
			break;
		}
		err = aacDecoder_DecodeFrame(dec, (INT_PCM *)dec_pcm,
									 sizeof(dec_pcm) / sizeof(dec_pcm[0]), 0);
		if (err != AAC_DEC_OK) {
			fprintf(stderr, "FAIL: aacDecoder_DecodeFrame frame %d, err %d\n", k, err);
			failures++;
			break;
		}

		if (map == NULL) {
			/* Open the decoder-side map once the stream info is available. */
			si = aacDecoder_GetStreamInfo(dec);
			CHECK(si != NULL && si->frameSize == 512,
				  "decoder frameSize must be 512 for AOT 23");
			if (aacFrameMap_OpenForDecoder(&map, dec) != AAC_FRAME_MAP_OK) {
				fprintf(stderr, "FAIL: aacFrameMap_OpenForDecoder\n");
				failures++;
				break;
			}
			CHECK(aacFrameMap_GetFrameLength(map) ==
					  aacFrameMap_GetAotFrameLength(AOT, 0),
				  "decoder map frame length must match the AOT 23 formula");
		}

		if (aacFrameMap_NextFrame(map, &iv) != AAC_FRAME_MAP_OK) {
			fprintf(stderr, "FAIL: decoder NextFrame frame %d\n", k);
			failures++;
			break;
		}
		if (iv.firstSample != enc_intervals[k].firstSample ||
			iv.frameLength != enc_intervals[k].frameLength) {
			fprintf(stderr,
					"FAIL: decoder frame %d interval {%llu, %u} != encoder {%llu, %u}\n",
					k, (unsigned long long)iv.firstSample, iv.frameLength,
					(unsigned long long)enc_intervals[k].firstSample,
					enc_intervals[k].frameLength);
			failures++;
		}
	}

	if (map != NULL) {
		CHECK(aacFrameMap_GetFramesMapped(map) == (UINT64)stream->num_frames,
			  "decoder must map as many frames as the encoder produced");
		aacFrameMap_Close(&map);
	}
	aacDecoder_Close(dec);
}

/* Flush path: after Finalize, trailing zero-padded frames map to
 * consecutive intervals beyond the real input. */
static void test_flush(void) {
	HANDLE_AACENCODER enc;
	HANDLE_AAC_FRAME_MAP map;
	AACENC_InfoStruct info;
	AAC_FRAME_INTERVAL intervals[MAX_FRAMES];
	struct encoded_stream stream;
	/* chunks_main plus a 100-sample tail: 4196 samples total. */
	int chunks[NUM_MAIN_CHUNKS + 1];
	int num_frames, k;
	UINT formula_len = aacFrameMap_GetAotFrameLength(AOT, 0);

	fprintf(stderr, "Test: finalize/flush maps trailing frames\n");

	memcpy(chunks, chunks_main, sizeof(chunks_main));
	chunks[NUM_MAIN_CHUNKS] = TAIL_SAMPLES;

	if (!open_ld_encoder(&enc, &info))
		return;
	if (aacFrameMap_OpenForEncoder(&map, enc) != AAC_FRAME_MAP_OK) {
		fprintf(stderr, "FAIL: aacFrameMap_OpenForEncoder\n");
		failures++;
		aacEncClose(&enc);
		return;
	}

	/* Feed all real input first (no flush yet). */
	num_frames = encode_chunks(enc, map, chunks, NUM_MAIN_CHUNKS + 1, 0,
							   intervals, &stream);
	CHECK(num_frames == MAIN_FRAMES,
		  "partial tail alone must not produce a frame yet");
	CHECK(aacFrameMap_GetSamplesFed(map) == MAIN_TOTAL_SAMPLES + TAIL_SAMPLES,
		  "fed sample count mismatch");

	/* The pending partial frame cannot be mapped before finalization. */
	{
		AAC_FRAME_INTERVAL iv;
		CHECK(aacFrameMap_NextFrame(map, &iv) == AAC_FRAME_MAP_INSUFFICIENT_INPUT,
			  "partial frame before Finalize must report INSUFFICIENT_INPUT");
	}

	CHECK(aacFrameMap_Finalize(map) == AAC_FRAME_MAP_OK, "Finalize failed");
	CHECK(aacFrameMap_FeedSamples(map, 1) == AAC_FRAME_MAP_INVALID_CALL,
		  "FeedSamples after Finalize must be rejected");

	/* Flush the encoder; trailing (zero-padded) frames get consecutive
	 * intervals following the last real input sample. */
	{
		AAC_FRAME_INTERVAL *tail = intervals + num_frames;
		struct encoded_stream flush_stream;
		/* No new input chunks; only flush the encoder. */
		int extra = encode_chunks(enc, map, chunks, 0, 1,
								  tail, &flush_stream);
		CHECK(extra >= 1, "flush must produce at least the final partial frame");
		num_frames += extra;
	}

	for (k = 0; k < num_frames; k++) {
		UINT64 expect_first = (UINT64)k * formula_len;
		if (intervals[k].firstSample != expect_first ||
			intervals[k].frameLength != formula_len) {
			fprintf(stderr,
					"FAIL: flush frame %d interval {%llu, %u}, expected {%llu, %u}\n",
					k, (unsigned long long)intervals[k].firstSample,
					intervals[k].frameLength,
					(unsigned long long)expect_first, formula_len);
			failures++;
		}
	}
	/* The frame holding the 100 tail samples starts right after the 8 full
	 * frames. */
	CHECK(intervals[MAIN_FRAMES].firstSample == MAIN_TOTAL_SAMPLES,
		  "final partial frame must start at the first tail sample");

	aacFrameMap_Close(&map);
	aacEncClose(&enc);
}

/* Unit tests for the frame length formula and argument checking. */
static void test_formula_and_errors(void) {
	HANDLE_AAC_FRAME_MAP map = NULL;
	AAC_FRAME_INTERVAL iv;

	fprintf(stderr, "Test: AOT frame length formula and error paths\n");

	/* ISO/IEC 14496-3 frame lengths. */
	CHECK(aacFrameMap_GetAotFrameLength(2, 0) == 1024, "AAC-LC default");
	CHECK(aacFrameMap_GetAotFrameLength(2, 960) == 960, "AAC-LC 960 granule");
	CHECK(aacFrameMap_GetAotFrameLength(23, 0) == 512, "AAC-LD default");
	CHECK(aacFrameMap_GetAotFrameLength(23, 480) == 480, "AAC-LD 480 granule");
	CHECK(aacFrameMap_GetAotFrameLength(39, 0) == 512, "AAC-ELD default");
	CHECK(aacFrameMap_GetAotFrameLength(39, 480) == 480, "AAC-ELD 480 granule");
	CHECK(aacFrameMap_GetAotFrameLength(23, 960) == 0, "invalid granule for LD");
	CHECK(aacFrameMap_GetAotFrameLength(2, 480) == 0, "invalid granule for LC");
	CHECK(aacFrameMap_GetAotFrameLength(5, 0) == 0, "SBR not covered by formula");
	CHECK(aacFrameMap_GetAotFrameLength(999, 0) == 0, "unknown AOT");

	/* Argument checking. */
	CHECK(aacFrameMap_Open(NULL, 512) == AAC_FRAME_MAP_INVALID_HANDLE,
		  "Open with NULL handle pointer");
	CHECK(aacFrameMap_Open(&map, 0) == AAC_FRAME_MAP_INVALID_ARGUMENT,
		  "Open with zero frame length");
	CHECK(map == NULL, "failed Open must not set the handle");
	CHECK(aacFrameMap_OpenForEncoder(&map, NULL) == AAC_FRAME_MAP_INVALID_ARGUMENT,
		  "OpenForEncoder with NULL encoder");
	CHECK(aacFrameMap_OpenForDecoder(&map, NULL) == AAC_FRAME_MAP_INVALID_ARGUMENT,
		  "OpenForDecoder with NULL decoder");

	CHECK(aacFrameMap_Open(&map, 512) == AAC_FRAME_MAP_OK, "Open failed");
	CHECK(aacFrameMap_GetInterval(map, 3, &iv) == AAC_FRAME_MAP_OK &&
			  iv.firstSample == 1536 && iv.frameLength == 512,
		  "GetInterval for frame 3");
	CHECK(aacFrameMap_GetInterval(NULL, 0, &iv) == AAC_FRAME_MAP_INVALID_HANDLE,
		  "GetInterval with NULL handle");
	CHECK(aacFrameMap_NextFrame(map, NULL) == AAC_FRAME_MAP_INVALID_HANDLE,
		  "NextFrame with NULL output");
	aacFrameMap_Close(&map);
	CHECK(map == NULL, "Close must clear the handle");
	aacFrameMap_Close(NULL); /* must be a no-op */
}

int main(void) {
	AAC_FRAME_INTERVAL intervals[MAX_FRAMES];
	struct encoded_stream stream;
	AACENC_InfoStruct enc_info;

	test_formula_and_errors();
	test_encoder_intervals(intervals, &stream, &enc_info);
	if (stream.num_frames > 0)
		test_decoder_intervals(intervals, &stream, &enc_info);
	test_flush();

	if (failures == 0)
		printf("All frame map tests passed\n");
	else
		fprintf(stderr, "%d failures\n", failures);
	return failures ? 1 : 0;
}
