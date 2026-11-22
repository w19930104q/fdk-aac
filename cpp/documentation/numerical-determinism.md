# Numerical determinism of the fixed-point AAC encoder

## Scope and guarantee

With the default build (`-DFDK_BITEXACT=ON`, or `make` via the autotools
makefiles which pass `-DFDK_BITEXACT`), encoding the same PCM input with the
same encoder parameters produces **byte-identical bitstreams on every
supported platform and SIMD path** (generic C, x86, ARM, MIPS, any compiler
optimization level). The guarantee covers the whole fixed-point AAC encoder,
including the MS stereo, intensity stereo (IS) and TNS tool decisions in
`libAACenc`.

The floating-point xHE-AAC encoder (`libfdk-xhe-aac`, USAC) is a separate,
inherently floating-point codebase and is outside this guarantee.

## Root cause being fixed

All encoder decisions operate on 32-bit integer fixed-point data and the
decision comparisons themselves are plain integer comparisons, so they are
already exact. The remaining platform dependence was in the *inputs* of those
decisions:

1. `cpp/libFDK/include/x86/fixpoint_math_x86.h` replaces the integer
   `sqrtFixp()`, `invSqrtNorm2()`, `invFixp()` and `schur_div()`
   implementations with versions built on single-precision `sqrtf()`,
   `frexpf()` and `ldexpf()`. Floating-point rounding differs from the integer
   table/Newton fallbacks (and can itself vary with libm/compiler flags).
   These primitives feed SFB energy loudness (`CalcLdData`), the IS
   correlation/normalized-loudness path in `intensity.cpp`, the TNS LPC/parcor
   path in `aacenc_tns.cpp` and SBR tonality. A 1-LSB difference in an energy
   or correlation value flips an MS/IS/TNS decision on a critical frame, which
   rewrites the whole frame — visible as left/right energy swap when an IS or
   MS mask changes, and as different output bytes across builds.
2. The x86 `fixmul` inline assembly uses 32-bit-only register constraints and
   cannot compile on x86-64; on 64-bit builds the generic INT64 fallback was
   already used. The generic fallback and every arch multiply
   (x86 `imul`, ARM `smull`+`asr`) implement exactly
   `(INT64)a * b >> 32`, so once selected uniformly they are bit-identical.

The fix adds a single build selector. `FDK_BITEXACT` (default) compiles out
the x86 overrides in `fixpoint_math.h`, `fixmul.h`, `abs.h` and `clz.h`, so all
targets execute the identical integer primitives. The generic multiply is
mathematically identical to the ARM/MIPS instruction sequences, hence ARM and
MIPS outputs converge onto the same bytes as well. `-DFDK_BITEXACT=OFF`
restores the previous architecture fast paths for users who explicitly want
them.

## Tie-breaking rules for critical decisions

All decisions use saturated 32-bit fixed-point values and fixed integer
comparisons. Once the shared integer primitives above are used, every critical
condition resolves to one documented side, with no platform-dependent rounding
left in the chain:

| Decision | Location | Exact condition | Result on equality (tie side) |
| --- | --- | --- | --- |
| MS stereo per SFB | `ms_stereo.cpp`, `FDKaacEnc_MsStereoProcessing` | `useMS = (allowMS != 0) && (pnmsLdData > pnlrLdData)` | Strict `>`: on equality (`pnms == pnlr`) MS is **not** used for that SFB; independent L/R coding wins the tie. |
| MS digest ALL vs SOME | `ms_stereo.cpp` | `numMsMaskFalse == 0`, or `numMsMaskFalse < maxSfbPerGroup && numMsMaskFalse < 9` | Strict inequalities; any equality falls through to `SI_MS_MASK_SOME`. A frame containing any IS book always sets `numMsMaskFalse = 9`, so MS_ALL is forbidden together with IS. |
| IS seed (correlation) | `intensity.cpp`, `FDKaacEnc_prepareIntensityDecision` | `fAbs(channelCorr) >= corr_thresh` (`0.95`) | Non-strict `>=`: at exactly the threshold IS **is** seeded. |
| IS blocked near center pan | `intensity.cpp` | `L >= 0.7*R && 0.7*L <= R` | Both non-strict: a ratio exactly at `0.7` blocks IS (MS handles it). |
| IS region expansion | `intensity.cpp`, `FDKaacEnc_finalizeIntensityDecision` | local error `< local_error_thresh/8` **and** total error `< total_error_thresh/64` | Strict `<`: equality keeps the SFB out of the IS region. |
| IS direction deviation | same | `fAbs(isScaleLast - realIsScale) < direction_deviation_thresh` | Strict `<`: at equality the IS block is terminated. |
| IS minimum region/loudness | same | `count < min_is_sfbs` or loudness `< region_min_loudness` rejects | Equality satisfies the minimum, so the region is **kept**. |
| IS in-phase vs out-of-phase | `intensity.cpp` | `lr < 0` | Strict: exactly zero cross-product is treated as **in-phase** IS. |
| TNS high filter activation | `aacenc_tns.cpp` | `predictionGain > threshOn` (e.g. 1437) | Strict `>`: gain exactly at threshold leaves TNS off. |
| TNS low filter activation | `aacenc_tns.cpp` | `predGain > threshOn && predGain < 16000*order`, or `9 < sumSqrCoef < 22*order` | Both strict; equality on either bound leaves the filter off. |
| TNS coefficient index | `aacenc_tns.cpp`, `FDKaacEnc_Search3/Search4` | `parcor > coeffBorders[i]` | Strict `>`: a parcor value exactly on a border takes the lower index. |
| TNS channel synchronization | `aacenc_tns.cpp`, `FDKaacEnc_TnsSync` | `absDiff > 1` or `absDiffSum > 2` aborts sync | Strict: equality still permits synchronization. |
| Band energy max index | `band_nrg.cpp` | `bandEnergyLdData[i] > maxNrgLd` | Strict: the **first** (lowest-index) SFB wins ties. |

Because every operand is an integer produced by the same bit-exact primitives,
the "tie side" above is now a property of the source rather than of the
hardware. No epsilon, hysteresis or randomization is introduced.

## Build switches

* CMake: `-DFDK_BITEXACT=ON` (default) / `-DFDK_BITEXACT=OFF`.
* Autotools: `Makefile.am` adds `-DFDK_BITEXACT` to `AM_CPPFLAGS` by default.
* A quick runtime check of which path is compiled in: a bit-exact
  `libfdk-aac` has no undefined `sqrtf`/`ldexpf` dynamic symbols
  (`nm -D libfdk-aac.so | grep sqrtf` prints nothing).

## Quality impact

The integer primitives are the same implementations already used on 32-bit
x86, all ARM and all MIPS builds; only 64-bit x86 previously took the
floating-point island. The integer `sqrtFixp`/`invSqrtNorm2` use the shipped
lookup tables plus one Newton refinement step, matching the precision the
rest of the fixed-point encoder was designed for. No thresholds, weighting
curves or decision logic were changed, so perceptual quality is not lower than
the integer-path baseline. The acceptance decode check (zero decoder
warnings, preserved L/R energy side) is enforced by
`test/determinism/check_determinism.py`.
