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
#include <common/qvec.hh>
#include <string>
#include <vector>

struct mbsp_t;

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

// protoanus-tools: static-prop per-vertex lighting (Source `-StaticPropLighting` analogue).
// One record per matched prop placement: the model, its placement, and one baked RGB per
// (global IQM-order) mesh vertex, sampled from the finished world lighting. Written to the
// RGBPROPLIGHT BSPX lump; an engine multiplies it over each instance's live lighting.
struct proplight_record_t
{
    qvec3f origin{};
    qvec3f angles{}; // (pitch, yaw, roll), same convention as _light_mesh
    std::string model;
    std::vector<qvec3b> vertexcolors; // one absolute RGB per global IQM vertex
};

// Scan the same classnames as the mesh occluders (see -propshadowclasses) for IQM props,
// sample the baked lighting at every mesh vertex in world space, and append one record per
// placement to `out`.
void GatherPropVertexLights(const mbsp_t *bsp, std::vector<proplight_record_t> &out);
} // namespace lightmesh
