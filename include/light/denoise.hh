/*  Copyright (C) 2025 ProtoAus (protoanus-tools fork)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    See file, 'COPYING', for details.
*/

#pragma once

#include <common/qvec.hh>
#include <vector>

// Optional lightmap denoising via Intel Open Image Denoise (OIDN).
// If the project was built without OIDN (HAVE_OIDN undefined), these are no-ops
// and `available()` returns false, so `-denoise` degrades gracefully.
namespace ltdenoise
{
// true if this build has OIDN linked in
bool available();

// Denoise an HDR lightmap image in place. `image` is width*height RGBA float
// pixels (qvec4f); only the RGB channels are denoised, alpha is left untouched.
// Safe to call from multiple threads. No-op for tiny images or without OIDN.
void denoise_image(std::vector<qvec4f> &image, int width, int height);
} // namespace ltdenoise
