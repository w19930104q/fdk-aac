/* Probe encoder open across input sample rates and low bitrates. */
#include <stdio.h>
#include "xHEAACEnc.h"

static int try_open(int sr, int channels, int bitrate, int transport) {
  IIS_XHEAACENC_CONFIG_INSTANCE_HANDLE cfg = NULL;
  IIS_XHEAACENC_INSTANCE_HANDLE enc = NULL;
  IIS_XHEAACENC_RETURN_CODE ret;

  ret = IIS_xHEAACEnc_Config_Open(&cfg, sr,
                                  channels == 1 ? IIS_XHEAACENC_CHANNELCONFIG_MONO
                                                : IIS_XHEAACENC_CHANNELCONFIG_STEREO);
  if (ret) { printf("sr=%d ch=%d br=%d Config_Open=%d\n", sr, channels, bitrate, ret); return ret; }

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
    printf("sr=%d ch=%d br=%d Finalize=%d\n", sr, channels, bitrate, ret);
    IIS_xHEAACEnc_Config_Delete(cfg);
    return ret;
  }
  ret = IIS_xHEAACEnc_Open(&enc, cfg);
  if (ret != IIS_XHEAACENC_NO_ERROR) {
    printf("sr=%d ch=%d br=%d Open=%d\n", sr, channels, bitrate, ret);
  } else {
    int fs = 0, outsr = 0;
    IIS_xHEAACEnc_GetParam(enc, IIS_XHEAACENC_PARAMETER_FRAMESAMPLES,
                           IIS_XHEAACENC_PARAM_INT, &fs, sizeof(fs));
    IIS_xHEAACEnc_GetParam(enc, IIS_XHEAACENC_PARAMETER_OUTSAMPLERATE,
                           IIS_XHEAACENC_PARAM_INT, &outsr, sizeof(outsr));
    printf("sr=%d ch=%d br=%d OK frame=%d outsr=%d\n", sr, channels, bitrate, fs, outsr);
    IIS_xHEAACEnc_Delete(enc);
  }
  IIS_xHEAACEnc_Config_Delete(cfg);
  return ret;
}

int main(void) {
  int srs[] = {8000, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
  int brs_m[] = {4000, 6000, 7000, 8000, 12000};
  int brs_s[] = {12000, 14000, 16000};
  int i, j;
  for (i = 0; i < (int)(sizeof(srs)/sizeof(srs[0])); i++)
    for (j = 0; j < (int)(sizeof(brs_m)/sizeof(brs_m[0])); j++)
      try_open(srs[i], 1, brs_m[j], IIS_XHEAACENC_TRANSPORTFORMAT_LATMLOAS);
  for (i = 0; i < (int)(sizeof(srs)/sizeof(srs[0])); i++)
    for (j = 0; j < (int)(sizeof(brs_s)/sizeof(brs_s[0])); j++)
      try_open(srs[i], 2, brs_s[j], IIS_XHEAACENC_TRANSPORTFORMAT_LATMLOAS);
  return 0;
}
