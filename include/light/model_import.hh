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

#include <common/polylib.hh>
#include <vector>

// protoanus-tools: external mesh occluders (Source `-StaticPropPolys` analogue).
// Loads prop meshes referenced by `_light_mesh` point entities and feeds their triangles
// to the ray tracer as shadow casters, so props throw accurate mesh-shaped shadows instead
// of collision-hull approximations.
namespace lightmesh
{
// Scan the loaded entities for `_light_mesh` (keys: model, origin, angles, _scale), load each
// mesh (.obj), transform it to world space, and append every triangle as a 3-vertex winding
// to `out`. These become unconditional shadow occluders in the Embree scene.
void GatherMeshOccluders(std::vector<polylib::winding3f_t> &out);
} // namespace lightmesh
