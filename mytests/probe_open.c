/* Probe xHE-AAC encoder open/finalize behavior across bitrate/channel/transport. */
#include <stdio.h>
#include <string.h>
#include "xHEAACEnc.h"

static int try_open(int sr, int channels, int bitrate, int transport, int do_open) {
  IIS_XHEAACENC_CONFIG_INSTANCE_HANDLE cfg = NULL;
  IIS_XHEAACENC_INSTANCE_HANDLE enc = NULL;
  IIS_XHEAACENC_RETURN_CODE ret;
  int rc = -1;

  ret = IIS_xHEAACEnc_Config_Open(&cfg, sr,
                                  channels == 1 ? IIS_XHEAACENC_CHANNELCONFIG_MONO
                                                : IIS_XHEAACENC_CHANNELCONFIG_STEREO);
  if (ret) { printf("Config_Open ret=%d\n", ret); return -1; }

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
  if (ret >= IIS_XHEAACENC_ERROR_FIRST) {
    printf("ch=%d br=%6d tf=%d Finalize ret=%d\n", channels, bitrate, transport, ret);
    rc = ret;
    goto out;
  }
  if (!do_open) { rc = 0; goto out; }

  ret = IIS_xHEAACEnc_Open(&enc, cfg);
  if (ret != IIS_XHEAACENC_NO_ERROR) {
    printf("ch=%d br=%6d tf=%d Open ret=%d\n", channels, bitrate, transport, ret);
    rc = ret;
  } else {
    int fs = 0;
    IIS_xHEAACEnc_GetParam(enc, IIS_XHEAACENC_PARAMETER_FRAMESAMPLES,
                           IIS_XHEAACENC_PARAM_INT, &fs, sizeof(fs));
    rc = 0;
    printf("ch=%d br=%6d tf=%d OK frameSamples=%d\n", channels, bitrate, transport, fs);
    IIS_xHEAACEnc_Delete(enc);
  }

out:
  IIS_xHEAACEnc_Config_Delete(cfg);
  return rc;
}

int main(void) {
  int brs[] = {2000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 32000};
  int i;
  printf("== RAW, finalize+open ==\n");
  for (i = 0; i < (int)(sizeof(brs)/sizeof(brs[0])); i++) {
    try_open(48000, 1, brs[i], IIS_XHEAACENC_TRANSPORTFORMAT_RAW, 1);
  }
  printf("== LATMLOAS mono ==\n");
  for (i = 0; i < (int)(sizeof(brs)/sizeof(brs[0])); i++) {
    try_open(48000, 1, brs[i], IIS_XHEAACENC_TRANSPORTFORMAT_LATMLOAS, 1);
  }
  printf("== RAW stereo ==\n");
  for (i = 0; i < (int)(sizeof(brs)/sizeof(brs[0])); i++) {
    try_open(48000, 2, brs[i], IIS_XHEAACENC_TRANSPORTFORMAT_RAW, 1);
  }
  printf("== LATMLOAS stereo ==\n");
  for (i = 0; i < (int)(sizeof(brs)/sizeof(brs[0])); i++) {
    try_open(48000, 2, brs[i], IIS_XHEAACENC_TRANSPORTFORMAT_LATMLOAS, 1);
  }
  return 0;
}
