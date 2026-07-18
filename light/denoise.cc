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

#include <light/denoise.hh>
#include <common/log.hh>

#include <mutex>

#ifdef HAVE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

namespace ltdenoise
{

#ifdef HAVE_OIDN

// One shared OIDN device for the whole run (device creation is heavy; filters are
// cheap). OIDN filter execution is serialized with a mutex — `denoise_image` is
// called from the parallel per-face lightmap writer, and a single device's filters
// are not safe to execute concurrently.
static std::once_flag s_device_once;
static oidn::DeviceRef s_device;
static bool s_device_ok = false;
static std::mutex s_mutex;

static void init_device()
{
    s_device = oidn::newDevice(); // default (CPU) device
    s_device.commit();
    const char *err = nullptr;
    if (s_device.getError(err) != oidn::Error::None) {
        logging::print("WARNING: OIDN device init failed ({}); -denoise disabled\n", err ? err : "unknown");
        s_device_ok = false;
    } else {
        s_device_ok = true;
    }
}

bool available()
{
    return true;
}

void denoise_image(std::vector<qvec4f> &image, int width, int height)
{
    // OIDN needs a little context to work; denoising a handful of luxels is pointless
    // (and the RTLightmap filter dislikes degenerate sizes).
    if (width < 4 || height < 4)
        return;
    if (static_cast<size_t>(width) * static_cast<size_t>(height) > image.size())
        return;

    std::call_once(s_device_once, init_device);
    if (!s_device_ok)
        return;

    std::lock_guard<std::mutex> lock(s_mutex);

    // qvec4f is 4 contiguous floats; treat the buffer as Float3 with a 16-byte pixel
    // stride so the alpha channel is skipped, and denoise in place (color == output).
    void *pixels = image.data();
    const size_t pixel_stride = sizeof(qvec4f);

    oidn::FilterRef filter = s_device.newFilter("RTLightmap");
    filter.setImage("color", pixels, oidn::Format::Float3, width, height, 0, pixel_stride, 0);
    filter.setImage("output", pixels, oidn::Format::Float3, width, height, 0, pixel_stride, 0);
    filter.set("hdr", true);
    filter.commit();
    filter.execute();

    const char *err = nullptr;
    if (s_device.getError(err) != oidn::Error::None)
        logging::print("WARNING: OIDN denoise failed ({})\n", err ? err : "unknown");
}

#else // !HAVE_OIDN

bool available()
{
    return false;
}

void denoise_image(std::vector<qvec4f> &, int, int) { }

#endif

} // namespace ltdenoise
