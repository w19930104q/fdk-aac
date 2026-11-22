#!/usr/bin/env python3
"""Cross-SIMD-path determinism acceptance test for the fdk-aac encoder.

Runs a fixed stereo sample matrix through two independently configured encoder
builds (the project's "SIMD" build with architecture code enabled and a pure
generic-C build), then enforces:

  a) bit-exact encoder output: every produced .aac has a pinned SHA-256 and
     the two builds emit byte-identical files;
  b) every stream decodes with zero decoder diagnostics (warnings/errors) and
     the decoded L/R channel energy ratio stays on the same side as the input
     (pinned per sample, with a fixed tolerance);
  c) this script is wired into the project test target so an existing-test
     regression fails the build.

The pinned values live in manifest.json next to this file. Regenerate them
deliberately (only after a reviewed algorithm change) with --update.

Usage:
  check_determinism.py --build-a DIR --build-b DIR [--samples DIR]
                       [--media DIR] [--update]
DIR must contain the aac-enc and aac-dec executables (or share libs next to
them; LD_LIBRARY_PATH is set automatically).
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "manifest.json")

# (wav name, AOT, afterburner, bitrate, adts) — fixed test matrix.
MATRIX = [
    ("s01_near_mono.wav", 2, 1, 48000),
    ("s02_antiphase.wav", 2, 1, 48000),
    ("s03_pan_sweep.wav", 2, 1, 48000),
    ("s04_corr_noise.wav", 2, 1, 48000),
    ("s05_ratio_threshold.wav", 2, 1, 48000),
    ("s06_tonal_transient.wav", 2, 1, 48000),
    ("s07_lsb_dualmono.wav", 2, 1, 48000),
    ("s08_multisine.wav", 2, 1, 48000),
    ("s03_pan_sweep.wav", 2, 1, 96000),
    ("s05_ratio_threshold.wav", 2, 1, 96000),
    ("s07_lsb_dualmono.wav", 2, 0, 48000),
    ("s08_multisine.wav", 5, 1, 48000),
    # HE-AAC v2 (parametric stereo): the most IS-sensitive AOT. AAC-LD (23)
    # is excluded because the aac-enc CLI hardcodes ADTS, which AAC-LD cannot
    # use; the existing test-encode-decode binary covers AOT 23 over RAW.
    ("s04_corr_noise.wav", 29, 1, 32000),
    ("s03_pan_sweep.wav", 29, 1, 40000),
]

# Encoder delay (per channel, samples) is trimmed from the decoder output
# before energy comparison; LC/HE use 2048, the script also tolerates the
# actual shorter length. Fixed tolerance for the pinned energy-side ratio.
RATIO_TOL = 0.10


def run(cmd, libdir):
    env = dict(os.environ)
    lib = os.path.abspath(libdir)
    env["LD_LIBRARY_PATH"] = lib + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    p = subprocess.run(cmd, cwd=lib, env=env, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def case_id(wav, aot, ab, br):
    return f"{os.path.splitext(wav)[0]}-aot{a_t(aot)}-ab{ab}-{br}"


def a_t(aot):
    return aot


def encode(build, wav_path, aot, ab, br, out_path):
    exe = os.path.join(build, "aac-enc")
    rc, out, err = run(
        [exe, "-t", str(aot), "-a", str(ab), "-r", str(br), wav_path,
         out_path], build)
    if rc != 0:
        raise RuntimeError(f"aac-enc failed ({build}): {err.strip()}")


def decode(build, aac_path, wav_path):
    exe = os.path.join(build, "aac-dec")
    rc, out, err = run([exe, aac_path, wav_path], build)
    # The decoder prints every anomaly (partial packet, decode error, ...) on
    # stderr; acceptance requires a completely silent, zero-status decode.
    if rc != 0 or err.strip() or out.strip():
        raise RuntimeError(
            f"decoder not warning-free ({build}): rc={rc} "
            f"stdout={out.strip()!r} stderr={err.strip()!r}")


def energy_ratio(wav_path):
    """Return E_left / E_right of the *input*, the pinned side reference."""
    _, pcm = wavfile.read(wav_path)
    pcm = pcm.astype(np.float64)
    e_l = float(np.sum(pcm[:, 0] ** 2))
    e_r = float(np.sum(pcm[:, 1] ** 2))
    return e_l / max(e_r, 1e-12)


def decoded_ratio(wav_path, trim):
    sr, pcm = wavfile.read(wav_path)
    pcm = pcm.astype(np.float64)
    if pcm.ndim != 2 or pcm.shape[1] != 2:
        raise RuntimeError("decoder output is not stereo")
    pcm = pcm[trim:] if len(pcm) > trim else pcm
    e_l = float(np.sum(pcm[:, 0] ** 2))
    e_r = float(np.sum(pcm[:, 1] ** 2))
    return sr, e_l / max(e_r, 1e-12)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-a", required=True, help="primary (SIMD) build dir")
    ap.add_argument("--build-b", required=True, help="generic-C build dir")
    ap.add_argument("--samples",
                    default=os.path.join(HERE, "..", "..", "artifacts",
                                         "samples"))
    ap.add_argument("--media", default=os.path.join(HERE, "..", "..",
                                                    "artifacts", "media"))
    ap.add_argument("--update", action="store_true",
                    help="rewrite manifest.json instead of checking it")
    args = ap.parse_args()

    builds = {"A": os.path.abspath(args.build_a),
              "B": os.path.abspath(args.build_b)}
    samples = os.path.abspath(args.samples)
    media = os.path.abspath(args.media)
    os.makedirs(media, exist_ok=True)
    manifest = json.load(open(MANIFEST)) if os.path.exists(MANIFEST) else {}
    failures, new_manifest = [], {}

    for wav, aot, ab, br in MATRIX:
        cid = case_id(wav, aot, ab, br)
        wav_path = os.path.join(samples, wav)
        if not os.path.exists(wav_path):
            failures.append(f"{cid}: missing sample {wav}")
            continue
        outs, hashes = {}, {}
        for tag, build in builds.items():
            out_aac = os.path.join(media, f"{cid}-{tag}.aac")
            encode(build, wav_path, aot, ab, br, out_aac)
            hashes[tag] = hashlib.sha256(open(out_aac, "rb").read()) \
                .hexdigest()
            outs[tag] = out_aac

        # (a) cross-build bit-exactness
        if hashes["A"] != hashes["B"]:
            failures.append(
                f"{cid}: builds differ A={hashes['A'][:16]} B={hashes['B'][:16]}")

        # (b) warning-free decode with build A and L/R energy-side check
        dec_wav = os.path.join(media, f"{cid}-dec.wav")
        try:
            decode(builds["A"], outs["A"], dec_wav)
        except RuntimeError as e:
            failures.append(str(e))
            continue

        in_ratio = energy_ratio(wav_path)
        # Encoder delay for LC/HE at 44.1 kHz is 2048 samples/channel.
        _, dec_ratio = decoded_ratio(dec_wav, trim=2048)

        if args.update:
            new_manifest[cid] = {
                "wav": wav, "aot": aot, "afterburner": ab, "bitrate": br,
                "sha256": hashes["A"],
                "input_lr_energy_ratio": round(in_ratio, 6),
                "decoded_lr_energy_ratio": round(dec_ratio, 6),
            }
            continue

        entry = manifest.get(cid)
        if entry is None:
            failures.append(f"{cid}: not in manifest (run --update)")
            continue
        if hashes["A"] != entry["sha256"]:
            failures.append(
                f"{cid}: SHA-256 drift got {hashes['A']} want {entry['sha256']}")
        pin = entry["decoded_lr_energy_ratio"]
        if not np.isfinite(dec_ratio) or abs(dec_ratio - pin) > RATIO_TOL:
            failures.append(
                f"{cid}: decoded L/R ratio {dec_ratio:.4f} outside "
                f"[{pin - RATIO_TOL:.4f}, {pin + RATIO_TOL:.4f}]")
        # same-side invariant: input and decode must agree on which channel
        # is louder (or both be balanced), no L/R energy swap.
        if (np.sign(np.log(in_ratio)) != np.sign(np.log(dec_ratio))) and \
           abs(np.log(in_ratio)) > 0.05:
            failures.append(
                f"{cid}: L/R energy side swapped input={in_ratio:.4f} "
                f"decoded={dec_ratio:.4f}")

    if args.update:
        with open(MANIFEST, "w") as f:
            json.dump(new_manifest, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"manifest updated: {len(new_manifest)} cases -> {MANIFEST}")
        return 0

    if failures:
        print("DETERMINISM CHECK FAILED:")
        for f_ in failures:
            print("  - " + f_)
        return 1
    print(f"determinism OK: {len(MATRIX)} cases, bit-exact across "
          f"{os.path.basename(builds['A'])} vs {os.path.basename(builds['B'])}, "
          f"warning-free decode, L/R energy side preserved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
