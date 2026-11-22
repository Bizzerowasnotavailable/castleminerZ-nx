/*
 * xact_audio_check: plays every cue of an XACT project through FACT the way
 * FNA's AudioEngine, WaveBank and SoundBank do, with a mixer thread that keeps
 * running. It checks that no source voice is left without a decoder and
 * measures the peak level reaching the mastering voice for each cue.
 *
 * Eden never runs the audio mixer past its first callback, so audio faults that
 * only occur while mixing are invisible there. This tool reproduces the mixing
 * on a desktop host. Run it with SDL_AUDIODRIVER=dummy for a paced mixer thread
 * without a sound device.
 *
 * Usage:
 *   xact_audio_check <settings.xgs> <in-memory.xwb> <streaming.xwb> <sounds.xsb> [ms-per-cue [cue-name]]
 *
 * With a cue name, only that cue plays, for the whole window, and its state is
 * sampled once a second. Use a window longer than the cue's wave to exercise
 * the end of the wave and FACT's streaming loop re-submission.
 *
 * The inputs are the user's own files. The tool reads them and writes nothing.
 *
 * It inspects FAudio's internal voice list, so it must be compiled with the
 * same HAVE_WMADEC/FAUDIO_*_WMADEC definitions as the libFAudio.a it links.
 * "wma" voices are those on FAudio's WMADEC path (xWMA/XMA2): with the silent
 * backend they play as silence, with the FFmpeg backend they are decoded.
 *
 * Exit status: 0 = every voice had a decoder and every cue ran; 2 = at least one
 * voice had no decoder; 1 = setup failure.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "FACT.h"
#include "FAudio.h"
#include "FAudioFX.h"
#include "FAPO.h"
/* FACT_internal.h includes FAudio_internal.h, which has no include guard. */
#include "FACT_internal.h"

/* A cue counts as audible when its mix peaks above this level (-60 dBFS). */
#define AUDIBLE_PEAK 0.001f

typedef struct VoiceReport
{
	int voices;
	int without_decoder;
	int wma_voices;
	uint64_t wma_samples;
	uint32_t wma_rate;
	/* First WMA voice: length of its current buffer, and position in it. */
	uint32_t wma_buffer_samples;
	uint32_t wma_offset;
} VoiceReport;

static int notification_counts[16];
static FAudioVoice *meter_voice;
static uint32_t meter_channels;

static void FACTCALL on_notification(const FACTNotification *notification)
{
	if (notification->type < 16)
	{
		notification_counts[notification->type] += 1;
	}
}

static uint8_t *load_file(const char *path, uint32_t *size)
{
	SDL_RWops *file = SDL_RWFromFile(path, "rb");
	Sint64 length;
	uint8_t *data;

	if (file == NULL)
	{
		fprintf(stderr, "cannot open %s: %s\n", path, SDL_GetError());
		return NULL;
	}
	length = SDL_RWsize(file);
	if (length <= 0 || length > 0x7FFFFFFF)
	{
		fprintf(stderr, "unexpected size for %s\n", path);
		SDL_RWclose(file);
		return NULL;
	}
	/* FACT frees buffers passed with FACT_FLAG_MANAGEDATA through SDL_free. */
	data = (uint8_t*) SDL_malloc((size_t) length);
	if (data == NULL || SDL_RWread(file, data, 1, (size_t) length) != (size_t) length)
	{
		fprintf(stderr, "cannot read %s\n", path);
		SDL_free(data);
		SDL_RWclose(file);
		return NULL;
	}
	SDL_RWclose(file);
	*size = (uint32_t) length;
	return data;
}

static void inspect_voices(FACTAudioEngine *engine, VoiceReport *report)
{
	FAudio *audio = engine->audio;
	LinkedList *entry;

	memset(report, 0, sizeof(*report));
	FAudio_PlatformLockMutex(audio->sourceLock);
	for (entry = audio->sources; entry != NULL; entry = entry->next)
	{
		FAudioSourceVoice *voice = (FAudioSourceVoice*) entry->entry;
		int has_decoder = voice->src.decode != NULL;

#ifdef HAVE_WMADEC
		if (voice->src.wmadec != NULL)
		{
			has_decoder = 1;
			if (report->wma_voices == 0)
			{
				FAudio_PlatformLockMutex(voice->src.bufferLock);
				if (voice->src.queued_buffer_count > 0)
				{
					const struct queued_buffer *buffer = &voice->src.queued_buffers[0];
					const uint32_t frame_bytes = voice->src.format->nChannels * voice->src.format->wBitsPerSample / 8;
					if (buffer->bufferWMA.PacketCount > 0 && frame_bytes > 0)
					{
						report->wma_buffer_samples =
							buffer->bufferWMA.pDecodedPacketCumulativeBytes[buffer->bufferWMA.PacketCount - 1] /
							frame_bytes;
					}
				}
				report->wma_offset = voice->src.curBufferOffset;
				FAudio_PlatformUnlockMutex(voice->src.bufferLock);
			}
			report->wma_voices += 1;
			report->wma_samples += voice->src.totalSamples;
			report->wma_rate = voice->src.format->nSamplesPerSec;
		}
#endif /* HAVE_WMADEC */

		report->voices += 1;
		if (!has_decoder)
		{
			report->without_decoder += 1;
		}
	}
	FAudio_PlatformUnlockMutex(audio->sourceLock);
}

static void register_notification(FACTAudioEngine *engine, uint8_t type)
{
	FACTNotificationDescription description;

	memset(&description, 0, sizeof(description));
	description.flags = FACT_FLAG_NOTIFICATION_PERSIST;
	description.type = type;
	FACTAudioEngine_RegisterNotification(engine, &description);
}

/* Measures the mix with FAudio's volume meter on the mastering voice. */
static void attach_meter(FACTAudioEngine *engine)
{
	FAPO *meter = NULL;
	FAudioVoiceDetails details;
	FAudioEffectDescriptor descriptor;
	FAudioEffectChain chain;

	if (engine->master == NULL || FAudioCreateVolumeMeter(&meter, 0) != 0)
	{
		printf("volume meter unavailable\n");
		return;
	}
	FAudioVoice_GetVoiceDetails(engine->master, &details);
	descriptor.pEffect = meter;
	descriptor.InitialState = 1;
	descriptor.OutputChannels = details.InputChannels;
	chain.EffectCount = 1;
	chain.pEffectDescriptors = &descriptor;
	if (FAudioVoice_SetEffectChain(engine->master, &chain) == 0 && details.InputChannels <= 8)
	{
		meter_voice = engine->master;
		meter_channels = details.InputChannels;
	}
	else
	{
		printf("volume meter not attached\n");
	}
	meter->Release(meter);
}

static float read_peak(void)
{
	float peaks[8] = {0};
	float rms[8] = {0};
	FAudioFXVolumeMeterLevels levels;
	float peak = 0.0f;
	uint32_t channel;

	if (meter_voice == NULL)
	{
		return -1.0f;
	}
	levels.pPeakLevels = peaks;
	levels.pRMSLevels = rms;
	levels.ChannelCount = meter_channels;
	FAudioVoice_GetEffectParameters(meter_voice, 0, &levels, sizeof(levels));
	for (channel = 0; channel < meter_channels; channel += 1)
	{
		if (peaks[channel] > peak)
		{
			peak = peaks[channel];
		}
	}
	return peak;
}

/* Waits `milliseconds` while the mixer runs, returning the highest peak seen. */
static float wait_and_measure(uint32_t milliseconds)
{
	float highest = 0.0f;
	uint32_t elapsed;

	for (elapsed = 0; elapsed < milliseconds; elapsed += 5)
	{
		float peak;
		SDL_Delay(5);
		peak = read_peak();
		if (peak > highest)
		{
			highest = peak;
		}
	}
	return highest;
}

int main(int argc, char **argv)
{
	FACTAudioEngine *engine = NULL;
	FACTRuntimeParameters parameters;
	FACTStreamingParameters streaming;
	FACTWaveBank *in_memory_bank = NULL;
	FACTWaveBank *streaming_bank = NULL;
	FACTSoundBank *sound_bank = NULL;
	FAudioIOStream *streaming_file;
	uint8_t *settings_data, *wave_data, *sound_data;
	uint32_t settings_size, wave_size, sound_size;
	uint16_t cue_count = 0, wave_count = 0;
	uint32_t window_ms = 200;
	const char *soak_cue = NULL;
	int cues_with_voices = 0, cues_without_decoder = 0, cues_wma = 0;
	int wma_cues_still_playing = 0, wma_cues_audible = 0, cues_audible = 0;
	uint32_t result;
	uint16_t index;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 5)
	{
		fprintf(stderr, "usage: %s <settings.xgs> <in-memory.xwb> <streaming.xwb> <sounds.xsb> [ms-per-cue [cue-name]]\n", argv[0]);
		return 1;
	}
	if (argc > 5)
	{
		window_ms = (uint32_t) strtoul(argv[5], NULL, 10);
	}
	if (argc > 6)
	{
		soak_cue = argv[6];
	}
	printf("SDL audio driver request: %s\n", SDL_getenv("SDL_AUDIODRIVER") ? SDL_getenv("SDL_AUDIODRIVER") : "(default)");

	settings_data = load_file(argv[1], &settings_size);
	wave_data = load_file(argv[2], &wave_size);
	sound_data = load_file(argv[4], &sound_size);
	if (settings_data == NULL || wave_data == NULL || sound_data == NULL)
	{
		return 1;
	}

	/* AudioEngine: settings buffer owned by FACT, notifications as FNA registers them. */
	if (FACTCreateEngine(0, &engine) != 0)
	{
		fprintf(stderr, "FACTCreateEngine failed\n");
		return 1;
	}
	memset(&parameters, 0, sizeof(parameters));
	parameters.pGlobalSettingsBuffer = settings_data;
	parameters.globalSettingsBufferSize = settings_size;
	parameters.globalSettingsFlags = FACT_FLAG_MANAGEDATA;
	parameters.fnNotificationCallback = on_notification;
	result = FACTAudioEngine_Initialize(engine, &parameters);
	if (result != 0)
	{
		fprintf(stderr, "FACTAudioEngine_Initialize failed: 0x%08X\n", result);
		return 1;
	}
	register_notification(engine, FACTNOTIFICATIONTYPE_WAVEBANKDESTROYED);
	register_notification(engine, FACTNOTIFICATIONTYPE_SOUNDBANKDESTROYED);
	register_notification(engine, FACTNOTIFICATIONTYPE_CUEDESTROYED);
	printf("audio driver in use: %s\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");
	attach_meter(engine);

	/* WaveBank(engine, file): whole file in memory. */
	result = FACTAudioEngine_CreateInMemoryWaveBank(engine, wave_data, wave_size, 0, 0, &in_memory_bank);
	if (result != 0)
	{
		fprintf(stderr, "in-memory wave bank failed: 0x%08X\n", result);
		return 1;
	}

	/* WaveBank(engine, file, offset, packetsize): FAudio_fopen, default parameters. */
	streaming_file = FAudio_fopen(argv[3]);
	if (streaming_file == NULL)
	{
		fprintf(stderr, "cannot open streaming wave bank %s\n", argv[3]);
		return 1;
	}
	memset(&streaming, 0, sizeof(streaming));
	streaming.file = streaming_file;
	result = FACTAudioEngine_CreateStreamingWaveBank(engine, &streaming, &streaming_bank);
	if (result != 0)
	{
		fprintf(stderr, "streaming wave bank failed: 0x%08X\n", result);
		return 1;
	}
	FACTWaveBank_GetNumWaves(streaming_bank, &wave_count);
	for (index = 0; index < wave_count; index += 1)
	{
		FACTWaveProperties wave;

		uint32_t packet_samples = 0;

		memset(&wave, 0, sizeof(wave));
		FACTWaveBank_GetWaveProperties(streaming_bank, index, &wave);
		/* xWMA voices take their length from the packet table (decoded bytes). */
		if (	streaming_bank->seekTables != NULL &&
			streaming_bank->seekTables[index].entryCount > 0 &&
			wave.format.nChannels > 0	)
		{
			const FACTSeekTable *table = &streaming_bank->seekTables[index];
			packet_samples = table->entries[table->entryCount - 1] / (wave.format.nChannels * 2);
		}
		printf(
			"streaming wave %2u: tag=%u channels=%u rate=%u duration=%u samples (%.1f s) packet-table=%u samples (%.1f s) loop=%u+%u\n",
			index,
			wave.format.wFormatTag,
			wave.format.nChannels,
			wave.format.nSamplesPerSec,
			wave.durationInSamples,
			wave.format.nSamplesPerSec ? (double) wave.durationInSamples / wave.format.nSamplesPerSec : 0.0,
			packet_samples,
			wave.format.nSamplesPerSec ? (double) packet_samples / wave.format.nSamplesPerSec : 0.0,
			wave.loopRegion.dwStartSample,
			wave.loopRegion.dwTotalSamples
		);
	}

	result = FACTAudioEngine_CreateSoundBank(engine, sound_data, sound_size, 0, 0, &sound_bank);
	if (result != 0)
	{
		fprintf(stderr, "sound bank failed: 0x%08X\n", result);
		return 1;
	}
	FACTSoundBank_GetNumCues(sound_bank, &cue_count);
	printf("cues: %u, window: %u ms per cue\n", cue_count, window_ms);

	for (index = 0; index < cue_count; index += 1)
	{
		FACTCueProperties properties;
		FACTCue *cue = NULL;
		VoiceReport started, later;
		uint32_t state = 0;
		float peak = 0.0f;

		memset(&properties, 0, sizeof(properties));
		FACTSoundBank_GetCueProperties(sound_bank, index, &properties);
		if (soak_cue != NULL && strcmp(soak_cue, properties.friendlyName) != 0)
		{
			continue;
		}
		printf("cue %3u %-32s ", index, properties.friendlyName);

		result = FACTSoundBank_Play(sound_bank, index, 0, 0, &cue);
		FACTAudioEngine_DoWork(engine);
		inspect_voices(engine, &started);
		printf("voices=%d no-decoder=%d wma=%d", started.voices, started.without_decoder, started.wma_voices);

		/* The mixer runs on the SDL audio thread meanwhile. */
		if (soak_cue == NULL)
		{
			peak = wait_and_measure(window_ms);
		}
		else
		{
			uint32_t elapsed;

			printf("\n");
			for (elapsed = 1000; elapsed <= window_ms; elapsed += 1000)
			{
				const float second_peak = wait_and_measure(1000);
				if (second_peak > peak)
				{
					peak = second_peak;
				}
				FACTAudioEngine_DoWork(engine);
				inspect_voices(engine, &later);
				state = 0;
				if (cue != NULL)
				{
					FACTCue_GetState(cue, &state);
				}
				printf(
					"  t=%3u s state=0x%02X voices=%d wma=%d wma-samples=%llu offset=%u/%u peak=%.4f\n",
					elapsed / 1000,
					state,
					later.voices,
					later.wma_voices,
					(unsigned long long) later.wma_samples,
					later.wma_offset,
					later.wma_buffer_samples,
					second_peak
				);
			}
		}
		FACTAudioEngine_DoWork(engine);
		inspect_voices(engine, &later);
		if (cue != NULL)
		{
			FACTCue_GetState(cue, &state);
		}
		printf(" state=0x%02X peak=%.4f", state, peak);

		if (started.voices > 0)
		{
			cues_with_voices += 1;
		}
		if (started.without_decoder > 0)
		{
			cues_without_decoder += 1;
		}
		if (peak > AUDIBLE_PEAK)
		{
			cues_audible += 1;
		}
		if (started.wma_voices > 0)
		{
			/* WMA voices must advance at the real rate, not end at once. */
			double expected = (double) later.wma_rate * window_ms / 1000.0 * later.wma_voices;
			double advanced = (double) (later.wma_samples - started.wma_samples);
			cues_wma += 1;
			if (state & FACT_STATE_PLAYING)
			{
				wma_cues_still_playing += 1;
			}
			if (peak > AUDIBLE_PEAK)
			{
				wma_cues_audible += 1;
			}
			printf(" wma-progress=%.0f/%.0f samples", advanced, expected);
		}
		printf(" (play=0x%08X)\n", result);

		if (cue != NULL)
		{
			FACTCue_Stop(cue, FACT_FLAG_STOP_IMMEDIATE);
			FACTAudioEngine_DoWork(engine);
			FACTCue_Destroy(cue);
		}
		FACTAudioEngine_DoWork(engine);
		/* Let the stopped cue's tail leave the meter before the next cue. */
		SDL_Delay(30);
	}

	FACTSoundBank_Destroy(sound_bank);
	FACTWaveBank_Destroy(streaming_bank);
	FACTWaveBank_Destroy(in_memory_bank);
	FAudio_close(streaming_file);
	FACTAudioEngine_ShutDown(engine);
	FACTAudioEngine_Release(engine);
	SDL_free(wave_data);
	SDL_free(sound_data);

	printf(
		"summary: cues=%u with-voices=%d without-decoder=%d audible=%d wma=%d wma-audible=%d wma-still-playing=%d cue-destroyed-notifications=%d\n",
		cue_count,
		cues_with_voices,
		cues_without_decoder,
		cues_audible,
		cues_wma,
		wma_cues_audible,
		wma_cues_still_playing,
		notification_counts[FACTNOTIFICATIONTYPE_CUEDESTROYED]
	);
	return cues_without_decoder > 0 ? 2 : 0;
}
