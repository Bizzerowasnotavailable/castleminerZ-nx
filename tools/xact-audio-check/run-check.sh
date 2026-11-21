#!/bin/bash
# run-check.sh <faudio-git-checkout> <patch-dir> <harness.c> <xact-data-dir> <work-dir> [ms-per-cue]
#
# Builds the pinned FAudio twice for the Linux host, as is and with the
# project's patches, links xact_audio_check against each with matching
# definitions, and plays every cue of the XACT project in <xact-data-dir>
# (Sounds.xgs, Sounds.xwb, SoundsStreaming.xwb, Sounds.xsb) through a live
# mixer. Each run is bounded by a timeout, so a hung mixer counts as a failure.
#
# Prints one RESULT line per variant: the exit status, and whether a voice was
# left without a decoder. Writes only under <work-dir>.
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
for variant in pristine patched; do
	extra_cmake=""
	extra_defs=""
	if [ "$variant" = patched ]; then
		extra_cmake="-DSILENT_WMA_DECODING=ON"
		extra_defs="-DHAVE_WMADEC=1 -DFAUDIO_SILENT_WMADEC=1"
	fi
	# shellcheck disable=SC2086
	cmake -S "$work/faudio-$variant" -B "$work/build-$variant" $common_cmake $extra_cmake > "$work/cmake-$variant.log" 2>&1
	cmake --build "$work/build-$variant" --parallel 8 >> "$work/cmake-$variant.log" 2>&1
	# shellcheck disable=SC2046,SC2086
	gcc -std=gnu11 -O1 -g -Wall -Wextra $common_defs $extra_defs \
		-I"$work/faudio-$variant/include" -I"$work/faudio-$variant/src" $(sdl2-config --cflags) \
		"$harness" "$work/build-$variant/libFAudio.a" $(sdl2-config --libs) -lm \
		-o "$work/check-$variant"
done

for variant in pristine patched; do
	set +e
	SDL_AUDIODRIVER=dummy timeout 300 "$work/check-$variant" \
		"$data/Sounds.xgs" "$data/Sounds.xwb" "$data/SoundsStreaming.xwb" "$data/Sounds.xsb" "$window_ms" \
		> "$work/run-$variant.log" 2>&1
	status=$?
	set -e
	no_decoder=$(grep -c ' no-decoder=[1-9]' "$work/run-$variant.log" || true)
	silent=$(grep -c ' silent=[1-9]' "$work/run-$variant.log" || true)
	echo "RESULT $variant status=$status cues-with-voice-without-decoder=$no_decoder cues-with-silent-voice=$silent log=$work/run-$variant.log"
done
