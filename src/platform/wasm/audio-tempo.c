/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "../sdl/sdl-audio.h"

#include <mgba/core/sync.h>
#include "third-party/sonic/sonic.h"

struct mWasmAudioTempo {
	sonicStream stream;
	int16_t input[];
};

void mWasmResetAudioTempo(struct mSDLAudio* audio) {
	if (audio->tempo) {
		sonicDestroyStream(audio->tempo->stream);
		free(audio->tempo);
		audio->tempo = NULL;
		audio->resampler.timestamp = 0;
	}
}

bool mWasmReadAudioTempo(struct mSDLAudio* audio, struct mAudioBuffer* source, unsigned rate,
                         double clock, int16_t* output, int frames) {
	if (!audio->preservePitch || audio->speed <= 1 || audio->speed > SONIC_MAX_SPEED) {
		return false;
	}
	if (audio->tempo && (unsigned) sonicGetSampleRate(audio->tempo->stream) != rate) {
		mWasmResetAudioTempo(audio);
	}
	if (!audio->tempo) {
		size_t bytes = mAudioBufferCapacity(source) * source->channels * sizeof(int16_t);
		struct mWasmAudioTempo* tempo = malloc(sizeof(*tempo) + bytes);
		if (!tempo) {
			return false;
		}
		tempo->stream = sonicCreateStream(rate, source->channels);
		if (!tempo->stream) {
			free(tempo);
			return false;
		}
		audio->tempo = tempo;
		mAudioBufferClear(&audio->buffer);
	}

	sonicStream stream = audio->tempo->stream;
	double inputPerOutput = rate / (clock * audio->obtainedSpec.freq);
	float deviceRate = inputPerOutput / audio->speed;
	if (sonicGetRate(stream) != deviceRate) {
		sonicSetRate(stream, deviceRate);
	}
	sonicSetSpeed(stream, audio->speed);
	// Keep at most one extra callback of output to absorb pitch-period boundaries.
	int needed = 2 * frames - sonicSamplesAvailable(stream);
	size_t wanted = needed > 0 ? (size_t) ceil(needed * inputPerOutput) : 0;
	mCoreSyncLockAudio(audio->sync);
	if (audio->sync) {
		audio->sync->audioHighWater = (size_t) ceil((frames + 32) * inputPerOutput);
	}
	size_t count = mAudioBufferRead(source, audio->tempo->input, wanted);
	mCoreSyncConsumeAudio(audio->sync);

	// The emulator can produce the next block while Sonic processes this one.
	if (count && !sonicWriteShortToStream(stream, audio->tempo->input, count)) {
		mWasmResetAudioTempo(audio);
		memset(output, 0, frames * source->channels * sizeof(*output));
		return true;
	}
	int read = sonicReadShortFromStream(stream, output, frames);
	memset(output + read * source->channels, 0, (frames - read) * source->channels * sizeof(*output));
	return true;
}
