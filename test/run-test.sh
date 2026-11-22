#!/bin/bash

set -e
set -o pipefail

if [ ! -f Sample01_4.wav ]; then
	curl -LO https://media.xiph.org/audio/HA_2011/Sample01_4.wav
fi

if [ "$(md5sum Sample01_4.wav | awk '{print $1}')" != "a5c105544c64ce92c6c5c06d280e6b9c" ]; then
	echo Incorrect checksum for Sample01_4.wav
	exit 1
fi

if [ ! -f Sample01_4-mono.wav ]; then
	ffmpeg -i Sample01_4.wav -ac 1 -fflags +bitexact -y Sample01_4-mono.wav
fi

if [ "$(md5sum Sample01_4-mono.wav | awk '{print $1}')" != "3bfccac9f2e527ba3ef888874f09a409" ]; then
	echo Incorrect checksum for Sample01_4-mono.wav
	exit 1
fi

./test-encode-decode Sample01_4.wav | tee log-stereo.txt
./test-encode-decode Sample01_4-mono.wav | tee log-mono.txt

# FDK_BITEXACT (the default build) makes all SIMD paths execute the same
# integer primitives, so every architecture now shares one reference hash.
# The legacy ref-*-aarch64.txt files are kept as identical copies for any
# out-of-tree tooling that still references them.
diff -u log-stereo.txt $(dirname $0)/ref-stereo.txt
diff -u log-mono.txt $(dirname $0)/ref-mono.txt

# The platform-specific copies must never drift from the shared reference.
diff -u $(dirname $0)/ref-stereo.txt $(dirname $0)/ref-stereo-aarch64.txt
diff -u $(dirname $0)/ref-mono.txt $(dirname $0)/ref-mono-aarch64.txt
