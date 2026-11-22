/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "../sdl/sdl-audio.h"
#include <emscripten.h>
#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#include <stdatomic.h>

#define QUEUE_FRAMES 8192
// The emulator owns write and floor. The AudioWorklet owns read.
static struct {
	_Atomic uint32_t write;
	_Atomic uint32_t read;
	_Atomic uint32_t floor;
	uint32_t capacity;
	int16_t pcm[QUEUE_FRAMES * 2];
} queue = { .capacity = QUEUE_FRAMES };
static atomic_bool active;

EMSCRIPTEN_KEEPALIVE void* getAudioQueue(void) {
	return &queue;
}

bool mWasmAudioQueueActive(void) {
	return atomic_load(&active);
}

// Call with the core audio lock held.
void mWasmFlushAudioQueue(void) {
	atomic_store(&queue.floor, atomic_load(&queue.write));
}

void mWasmUseAudioQueue(struct mSDLAudio* audio, bool enable) {
	if (!audio->core) {
		return;
	}
	enable = enable && audio->frameAudio && audio->speed == 1;
	mCoreSyncLockAudio(audio->sync);
	if (enable != atomic_load(&active)) {
		atomic_store(&active, enable);
		mWasmFlushAudioQueue();
		mAudioBufferClear(audio->core->getAudioBuffer(audio->core));
		mAudioBufferClear(&audio->buffer);
		audio->resampler.timestamp = 0;
		audio->primeAudio = true;
	}
	mCoreSyncConsumeAudio(audio->sync);
}

void mWasmPublishAudio(struct mCoreThread* thread) {
	if (!atomic_load(&active)) {
		return;
	}
	struct mSDLAudio* audio = thread->userData;
	struct mCore* core = thread->core;
	mCoreSyncLockAudio(audio->sync);
	if (atomic_load(&active)) {
		double ratio = mCoreCalculateFramerateRatio(core, audio->sync->fpsTarget);
		mAudioResamplerSetSource(&audio->resampler, core->getAudioBuffer(core), core->audioSampleRate(core) / ratio, true);
		uint32_t write = atomic_load(&queue.write);
		uint32_t freeFrames = QUEUE_FRAMES - (write - atomic_load(&queue.read));
		// Drain every emulated frame even when the browser has suspended its audio device.
		while (mAudioResamplerProcess(&audio->resampler)) {
			size_t ready = mAudioBufferAvailable(&audio->buffer);
			while (ready && freeFrames) {
				uint32_t slot = write % QUEUE_FRAMES;
				size_t count = ready < QUEUE_FRAMES - slot ? ready : QUEUE_FRAMES - slot;
				if (count > freeFrames) {
					count = freeFrames;
				}
				mAudioBufferRead(&audio->buffer, queue.pcm + slot * 2, count);
				write += count;
				freeFrames -= count;
				ready -= count;
			}
			mAudioBufferClear(&audio->buffer);
		}
		atomic_store(&queue.write, write);
	}
	mCoreSyncConsumeAudio(audio->sync);
}
