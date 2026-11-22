/* Probe full encode lifecycle (finalize -> open -> EncodeFrame) at low rates. */
#include <stdio.h>
#include <stdlib.h>
#include "xHEAACEnc.h"

#define FRAMES 8

static int run(int sr, int channels, int bitrate, int transport) {
  IIS_XHEAACENC_CONFIG_INSTANCE_HANDLE cfg = NULL;
  IIS_XHEAACENC_INSTANCE_HANDLE enc = NULL;
  IIS_XHEAACENC_RETURN_CODE ret;
  int frameSamples = 0, rc = 0, f;
  float *inBuf = NULL;
  unsigned char *outBuf = NULL;
  unsigned int outSize = 0;
  IIS_XHEAACENC_AUINFO auInfo;

  ret = IIS_xHEAACEnc_Config_Open(&cfg, sr,
                                  channels == 1 ? IIS_XHEAACENC_CHANNELCONFIG_MONO
                                                : IIS_XHEAACENC_CHANNELCONFIG_STEREO);
  if (ret) { printf("br=%d Config_Open ret=%d\n", bitrate, ret); return -1; }
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_BITRATE, bitrate);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_BITRATEMODE,
                                        IIS_XHEAACENC_BITRATEMODE_CBR);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_AOT,
                                        IIS_XHEAACENC_AOT_USAC);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_TRANSPORTFORMAT,
                                        transport);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_STREAMID, 0);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_RAP_OCCURRENCE,
                                        IIS_XHEAACENC_RAP_OCCURRENCE_CONSTANT_INTERVAL);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_RAP_INTERVAL_MS, 1000);
  IIS_xHEAACEnc_Config_AddParamValueFloat(cfg, IIS_XHEAACENC_PARAMETER_LIVE_LOUDNESS_LEVEL,
                                          -23.0f);
  ret = IIS_xHEAACEnc_Config_Finalize(cfg);
  if (ret) { printf("br=%d Finalize ret=%d\n", bitrate, ret); rc = -1; goto out_cfg; }

  ret = IIS_xHEAACEnc_Open(&enc, cfg);
  if (ret) { printf("br=%d Open ret=%d\n", bitrate, ret); rc = -1; goto out_cfg; }

  IIS_xHEAACEnc_GetParam(enc, IIS_XHEAACENC_PARAMETER_FRAMESAMPLES,
                         IIS_XHEAACENC_PARAM_INT, &frameSamples, sizeof(frameSamples));
  inBuf = calloc((size_t)frameSamples * (size_t)channels, sizeof(float));
  outBuf = malloc(64 * 1024);
  printf("br=%d opened frameSamples=%d\n", bitrate, frameSamples);

  for (f = 0; f < FRAMES; f++) {
    ret = IIS_xHEAACEnc_EncodeFrame(enc, inBuf, frameSamples * channels,
                                    outBuf, &outSize, 64 * 1024, &auInfo);
    if (ret) {
      printf("br=%d frame %d EncodeFrame ret=%d outSize=%u\n", bitrate, f, ret, outSize);
      rc = -1;
      break;
    }
  }
  if (rc == 0) printf("br=%d %d frames OK (last outSize=%u)\n", bitrate, FRAMES, outSize);
  free(inBuf);
  free(outBuf);
  IIS_xHEAACEnc_Delete(enc);
out_cfg:
  IIS_xHEAACEnc_Config_Delete(cfg);
  return rc;
}

int main(void) {
  int brs[] = {4000, 5000, 6000};
  int i;
  for (i = 0; i < (int)(sizeof(brs) / sizeof(brs[0])); i++) {
    printf("== RAW mono %d ==\n", brs[i]);
    run(48000, 1, brs[i], IIS_XHEAACENC_TRANSPORTFORMAT_RAW);
    printf("== LATMLOAS mono %d ==\n", brs[i]);
    run(48000, 1, brs[i], IIS_XHEAACENC_TRANSPORTFORMAT_LATMLOAS);
  }
  return 0;
}
