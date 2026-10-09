// VolumeBoundary.cpp — see VolumeBoundary.h.
#include "VolumeBoundary.h"
#include "MeshGrid.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace Flow3D
{
    const char* BoundaryTagName(uint8_t tag)
    {
        switch (tag)
        {
        case TagWall:    return "Wall";
        case TagInlet:   return "Inlet";
        case TagVent:    return "Vent";
        case TagParting: return "Parting line";
        default:         return "Interior";
        }
    }

    // ---------------------------------------------------------------------
    struct SolidTester::Impl
    {
        std::vector<float>        xyz;
        std::vector<unsigned int> idx;
        std::vector<glm::vec3>    n;
        Flow::MeshGrid            grid;
    };

    SolidTester::SolidTester() : m(new Impl) {}
    SolidTester::~SolidTester() { delete m; }

    void SolidTester::Build(std::vector<float> xyz, std::vector<unsigned int> indices,
                            std::vector<glm::vec3> triNormals)
    {
        m->xyz = std::move(xyz);
        m->idx = std::move(indices);
        m->n = std::move(triNormals);
        m->n.resize(m->idx.size() / 3, glm::vec3(0.0f));
        m->grid.build(m->xyz, m->idx);   // keeps pointers to the members above
    }

    bool SolidTester::Empty() const { return m->grid.empty(); }

    bool SolidTester::Inside(const glm::dvec3& p) const
    {
        if (m->grid.empty()) return false;
        // Three skew directions (never along an axis, so flat walls and grid
        // planes don't line up with the rays).
        static const glm::vec3 dirs[3] = {
            glm::normalize(glm::vec3(0.5377f, 0.8311f, 0.1419f)),
            glm::normalize(glm::vec3(-0.6015f, 0.2013f, 0.7729f)),
            glm::normalize(glm::vec3(0.3129f, -0.5627f, -0.7650f)) };
        const glm::vec3 o((float)p.x, (float)p.y, (float)p.z);
        int inside = 0, outside = 0;
        for (const glm::vec3& d : dirs)
        {
            float t = 0.0f;
            const int tri = m->grid.nearestHit(o, d, 0.0f, {}, t);
            if (tri >= 0 && glm::dot(m->n[(size_t)tri], d) > 0.0f) ++inside;
            else ++outside;
            if (inside >= 2 || outside >= 2) break;
        }
        return inside >= 2;
    }

    int Boundary::Count(uint8_t tag) const
    {
        int n = 0;
        for (const BoundaryRegion& r : regions)
            if (r.tag == tag) ++n;
        return n;
    }

    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // One face on the mesh surface.
        struct BFace
        {
            uint32_t   slot = 0;        // tet*4 + face
            int32_t    v[3] = { 0, 0, 0 };
            glm::dvec3 c{ 0.0 };        // centroid
            glm::dvec3 n{ 0.0 };        // outward unit normal
            double     area = 0.0;
        };

        glm::dvec3 SafeNormalize(const glm::dvec3& v)
        {
            const double L = glm::length(v);
            return L > 0.0 ? v / L : glm::dvec3(0.0);
        }

        // Union-find over indices.
        struct DSU
        {
            std::vector<int> p;
            explicit DSU(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
            int Find(int x) { while (p[(size_t)x] != x) { p[(size_t)x] = p[(size_t)p[(size_t)x]]; x = p[(size_t)x]; } return x; }
            void Union(int a, int b) { a = Find(a); b = Find(b); if (a != b) p[(size_t)b] = a; }
        };
    }

    Boundary TagBoundary(const TetMesh::Mesh& mesh, const std::vector<int32_t>& nb, const BoundarySpec& spec)
    {
        Boundary B;
        const size_t nt = mesh.TetCount();
        if (nt == 0 || nb.size() != 4 * nt) return B;
        B.slotTag.assign(4 * nt, (uint8_t)TagInterior);
        B.slotRegion.assign(4 * nt, (int16_t)-1);

        // ---- Collect the surface ---------------------------------------------
        std::vector<BFace> F;
        for (size_t i = 0; i < nt; ++i)
            for (int f = 0; f < 4; ++f)
            {
                if (nb[4 * i + (size_t)f] >= 0) continue;
                BFace b;
                b.slot = (uint32_t)(4 * i + (size_t)f);
                const int32_t* t = &mesh.tets[4 * i];
                glm::dvec3 p[3];
                for (int k = 0; k < 3; ++k)
                {
                    b.v[k] = t[TetMesh::kTetFace[f][k]];
                    const double* q = &mesh.verts[3 * (size_t)b.v[k]];
                    p[k] = glm::dvec3(q[0], q[1], q[2]);
                }
                const glm::dvec3 cr = glm::cross(p[1] - p[0], p[2] - p[0]);
                b.area = 0.5 * glm::length(cr);
                b.n = SafeNormalize(cr);
                b.c = (p[0] + p[1] + p[2]) / 3.0;
                F.push_back(b);
            }
        const double tol = std::max(spec.tolMm, 1.0e-6);

        std::vector<int> faceRegion(F.size(), -1);   // index into B.regions
        auto newRegion = [&B](const std::string& label, uint8_t tag, int feedNode, double nominal) -> int
        {
            BoundaryRegion r;
            r.label = label;
            r.tag = tag;
            r.feedNode = feedNode;
            r.nominalAreaMm2 = nominal;
            B.regions.push_back(r);
            return (int)B.regions.size() - 1;
        };
        auto nominalArea = [](double r) { return r > 0.0 ? kPi * r * r : 0.0; };

        // The nearest free face to `pos` (optionally facing along `dir`), for
        // a gate / vent too small for the elements to show its footprint.
        auto nearestFree = [&](const glm::dvec3& pos, const glm::dvec3& dir) -> int
        {
            int best = -1, bestAny = -1;
            double bd = 1e300, bdAny = 1e300;
            for (size_t k = 0; k < F.size(); ++k)
            {
                if (faceRegion[k] >= 0) continue;
                const double d = glm::length(F[k].c - pos);
                if (d < bdAny) { bdAny = d; bestAny = (int)k; }
                if (glm::dot(F[k].n, dir) > 0.0 && d < bd) { bd = d; best = (int)k; }
            }
            return best >= 0 ? best : bestAny;
        };

        // A disk footprint: free faces within `radius` of `pos` facing `dir`.
        auto diskFaces = [&](const glm::dvec3& pos, const glm::dvec3& dir, double radius, double minDot)
        {
            std::vector<int> out;
            for (size_t k = 0; k < F.size(); ++k)
                if (faceRegion[k] < 0 && glm::length(F[k].c - pos) <= radius &&
                    glm::dot(F[k].n, dir) > minDot)
                    out.push_back((int)k);
            return out;
        };

        // ---- Inlets ------------------------------------------------------------
        if (spec.sprueCap && !spec.inlets.empty())
        {
            // The sprue's entry cap: faces facing out along the sprue, inside its
            // radius, at the far end (the flat the nozzle seats on).
            const InletSpec& in = spec.inlets.front();
            const glm::dvec3 ax = SafeNormalize(in.dirOut);
            const double r = std::max(in.radiusMm, tol);
            std::vector<int> cand;
            double sMax = -1e300;
            for (size_t k = 0; k < F.size(); ++k)
            {
                if (glm::dot(F[k].n, ax) < 0.8) continue;
                const glm::dvec3 d = F[k].c - in.pos;
                const double s = glm::dot(d, ax);
                const double radial = glm::length(d - s * ax);
                if (radial > 1.25 * r + tol) continue;
                cand.push_back((int)k);
                sMax = std::max(sMax, s);
            }
            const int reg = newRegion(in.label, TagInlet, in.feedNode, nominalArea(in.radiusMm));
            for (int k : cand)
                if (glm::dot(F[(size_t)k].c - in.pos, ax) >= sMax - (0.1 * r + 4.0 * tol))
                    faceRegion[(size_t)k] = reg;
            if (std::find(faceRegion.begin(), faceRegion.end(), reg) == faceRegion.end())
            {
                const int k = nearestFree(in.pos, ax);
                if (k >= 0) faceRegion[(size_t)k] = reg;
                B.regions[(size_t)reg].byPosition = true;
                B.warnings.push_back(in.label + ": no flat entry face found at the sprue inlet; the nearest face "
                                     "is used instead.");
            }
        }
        else
        {
            std::vector<int> inletReg(spec.inlets.size(), -1);
            for (size_t g = 0; g < spec.inlets.size(); ++g)
                inletReg[g] = newRegion(spec.inlets[g].label, TagInlet, spec.inlets[g].feedNode,
                                        nominalArea(spec.inlets[g].radiusMm));

            // Footprint test: the wall faces with feed material just outside.
            if (spec.coveredByFeed)
            {
                std::vector<int> covered;
                for (size_t k = 0; k < F.size(); ++k)
                    if (spec.coveredByFeed(F[k].c + F[k].n * spec.pushMm)) covered.push_back((int)k);

                // Connected patches (sharing an edge).
                DSU dsu(covered.size());
                std::unordered_map<uint64_t, int> edgeOwner;
                edgeOwner.reserve(covered.size() * 3);
                for (size_t j = 0; j < covered.size(); ++j)
                {
                    const BFace& b = F[(size_t)covered[j]];
                    for (int e = 0; e < 3; ++e)
                    {
                        const uint32_t a = (uint32_t)b.v[e], c = (uint32_t)b.v[(e + 1) % 3];
                        const uint64_t key = ((uint64_t)std::min(a, c) << 32) | std::max(a, c);
                        auto it = edgeOwner.find(key);
                        if (it == edgeOwner.end()) edgeOwner.emplace(key, (int)j);
                        else dsu.Union(it->second, (int)j);
                    }
                }
                std::unordered_map<int, std::vector<int>> patches;
                for (size_t j = 0; j < covered.size(); ++j) patches[dsu.Find((int)j)].push_back(covered[j]);

                // Each patch belongs to the nearest inlet. A patch with no inlet
                // anywhere near is feed material the network doesn't know about
                // (a runner lying on the part, say): its own region.
                int extra = 0;
                for (auto& [root, faces] : patches)
                {
                    (void)root;
                    glm::dvec3 c(0.0);
                    double a = 0.0;
                    for (int k : faces) { c += F[(size_t)k].c * F[(size_t)k].area; a += F[(size_t)k].area; }
                    if (a > 0.0) c /= a;
                    int best = -1;
                    double bd = 1e300;
                    for (size_t g = 0; g < spec.inlets.size(); ++g)
                    {
                        const double d = glm::length(spec.inlets[g].pos - c);
                        const double reach = 3.0 * std::max(spec.inlets[g].radiusMm, 0.5) + std::sqrt(a);
                        if (d <= reach && d < bd) { bd = d; best = (int)g; }
                    }
                    int reg;
                    if (best >= 0)
                        reg = inletReg[(size_t)best];
                    else
                    {
                        reg = newRegion("Feed contact " + std::to_string(++extra), TagInlet, -1, 0.0);
                        B.warnings.push_back("Feed material touches the part away from any gate (" +
                                             B.regions[(size_t)reg].label + "); it is treated as an inlet.");
                    }
                    for (int k : faces) faceRegion[(size_t)k] = reg;
                }
            }

            // Gates the footprint test didn't find: place by position.
            for (size_t g = 0; g < spec.inlets.size(); ++g)
            {
                const int reg = inletReg[g];
                if (std::find(faceRegion.begin(), faceRegion.end(), reg) != faceRegion.end()) continue;
                const InletSpec& in = spec.inlets[g];
                const glm::dvec3 dir = SafeNormalize(in.dirOut);
                std::vector<int> disk = diskFaces(in.pos, dir, 1.2 * in.radiusMm + spec.pushMm, 0.3);
                if (disk.empty())
                {
                    const int k = nearestFree(in.pos, dir);
                    if (k >= 0) disk.push_back(k);
                }
                for (int k : disk) faceRegion[(size_t)k] = reg;
                B.regions[(size_t)reg].byPosition = true;
                B.warnings.push_back(in.label + ": the gate's footprint wasn't found on the mesh (a gate "
                                     "smaller than the elements, or not touching the part); it is placed "
                                     "by position instead.");
            }
        }

        // ---- Vents --------------------------------------------------------------
        for (const VentSpec& v : spec.vents)
        {
            const int reg = newRegion(v.label, TagVent, v.feedNode, 0.0);
            const glm::dvec3 dir = SafeNormalize(v.dirOut);
            std::vector<int> disk = diskFaces(v.pos, dir, 1.1 * v.halfWidthMm + spec.pushMm, 0.3);
            if (disk.empty())
            {
                const int k = nearestFree(v.pos, dir);
                if (k >= 0) disk.push_back(k);
                B.regions[(size_t)reg].byPosition = true;
            }
            for (int k : disk) faceRegion[(size_t)k] = reg;
        }

        // ---- Write tags: regions, then parting line, then wall ------------------
        const glm::dvec3 ax = SafeNormalize(spec.drawAxis);
        for (size_t k = 0; k < F.size(); ++k)
        {
            const BFace& b = F[k];
            uint8_t tag = TagWall;
            const int reg = faceRegion[k];
            if (reg >= 0)
            {
                tag = B.regions[(size_t)reg].tag;
                BoundaryRegion& R = B.regions[(size_t)reg];
                R.faces += 1;
                R.areaMm2 += b.area;
                R.centre += b.c * b.area;
                B.slotRegion[b.slot] = (int16_t)reg;
            }
            else if (glm::length(ax) > 0.0)
            {
                // Crosses (or touches, from one side) the parting plane — but not
                // a face lying flat in it.
                double lo = 1e300, hi = -1e300;
                for (int e = 0; e < 3; ++e)
                {
                    const double* q = &mesh.verts[3 * (size_t)b.v[e]];
                    const double s = glm::dot(glm::dvec3(q[0], q[1], q[2]), ax) - spec.partingOffset;
                    lo = std::min(lo, s);
                    hi = std::max(hi, s);
                }
                if (lo <= tol && hi >= -tol && hi - lo > tol) tag = TagParting;
            }
            B.slotTag[b.slot] = tag;
            B.faces[tag] += 1;
            B.areaMm2[tag] += b.area;
        }
        for (BoundaryRegion& R : B.regions)
            if (R.areaMm2 > 0.0) R.centre /= R.areaMm2;

        // Regions that ended up empty (e.g. a vent whose faces a gate took).
        for (const BoundaryRegion& R : B.regions)
            if (R.faces == 0)
                B.warnings.push_back(R.label + ": no faces could be assigned.");
        return B;
    }
}
