/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "video-shader.h"

#include "../opengl/gles2.h"
#include <emscripten/html5.h>
#include <mgba-util/vfs.h>
#include <mgba/core/core.h>

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context;
static struct mGLES2Context display;
static struct VideoShader preset;
static int viewportWidth, viewportHeight;

static bool activateDisplay(const char* canvas) {
	if (context) {
		return emscripten_webgl_make_context_current(context) == EMSCRIPTEN_RESULT_SUCCESS;
	}
	EmscriptenWebGLContextAttributes attrs;
	emscripten_webgl_init_context_attributes(&attrs);
	attrs.majorVersion = 2;
	attrs.alpha = attrs.depth = attrs.stencil = attrs.antialias = false;
	context = emscripten_webgl_create_context(canvas, &attrs);
	if (context <= 0) {
		context = 0;
		return false;
	}
	if (emscripten_is_webgl_context_lost(context) ||
	    emscripten_webgl_make_context_current(context) != EMSCRIPTEN_RESULT_SUCCESS) {
		emscripten_webgl_destroy_context(context);
		context = 0;
		return false;
	}
	mGLES2ContextCreate(&display);
	display.d.init(&display.d, NULL);
	return true;
}

void mWasmCloseVideoShader(void) {
	if (!context) {
		return;
	}
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE previous = emscripten_webgl_get_current_context();
	emscripten_webgl_make_context_current(context);
	mGLES2ShaderDetach(&display);
	mGLES2ShaderFree(&preset);
	display.d.deinit(&display.d);
	emscripten_webgl_make_context_current(previous);
	emscripten_webgl_destroy_context(context);
	context = 0;
	viewportWidth = viewportHeight = 0;
}

bool mWasmSetVideoShader(const char* directory, const char* canvas) {
	if (!directory || !*directory) {
		mWasmCloseVideoShader();
		return true;
	}
	struct VDir* dir = VDirOpen(directory);
	if (!dir) {
		return false;
	}
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE previous = emscripten_webgl_get_current_context();
	if (!activateDisplay(canvas)) {
		dir->close(dir);
		return false;
	}
	struct VideoShader next = {0};
	bool ok = mGLES2ShaderLoad(&next, dir);
	dir->close(dir);
	// A valid manifest can still contain a program the device cannot link.
	struct mGLES2Shader* passes = next.passes;
	for (size_t i = 0; ok && i < next.nPasses; ++i) {
		GLint linked = GL_FALSE;
		glGetProgramiv(passes[i].program, GL_LINK_STATUS, &linked);
		ok = linked == GL_TRUE;
	}
	if (ok) {
		mGLES2ShaderDetach(&display);
		mGLES2ShaderFree(&preset);
		preset = next;
		mGLES2ShaderAttach(&display, preset.passes, preset.nPasses);
	} else {
		mGLES2ShaderFree(&next);
	}
	emscripten_webgl_make_context_current(previous);
	if (!ok && !preset.nPasses) {
		mWasmCloseVideoShader();
	}
	return ok;
}

bool mWasmDrawVideoShader(struct mCore* core) {
	if (!preset.nPasses) {
		return false;
	}
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE previous = emscripten_webgl_get_current_context();
	emscripten_webgl_make_context_current(context);
	unsigned w, h;
	core->currentVideoSize(core, &w, &h);
	int width, height;
	emscripten_webgl_get_drawing_buffer_size(context, &width, &height);
	if (display.width != (int) w || display.height != (int) h ||
	    width != viewportWidth || height != viewportHeight) {
		struct mRectangle dims = { .width = w, .height = h };
		display.d.setLayerDimensions(&display.d, VIDEO_LAYER_IMAGE, &dims);
		display.d.contextResized(&display.d, width, height, 0, 0);
		viewportWidth = width;
		viewportHeight = height;
	}
	const void* pixels;
	size_t stride;
	core->getPixels(core, &pixels, &stride);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
	display.d.setImage(&display.d, VIDEO_LAYER_IMAGE, pixels);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	display.d.drawFrame(&display.d);
	emscripten_webgl_make_context_current(previous);
	return true;
}
