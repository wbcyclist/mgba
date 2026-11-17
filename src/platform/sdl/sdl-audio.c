/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "sdl-audio.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>

mLOG_DEFINE_CATEGORY(SDL_AUDIO, "SDL Audio", "platform.sdl.audio");

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void _mSDLAudioCallback(void* context, SDL_AudioStream* stream, int additionalLen, int totalLen);
#else
static void _mSDLAudioCallback(void* context, Uint8* data, int len);
#endif

#ifdef M_WASM_AUDIO_TEMPO
// About 1.3 ms at 48 kHz.
#define AUDIO_RAMP_FRAMES 64

// Drop both source samples and resampled output when the audible timeline changes.
void mWasmResetAudioOutput(struct mSDLAudio* audio) {
	if (!audio->core) {
		return;
	}
	mWasmResetAudioTempo(audio);
	mCoreSyncLockAudio(audio->sync);
	mAudioBufferClear(audio->core->getAudioBuffer(audio->core));
	mCoreSyncConsumeAudio(audio->sync);
	mAudioBufferClear(&audio->buffer);
	audio->resampler.timestamp = 0;
	audio->primeAudio = true;
}

static void _smoothAudioBoundary(struct mSDLAudio* audio, int16_t* pcm, int frames, bool shortRead) {
	int channels = audio->obtainedSpec.channels;
	for (int i = 0; i < frames && audio->fadeInRemaining; ++i) {
		if (audio->fadeInRemaining == AUDIO_RAMP_FRAMES && !pcm[i * channels] && !pcm[i * channels + 1]) {
			continue;
		}
		int gain = AUDIO_RAMP_FRAMES - audio->fadeInRemaining;
		--audio->fadeInRemaining;
		for (int c = 0; c < channels; ++c) {
			pcm[i * channels + c] = pcm[i * channels + c] * gain / AUDIO_RAMP_FRAMES;
		}
	}
	if (shortRead) {
		int ramp = frames < AUDIO_RAMP_FRAMES ? frames : AUDIO_RAMP_FRAMES;
		for (int i = 0; i < ramp; ++i) {
			for (int c = 0; c < channels; ++c) {
				int index = (frames - ramp + i) * channels + c;
				pcm[index] = pcm[index] * (ramp - 1 - i) / ramp;
			}
		}
		audio->primeAudio = frames == 0;
		audio->fadeInRemaining = AUDIO_RAMP_FRAMES;
	}
}
#endif

bool mSDLInitAudio(struct mSDLAudio* context, struct mCoreThread* threadContext) {
#if defined(_WIN32) && SDL_VERSION_ATLEAST(2, 0, 8)
	if (!getenv("SDL_AUDIODRIVER")) {
		_putenv_s("SDL_AUDIODRIVER", "directsound");
	}
#endif
	if (!SDL_OK(SDL_InitSubSystem(SDL_INIT_AUDIO))) {
		mLOG(SDL_AUDIO, ERROR, "Could not initialize SDL sound system: %s", SDL_GetError());
		return false;
	}

	context->desiredSpec.freq = context->sampleRate;
	context->desiredSpec.channels = 2;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	context->desiredSpec.format = SDL_AUDIO_S16;
	char hint[21];
	snprintf(hint, sizeof(hint), "%" PRIz "u", context->samples);
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, hint);
	context->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &context->desiredSpec, _mSDLAudioCallback, context);
	if (!context->stream) {
#else
	context->desiredSpec.callback = _mSDLAudioCallback;
	context->desiredSpec.userdata = context;
	context->desiredSpec.samples = context->samples;
	context->desiredSpec.format = AUDIO_S16SYS;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	context->deviceId =
	    SDL_OpenAudioDevice(0, 0, &context->desiredSpec, &context->obtainedSpec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (context->deviceId == 0) {
#else
	if (SDL_OpenAudio(&context->desiredSpec, &context->obtainedSpec) < 0) {
#endif
#endif
		mLOG(SDL_AUDIO, ERROR, "Could not open SDL sound system");
		return false;
	}
	context->core = NULL;
#ifdef M_WASM_AUDIO_TEMPO
	context->primeAudio = true;
#endif

#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_GetAudioStreamFormat(context->stream, NULL, &context->obtainedSpec);
#endif

	mAudioBufferInit(&context->buffer, context->samples, context->obtainedSpec.channels);
	mAudioResamplerInit(&context->resampler, mINTERPOLATOR_SINC);
	mAudioResamplerSetDestination(&context->resampler, &context->buffer, context->obtainedSpec.freq);

	if (threadContext) {
		context->core = threadContext->core;
		context->sync = &threadContext->impl->sync;

		mSDLResumeAudio(context);
	}

	return true;
}

void mSDLDeinitAudio(struct mSDLAudio* context) {
	UNUSED(context);
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_DestroyAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 1);
	SDL_CloseAudioDevice(context->deviceId);
#else
	SDL_PauseAudio(1);
	SDL_CloseAudio();
#endif
#ifdef M_WASM_AUDIO_TEMPO
	mWasmResetAudioTempo(context);
#endif
	mAudioBufferDeinit(&context->buffer);
	mAudioResamplerDeinit(&context->resampler);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void mSDLPauseAudio(struct mSDLAudio* context) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_PauseAudioStreamDevice(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 1);
#else
	UNUSED(context);
	SDL_PauseAudio(1);
#endif
#ifdef M_WASM_AUDIO_TEMPO
	mWasmResetAudioTempo(context);
	context->primeAudio = true;
#endif
}

void mSDLResumeAudio(struct mSDLAudio* context) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_ResumeAudioStreamDevice(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 0);
#else
	UNUSED(context);
	SDL_PauseAudio(0);
#endif
}

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void _mSDLAudioCallback(void* context, SDL_AudioStream* stream, int additionalLen, int len) {
	UNUSED(additionalLen);
#else
static void _mSDLAudioCallback(void* context, Uint8* data, int len) {
#endif
	struct mSDLAudio* audioContext = context;
	if (!context || !audioContext->core) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
		return;
#else
		memset(data, 0, len);
		return;
#endif
	}
	struct mAudioBuffer* buffer = NULL;
	unsigned sampleRate = 32768;
	if (audioContext->core) {
		buffer = audioContext->core->getAudioBuffer(audioContext->core);
		sampleRate = audioContext->core->audioSampleRate(audioContext->core);
	}
	double fauxClock = 1;
	if (audioContext->sync && audioContext->sync->fpsTarget > 0) {
		fauxClock = mCoreCalculateFramerateRatio(audioContext->core, audioContext->sync->fpsTarget);
	}
#ifdef M_WASM_AUDIO_TEMPO
	if (mWasmReadAudioTempo(audioContext, buffer, sampleRate, fauxClock, (int16_t*) data,
	                        len / (int) (sizeof(int16_t) * audioContext->obtainedSpec.channels))) {
		return;
	}
#endif
	if (audioContext->sync) {
		mCoreSyncLockAudio(audioContext->sync);
		audioContext->sync->audioHighWater = audioContext->samples + audioContext->resampler.highWaterMark +
		    audioContext->resampler.lowWaterMark + (audioContext->samples >> 6);
		audioContext->sync->audioHighWater *= sampleRate / (fauxClock * audioContext->obtainedSpec.freq);
	}

#ifdef M_WASM_AUDIO_TEMPO
	if (audioContext->speed == 1 && audioContext->primeAudio) {
		// The GBA produces PCM in video-frame bursts. Keep one frame in reserve
		// before starting the device's continuous stream after a discontinuity.
		size_t deviceFrames = len / (sizeof(int16_t) * audioContext->obtainedSpec.channels);
		double rateRatio = sampleRate / (fauxClock * audioContext->obtainedSpec.freq);
		size_t required = (size_t) ceil(deviceFrames * rateRatio) + audioContext->resampler.highWaterMark + sampleRate / 60;
		size_t capacity = mAudioBufferCapacity(buffer);
		if (required > capacity) {
			required = capacity;
		}
		if (audioContext->sync && audioContext->sync->audioHighWater < required) {
			audioContext->sync->audioHighWater = required;
		}
		if (mAudioBufferAvailable(buffer) < required) {
			mCoreSyncConsumeAudio(audioContext->sync);
			memset(data, 0, len);
			return;
		}
		audioContext->primeAudio = false;
		audioContext->fadeInRemaining = AUDIO_RAMP_FRAMES;
	}
#endif
	mAudioResamplerSetSource(&audioContext->resampler, buffer, sampleRate / fauxClock, true);
	mAudioResamplerProcess(&audioContext->resampler);
	if (audioContext->sync) {
		mCoreSyncConsumeAudio(audioContext->sync);
	}
#if SDL_VERSION_ATLEAST(3, 0, 0)
	int16_t data[2048];
	while (len > 0) {
		int thisRead = sizeof(data);
		if (len < thisRead) {
			thisRead = len;
		}
		int available = mAudioBufferRead(&audioContext->buffer, data, thisRead / 4) * 4;
		if (!available) {
			break;
		}
		SDL_PutAudioStreamData(stream, data, available);
		len -= available;
	}
#else
	len /= 2 * audioContext->obtainedSpec.channels;
	int available = mAudioBufferRead(&audioContext->buffer, (int16_t*) data, len);

#ifdef M_WASM_AUDIO_TEMPO
	if (audioContext->speed == 1 && (audioContext->fadeInRemaining || available < len)) {
		_smoothAudioBoundary(audioContext, (int16_t*) data, available, available < len);
	}
#endif
	if (available < len) {
		memset(((short*) data) + audioContext->obtainedSpec.channels * available, 0,
		       (len - available) * audioContext->obtainedSpec.channels * sizeof(short));
	}
#endif
}
