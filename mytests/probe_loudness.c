/* Probe low-rate encode with/without live loudness, correct SAMPLES_NEXT handling. */
#include <stdio.h>
#include <stdlib.h>
#include "xHEAACEnc.h"

#define FRAMES 12

static int run(int bitrate, int with_loudness) {
  IIS_XHEAACENC_CONFIG_INSTANCE_HANDLE cfg = NULL;
  IIS_XHEAACENC_INSTANCE_HANDLE enc = NULL;
  IIS_XHEAACENC_RETURN_CODE ret;
  int rc = 0, f;
  float *inBuf = NULL;
  unsigned char outBuf[20480];
  unsigned int outSize = 0;
  IIS_XHEAACENC_AUINFO auInfo;

  ret = IIS_xHEAACEnc_Config_Open(&cfg, 48000, IIS_XHEAACENC_CHANNELCONFIG_MONO);
  if (ret) { printf("br=%d Config_Open ret=%d\n", bitrate, ret); return -1; }
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_BITRATE, bitrate);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_BITRATEMODE,
                                        IIS_XHEAACENC_BITRATEMODE_CBR);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_AOT,
                                        IIS_XHEAACENC_AOT_USAC);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_TRANSPORTFORMAT,
                                        IIS_XHEAACENC_TRANSPORTFORMAT_LATMLOAS);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_STREAMID, 0);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_RAP_OCCURRENCE,
                                        IIS_XHEAACENC_RAP_OCCURRENCE_CONSTANT_INTERVAL);
  IIS_xHEAACEnc_Config_AddParamValueInt(cfg, IIS_XHEAACENC_PARAMETER_RAP_INTERVAL_MS, 1000);
  if (with_loudness) {
    IIS_xHEAACEnc_Config_AddParamValueFloat(cfg,
                                            IIS_XHEAACENC_PARAMETER_LIVE_LOUDNESS_LEVEL,
                                            -23.0f);
  }
  ret = IIS_xHEAACEnc_Config_Finalize(cfg);
  if (ret) { printf("br=%d loud=%d Finalize ret=%d\n", bitrate, with_loudness, ret);
              rc = -1; goto out_cfg; }

  ret = IIS_xHEAACEnc_Open(&enc, cfg);
  if (ret) { printf("br=%d loud=%d Open ret=%d\n", bitrate, with_loudness, ret);
              rc = -1; goto out_cfg; }

  size_t inCap = 0; inBuf = NULL;
  for (f = 0; f < FRAMES; f++) {
    int samplesNext = 0, i;
    ret = IIS_xHEAACEnc_GetParam(enc, IIS_XHEAACENC_PARAMETER_SAMPLES_NEXT,
                           IIS_XHEAACENC_PARAM_INT, &samplesNext, sizeof(samplesNext));
    if (ret) { printf("br=%d GetParam ret=%d\n", bitrate, ret); rc=-1; break; }
    if ((size_t)samplesNext > inCap) {
      inCap = (size_t)samplesNext;
      inBuf = realloc(inBuf, inCap * sizeof(float));
    }
    /* Non-zero low-level tone so the signal is not digital silence. */
    for (i = 0; i < samplesNext; i++)
      inBuf[i] = 0.01f * (float)((i + f) & 0xff) / 255.0f;
    ret = IIS_xHEAACEnc_EncodeFrame(enc, inBuf, samplesNext, outBuf, &outSize,
                                    sizeof(outBuf), &auInfo);
    if (ret) {
      printf("br=%d loud=%d frame %d EncodeFrame ret=%d (outSize=%u)\n",
             bitrate, with_loudness, f, ret, outSize);
      rc = -1;
      break;
    }
  }
  if (rc == 0) printf("br=%d loud=%d: %d frames OK\n", bitrate, with_loudness, FRAMES);
  free(inBuf);
  IIS_xHEAACEnc_Delete(enc);
out_cfg:
  IIS_xHEAACEnc_Config_Delete(cfg);
  return rc;
}

int main(void) {
  int brs[] = {4000, 5000, 6000, 7000};
  int i;
  for (i = 0; i < (int)(sizeof(brs) / sizeof(brs[0])); i++) {
    run(brs[i], 1);
    run(brs[i], 0);
  }
  return 0;
}
