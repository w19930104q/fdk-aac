#!/bin/bash
#
# Acceptance test for the xHE-AAC low-bitrate extension.
#
# Verifies that:
#   a) mono 6/7/8 kbit/s and the matching stereo rates (12/14/16 kbit/s,
#      i.e. 2x the per-channel rate) encode the fixed sample, decode back
#      with the library decoder, and keep energy in the 4-8 kHz band. The
#      decoded band energy relative to the same content encoded at
#      32 kbit/s must be >= the hard-coded floor (see below). A silent high
#      band produces a ratio near zero and fails.
#   b) Requests below the supported floor fail with a documented error code
#      instead of crashing (see xhe-low-bitrate-errors).
#
# Existing-rate bit-exactness and the rest of the regression suite are
# covered separately by run-test.sh.
#
# Required tools (override with environment variables):
#   XHE_ENC     path to xhe-aac-enc
#   AAC_DEC     path to aac-dec
#   ERROR_PROBE path to the xhe-low-bitrate-errors binary
# Input WAVs are the same fixed samples used by run-test.sh.

set -e
set -o pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR=${BUILD_DIR:-"$SCRIPT_DIR/../build"}
XHE_ENC=${XHE_ENC:-"$BUILD_DIR/xhe-aac-enc"}
AAC_DEC=${AAC_DEC:-"$BUILD_DIR/aac-dec"}
ERROR_PROBE=${ERROR_PROBE:-"$BUILD_DIR/xhe-low-bitrate-errors"}
PYTHON=${PYTHON:-python3}

# Hard-coded acceptance floor for the 4-8 kHz relative-energy ratio. The
# worst measured value over the supported rates is ~0.79 (mono 7 kbit/s);
# 0.50 leaves margin while a missing/silent SBR high band scores ~0.
ENERGY_FLOOR=0.50

MONO_WAV="$SCRIPT_DIR/Sample01_4-mono.wav"
STEREO_WAV="$SCRIPT_DIR/Sample01_4.wav"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

failures=0

check_tools() {
	for tool in "$XHE_ENC" "$AAC_DEC" "$ERROR_PROBE"; do
		if [ ! -x "$tool" ]; then
			echo "Missing executable: $tool (build with -DBUILD_PROGRAMS=ON)"
			exit 1
		fi
	done
	for wav in "$MONO_WAV" "$STEREO_WAV"; do
		if [ ! -f "$wav" ]; then
			echo "Missing sample: $wav (run run-test.sh once to fetch it)"
			exit 1
		fi
	done
}

# encode_decode <input.wav> <bitrate> <tag>
# On success leaves the decoded WAV at $WORK/<tag>.wav.
encode_decode() {
	local input=$1 bitrate=$2 tag=$3
	if ! "$XHE_ENC" -l -23 -r "$bitrate" "$input" "$WORK/$tag.loas" \
			>"$WORK/$tag.enc.log" 2>&1; then
		echo "FAIL encode $tag @ ${bitrate} bit/s"
		cat "$WORK/$tag.enc.log"
		failures=$((failures + 1))
		return 1
	fi
	if ! "$AAC_DEC" "$WORK/$tag.loas" "$WORK/$tag.wav" >"$WORK/$tag.dec.log" 2>&1; then
		echo "FAIL decode $tag @ ${bitrate} bit/s"
		cat "$WORK/$tag.dec.log"
		failures=$((failures + 1))
		return 1
	fi
}

# check_energy <tag> <ref_tag>
check_energy() {
	local tag=$1 ref_tag=$2
	local line ratio
	line=$("$PYTHON" -I "$SCRIPT_DIR/band_energy.py" \
		"$WORK/$tag.wav" "$WORK/$ref_tag.wav" 4000 8000)
	ratio=$(echo "$line" | sed -n 's/.*ratio=\([0-9.eE+-]*\).*/\1/p')
	echo "$tag: $line"
	# Compare against the hard-coded floor without relying on bc.
	if ! awk "BEGIN {exit !($ratio >= $ENERGY_FLOOR)}"; then
		echo "FAIL $tag: 4-8 kHz relative energy $ratio < floor $ENERGY_FLOOR"
		failures=$((failures + 1))
	fi
}

check_tools

echo "=== Baselines (32 kbit/s) ==="
encode_decode "$MONO_WAV" 32000 mono-32000
encode_decode "$STEREO_WAV" 32000 stereo-32000

echo "=== Mono 6/7/8 kbit/s ==="
for br in 6000 7000 8000; do
	tag=mono-$br
	encode_decode "$MONO_WAV" "$br" "$tag" && check_energy "$tag" mono-32000
done

echo "=== Stereo 12/14/16 kbit/s (2x the per-channel mono rates) ==="
for br in 12000 14000 16000; do
	tag=stereo-$br
	encode_decode "$STEREO_WAV" "$br" "$tag" && check_energy "$tag" stereo-32000
done

echo "=== Documented error codes on impossible open requests ==="
if "$ERROR_PROBE"; then
	:
else
	echo "FAIL error-code probe"
	failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
	echo "LOW-BITRATE ACCEPTANCE: $failures FAILURE(S)"
	exit 1
fi
echo "LOW-BITRATE ACCEPTANCE: ALL CHECKS PASSED (energy floor $ENERGY_FLOOR)"
