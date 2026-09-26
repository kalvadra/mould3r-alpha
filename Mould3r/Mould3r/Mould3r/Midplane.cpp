// ===========================================================================
// Midplane.cpp — planform midplane of a moulded part. See Midplane.h.
//
// Coordinates: plan view is (u, v) = (world X, world Z); the pull axis is Y.
// ===========================================================================

#include "Midplane.h"

#include <clipper2/clipper.h>
#include <CDT.h>              // header-only (vcpkg "cdt" default)

#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <utility>

namespace Flow
{
    namespace
    {
        using dvec2 = glm::dvec2;
        using dvec3 = glm::dvec3;
        using Edge2 = std::pair<std::uint32_t, std::uint32_t>;

        constexpr double kPi = 3.14159265358979323846;

        inline double Cross2(const dvec2& a, const dvec2& b) { return a.x * b.y - a.y * b.x; }
        inline std::uint64_t EdgeKey(std::uint32_t a, std::uint32_t b)
        {
            if (a > b) std::swap(a, b);
            return ((std::uint64_t)a << 32) | (std::uint64_t)b;
        }

        // ---- Uniform 2D bucket grid ------------------------------------------
        struct Grid2
        {
            double x0 = 0.0, y0 = 0.0, cell = 1.0;
            int nx = 1, ny = 1;
            std::vector<std::vector<int>> cells;

            void init(double minx, double miny, double maxx, double maxy, double c, int maxDim = 1024)
            {
                const double span = std::max(maxx - minx, maxy - miny);
                cell = std::max(c, span / (double)maxDim);
                if (!(cell > 0.0)) cell = 1.0;
                x0 = minx; y0 = miny;
                nx = std::max(1, (int)std::floor((maxx - minx) / cell) + 1);
                ny = std::max(1, (int)std::floor((maxy - miny) / cell) + 1);
                cells.assign((size_t)nx * (size_t)ny, {});
            }
            int ix(double x) const { return std::min(nx - 1, std::max(0, (int)std::floor((x - x0) / cell))); }
            int iy(double y) const { return std::min(ny - 1, std::max(0, (int)std::floor((y - y0) / cell))); }
            void insert(int id, double ax, double ay, double bx, double by)
            {
                const int i0 = ix(std::min(ax, bx)), i1 = ix(std::max(ax, bx));
                const int j0 = iy(std::min(ay, by)), j1 = iy(std::max(ay, by));
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i)
                        cells[(size_t)j * nx + i].push_back(id);
            }
            const std::vector<int>& at(double x, double y) const
            { return cells[(size_t)iy(y) * nx + ix(x)]; }
        };

        // ---- Source surface, prepared for Y-column queries -------------------
        struct SrcTri
        {
            dvec3  p[3];
            double area2 = 0.0;   // signed 2x projected area in (x, z)
            double ny    = 0.0;   // |unit normal . Y|
        };

        struct Column
        {
            bool   ok = false, multi = false, odd = false;
            double y0 = 0.0, y1 = 0.0;      // chosen material interval
            double cBot = 1.0, cTop = 1.0;  // |n_y| of the walls bounding it
        };

        struct Hit { double y; int tri; double nySigned; };

        Column SampleColumn(const std::vector<SrcTri>& T, const std::vector<double>& nySigned,
                            const Grid2& g, double u, double v, double yTol,
                            std::vector<Hit>& hits)
        {
            Column c;
            hits.clear();
            for (int id : g.at(u, v))
            {
                const SrcTri& t = T[(size_t)id];
                const double x0 = t.p[0].x, z0 = t.p[0].z;
                const double w1 = ((u - x0) * (t.p[2].z - z0) - (t.p[2].x - x0) * (v - z0)) / t.area2;
                const double w2 = ((t.p[1].x - x0) * (v - z0) - (u - x0) * (t.p[1].z - z0)) / t.area2;
                const double w0 = 1.0 - w1 - w2;
                const double eps = -1.0e-9;
                if (w0 < eps || w1 < eps || w2 < eps) continue;
                const double y = w0 * t.p[0].y + w1 * t.p[1].y + w2 * t.p[2].y;
                hits.push_back({ y, id, nySigned[(size_t)id] });
            }
            if (hits.size() < 2) return c;
            std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.y < b.y; });

            // A column through a shared edge / vertex hits every triangle there at
            // the same y: keep one per surface (same facing), but never merge an
            // entry with an exit (opposite facing) at a knife edge.
            std::vector<Hit> uniq;
            uniq.reserve(hits.size());
            for (const Hit& h : hits)
            {
                if (!uniq.empty() && std::fabs(h.y - uniq.back().y) <= yTol &&
                    ((h.nySigned >= 0.0) == (uniq.back().nySigned >= 0.0)))
                    continue;
                uniq.push_back(h);
            }
            if (uniq.size() < 2) return c;

            size_t lo = 0, hi = 1;
            if (uniq.size() % 2 != 0)
            {
                c.odd = true;                       // open / non-manifold surface
                lo = 0; hi = uniq.size() - 1;
            }
            else
            {
                double best = -1.0;
                for (size_t k = 0; k + 1 < uniq.size(); k += 2)
                {
                    const double len = uniq[k + 1].y - uniq[k].y;
                    if (len > best) { best = len; lo = k; hi = k + 1; }
                }
                c.multi = uniq.size() > 2;
            }
            c.y0 = uniq[lo].y; c.y1 = uniq[hi].y;
            c.cBot = T[(size_t)uniq[lo].tri].ny;
            c.cTop = T[(size_t)uniq[hi].tri].ny;
            c.ok = (c.y1 - c.y0) > 0.0;
            return c;
        }

        // ---- Outline cleanup + corner-preserving resample --------------------
        std::vector<dvec2> CleanLoop(const Clipper2Lib::PathD& path, double eps)
        {
            std::vector<dvec2> L;
            L.reserve(path.size());
            for (const auto& p : path)
            {
                const dvec2 q(p.x, p.y);
                if (L.empty() || glm::length(q - L.back()) > eps) L.push_back(q);
            }
            while (L.size() > 1 && glm::length(L.front() - L.back()) <= eps) L.pop_back();

            // Drop collinear points (straight runs come out of the triangle union
            // with a vertex at every original mesh vertex along the edge).
            bool changed = true;
            while (changed && L.size() > 3)
            {
                changed = false;
                std::vector<dvec2> M;
                M.reserve(L.size());
                const size_t n = L.size();
                for (size_t i = 0; i < n; ++i)
                {
                    const dvec2 a = L[(i + n - 1) % n], b = L[i], c = L[(i + 1) % n];
                    const dvec2 d1 = b - a, d2 = c - b;
                    const double l1 = glm::length(d1), l2 = glm::length(d2);
                    if (l1 > 0.0 && l2 > 0.0 &&
                        std::fabs(Cross2(d1, d2)) / (l1 * l2) < 1.0e-4 && glm::dot(d1, d2) > 0.0)
                    { changed = true; continue; }
                    M.push_back(b);
                }
                if (M.size() >= 3) L.swap(M); else changed = false;
            }
            return L;
        }

        dvec2 PointAtArc(const std::vector<dvec2>& run, const std::vector<double>& cum, double t)
        {
            if (t <= 0.0) return run.front();
            for (size_t k = 0; k + 1 < run.size(); ++k)
                if (t <= cum[k + 1])
                {
                    const double seg = cum[k + 1] - cum[k];
                    const double f = (seg > 0.0) ? (t - cum[k]) / seg : 0.0;
                    return run[k] + (run[k + 1] - run[k]) * f;
                }
            return run.back();
        }

        // Resample a closed loop at spacing <= s, keeping every corner (turn >
        // cornerDeg) exactly. Output: loop points, first not repeated.
        void ResampleLoop(const std::vector<dvec2>& L, double s, double cornerDeg,
                          std::vector<dvec2>& out)
        {
            out.clear();
            const size_t n = L.size();
            if (n < 3) return;
            const double cosC = std::cos(cornerDeg * kPi / 180.0);
            std::vector<size_t> corners;
            for (size_t i = 0; i < n; ++i)
            {
                const dvec2 d1 = L[i] - L[(i + n - 1) % n], d2 = L[(i + 1) % n] - L[i];
                const double l1 = glm::length(d1), l2 = glm::length(d2);
                if (l1 <= 0.0 || l2 <= 0.0) continue;
                if (glm::dot(d1, d2) / (l1 * l2) < cosC) corners.push_back(i);
            }

            auto emitRun = [&](size_t a, size_t count)   // run of `count` segments from a
            {
                std::vector<dvec2> run;
                run.reserve(count + 1);
                for (size_t k = 0; k <= count; ++k) run.push_back(L[(a + k) % n]);
                std::vector<double> cum(run.size(), 0.0);
                for (size_t k = 1; k < run.size(); ++k) cum[k] = cum[k - 1] + glm::length(run[k] - run[k - 1]);
                const double len = cum.back();
                const int pieces = std::max(1, (int)std::ceil(len / s - 1.0e-9));
                out.push_back(run.front());
                for (int k = 1; k < pieces; ++k) out.push_back(PointAtArc(run, cum, len * k / pieces));
            };

            if (corners.empty())
            {
                // Smooth loop (e.g. a circle): treat as one run, at least 3 pieces.
                std::vector<dvec2> run(L.begin(), L.end());
                run.push_back(L.front());
                std::vector<double> cum(run.size(), 0.0);
                for (size_t k = 1; k < run.size(); ++k) cum[k] = cum[k - 1] + glm::length(run[k] - run[k - 1]);
                const double len = cum.back();
                const int pieces = std::max(3, (int)std::ceil(len / s - 1.0e-9));
                for (int k = 0; k < pieces; ++k) out.push_back(PointAtArc(run, cum, len * k / pieces));
                return;
            }
            for (size_t c = 0; c < corners.size(); ++c)
            {
                const size_t a = corners[c];
                const size_t b = corners[(c + 1) % corners.size()];
                const size_t count = (corners.size() == 1) ? n : (b + n - a) % n;
                emitRun(a, count);
            }
        }

        // ---- Footprint union ---------------------------------------------------
        // Union of every projected triangle. One Clipper call over hundreds of
        // thousands of tiny triangles is slow (~18 s for 640k); sorting them into
        // spatial cells, unioning local batches, then merging the batch results
        // pairwise collapses interior edges early and is ~18x faster with the
        // identical result.
        Clipper2Lib::PathsD FootprintUnion(Clipper2Lib::PathsD& tris, const dvec3& bmin, const dvec3& bmax)
        {
            using namespace Clipper2Lib;
            const int prec = 4;
            const size_t n = tris.size();
            if (n == 0) return {};
            const size_t kBatch = 2048;
            if (n <= kBatch) return Union(tris, FillRule::NonZero, prec);

            const double span = std::max(1.0e-9, std::max(bmax.x - bmin.x, bmax.z - bmin.z));
            const int cells = std::max(1, (int)std::sqrt((double)n / 1024.0));
            const double cs = span / cells;
            std::vector<std::pair<std::uint64_t, std::uint32_t>> order(n);
            for (size_t i = 0; i < n; ++i)
            {
                const PathD& t = tris[i];
                const double cx = (t[0].x + t[1].x + t[2].x) / 3.0, cz = (t[0].y + t[1].y + t[2].y) / 3.0;
                const std::uint64_t ix = (std::uint64_t)std::min(cells - 1, std::max(0, (int)((cx - bmin.x) / cs)));
                const std::uint64_t iz = (std::uint64_t)std::min(cells - 1, std::max(0, (int)((cz - bmin.z) / cs)));
                order[i] = { iz * (std::uint64_t)cells + ix, (std::uint32_t)i };
            }
            std::sort(order.begin(), order.end());

            std::vector<PathsD> level;
            level.reserve(n / kBatch + 1);
            for (size_t i = 0; i < n; i += kBatch)
            {
                PathsD chunk;
                chunk.reserve(std::min(kBatch, n - i));
                for (size_t j = i; j < std::min(n, i + kBatch); ++j) chunk.push_back(std::move(tris[order[j].second]));
                level.push_back(Union(chunk, FillRule::NonZero, prec));
            }
            while (level.size() > 1)
            {
                std::vector<PathsD> next;
                next.reserve(level.size() / 2 + 1);
                for (size_t i = 0; i < level.size(); i += 2)
                {
                    if (i + 1 == level.size()) { next.push_back(std::move(level[i])); break; }
                    PathsD a = std::move(level[i]);
                    a.insert(a.end(), level[i + 1].begin(), level[i + 1].end());
                    next.push_back(Union(a, FillRule::NonZero, prec));
                }
                level.swap(next);
            }
            return level.empty() ? PathsD{} : level.front();
        }

        // ---- CDT wrapper -------------------------------------------------------
        // Triangulate `pts` with `cons` as constraints, dropping outside / hole
        // triangles. CDT may add vertices where constraints cross; pts and cons are
        // re-synced from the triangulation. Returns false if CDT throws.
        bool RunCDT(std::vector<dvec2>& pts, std::vector<Edge2>& cons,
                    std::vector<glm::ivec3>& tris)
        {
            tris.clear();
            try
            {
                CDT::Triangulation<double> cdt(CDT::VertexInsertionOrder::Auto,
                                               CDT::IntersectingConstraintEdges::TryResolve, 0.0);
                cdt.insertVertices(pts.begin(), pts.end(),
                    [](const dvec2& p) { return p.x; }, [](const dvec2& p) { return p.y; });
                cdt.insertEdges(cons.begin(), cons.end(),
                    [](const Edge2& e) { return (CDT::VertInd)e.first; },
                    [](const Edge2& e) { return (CDT::VertInd)e.second; });
                cdt.eraseOuterTrianglesAndHoles();

                pts.resize(cdt.vertices.size());
                for (size_t i = 0; i < cdt.vertices.size(); ++i)
                    pts[i] = dvec2(cdt.vertices[i].x, cdt.vertices[i].y);
                cons.clear();
                cons.reserve(cdt.fixedEdges.size());
                for (const CDT::Edge& e : cdt.fixedEdges)
                    cons.push_back({ (std::uint32_t)e.v1(), (std::uint32_t)e.v2() });
                tris.reserve(cdt.triangles.size());
                for (const CDT::Triangle& t : cdt.triangles)
                    tris.push_back(glm::ivec3((int)t.vertices[0], (int)t.vertices[1], (int)t.vertices[2]));
            }
            catch (...)
            {
                return false;
            }
            return !tris.empty();
        }

        // Merge coincident points (e.g. where two outline loops touch) and remap
        // the constraint edges; drops edges that collapse.
        void DedupePoints(std::vector<dvec2>& pts, std::vector<Edge2>& cons, double q)
        {
            std::unordered_map<std::uint64_t, std::uint32_t> seen;
            std::vector<std::uint32_t> remap(pts.size());
            std::vector<dvec2> out;
            out.reserve(pts.size());
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const std::int64_t kx = (std::int64_t)std::llround(pts[i].x / q);
                const std::int64_t ky = (std::int64_t)std::llround(pts[i].y / q);
                const std::uint64_t key = ((std::uint64_t)(kx & 0xffffffff) << 32) | (std::uint64_t)(ky & 0xffffffff);
                auto it = seen.find(key);
                if (it != seen.end()) { remap[i] = it->second; continue; }
                remap[i] = (std::uint32_t)out.size();
                seen.emplace(key, remap[i]);
                out.push_back(pts[i]);
            }
            pts.swap(out);
            std::vector<Edge2> ec;
            ec.reserve(cons.size());
            std::unordered_set<std::uint64_t> have;
            for (const Edge2& e : cons)
            {
                const std::uint32_t a = remap[e.first], b = remap[e.second];
                if (a == b) continue;
                if (have.insert(EdgeKey(a, b)).second) ec.push_back({ a, b });
            }
            cons.swap(ec);
        }
    } // namespace

    // =======================================================================
    bool BuildPlanformMidplane(const PartSurface& part, const MidplaneParams& params,
                               MidplaneMesh& out)
    {
        out = MidplaneMesh{};
        out.objectIndex = part.objectIndex;
        out.label = part.label;
        MidplaneStats& S = out.stats;

        const size_t nv = part.xyz.size() / 3;
        const size_t nt = part.indices.size() / 3;
        if (nv < 3 || nt < 1) { S.message = "Part has no surface triangles."; return false; }
        const double A = std::max(1.0e-6, (double)params.targetAreaMm2);

        auto V = [&](unsigned int i) {
            return dvec3(part.xyz[(size_t)i * 3], part.xyz[(size_t)i * 3 + 1], part.xyz[(size_t)i * 3 + 2]);
        };

        // ---- Source triangles, footprint, volume -----------------------------
        std::vector<SrcTri> src;
        std::vector<double> nySigned;
        src.reserve(nt); nySigned.reserve(nt);
        Clipper2Lib::PathsD proj;
        proj.reserve(nt);
        double vol6 = 0.0;
        dvec3 bmin(1e300), bmax(-1e300);
        for (size_t t = 0; t < nt; ++t)
        {
            const unsigned int ia = part.indices[t * 3], ib = part.indices[t * 3 + 1], ic = part.indices[t * 3 + 2];
            if (ia >= nv || ib >= nv || ic >= nv) continue;
            const dvec3 a = V(ia), b = V(ib), c = V(ic);
            vol6 += glm::dot(a, glm::cross(b, c));
            bmin = glm::min(bmin, glm::min(a, glm::min(b, c)));
            bmax = glm::max(bmax, glm::max(a, glm::max(b, c)));
            const dvec3 n = glm::cross(b - a, c - a);
            const double nl = glm::length(n);
            const double area2 = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
            if (nl <= 0.0 || std::fabs(area2) < 1.0e-10) continue;   // vertical / degenerate
            SrcTri s;
            s.p[0] = a; s.p[1] = b; s.p[2] = c;
            s.area2 = area2;
            s.ny = std::fabs(n.y) / nl;
            src.push_back(s);
            nySigned.push_back(n.y / nl);
            // Positive winding for every projection so a NonZero union never
            // cancels a top face against the bottom face beneath it.
            if (area2 > 0.0)
                proj.push_back({ Clipper2Lib::PointD(a.x, a.z), Clipper2Lib::PointD(b.x, b.z), Clipper2Lib::PointD(c.x, c.z) });
            else
                proj.push_back({ Clipper2Lib::PointD(a.x, a.z), Clipper2Lib::PointD(c.x, c.z), Clipper2Lib::PointD(b.x, b.z) });
        }
        S.sourceVolumeMm3 = std::fabs(vol6) / 6.0;
        if (src.empty()) { S.message = "Part has no faces visible along the pull axis."; return false; }

        Clipper2Lib::PathsD fp = FootprintUnion(proj, bmin, bmax);
        proj.clear(); proj.shrink_to_fit();
        {
            Clipper2Lib::PathsD keep;
            for (const auto& p : fp)
                if (std::fabs(Clipper2Lib::Area(p)) >= 0.25 * A) keep.push_back(p);
            fp.swap(keep);
        }
        S.footprintAreaMm2 = std::fabs(Clipper2Lib::Area(fp));
        if (fp.empty() || S.footprintAreaMm2 <= 0.0) { S.message = "Empty footprint."; return false; }

        const double estTris = S.footprintAreaMm2 / (0.9 * A);
        if (estTris > (double)params.maxTriangles)
        {
            S.message = "Target triangle area is too small for this part (about " +
                        std::to_string((long long)estTris) + " triangles; limit " +
                        std::to_string(params.maxTriangles) + ").";
            return false;
        }

        // ---- Column sampler (source triangles bucketed in plan view) ---------
        const double diag = glm::length(bmax - bmin);
        const double yTol = std::max(1.0e-9, diag * 1.0e-7);
        // Base lattice spacing: an equilateral triangle of 90% of the target area.
        const double s0 = std::sqrt(4.0 * 0.9 * A / std::sqrt(3.0));
        Grid2 triGrid;
        {
            const double cellSz = std::max(s0, std::sqrt(S.footprintAreaMm2 / std::max<size_t>(1, src.size())) * 2.0);
            triGrid.init(bmin.x, bmin.z, bmax.x, bmax.z, cellSz);
            for (size_t t = 0; t < src.size(); ++t)
            {
                const SrcTri& q = src[t];
                triGrid.insert((int)t,
                    std::min(q.p[0].x, std::min(q.p[1].x, q.p[2].x)), std::min(q.p[0].z, std::min(q.p[1].z, q.p[2].z)),
                    std::max(q.p[0].x, std::max(q.p[1].x, q.p[2].x)), std::max(q.p[0].z, std::max(q.p[1].z, q.p[2].z)));
            }
        }

        // Outline loops, cleaned once (resampled per lattice spacing below).
        std::vector<std::vector<dvec2>> loops;
        for (const auto& path : fp)
        {
            std::vector<dvec2> L = CleanLoop(path, 1.0e-6 * std::max(1.0, diag));
            if (L.size() >= 3) loops.push_back(std::move(L));
        }
        if (loops.empty()) { S.message = "Footprint outline degenerated after cleanup."; return false; }

        std::vector<dvec2>      pts;
        std::vector<Edge2>      cons;
        std::vector<glm::ivec3> tris;
        std::vector<double>     hv, hc, my;        // vertical thickness, gap, mid Y
        std::vector<unsigned char> flags;
        std::vector<Hit>        hitBuf;
        const double cosSteep = std::cos((double)params.steepTiltDeg * kPi / 180.0);

        auto buildAdj = [&](std::vector<std::vector<int>>& vt)
        {
            vt.assign(pts.size(), {});
            for (int t = 0; t < (int)tris.size(); ++t)
                for (int k = 0; k < 3; ++k) vt[(size_t)tris[(size_t)t][k]].push_back(t);
        };
        auto boundaryMask = [&]()
        {
            std::vector<char> b(pts.size(), 0);
            for (const Edge2& e : cons) { b[e.first] = 1; b[e.second] = 1; }
            return b;
        };

        // ---- 2D base mesh at lattice spacing s --------------------------------
        // Outline resampled at <= s (corners kept), interior equilateral lattice
        // kept >= 0.55 s off the outline, constrained Delaunay, then a safe
        // Laplacian pass on the interior nodes.
        auto buildBase = [&](double s) -> bool
        {
            pts.clear(); cons.clear(); tris.clear();
            const double sh = s * std::sqrt(3.0) * 0.5;
            double umin = 1e300, vmin = 1e300, umax = -1e300, vmax = -1e300;
            std::vector<dvec2> R;
            for (const std::vector<dvec2>& L : loops)
            {
                ResampleLoop(L, s, 30.0, R);
                if (R.size() < 3) continue;
                const std::uint32_t base = (std::uint32_t)pts.size();
                for (size_t k = 0; k < R.size(); ++k)
                {
                    pts.push_back(R[k]);
                    cons.push_back({ base + (std::uint32_t)k, base + (std::uint32_t)((k + 1) % R.size()) });
                    umin = std::min(umin, R[k].x); umax = std::max(umax, R[k].x);
                    vmin = std::min(vmin, R[k].y); vmax = std::max(vmax, R[k].y);
                }
            }
            if (pts.size() < 3) return false;

            Grid2 segGrid;
            segGrid.init(umin - s, vmin - s, umax + s, vmax + s, s);
            for (size_t e = 0; e < cons.size(); ++e)
            {
                const dvec2 a = pts[cons[e].first], b = pts[cons[e].second];
                segGrid.insert((int)e, std::min(a.x, b.x) - s, std::min(a.y, b.y) - s,
                               std::max(a.x, b.x) + s, std::max(a.y, b.y) + s);
            }
            const double clear = 0.55 * s;
            std::vector<double> xs;
            int row = 0;
            for (double v = vmin + 0.5 * sh; v < vmax; v += sh, ++row)
            {
                xs.clear();                                   // scanline: even-odd crossings
                for (const Edge2& e : cons)
                {
                    const dvec2 a = pts[e.first], b = pts[e.second];
                    if ((a.y <= v && b.y > v) || (b.y <= v && a.y > v))
                        xs.push_back(a.x + (v - a.y) * (b.x - a.x) / (b.y - a.y));
                }
                if (xs.size() < 2) continue;
                std::sort(xs.begin(), xs.end());
                const double off = (row % 2) ? 0.5 * s : 0.0;
                for (double u = umin + off; u <= umax; u += s)
                {
                    const size_t below = (size_t)(std::lower_bound(xs.begin(), xs.end(), u) - xs.begin());
                    if (below % 2 == 0) continue;               // outside
                    double dmin = 1e300;
                    for (int e : segGrid.at(u, v))
                    {
                        const dvec2 a = pts[cons[(size_t)e].first], b = pts[cons[(size_t)e].second];
                        const dvec2 ab = b - a;
                        const double l2 = glm::dot(ab, ab);
                        const double t = (l2 > 0.0) ? std::min(1.0, std::max(0.0, glm::dot(dvec2(u, v) - a, ab) / l2)) : 0.0;
                        dmin = std::min(dmin, glm::length(dvec2(u, v) - (a + ab * t)));
                    }
                    if (dmin >= clear) pts.push_back(dvec2(u, v));
                }
            }
            DedupePoints(pts, cons, 1.0e-7 * std::max(1.0, diag));
            if (!RunCDT(pts, cons, tris)) return false;

            for (int it = 0; it < params.smoothIterations; ++it)
            {
                std::vector<std::vector<int>> vt;
                buildAdj(vt);
                const std::vector<char> isB = boundaryMask();
                for (size_t i = 0; i < pts.size(); ++i)
                {
                    if (isB[i] || vt[i].empty()) continue;
                    dvec2 sum(0.0); int cnt = 0;
                    for (int t : vt[i])
                        for (int k = 0; k < 3; ++k)
                        {
                            const int j = tris[(size_t)t][k];
                            if (j != (int)i) { sum += pts[(size_t)j]; ++cnt; }
                        }
                    if (cnt == 0) continue;
                    const dvec2 cand = sum / (double)cnt;
                    bool ok = true;                           // no flipped / collapsed star
                    for (int t : vt[i])
                    {
                        dvec2 q[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            const int j = tris[(size_t)t][k];
                            q[k] = (j == (int)i) ? cand : pts[(size_t)j];
                        }
                        if (Cross2(q[1] - q[0], q[2] - q[0]) <= 1.0e-3 * s * s) { ok = false; break; }
                    }
                    if (ok) pts[i] = cand;
                }
                if (!RunCDT(pts, cons, tris)) return false;
            }
            return true;
        };

        // ---- Sample every node's column ---------------------------------------
        // Boundary nodes are nudged just inside, off the silhouette where the
        // column would graze a wall. Misses are filled from their neighbours.
        auto sampleAll = [&](double s)
        {
            std::vector<std::vector<int>> vt;
            buildAdj(vt);
            const std::vector<char> isB = boundaryMask();
            const size_t N = pts.size();
            hv.assign(N, -1.0); hc.assign(N, -1.0); my.assign(N, 0.0); flags.assign(N, 0);
            for (size_t i = 0; i < N; ++i)
            {
                dvec2 q = pts[i];
                if (isB[i])
                {
                    flags[i] |= MidNodeBoundary;
                    dvec2 cen(0.0); int cnt = 0;
                    for (int t : vt[i])
                    {
                        const glm::ivec3& T = tris[(size_t)t];
                        cen += (pts[(size_t)T.x] + pts[(size_t)T.y] + pts[(size_t)T.z]) / 3.0; ++cnt;
                    }
                    if (cnt > 0)
                    {
                        cen /= (double)cnt;
                        const dvec2 d = cen - q;
                        const double dl = glm::length(d);
                        if (dl > 0.0) q += d * (std::min(0.05 * s, 0.5 * dl) / dl);
                    }
                }
                if (vt[i].empty()) continue;             // not in the mesh
                const Column c = SampleColumn(src, nySigned, triGrid, q.x, q.y, yTol, hitBuf);
                if (!c.ok) continue;
                hv[i] = c.y1 - c.y0;
                hc[i] = hv[i] * 0.5 * (c.cBot + c.cTop);
                my[i] = 0.5 * (c.y0 + c.y1);
                if (c.multi || c.odd) flags[i] |= MidNodeMultiLayer;
            }
            for (int sweep = 0; sweep < 256; ++sweep)   // propagate known neighbours inward
            {
                bool changed = false, missing = false;
                for (size_t i = 0; i < N; ++i)
                {
                    if (hv[i] >= 0.0 || vt[i].empty()) continue;
                    double a = 0.0, b = 0.0, m = 0.0; int cnt = 0;
                    for (int t : vt[i])
                        for (int k = 0; k < 3; ++k)
                        {
                            const size_t j = (size_t)tris[(size_t)t][k];
                            if (j == i || hv[j] < 0.0) continue;
                            a += hv[j]; b += hc[j]; m += my[j]; ++cnt;
                        }
                    if (cnt == 0) { missing = true; continue; }
                    hv[i] = a / cnt; hc[i] = b / cnt; my[i] = m / cnt;
                    flags[i] |= MidNodeFilled;
                    changed = true;
                }
                if (!missing || !changed) break;
            }
            for (size_t i = 0; i < N; ++i)
                if (hv[i] < 0.0) { hv[i] = 0.0; hc[i] = 0.0; my[i] = 0.0; flags[i] |= MidNodeFilled; }
        };

        // ---- Facet measures -----------------------------------------------------
        // 3D (midplane) and plan area, and whether the facet is a WALL facet: tilted
        // past the threshold AND the midplane jumps by a large share of the column
        // (a tall wall along the pull, where the planform is not valid). A thickness
        // step is steep but shallow relative to the columns, and flows straight
        // through, so it is not flagged.
        auto facet = [&](const glm::ivec3& T, double& a3, double& a2) -> bool
        {
            dvec3 P[3];
            for (int k = 0; k < 3; ++k)
                P[k] = dvec3(pts[(size_t)T[k]].x, my[(size_t)T[k]], pts[(size_t)T[k]].y);
            const dvec3 n3 = glm::cross(P[1] - P[0], P[2] - P[0]);
            a3 = 0.5 * glm::length(n3);
            a2 = 0.5 * std::fabs(Cross2(pts[(size_t)T.y] - pts[(size_t)T.x], pts[(size_t)T.z] - pts[(size_t)T.x]));
            if (a3 <= 0.0) return false;
            if (std::fabs(n3.y) / (2.0 * a3) >= cosSteep) return false;
            const double yMin = std::min(P[0].y, std::min(P[1].y, P[2].y));
            const double yMax = std::max(P[0].y, std::max(P[1].y, P[2].y));
            const double hMin = std::min(hv[(size_t)T.x], std::min(hv[(size_t)T.y], hv[(size_t)T.z]));
            const double hMax = std::max(hv[(size_t)T.x], std::max(hv[(size_t)T.y], hv[(size_t)T.z]));
            return (yMax - yMin) > 0.5 * std::max(hMin, 0.5 * hMax);
        };

        double s = s0;
        if (!buildBase(s)) { S.message = "Triangulation failed."; return false; }
        sampleAll(s);

        // Inclined regions have more midplane area than plan area. Tighten the
        // lattice once so the regular pattern itself meets the target there,
        // rather than leaving most of the work to refinement.
        {
            std::vector<double> ratio;
            ratio.reserve(tris.size());
            for (const glm::ivec3& T : tris)
            {
                double a3, a2;
                if (!facet(T, a3, a2) && a2 > 0.0) ratio.push_back(a3 / a2);
            }
            if (!ratio.empty())
            {
                const size_t k = (size_t)(0.9 * (double)(ratio.size() - 1));
                std::nth_element(ratio.begin(), ratio.begin() + (std::ptrdiff_t)k, ratio.end());
                const double r90 = ratio[k];
                if (r90 > 1.02)
                {
                    const double est = S.footprintAreaMm2 * r90 / (0.9 * A);
                    if (est > (double)params.maxTriangles)
                    {
                        S.message = "Target triangle area is too small for this part (about " +
                                    std::to_string((long long)est) + " triangles; limit " +
                                    std::to_string(params.maxTriangles) + ").";
                        return false;
                    }
                    s = s0 / std::sqrt(r90);
                    if (!buildBase(s)) { S.message = "Triangulation failed."; return false; }
                    sampleAll(s);
                    S.latticeScale = (float)(s / s0);
                }
            }
        }

        // Refine: insert the centroid of every over-target facet (midplane area;
        // plan area on wall facets) and re-triangulate. On a regular lattice this
        // is the sqrt(3)-subdivision, so the pattern stays equilateral.
        int pass = 0;
        for (; pass < params.maxRefinePasses; ++pass)
        {
            const size_t before = pts.size();
            for (const glm::ivec3& T : tris)
            {
                double a3, a2;
                const bool wall = facet(T, a3, a2);
                if ((wall ? a2 : a3) <= A) continue;
                pts.push_back((pts[(size_t)T.x] + pts[(size_t)T.y] + pts[(size_t)T.z]) / 3.0);
            }
            if (pts.size() == before) break;
            if (!RunCDT(pts, cons, tris)) { S.message = "Triangulation failed during refinement."; return false; }
            sampleAll(s);
        }
        S.refinePasses = pass;

        // ---- Output + statistics ---------------------------------------------
        const size_t N = pts.size();
        std::vector<char> used(N, 0);
        for (const glm::ivec3& T : tris) for (int k = 0; k < 3; ++k) used[(size_t)T[k]] = 1;
        std::vector<int> remap(N, -1);
        for (size_t i = 0; i < N; ++i)
        {
            if (!used[i]) continue;
            remap[i] = (int)out.nodes.size();
            out.nodes.push_back(glm::vec3((float)pts[i].x, (float)my[i], (float)pts[i].y));
            out.thicknessMm.push_back((float)hc[i]);
            out.verticalThicknessMm.push_back((float)hv[i]);
            out.nodeFlags.push_back(flags[i]);
        }

        double areaSum3 = 0.0, areaFlat3 = 0.0, qSum = 0.0, vol = 0.0, steepA2 = 0.0;
        double aMin = 1e300, aMax = 0.0, angMin = 180.0, qMin = 1.0;
        int nFlat = 0;
        out.tris.reserve(tris.size());
        out.triSteep.reserve(tris.size());
        auto ang = [](const dvec3& a, const dvec3& b) {
            const double la = glm::length(a), lb = glm::length(b);
            if (la <= 0.0 || lb <= 0.0) return 0.0;
            return std::acos(std::max(-1.0, std::min(1.0, glm::dot(a, b) / (la * lb)))) * 180.0 / kPi;
        };
        for (const glm::ivec3& T : tris)
        {
            double a3, a2;
            const bool wall = facet(T, a3, a2);
            const glm::ivec3 R(remap[(size_t)T.x], remap[(size_t)T.y], remap[(size_t)T.z]);
            out.tris.push_back(R);
            out.triSteep.push_back(wall ? 1 : 0);
            vol += a2 * (hv[(size_t)T.x] + hv[(size_t)T.y] + hv[(size_t)T.z]) / 3.0;
            areaSum3 += a3;
            if (wall)
            {
                ++S.steepTris; steepA2 += a2;
                for (int k = 0; k < 3; ++k) out.nodeFlags[(size_t)R[k]] |= MidNodeSteep;
                continue;
            }
            ++nFlat; areaFlat3 += a3;
            aMin = std::min(aMin, a3); aMax = std::max(aMax, a3);
            const dvec3 P0(pts[(size_t)T.x].x, my[(size_t)T.x], pts[(size_t)T.x].y);
            const dvec3 P1(pts[(size_t)T.y].x, my[(size_t)T.y], pts[(size_t)T.y].y);
            const dvec3 P2(pts[(size_t)T.z].x, my[(size_t)T.z], pts[(size_t)T.z].y);
            const double l0 = glm::length(P1 - P0), l1 = glm::length(P2 - P1), l2 = glm::length(P0 - P2);
            const double ls = l0 * l0 + l1 * l1 + l2 * l2;
            const double q = (ls > 0.0) ? 4.0 * std::sqrt(3.0) * a3 / ls : 0.0;
            qSum += q; qMin = std::min(qMin, q);
            angMin = std::min(angMin, std::min(ang(P1 - P0, P2 - P0), std::min(ang(P0 - P1, P2 - P1), ang(P0 - P2, P1 - P2))));
        }

        S.nodes = (int)out.nodes.size();
        S.tris  = (int)out.tris.size();
        S.midplaneAreaMm2 = areaSum3;
        S.midplaneVolumeMm3 = vol;
        S.steepAreaPct = (S.footprintAreaMm2 > 0.0) ? (float)(100.0 * steepA2 / S.footprintAreaMm2) : 0.0f;
        if (nFlat > 0)
        {
            S.minAreaMm2 = (float)aMin; S.maxAreaMm2 = (float)aMax;
            S.meanAreaMm2 = (float)(areaFlat3 / nFlat);
            S.minAngleDeg = (float)angMin;
            S.meanQuality = (float)(qSum / nFlat);
            S.worstQuality = (float)qMin;
        }
        double tMin = 1e300, tMax = 0.0, tSum = 0.0; int tCnt = 0;
        for (size_t i = 0; i < out.nodes.size(); ++i)
        {
            const unsigned char f = out.nodeFlags[i];
            if (f & MidNodeFilled) ++S.filledNodes;
            if (f & MidNodeMultiLayer) ++S.multiLayerNodes;
            if (f & MidNodeFilled) continue;
            tMin = std::min(tMin, (double)out.thicknessMm[i]);
            tMax = std::max(tMax, (double)out.thicknessMm[i]);
            tSum += out.thicknessMm[i]; ++tCnt;
        }
        if (tCnt > 0) { S.minThicknessMm = (float)tMin; S.maxThicknessMm = (float)tMax; S.meanThicknessMm = (float)(tSum / tCnt); }

        S.ok = !out.tris.empty();
        if (!S.ok)                                   S.message = "Midplane mesh is empty.";
        else if (S.maxAreaMm2 > (float)A * 1.0001f) S.message = "Midplane built (refinement cap reached; a few facets exceed the target).";
        else                                         S.message = "Midplane built.";
        return S.ok;
    }

    SolveMesh MidplaneToSolveMesh(const MidplaneMesh& m)
    {
        SolveMesh s;
        s.nodes = m.nodes;
        s.tris  = m.tris;
        s.halfGapMm.resize(m.thicknessMm.size());
        for (size_t i = 0; i < m.thicknessMm.size(); ++i) s.halfGapMm[i] = 0.5f * m.thicknessMm[i];
        return s;
    }

    int NearestMidplaneNode(const MidplaneMesh& m, const glm::vec3& p, float* outDistMm)
    {
        int best = -1;
        float bestPlan = std::numeric_limits<float>::infinity(), best3 = bestPlan;
        for (int i = 0; i < (int)m.nodes.size(); ++i)
        {
            const glm::vec3 d = m.nodes[(size_t)i] - p;
            const float plan = d.x * d.x + d.z * d.z;
            const float full = plan + d.y * d.y;
            if (plan < bestPlan - 1.0e-9f || (std::fabs(plan - bestPlan) <= 1.0e-9f && full < best3))
            { bestPlan = plan; best3 = full; best = i; }
        }
        if (outDistMm) *outDistMm = (best >= 0) ? std::sqrt(best3) : 0.0f;
        return best;
    }

} // namespace Flow
