// ===========================================================================
// FlowMesh.cpp — dual-domain thickness pairing for the Hele-Shaw flow mesh.
// See FlowMesh.h. Uses the standalone Flow::MeshGrid ray accelerator; casts
// each facet's inward-normal ray against the shot's own soup and keeps the
// nearest anti-parallel (opposing-wall) hit as the local wall thickness.
// ===========================================================================

#include "FlowMesh.h"
#include "MeshGrid.h"

#include <cmath>
#include <algorithm>
#include <vector>
#include <map>
#include <array>
#include <limits>

namespace Flow
{
    namespace
    {
        constexpr size_t kStride = 6;   // xyz + nxyz per vertex

        inline glm::vec3 posOf(const std::vector<float>& pn, unsigned int i)
        {
            return glm::vec3(pn[i*kStride+0], pn[i*kStride+1], pn[i*kStride+2]);
        }
        inline glm::vec3 nrmOf(const std::vector<float>& pn, unsigned int i)
        {
            return glm::vec3(pn[i*kStride+3], pn[i*kStride+4], pn[i*kStride+5]);
        }

        // Outward facet normal, matching BuildFaceDraftSamples' convention:
        // geometric normal oriented by the summed vertex normals. Returns false
        // for a degenerate triangle.
        bool facetNormal(const std::vector<float>& pn,
                         unsigned int a, unsigned int b, unsigned int c,
                         glm::vec3& outN, glm::vec3& outCtr)
        {
            const glm::vec3 p0 = posOf(pn, a), p1 = posOf(pn, b), p2 = posOf(pn, c);
            const glm::vec3 cr = glm::cross(p1 - p0, p2 - p0);
            const float len = std::sqrt(cr.x*cr.x + cr.y*cr.y + cr.z*cr.z);
            if (len <= 1.0e-12f) return false;
            glm::vec3 nrm = cr / len;
            const glm::vec3 vn = nrmOf(pn, a) + nrmOf(pn, b) + nrmOf(pn, c);
            if (glm::dot(nrm, vn) < 0.0f) nrm = -nrm;
            outN = nrm;
            outCtr = (p0 + p1 + p2) / 3.0f;
            return true;
        }
    } // namespace

    bool BuildFlowMesh(const std::vector<float>& posNorm,
                       const std::vector<unsigned int>& indices,
                       const FlowMeshParams& params,
                       FlowMesh& out,
                       FlowMeshStats* stats)
    {
        out = FlowMesh{};
        if (stats) *stats = FlowMeshStats{};
        if (posNorm.size() < kStride || indices.size() < 3) return false;
        const size_t vcount = posNorm.size() / kStride;
        const size_t ntri   = indices.size() / 3;

        // The ray accelerator needs a plain xyz soup (drop the normals).
        std::vector<float> xyz(vcount * 3);
        for (size_t v = 0; v < vcount; ++v)
        {
            xyz[v*3+0] = posNorm[v*kStride+0];
            xyz[v*3+1] = posNorm[v*kStride+1];
            xyz[v*3+2] = posNorm[v*kStride+2];
        }
        MeshGrid grid;
        grid.build(xyz, indices);

        // Auto thickness cap: rays longer than a fraction of the model diagonal
        // have shot across a hollow / out an opening rather than to the far wall.
        float maxThick = params.maxThicknessMm;
        if (maxThick <= 0.0f)
        {
            const glm::vec3 d = grid.bmax - grid.bmin;
            const float diag = std::sqrt(glm::dot(d, d));
            maxThick = diag * 0.5f + 1.0e-3f;
        }
        const float minThick   = std::max(params.minThicknessMm, 0.0f);
        const float opposeDot  = params.minOpposeDot;
        const float rayEps     = std::max(params.rayEpsilon, 0.0f);

        // Precompute per-facet outward normals once (reused by the accept
        // predicate to test the candidate hit triangle's orientation).
        std::vector<glm::vec3> facetN(ntri, glm::vec3(0.0f));
        std::vector<glm::vec3> facetC(ntri, glm::vec3(0.0f));
        std::vector<unsigned char> facetOk(ntri, 0);
        for (size_t t = 0; t < ntri; ++t)
        {
            const unsigned int a = indices[t*3], b = indices[t*3+1], c = indices[t*3+2];
            if (a >= vcount || b >= vcount || c >= vcount) continue;
            glm::vec3 n, ctr;
            if (facetNormal(posNorm, a, b, c, n, ctr))
            { facetN[t] = n; facetC[t] = ctr; facetOk[t] = 1; }
        }

        // Mirror the input soup so the caller can render it with the overlay.
        out.posNorm = posNorm;
        out.indices = indices;
        out.thicknessMm.assign(ntri, 0.0f);
        out.halfGapMm.assign(ntri, 0.0f);
        out.paired.assign(ntri, 0);
        out.opposite.assign(ntri, -1);

        int   pairedCount = 0;
        float minMm = 1e30f, maxMm = 0.0f;
        double sumMm = 0.0;

        for (size_t t = 0; t < ntri; ++t)
        {
            if (!facetOk[t]) continue;
            const glm::vec3 n   = facetN[t];      // outward
            const glm::vec3 dir = -n;             // inward, into the plastic
            const glm::vec3 o   = facetC[t] + dir * rayEps;

            // Accept only a candidate whose outward normal is anti-parallel to
            // this facet's (the true opposing wall). Reject the origin triangle.
            const size_t self = t;
            auto accept = [&](int tri) -> bool
            {
                if ((size_t)tri == self) return false;
                if (!facetOk[tri]) return false;
                return glm::dot(facetN[tri], n) <= -opposeDot;
            };

            float tHit = 0.0f;
            const int hit = grid.nearestHit(o, dir, 0.0f, accept, tHit);
            if (hit < 0) continue;

            // Distance from the true facet centroid (add back the epsilon skip).
            const float thick = tHit + rayEps;
            if (thick < minThick || thick > maxThick) continue;

            out.thicknessMm[t] = thick;
            out.halfGapMm[t]   = 0.5f * thick;
            out.paired[t]      = 1;
            out.opposite[t]    = hit;

            ++pairedCount;
            minMm = std::min(minMm, thick);
            maxMm = std::max(maxMm, thick);
            sumMm += (double)thick;
        }

        if (stats)
        {
            stats->totalFacets    = (int)ntri;
            stats->pairedFacets   = pairedCount;
            stats->unpairedFacets = (int)ntri - pairedCount;
            stats->minThicknessMm  = (pairedCount > 0) ? minMm : 0.0f;
            stats->maxThicknessMm  = maxMm;
            stats->meanThicknessMm = (pairedCount > 0) ? (float)(sumMm / pairedCount) : 0.0f;
            stats->pairedFraction  = (ntri > 0) ? (float)pairedCount / (float)ntri : 0.0f;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Dual-domain FlowMesh -> single-wall mid-surface SolveMesh.
    // -----------------------------------------------------------------------
    bool BuildSolveMesh(const FlowMesh& fm,
                        const std::vector<glm::vec3>& gatePoints,
                        const glm::vec3& drawAxis,
                        SolveMesh& out,
                        SolveMeshStats* stats)
    {
        out = SolveMesh{};
        if (stats) *stats = SolveMeshStats{};
        const size_t ntri = fm.triCount();
        if (ntri < 1 || fm.posNorm.size() < kStride) return false;

        const std::vector<float>&        PN = fm.posNorm;
        const std::vector<unsigned int>& I  = fm.indices;
        const size_t vcount = PN.size() / kStride;

        glm::vec3 draw = drawAxis;
        { const float L = std::sqrt(glm::dot(draw, draw));
          draw = (L > 1e-12f) ? draw / L : glm::vec3(0.0f, 1.0f, 0.0f); }

        // Weld tolerance from the bounding box.
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (size_t v = 0; v < vcount; ++v)
        {
            const glm::vec3 p = posOf(PN, v);
            lo = glm::min(lo, p); hi = glm::max(hi, p);
        }
        const glm::vec3 d = hi - lo;
        const float diag = std::sqrt(glm::dot(d, d));
        const double q = std::max(1.0e-5, (double)diag * 1.0e-6);   // snap cell

        // Weld shot vertices by position -> welded node ids; accumulate the
        // outward normal per welded node (from the soup's per-vertex normals).
        std::map<std::array<long long, 3>, int> weldMap;
        std::vector<int>       soup2weld(vcount, -1);
        std::vector<glm::dvec3> wPos;      // welded positions (summed, then averaged)
        std::vector<int>        wCount;
        std::vector<glm::dvec3> wNrm;      // outward normal accum
        wPos.reserve(vcount); wNrm.reserve(vcount); wCount.reserve(vcount);

        auto weldOf = [&](unsigned int v) -> int
        {
            const glm::vec3 p = posOf(PN, v);
            std::array<long long, 3> key{
                (long long)std::llround(p.x / q),
                (long long)std::llround(p.y / q),
                (long long)std::llround(p.z / q) };
            auto it = weldMap.find(key);
            int id;
            if (it == weldMap.end())
            {
                id = (int)wPos.size();
                weldMap.emplace(key, id);
                wPos.push_back(glm::dvec3(p));
                wNrm.push_back(glm::dvec3(0.0));
                wCount.push_back(1);
            }
            else
            {
                id = it->second;
                wPos[id] += glm::dvec3(p);
                wCount[id] += 1;
            }
            wNrm[id] += glm::dvec3(nrmOf(PN, v));
            return id;
        };

        std::vector<glm::ivec3> triWeld(ntri, glm::ivec3(-1));
        for (size_t t = 0; t < ntri; ++t)
        {
            const unsigned int a = I[t*3], b = I[t*3+1], c = I[t*3+2];
            if (a >= vcount || b >= vcount || c >= vcount) continue;
            triWeld[t] = glm::ivec3(weldOf(a), weldOf(b), weldOf(c));
        }
        const int wN = (int)wPos.size();
        for (int i = 0; i < wN; ++i)
        {
            if (wCount[i] > 0) wPos[i] /= (double)wCount[i];
            const double nl = std::sqrt(glm::dot(wNrm[i], wNrm[i]));
            wNrm[i] = (nl > 1e-12) ? wNrm[i] / nl : glm::dvec3(0.0);
        }

        // Choose one representative facet per wall pair: the +draw side (its
        // centroid farther along the draw axis); tie-break by index so exactly
        // one of a pair is kept.
        std::vector<char> consumed(ntri, 0);
        std::vector<int>  reps;
        reps.reserve(ntri / 2 + 1);

        auto centroidDot = [&](size_t t) -> double
        {
            const glm::ivec3 w = triWeld[t];
            if (w.x < 0) return 0.0;
            const glm::dvec3 ctr = (wPos[w.x] + wPos[w.y] + wPos[w.z]) / 3.0;
            return glm::dot(ctr, glm::dvec3(draw));
        };

        int paired = 0;
        for (size_t t = 0; t < ntri; ++t)
        {
            if (t < fm.paired.size() && fm.paired[t]) ++paired;
            if (consumed[t] || triWeld[t].x < 0) continue;
            if (t >= fm.paired.size() || !fm.paired[t]) continue;  // drop unpaired
            const int o = (t < fm.opposite.size()) ? fm.opposite[t] : -1;

            int rep = (int)t;
            if (o >= 0 && o < (int)ntri && !consumed[o] && triWeld[o].x >= 0)
            {
                const double dt = centroidDot(t), doo = centroidDot((size_t)o);
                if (doo > dt + 1e-9) rep = o;                 // pick +draw side
                else if (std::fabs(doo - dt) <= 1e-9 && o < (int)t) rep = o;
                consumed[o] = 1;
            }
            consumed[t] = 1;
            reps.push_back(rep);
        }
        if (reps.empty()) return false;   // nothing paired -> no flow domain

        // Per welded node: average half-gap over the representative facets that
        // use it (the gap that both offsets the node and feeds the solver).
        std::vector<double> nodeGapSum(wN, 0.0);
        std::vector<int>    nodeGapCnt(wN, 0);
        for (int rt : reps)
        {
            const float hg = (rt < (int)fm.halfGapMm.size()) ? fm.halfGapMm[(size_t)rt] : 0.0f;
            const glm::ivec3 w = triWeld[(size_t)rt];
            for (int k = 0; k < 3; ++k)
            { nodeGapSum[w[k]] += hg; nodeGapCnt[w[k]] += 1; }
        }

        // Compact the welded nodes actually used, offset each to the mid-plane.
        std::vector<int> weld2mid(wN, -1);
        for (int rt : reps)
        {
            const glm::ivec3 w = triWeld[(size_t)rt];
            for (int k = 0; k < 3; ++k)
            {
                const int wi = w[k];
                if (weld2mid[wi] >= 0) continue;
                const double hg = (nodeGapCnt[wi] > 0)
                                ? nodeGapSum[wi] / (double)nodeGapCnt[wi] : 0.0;
                const glm::dvec3 mid = wPos[wi] - wNrm[wi] * hg;   // inward to mid-plane
                weld2mid[wi] = (int)out.nodes.size();
                out.nodes.push_back(glm::vec3(mid));
                out.halfGapMm.push_back((float)hg);
            }
        }
        out.tris.reserve(reps.size());
        for (int rt : reps)
        {
            const glm::ivec3 w = triWeld[(size_t)rt];
            out.tris.push_back(glm::ivec3(weld2mid[w.x], weld2mid[w.y], weld2mid[w.z]));
        }

        // Gate nodes: snap each supplied point to the nearest mid-surface node.
        const int mN = (int)out.nodes.size();
        auto nearestNode = [&](const glm::vec3& p) -> int
        {
            int best = -1; float bd = std::numeric_limits<float>::infinity();
            for (int i = 0; i < mN; ++i)
            {
                const glm::vec3 dd = out.nodes[i] - p;
                const float d2 = glm::dot(dd, dd);
                if (d2 < bd) { bd = d2; best = i; }
            }
            return best;
        };
        std::vector<char> gateUsed(mN, 0);
        for (const glm::vec3& gp : gatePoints)
        {
            const int gn = nearestNode(gp);
            if (gn >= 0 && !gateUsed[gn]) { out.gateNodes.push_back(gn); gateUsed[gn] = 1; }
        }
        if (out.gateNodes.empty() && mN > 0)
        {
            // Fallback: the extreme +draw node (a sensible top-centre injection).
            int best = 0; float bestDot = -1e30f;
            for (int i = 0; i < mN; ++i)
            {
                const float dp = glm::dot(out.nodes[i], draw);
                if (dp > bestDot) { bestDot = dp; best = i; }
            }
            out.gateNodes.push_back(best);
        }

        if (stats)
        {
            stats->nodes        = mN;
            stats->tris         = (int)out.tris.size();
            stats->sourceFacets = paired;
            stats->usedFacets   = (int)reps.size();
            stats->droppedUnpaired = (int)ntri - paired;
            stats->gateNode     = out.gateNodes.empty() ? -1 : out.gateNodes.front();
            stats->gateCount    = (int)out.gateNodes.size();
            float gmin = 1e30f, gmax = 0.0f;
            for (float hg : out.halfGapMm) { gmin = std::min(gmin, hg); gmax = std::max(gmax, hg); }
            stats->minGapMm = (mN > 0) ? gmin * 2.0f : 0.0f;   // full gap
            stats->maxGapMm = gmax * 2.0f;
        }
        return !out.tris.empty();
    }

} // namespace Flow
