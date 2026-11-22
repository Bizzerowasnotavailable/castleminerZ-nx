#!/bin/bash
# run-check.sh <faudio-git-checkout> <patch-dir> <harness.c> <xact-data-dir> <work-dir> [ms-per-cue]
#
# Builds the pinned FAudio for the Linux host three times: as is ("pristine"),
# and with the project's patches using the silent WMA backend ("silent") and the
# FFmpeg WMA backend ("ffmpeg"). Links xact_audio_check against each with
# matching definitions and plays every cue of the XACT project in
# <xact-data-dir> (Sounds.xgs, Sounds.xwb, SoundsStreaming.xwb, Sounds.xsb)
# through a live mixer. Each run is bounded by a timeout, so a hung mixer counts
# as a failure. The ffmpeg variant needs the libavcodec/libavutil development
# packages (pkg-config names libavcodec and libavutil).
#
# Prints one RESULT line per variant: the exit status, cues with a voice left
# without a decoder, cues with WMA-path voices, and how many of those cues were
# audible on the mastering voice. Writes only under <work-dir>.
set -euo pipefail

faudio=$1
patches=$2
harness=$3
data=$4
work=$5
window_ms=${6:-200}

# /tmp can be a tmpfs that is cleared when the WSL VM stops; /var/tmp persists.
case "$work" in
	/tmp/?*|/var/tmp/?*) ;;
	*) echo "work dir must be under /tmp or /var/tmp" >&2; exit 1 ;;
esac
rm -rf "$work"
mkdir -p "$work"

git -c safe.directory='*' -C "$faudio" archive --format=tar --prefix=faudio-pristine/ HEAD | tar -x -C "$work"
cp -a "$work/faudio-pristine" "$work/faudio-patched"
for patch in "$patches"/*.patch; do
	git -C "$work/faudio-patched" apply --whitespace=nowarn "$patch"
done

common_cmake="-DBUILD_SDL3=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_UTILS=OFF"
common_defs="-DFAUDIO_DISABLE_DEBUGCONFIGURATION"
variants="pristine silent ffmpeg"
for variant in $variants; do
	source_dir="$work/faudio-patched"
	extra_cmake=""
	extra_defs=""
	extra_libs=""
	case "$variant" in
		pristine)
			source_dir="$work/faudio-pristine"
			;;
		silent)
			extra_cmake="-DSILENT_WMA_DECODING=ON"
			extra_defs="-DHAVE_WMADEC=1 -DFAUDIO_SILENT_WMADEC=1"
			;;
		ffmpeg)
			extra_cmake="-DFFMPEG_WMA_DECODING=ON"
			extra_defs="-DHAVE_WMADEC=1 -DFAUDIO_FFMPEG_WMADEC=1"
			extra_libs="$(pkg-config --libs libavcodec libavutil)"
			;;
	esac
	# shellcheck disable=SC2086
	cmake -S "$source_dir" -B "$work/build-$variant" $common_cmake $extra_cmake > "$work/cmake-$variant.log" 2>&1
	cmake --build "$work/build-$variant" --parallel 8 >> "$work/cmake-$variant.log" 2>&1
	# shellcheck disable=SC2046,SC2086
	gcc -std=gnu11 -O1 -g -Wall -Wextra $common_defs $extra_defs \
		-I"$source_dir/include" -I"$source_dir/src" $(sdl2-config --cflags) \
		"$harness" "$work/build-$variant/libFAudio.a" $extra_libs $(sdl2-config --libs) -lm \
		-o "$work/check-$variant"
done

for variant in $variants; do
	set +e
	SDL_AUDIODRIVER=dummy timeout 600 "$work/check-$variant" \
		"$data/Sounds.xgs" "$data/Sounds.xwb" "$data/SoundsStreaming.xwb" "$data/Sounds.xsb" "$window_ms" \
		> "$work/run-$variant.log" 2>&1
	status=$?
	set -e
	# A log message from FAudio can split a cue line, so match the fields on
	# any line except the summary.
	no_decoder=$(grep -v '^summary:' "$work/run-$variant.log" | grep -c ' no-decoder=[1-9]' || true)
	wma=$(grep -v '^summary:' "$work/run-$variant.log" | grep -c ' wma=[1-9]' || true)
	wma_audible=$(sed -n 's/^summary: .* wma-audible=\([0-9]*\) .*/\1/p' "$work/run-$variant.log")
	echo "RESULT $variant status=$status cues-with-voice-without-decoder=$no_decoder cues-with-wma-voice=$wma wma-cues-audible=${wma_audible:-none} log=$work/run-$variant.log"
done
