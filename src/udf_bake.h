// udf_bake.h — bake an unsigned distance field of a triangle mesh into a dense
// grid (world-unit distances). Used as a refraction proxy for the loaded glTF
// object: the water shader sphere-traces this field instead of a bounding sphere.
// A uniform acceleration grid makes the nearest-triangle search fast.
// Zero dependencies (std + math3d.h).
#pragma once
#include "math3d.h"
#include <vector>
#include <thread>
#include <algorithm>
#include <cstdint>

namespace udf {

// Shortest distance from point p to triangle (a,b,c). Real-Time Collision
// Detection closest-point construction.
inline float pointTriangleDist(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return glm::length(ap);
    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return glm::length(bp);
    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return glm::length(cp);
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) { float v = d1 / (d1 - d3); return glm::length(p - (a + ab * v)); }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) { float w = d2 / (d2 - d6); return glm::length(p - (a + ac * w)); }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return glm::length(p - (b + (c - b) * w));
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom, w = vc * denom;
    glm::vec3 proj = a + ab * v + ac * w;          // closest point is on the triangle face
    return glm::length(p - proj);
}

struct Field {
    int res = 0;
    glm::vec3 mn, mx;
    float voxel = 1.0f;
    std::vector<float> data;                       // res^3, x fastest then y then z
};

inline Field bake(const std::vector<glm::vec3>& verts, const std::vector<uint32_t>& idx,
                  const glm::vec3& mn, const glm::vec3& mx, int res) {
    Field f; f.res = res; f.mn = mn; f.mx = mx;
    glm::vec3 ext = mx - mn;
    f.voxel = std::max(ext.x, std::max(ext.y, ext.z)) / float(res);
    float maxd = glm::length(ext);
    f.data.assign((size_t)res * res * res, maxd);
    int ntri = (int)idx.size() / 3;
    if (ntri == 0) return f;

    // ---- build a uniform acceleration grid of triangles ----
    int g = std::max(1, std::min(64, res / 3));
    glm::vec3 cell = ext / (float)g;
    float cellMin = std::max(1e-6f, std::min(cell.x, std::min(cell.y, cell.z)));
    auto clampi = [](int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); };
    auto cellOf = [&](const glm::vec3& p, int& cx, int& cy, int& cz){
        cx = clampi((int)((p.x - mn.x) / std::max(ext.x,1e-6f) * g), 0, g - 1);
        cy = clampi((int)((p.y - mn.y) / std::max(ext.y,1e-6f) * g), 0, g - 1);
        cz = clampi((int)((p.z - mn.z) / std::max(ext.z,1e-6f) * g), 0, g - 1);
    };
    std::vector<std::vector<int>> grid((size_t)g * g * g);
    for (int t = 0; t < ntri; ++t) {
        const glm::vec3& a = verts[idx[(size_t)t*3+0]];
        const glm::vec3& b = verts[idx[(size_t)t*3+1]];
        const glm::vec3& c = verts[idx[(size_t)t*3+2]];
        glm::vec3 tmn = glm::min(a, glm::min(b, c));
        glm::vec3 tmx = glm::max(a, glm::max(b, c));
        int x0,y0,z0,x1,y1,z1; cellOf(tmn,x0,y0,z0); cellOf(tmx,x1,y1,z1);
        for (int z=z0; z<=z1; ++z) for (int y=y0; y<=y1; ++y) for (int x=x0; x<=x1; ++x)
            grid[((size_t)z*g + y)*g + x].push_back(t);
    }

    auto worker = [&](int z0, int z1) {
        std::vector<int> stamp((size_t)ntri, -1);   // per-thread: triangle already tested for this voxel
        int voxelId = 0;
        for (int z = z0; z < z1; ++z)
        for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            glm::vec3 p(mn.x + ext.x * (x + 0.5f) / res,
                        mn.y + ext.y * (y + 0.5f) / res,
                        mn.z + ext.z * (z + 0.5f) / res);
            int cx,cy,cz; cellOf(p, cx, cy, cz);
            float best = maxd;
            ++voxelId;
            for (int r = 0; r < g; ++r) {
                // test the shell of cells at Chebyshev radius r
                int lo[3] = { cx - r, cy - r, cz - r };
                int hi[3] = { cx + r, cy + r, cz + r };
                for (int zz = lo[2]; zz <= hi[2]; ++zz) {
                    if (zz < 0 || zz >= g) continue;
                    bool zEdge = (zz == lo[2] || zz == hi[2]);
                    for (int yy = lo[1]; yy <= hi[1]; ++yy) {
                        if (yy < 0 || yy >= g) continue;
                        bool yEdge = (yy == lo[1] || yy == hi[1]);
                        for (int xx = lo[0]; xx <= hi[0]; ++xx) {
                            if (xx < 0 || xx >= g) continue;
                            bool xEdge = (xx == lo[0] || xx == hi[0]);
                            if (r > 0 && !(xEdge || yEdge || zEdge)) continue; // interior already done
                            for (int t : grid[((size_t)zz*g + yy)*g + xx]) {
                                if (stamp[t] == voxelId) continue;
                                stamp[t] = voxelId;
                                const glm::vec3& a = verts[idx[(size_t)t*3+0]];
                                const glm::vec3& b = verts[idx[(size_t)t*3+1]];
                                const glm::vec3& c = verts[idx[(size_t)t*3+2]];
                                float d = pointTriangleDist(p, a, b, c);
                                if (d < best) best = d;
                            }
                        }
                    }
                }
                // any triangle in an untested cell is at least r*cellMin away
                if (best <= (float)r * cellMin) break;
            }
            f.data[(((size_t)z * res) + y) * res + x] = best;
        }
    };

    unsigned hw = std::thread::hardware_concurrency(); if (hw == 0) hw = 4;
    int nthreads = (int)std::min<unsigned>(hw, (unsigned)res);
    int chunk = (res + nthreads - 1) / nthreads;
    std::vector<std::thread> pool;
    for (int i = 0; i < nthreads; ++i) {
        int z0 = i * chunk, z1 = std::min(res, z0 + chunk);
        if (z0 < z1) pool.emplace_back(worker, z0, z1);
    }
    for (auto& th : pool) th.join();
    return f;
}

} // namespace udf
