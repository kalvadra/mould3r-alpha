#include "MeshSimplify.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace
{
    struct AABB {
        float minx, miny, minz;
        float maxx, maxy, maxz;
    };

    AABB ComputeAABB(const std::vector<float>& verts)
    {
        AABB b;
        b.minx = b.miny = b.minz = std::numeric_limits<float>::infinity();
        b.maxx = b.maxy = b.maxz = -std::numeric_limits<float>::infinity();
        const size_t nv = verts.size() / 3;
        for (size_t i = 0; i < nv; ++i) {
            const float x = verts[i * 3 + 0];
            const float y = verts[i * 3 + 1];
            const float z = verts[i * 3 + 2];
            if (x < b.minx) b.minx = x; if (x > b.maxx) b.maxx = x;
            if (y < b.miny) b.miny = y; if (y > b.maxy) b.maxy = y;
            if (z < b.minz) b.minz = z; if (z > b.maxz) b.maxz = z;
        }
        return b;
    }

    // Pack three 21-bit grid coords into one 64-bit key. 21 bits each gives up
    // to 2,097,152 divisions per axis — far beyond anything we'll use, but
    // cheap and collision-free for our range.
    inline uint64_t CellKey(uint32_t ix, uint32_t iy, uint32_t iz)
    {
        return ((uint64_t)(ix & 0x1FFFFF) << 42)
             | ((uint64_t)(iy & 0x1FFFFF) << 21)
             |  (uint64_t)(iz & 0x1FFFFF);
    }

    // Cluster vertices at grid resolution `N` (cells per axis).
    // Emits averaged cell centroids as output vertices and a remapped index
    // buffer (with degenerate triangles dropped).
    //
    // Returns the number of output triangles (== outIndices.size() / 3).
    size_t ClusterOnce(const std::vector<float>& inVerts,
                       const std::vector<uint32_t>& inIndices,
                       const AABB& aabb,
                       uint32_t N,
                       std::vector<float>& outVerts,
                       std::vector<uint32_t>& outIndices)
    {
        outVerts.clear();
        outIndices.clear();
        if (N == 0) return 0;

        // Avoid zero-sized cells for degenerate (planar / linear) inputs.
        const float sx = std::max(aabb.maxx - aabb.minx, 1e-8f);
        const float sy = std::max(aabb.maxy - aabb.miny, 1e-8f);
        const float sz = std::max(aabb.maxz - aabb.minz, 1e-8f);
        const float cx = sx / (float)N;
        const float cy = sy / (float)N;
        const float cz = sz / (float)N;

        // For each input vertex, pick its cell. Accumulate a running mean
        // per cell and assign an output index on first touch.
        struct Cell { uint32_t outIdx; uint32_t count; float sx, sy, sz; };
        std::unordered_map<uint64_t, Cell> cells;
        cells.reserve(inVerts.size() / 3);

        const size_t nv = inVerts.size() / 3;
        std::vector<uint32_t> remap(nv);

        for (size_t i = 0; i < nv; ++i) {
            const float x = inVerts[i * 3 + 0];
            const float y = inVerts[i * 3 + 1];
            const float z = inVerts[i * 3 + 2];

            // Clamp to [0, N-1] so points exactly on the max face land in the
            // last cell rather than overflowing into "cell N".
            int ix = (int)std::floor((x - aabb.minx) / cx);
            int iy = (int)std::floor((y - aabb.miny) / cy);
            int iz = (int)std::floor((z - aabb.minz) / cz);
            ix = std::clamp(ix, 0, (int)N - 1);
            iy = std::clamp(iy, 0, (int)N - 1);
            iz = std::clamp(iz, 0, (int)N - 1);

            const uint64_t k = CellKey((uint32_t)ix, (uint32_t)iy, (uint32_t)iz);
            auto it = cells.find(k);
            if (it == cells.end()) {
                Cell c;
                c.outIdx = (uint32_t)(outVerts.size() / 3);
                c.count  = 1;
                c.sx = x; c.sy = y; c.sz = z;
                cells.emplace(k, c);
                outVerts.push_back(x);
                outVerts.push_back(y);
                outVerts.push_back(z);
                remap[i] = c.outIdx;
            } else {
                Cell& c = it->second;
                c.count += 1;
                c.sx += x; c.sy += y; c.sz += z;
                remap[i] = c.outIdx;
            }
        }

        // Replace each cell's representative with the cluster centroid.
        for (auto& kv : cells) {
            const Cell& c = kv.second;
            const float inv = 1.0f / (float)c.count;
            outVerts[c.outIdx * 3 + 0] = c.sx * inv;
            outVerts[c.outIdx * 3 + 1] = c.sy * inv;
            outVerts[c.outIdx * 3 + 2] = c.sz * inv;
        }

        // Remap triangles, dropping collapsed (< 3 distinct verts) ones.
        const size_t nt = inIndices.size() / 3;
        outIndices.reserve(nt * 3);
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t a = remap[inIndices[t * 3 + 0]];
            const uint32_t b = remap[inIndices[t * 3 + 1]];
            const uint32_t c = remap[inIndices[t * 3 + 2]];
            if (a == b || b == c || a == c) continue;
            outIndices.push_back(a);
            outIndices.push_back(b);
            outIndices.push_back(c);
        }
        return outIndices.size() / 3;
    }
}

void MeshSimplify::Decimate(const std::vector<float>& inVerts,
                            const std::vector<uint32_t>& inIndices,
                            uint32_t targetTriCount,
                            std::vector<float>& outVerts,
                            std::vector<uint32_t>& outIndices)
{
    const size_t inTriCount = inIndices.size() / 3;

    // No-op paths: disabled, or already at/under target.
    if (targetTriCount == 0 || inTriCount <= (size_t)targetTriCount) {
        outVerts = inVerts;
        outIndices = inIndices;
        return;
    }
    if (inVerts.empty() || inIndices.empty()) {
        outVerts.clear();
        outIndices.clear();
        return;
    }

    const AABB aabb = ComputeAABB(inVerts);

    // Binary search on grid resolution N. Smaller N → coarser grid → fewer
    // output triangles. Bounds:
    //   lo = 2   (pathological lower)
    //   hi = min(1024, cbrt(#inputVerts))  (no point in more cells than verts)
    const uint32_t inVertCount = (uint32_t)(inVerts.size() / 3);
    uint32_t hi = (uint32_t)std::max(2.0, std::cbrt((double)inVertCount));
    hi = std::min<uint32_t>(hi, 1024);
    uint32_t lo = 2;

    // Track the best (closest-to-target) result across iterations so we don't
    // lose it if the search overshoots on its last step.
    std::vector<float>    bestVerts;
    std::vector<uint32_t> bestIndices;
    int64_t               bestDelta = std::numeric_limits<int64_t>::max();

    std::vector<float>    tmpVerts;
    std::vector<uint32_t> tmpIndices;

    const int maxIters = 10;
    for (int iter = 0; iter < maxIters && lo <= hi; ++iter) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const size_t got = ClusterOnce(inVerts, inIndices, aabb, mid,
                                       tmpVerts, tmpIndices);

        const int64_t delta = (int64_t)got - (int64_t)targetTriCount;
        if (std::abs(delta) < std::abs(bestDelta)) {
            bestDelta   = delta;
            bestVerts   = tmpVerts;
            bestIndices = tmpIndices;
        }

        if (got == (size_t)targetTriCount) break;
        if (got > (size_t)targetTriCount) {
            // Too many triangles → need a coarser grid → smaller N.
            if (mid == 0) break;
            hi = mid - 1;
        } else {
            // Too few triangles → finer grid → larger N.
            lo = mid + 1;
        }
    }

    if (bestVerts.empty()) {
        // Search never ran (shouldn't happen given guards above).
        outVerts = inVerts;
        outIndices = inIndices;
        return;
    }

    outVerts   = std::move(bestVerts);
    outIndices = std::move(bestIndices);
}
