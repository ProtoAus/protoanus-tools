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

#include <light/model_import.hh>
#include <light/entities.hh>
#include <light/light.hh>
#include <light/lightgrid.hh>
#include <light/ltface.hh>

#include <common/fs.hh>
#include <common/log.hh>
#include <common/mathlib.hh>
#include <common/qvec.hh>

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lightmesh
{

// Minimal Wavefront .obj reader: reads `v x y z` positions and `f ...` faces (fan-triangulated),
// ignoring texcoords/normals. Face indices may be `v`, `v/vt`, or `v/vt/vn`, 1-based (negative =
// relative to the current vertex count). Fills `tris` with model-space triangles.
static bool LoadObjTris(const fs::path &path, std::vector<std::array<qvec3f, 3>> &tris)
{
    fs::data d = fs::load(path);
    if (!d)
        return false;

    std::string text(reinterpret_cast<const char *>(d->data()), d->size());
    std::istringstream ss(text);
    std::string line;
    std::vector<qvec3f> verts;

    while (std::getline(ss, line)) {
        if (line.size() < 2)
            continue;
        if (line[0] == 'v' && line[1] == ' ') {
            std::istringstream ls(line.substr(2));
            qvec3f v{};
            ls >> v[0] >> v[1] >> v[2];
            verts.push_back(v);
        } else if (line[0] == 'f' && line[1] == ' ') {
            std::istringstream ls(line.substr(2));
            std::string vtok;
            std::vector<int> idx;
            while (ls >> vtok) {
                // vtok is like "12", "12/3", or "12/3/4"; atoi stops at the first '/'
                int vi = std::atoi(vtok.c_str());
                if (vi < 0)
                    vi = static_cast<int>(verts.size()) + vi; // relative: -1 == last vertex
                else
                    vi -= 1; // 1-based -> 0-based
                idx.push_back(vi);
            }
            for (size_t j = 2; j < idx.size(); j++) {
                const int a = idx[0], b = idx[j - 1], c = idx[j];
                if (a < 0 || b < 0 || c < 0)
                    continue;
                if (a >= (int)verts.size() || b >= (int)verts.size() || c >= (int)verts.size())
                    continue;
                tris.push_back({verts[a], verts[b], verts[c]});
            }
        }
    }
    return !tris.empty();
}

static inline uint32_t rd_u32(const uint8_t *p, size_t off)
{
    uint32_t v;
    std::memcpy(&v, p + off, 4);
    return v;
}
static inline float rd_f32(const uint8_t *p, size_t off)
{
    float v;
    std::memcpy(&v, p + off, 4);
    return v;
}

// Minimal IQM (Inter-Quake Model, v2) triangle reader: the base-frame vertex positions
// (IQM_POSITION vertex array, float3) + the triangle index list. Winding is irrelevant for
// shadow occlusion. Header field offsets per the IQM spec (u32s after the 16-byte magic).
static bool LoadIqmTris(const fs::path &path, std::vector<std::array<qvec3f, 3>> &tris)
{
    fs::data d = fs::load(path);
    if (!d || d->size() < 124)
        return false;
    const uint8_t *p = d->data();
    const size_t n = d->size();
    if (std::memcmp(p, "INTERQUAKEMODEL\0", 16) != 0)
        return false;
    if (rd_u32(p, 16) != 2) // version
        return false;

    const uint32_t num_va = rd_u32(p, 44), num_vtx = rd_u32(p, 48), ofs_va = rd_u32(p, 52);
    const uint32_t num_tri = rd_u32(p, 56), ofs_tri = rd_u32(p, 60);

    // find the float3 IQM_POSITION (type 0, format 7=FLOAT, size 3) vertex array
    size_t pos_ofs = 0;
    bool have_pos = false;
    for (uint32_t i = 0; i < num_va; i++) {
        const size_t b = (size_t)ofs_va + (size_t)i * 20;
        if (b + 20 > n)
            break;
        if (rd_u32(p, b) == 0 && rd_u32(p, b + 8) == 7 && rd_u32(p, b + 12) == 3) {
            pos_ofs = rd_u32(p, b + 16);
            have_pos = true;
            break;
        }
    }
    if (!have_pos)
        return false;
    if (pos_ofs + (size_t)num_vtx * 12 > n || (size_t)ofs_tri + (size_t)num_tri * 12 > n)
        return false;

    auto vpos = [&](uint32_t idx) {
        const size_t o = pos_ofs + (size_t)idx * 12;
        return qvec3f(rd_f32(p, o), rd_f32(p, o + 4), rd_f32(p, o + 8));
    };
    for (uint32_t t = 0; t < num_tri; t++) {
        const size_t b = (size_t)ofs_tri + (size_t)t * 12;
        const uint32_t a = rd_u32(p, b), c = rd_u32(p, b + 4), e = rd_u32(p, b + 8);
        if (a < num_vtx && c < num_vtx && e < num_vtx)
            tris.push_back({vpos(a), vpos(c), vpos(e)});
    }
    return !tris.empty();
}

// Minimal IQM (v2) ORDERED vertex reader: the base-frame IQM_POSITION float3 array in its
// global vertex order (index i == FTE's opos[i]), so an engine can map one baked color per
// vertex straight back by index. Used by the static-prop vertex-lighting bake (below).
static bool LoadIqmVerts(const fs::path &path, std::vector<qvec3f> &verts)
{
    fs::data d = fs::load(path);
    if (!d || d->size() < 124)
        return false;
    const uint8_t *p = d->data();
    const size_t n = d->size();
    if (std::memcmp(p, "INTERQUAKEMODEL\0", 16) != 0)
        return false;
    if (rd_u32(p, 16) != 2) // version
        return false;

    const uint32_t num_va = rd_u32(p, 44), num_vtx = rd_u32(p, 48), ofs_va = rd_u32(p, 52);

    size_t pos_ofs = 0;
    bool have_pos = false;
    for (uint32_t i = 0; i < num_va; i++) {
        const size_t b = (size_t)ofs_va + (size_t)i * 20;
        if (b + 20 > n)
            break;
        if (rd_u32(p, b) == 0 && rd_u32(p, b + 8) == 7 && rd_u32(p, b + 12) == 3) {
            pos_ofs = rd_u32(p, b + 16);
            have_pos = true;
            break;
        }
    }
    if (!have_pos || pos_ofs + (size_t)num_vtx * 12 > n)
        return false;

    verts.reserve(num_vtx);
    for (uint32_t i = 0; i < num_vtx; i++) {
        const size_t o = pos_ofs + (size_t)i * 12;
        verts.push_back(qvec3f(rd_f32(p, o), rd_f32(p, o + 4), rd_f32(p, o + 8)));
    }
    return !verts.empty();
}

// Load a prop mesh, dispatching on file extension (.iqm -> IQM, else Wavefront .obj).
static bool LoadMeshTris(const fs::path &path, std::vector<std::array<qvec3f, 3>> &tris)
{
    std::string ext = path.extension().string();
    for (char &c : ext)
        c = (char)std::tolower((unsigned char)c);
    if (ext == ".iqm")
        return LoadIqmTris(path, tris);
    return LoadObjTris(path, tris);
}

// Split a "prop_static prop_detail,foo" style list into a set of classnames.
static void AddClasses(const std::string &s, std::set<std::string> &out)
{
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) {
                out.insert(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.insert(cur);
}

void GatherMeshOccluders(std::vector<polylib::winding3f_t> &out)
{
    int nmesh = 0;
    size_t ntris = 0;

    // Classnames that cast baked mesh shadows: the dedicated _light_mesh entity, plus any
    // configured via -propshadowclasses (or the _propshadowclasses worldspawn key), so a mod's
    // existing prop_static/prop_detail/... entities cast directly with no per-entity edits.
    std::set<std::string> castset;
    castset.insert("_light_mesh");
    AddClasses(light_options.propshadowclasses.value(), castset);
    if (!GetEntdicts().empty() && GetEntdicts().at(0).get("classname") == "worldspawn")
        AddClasses(GetEntdicts().at(0).get("_propshadowclasses"), castset);

    for (const entdict_t &ent : GetEntdicts()) {
        if (castset.find(ent.get("classname")) == castset.end())
            continue;

        const std::string model = ent.get("model");
        if (model.empty())
            continue;

        qvec3f origin_f{};
        ent.get_vector("origin", origin_f);
        qvec3f angles_f{};
        ent.get_vector("angles", angles_f);
        const qvec3d origin = qvec3d(origin_f);

        double scale = 1.0;
        if (!ent.get("_scale").empty())
            scale = std::atof(ent.get("_scale").c_str());
        else if (!ent.get("scale").empty())
            scale = std::atof(ent.get("scale").c_str());
        if (scale == 0.0)
            scale = 1.0;

        std::vector<std::array<qvec3f, 3>> tris;
        if (!LoadMeshTris(model, tris)) {
            logging::print("WARNING: _light_mesh: could not load or empty mesh '{}'\n", model);
            continue;
        }

        // Same convention as qbsp's misc_external_map: angles = (pitch, yaw, roll).
        const double pitch = DEG2RAD(angles_f[0]);
        const double yaw = DEG2RAD(angles_f[1]);
        const double roll = DEG2RAD(angles_f[2]);
        const qmat3x3d rotation = RotateAboutZ(yaw) * RotateAboutY(pitch) * RotateAboutX(roll);

        for (const std::array<qvec3f, 3> &t : tris) {
            qvec3f w[3];
            for (int i = 0; i < 3; i++) {
                qvec3d v = qvec3d(t[i]) * scale; // scale
                v = rotation * v; // rotate
                v = v + origin; // translate
                w[i] = qvec3f(v);
            }
            out.push_back(polylib::winding3f_t{w[0], w[1], w[2]});
            ntris++;
        }
        nmesh++;
    }

    if (nmesh > 0)
        logging::print("{} _light_mesh occluder(s), {} triangles added to the shadow scene\n", nmesh, ntris);
}

void GatherPropVertexLights(const mbsp_t *bsp, std::vector<proplight_record_t> &out)
{
    // Same classname set as the mesh-shadow occluders, so baked shadows and baked vertex
    // lighting agree: the dedicated _light_mesh entity, plus -propshadowclasses / the
    // _propshadowclasses worldspawn key.
    std::set<std::string> castset;
    castset.insert("_light_mesh");
    AddClasses(light_options.propshadowclasses.value(), castset);
    if (!GetEntdicts().empty() && GetEntdicts().at(0).get("classname") == "worldspawn")
        AddClasses(GetEntdicts().at(0).get("_propshadowclasses"), castset);

    size_t nprop = 0, nvtx = 0;

    for (const entdict_t &ent : GetEntdicts()) {
        if (castset.find(ent.get("classname")) == castset.end())
            continue;

        const std::string model = ent.get("model");
        if (model.empty())
            continue;

        // Per-vertex lighting is IQM-only: FTE renders IQM in-engine and applies the colors
        // by vertex index. (.obj props remain shadow-only via GatherMeshOccluders.)
        std::string ext = fs::path(model).extension().string();
        for (char &c : ext)
            c = (char)std::tolower((unsigned char)c);
        if (ext != ".iqm")
            continue;

        std::vector<qvec3f> verts;
        if (!LoadIqmVerts(model, verts)) {
            logging::print("WARNING: propvertexlight: could not load IQM vertices '{}'\n", model);
            continue;
        }

        qvec3f origin_f{};
        ent.get_vector("origin", origin_f);
        qvec3f angles_f{};
        ent.get_vector("angles", angles_f);
        const qvec3d origin = qvec3d(origin_f);

        double scale = 1.0;
        if (!ent.get("_scale").empty())
            scale = std::atof(ent.get("_scale").c_str());
        else if (!ent.get("scale").empty())
            scale = std::atof(ent.get("scale").c_str());
        if (scale == 0.0)
            scale = 1.0;

        // Same placement convention as _light_mesh / qbsp's misc_external_map: angles = (pitch, yaw, roll).
        const double pitch = DEG2RAD(angles_f[0]);
        const double yaw = DEG2RAD(angles_f[1]);
        const double roll = DEG2RAD(angles_f[2]);
        const qmat3x3d rotation = RotateAboutZ(yaw) * RotateAboutY(pitch) * RotateAboutX(roll);

        proplight_record_t rec;
        rec.origin = origin_f;
        rec.angles = angles_f;
        rec.model = model;
        rec.vertexcolors.reserve(verts.size());

        for (const qvec3f &v : verts) {
            qvec3d w = qvec3d(v) * scale; // scale
            w = rotation * w; // rotate
            w = w + origin; // translate to world
            const lightgrid_samples_t s = FixPointAndCalcLightgrid(bsp, qvec3f(w));
            if (s.occluded) // buried vertex the fixup couldn't rescue: neutral, normalizes near 1.0
                rec.vertexcolors.push_back(qvec3b{128, 128, 128});
            else
                rec.vertexcolors.push_back(round_to_int(s.samples_by_style[0].undirectional_color));
        }

        out.push_back(std::move(rec));
        nprop++;
        nvtx += verts.size();
    }

    if (nprop > 0)
        logging::print("{} prop vertex-light record(s), {} vertices baked for RGBPROPLIGHT\n", nprop, nvtx);
}

} // namespace lightmesh
