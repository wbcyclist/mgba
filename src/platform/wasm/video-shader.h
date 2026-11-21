/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef M_WASM_VIDEO_SHADER_H
#define M_WASM_VIDEO_SHADER_H

#include <mgba-util/common.h>

struct mCore;

bool mWasmSetVideoShader(const char* directory, const char* canvas, bool interframeBlending, bool bilinearFiltering);
bool mWasmDrawVideoShader(struct mCore* core, bool redraw);
void mWasmCloseVideoShader(void);

#endif
