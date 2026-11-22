/* Probe the documented error codes returned on impossible open requests.
 *
 * The xHE-AAC low-bitrate extension guarantees that unsupported
 * configurations fail with a documented IIS_XHEAACENC_RETURN_CODE rather
 * than crashing. This program exercises the boundary:
 *   - 2 kbit/s mono is below the lowest configuration-table row and must be
 *     rejected at Finalize with IIS_XHEAACENC_ERROR_PARAM_BITRATE (200002)
 *     or IIS_XHEAACENC_ERROR_PARAMCOMB_BASE (300002);
 *   - a non-1-kbit/s-aligned rate (500 bit/s) must be rejected with
 *     IIS_XHEAACENC_ERROR_PARAM_BITRATE (200002).
 *
 * Exit code 0 means every rejected request returned a documented, expected
 * code and no call crashed. Any other outcome exits non-zero.
 */
#include <stdio.h>
#include "xHEAACEnc.h"

#define EXPECT_PARAM_BITRATE 200002
#define EXPECT_PARAMCOMB_BASE 300002

static int finalize_only(int sr, int channels, int bitrate) {
  IIS_XHEAACENC_CONFIG_INSTANCE_HANDLE cfg = NULL;
  IIS_XHEAACENC_RETURN_CODE ret;

  ret = IIS_xHEAACEnc_Config_Open(&cfg, sr,
                                  channels == 1 ? IIS_XHEAACENC_CHANNELCONFIG_MONO
                                                : IIS_XHEAACENC_CHANNELCONFIG_STEREO);
  if (ret) {
    fprintf(stderr, "Config_Open unexpectedly failed: %d\n", ret);
    return -1;
  }
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
  IIS_xHEAACEnc_Config_AddParamValueFloat(cfg, IIS_XHEAACENC_PARAMETER_LIVE_LOUDNESS_LEVEL,
                                          -23.0f);
  ret = IIS_xHEAACEnc_Config_Finalize(cfg);
  IIS_xHEAACEnc_Config_Delete(cfg);
  return (int)ret;
}

int main(void) {
  struct {
    int bitrate;
    int expected;
    const char *desc;
  } cases[] = {
      {2000, EXPECT_PARAM_BITRATE, "2 kbit/s mono below supported floor"},
      {500, EXPECT_PARAM_BITRATE, "500 bit/s not 1 kbit/s aligned"},
  };
  int i, failures = 0;

  for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
    int ret = finalize_only(48000, 1, cases[i].bitrate);
    if (ret == cases[i].expected || ret == EXPECT_PARAMCOMB_BASE) {
      printf("PASS %s: documented error %d\n", cases[i].desc, ret);
    } else {
      printf("FAIL %s: got %d, expected %d or %d\n", cases[i].desc, ret,
             cases[i].expected, EXPECT_PARAMCOMB_BASE);
      failures++;
    }
  }
  return failures ? 1 : 0;
}
