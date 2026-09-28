// ===========================================================================
// FillDefects.cpp — see FillDefects.h.
// ===========================================================================

#include "FillDefects.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace Flow
{
    namespace
    {
        using dvec3 = glm::dvec3;
        constexpr double kPi = 3.14159265358979323846;

        struct UnionFind
        {
            std::vector<int> p;
            explicit UnionFind(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
            int find(int x) { while (p[(size_t)x] != x) { p[(size_t)x] = p[(size_t)p[(size_t)x]]; x = p[(size_t)x]; } return x; }
        };

        // Spatial hash over node positions for radius queries.
        struct Grid
        {
            double cell = 1.0;
            std::unordered_map<int64_t, std::vector<int>> cells;
            static int64_t key(int x, int y, int z)
            { return ((int64_t)(x & 0x1FFFFF) << 42) | ((int64_t)(y & 0x1FFFFF) << 21) | (int64_t)(z & 0x1FFFFF); }
            void build(const std::vector<glm::vec3>& pts, double c)
            {
                cell = std::max(1e-3, c);
                for (size_t i = 0; i < pts.size(); ++i)
                    cells[key((int)std::floor(pts[i].x / cell), (int)std::floor(pts[i].y / cell), (int)std::floor(pts[i].z / cell))].push_back((int)i);
            }
            template <class F> void around(const glm::vec3& q, F&& f) const
            {
                const int cx = (int)std::floor(q.x / cell), cy = (int)std::floor(q.y / cell), cz = (int)std::floor(q.z / cell);
                for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz)
                {
                    auto it = cells.find(key(cx + dx, cy + dy, cz + dz));
                    if (it != cells.end()) for (int j : it->second) f(j);
                }
            }
        };
    } // namespace

    FillDefects DetectFillDefects(const FeedNetwork& net,
                                  const std::vector<MidplaneMesh>& parts,
                                  const CoupledFillResult& fill,
                                  const FillDefectParams& P)
    {
        FillDefects D;
        const double cosMin = std::cos(P.weldMinAngleDeg * kPi / 180.0);
        const double inf = std::numeric_limits<double>::infinity();

        for (size_t k = 0; k < parts.size() && k < fill.parts.size(); ++k)
        {
            const MidplaneMesh& m = parts[k];
            const PartFillResult& pr = fill.parts[k];
            if (m.empty() || !pr.fed || pr.fillTimeS.size() != m.nodes.size()) continue;
            const size_t nn = m.nodes.size();
            const std::vector<float>& tf = pr.fillTimeS;
            const bool haveT = fill.thermal && pr.frontTempC.size() == nn;

            // ---- Per-node volume, neighbours --------------------------------------
            std::vector<double> vol(nn, 0.0);
            std::vector<std::vector<int>> nb(nn);
            for (const glm::ivec3& T : m.tris)
            {
                const dvec3 p0(m.nodes[(size_t)T.x]), p1(m.nodes[(size_t)T.y]), p2(m.nodes[(size_t)T.z]);
                const double area = 0.5 * glm::length(glm::cross(p1 - p0, p2 - p0));
                const double h = ((double)m.thicknessMm[(size_t)T.x] + m.thicknessMm[(size_t)T.y] + m.thicknessMm[(size_t)T.z]) / 3.0;
                const int v[3] = { T.x, T.y, T.z };
                for (int j = 0; j < 3; ++j)
                {
                    vol[(size_t)v[j]] += area * h / 3.0;
                    nb[(size_t)v[j]].push_back(v[(j + 1) % 3]);
                    nb[(size_t)v[j]].push_back(v[(j + 2) % 3]);
                }
            }
            for (auto& v : nb) { std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); }

            // ---- Vent mouths on this part -------------------------------------------
            struct Mouth { glm::vec3 pos; double reach; };
            std::vector<Mouth> mouths;
            for (size_t n = 0; n < net.nodes.size(); ++n)
            {
                const FeedNode& fn = net.nodes[n];
                if (fn.kind != FeedNodeKind::VentStart || fn.objectIndex != m.objectIndex) continue;
                double width = 0.0;
                for (const FeedEdge& e : net.edges)
                    if (e.kind == FeedEdgeKind::Vent && (e.a == (int)n || e.b == (int)n))
                        width = std::max({ width, (double)e.section.widthMm, (double)e.section.diameterMm });
                mouths.push_back({ fn.pos, P.ventCaptureMm + 0.5 * width });
            }
            auto ventDist = [&](const glm::vec3& p, bool& inReach) -> double
            {
                double best = -1.0; inReach = false;
                for (const Mouth& mo : mouths)
                {
                    const double dx = p.x - mo.pos.x, dz = p.z - mo.pos.z;
                    const double d = std::sqrt(dx * dx + dz * dz);
                    if (best < 0.0 || d < best) best = d;
                    if (d <= mo.reach) inReach = true;
                }
                return best;
            };
            std::vector<char> isVent(nn, 0), isOutline(nn, 0);
            for (size_t i = 0; i < nn; ++i)
            {
                bool r = false; ventDist(m.nodes[i], r);
                isVent[i] = r;
                isOutline[i] = (i < m.nodeFlags.size() && (m.nodeFlags[i] & MidNodeBoundary)) ? 1 : 0;
            }

            // ---- Weld / meld lines ------------------------------------------------------
            {
                const size_t nt = m.tris.size();
                std::vector<dvec3> grad(nt), cen(nt);
                std::vector<char> ok(nt, 0);
                for (size_t t = 0; t < nt; ++t)
                {
                    const glm::ivec3& T = m.tris[t];
                    if (tf[(size_t)T.x] < 0 || tf[(size_t)T.y] < 0 || tf[(size_t)T.z] < 0) continue;
                    const dvec3 p0(m.nodes[(size_t)T.x]), p1(m.nodes[(size_t)T.y]), p2(m.nodes[(size_t)T.z]);
                    const dvec3 cr = glm::cross(p1 - p0, p2 - p0);
                    const double twoA = glm::length(cr);
                    if (twoA <= 1e-12) continue;
                    const dvec3 nh = cr / twoA;
                    const dvec3 g = (glm::cross(nh, p2 - p1) * (double)tf[(size_t)T.x] +
                                     glm::cross(nh, p0 - p2) * (double)tf[(size_t)T.y] +
                                     glm::cross(nh, p1 - p0) * (double)tf[(size_t)T.z]) / twoA;
                    const double gl = glm::length(g);
                    if (gl <= 1e-12) continue;
                    grad[t] = g / gl;
                    cen[t] = (p0 + p1 + p2) / 3.0;
                    ok[t] = 1;
                }
                std::unordered_map<uint64_t, std::pair<int, int>> edgeTris;
                edgeTris.reserve(nt * 2);
                for (size_t t = 0; t < nt; ++t)
                {
                    const int v[3] = { m.tris[t].x, m.tris[t].y, m.tris[t].z };
                    for (int j = 0; j < 3; ++j)
                    {
                        const uint32_t a = (uint32_t)std::min(v[j], v[(j + 1) % 3]), b = (uint32_t)std::max(v[j], v[(j + 1) % 3]);
                        const uint64_t key = ((uint64_t)a << 32) | b;
                        auto it = edgeTris.find(key);
                        if (it == edgeTris.end()) edgeTris.emplace(key, std::make_pair((int)t, -1));
                        else it->second.second = (int)t;
                    }
                }
                struct WE { int a, b; double angle, len; };
                std::vector<WE> we;
                for (const auto& kv : edgeTris)
                {
                    const int t1 = kv.second.first, t2 = kv.second.second;
                    if (t2 < 0 || !ok[(size_t)t1] || !ok[(size_t)t2]) continue;
                    const int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xFFFFFFFFu);
                    const dvec3 pa(m.nodes[(size_t)a]), pb(m.nodes[(size_t)b]);
                    const dvec3 mid = 0.5 * (pa + pb);
                    // Both flows run into the edge (fronts converging onto it).
                    if (glm::dot(grad[(size_t)t1], mid - cen[(size_t)t1]) <= 0.0) continue;
                    if (glm::dot(grad[(size_t)t2], mid - cen[(size_t)t2]) <= 0.0) continue;
                    const double c = glm::dot(grad[(size_t)t1], grad[(size_t)t2]);
                    if (c > cosMin) continue;
                    const double ang = std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / kPi;
                    we.push_back({ a, b, ang, glm::length(pb - pa) });
                }
                UnionFind uf(nn);
                for (const WE& e : we) uf.p[(size_t)uf.find(e.a)] = uf.find(e.b);
                std::unordered_map<int, std::vector<size_t>> groups;
                for (size_t i = 0; i < we.size(); ++i) groups[uf.find(we[i].a)].push_back(i);
                std::vector<WeldLine> partLines;
                for (const auto& g : groups)
                {
                    WeldLine L;
                    L.part = (int)k;
                    double len = 0.0, angL = 0.0, weldL = 0.0, amax = 0.0, formed = inf, tmin = inf;
                    dvec3 c(0.0);
                    for (size_t ei : g.second)
                    {
                        const WE& e = we[ei];
                        L.edges.push_back({ e.a, e.b });
                        len += e.len; angL += e.angle * e.len;
                        if (e.angle >= P.weldAngleDeg) weldL += e.len;
                        amax = std::max(amax, e.angle);
                        formed = std::min(formed, (double)std::max(tf[(size_t)e.a], tf[(size_t)e.b]));
                        c += e.len * 0.5 * (dvec3(m.nodes[(size_t)e.a]) + dvec3(m.nodes[(size_t)e.b]));
                        if (haveT) tmin = std::min({ tmin, (double)pr.frontTempC[(size_t)e.a], (double)pr.frontTempC[(size_t)e.b] });
                    }
                    if (len < P.minLineLengthMm) continue;
                    L.lengthMm = (float)len;
                    L.meanAngleDeg = (float)(angL / len);
                    L.maxAngleDeg = (float)amax;
                    L.weldFraction = (float)(weldL / len);
                    L.weld = L.weldFraction >= 1.0f / 3.0f;
                    L.formedS = (float)formed;
                    L.hasFrontTemp = haveT;
                    L.minFrontTempC = haveT ? (float)tmin : 0.0f;
                    c /= len;
                    double bd = inf;
                    for (size_t ei : g.second)
                        for (int v : { we[ei].a, we[ei].b })
                        {
                            const double d = glm::length(dvec3(m.nodes[(size_t)v]) - c);
                            if (d < bd) { bd = d; L.anchor = m.nodes[(size_t)v]; }
                        }
                    partLines.push_back(std::move(L));
                }
                std::sort(partLines.begin(), partLines.end(), [](const WeldLine& a, const WeldLine& b) { return a.lengthMm > b.lengthMm; });
                for (WeldLine& L : partLines) { (L.weld ? D.weldCount : D.meldCount)++; D.lines.push_back(std::move(L)); }
            }

            // ---- Air traps: pockets cut off from the outline and the vents -----------
            std::vector<AirTrap> partTraps;
            {
                auto tEff = [&](size_t i) { return tf[i] >= 0.0f ? (double)tf[i] : inf; };
                std::vector<int> order(nn);
                std::iota(order.begin(), order.end(), 0);
                std::sort(order.begin(), order.end(), [&](int a, int b)
                {
                    const double ta = tEff((size_t)a), tb = tEff((size_t)b);
                    return ta != tb ? ta > tb : a < b;
                });
                UnionFind uf(nn);
                std::vector<char> added(nn, 0), openC(nn, 0);
                std::vector<double> cvol(nn, 0.0);
                std::vector<int> last(nn), cnt(nn, 0);
                std::vector<int> roots;
                for (int i : order)
                {
                    added[(size_t)i] = 1;
                    openC[(size_t)i] = (isOutline[(size_t)i] || isVent[(size_t)i]) ? 1 : 0;
                    cvol[(size_t)i] = vol[(size_t)i];
                    last[(size_t)i] = i;
                    cnt[(size_t)i] = 1;
                    roots.clear();
                    for (int j : nb[(size_t)i])
                        if (added[(size_t)j])
                        {
                            const int r = uf.find(j);
                            if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
                        }
                    bool unionOpen = openC[(size_t)i] != 0;
                    for (int r : roots) unionOpen = unionOpen || openC[(size_t)r];
                    // Forward in time: when node i fills, the closed components it
                    // was joining to open air are cut off — sealed pockets.
                    const double ti = tEff((size_t)i);
                    if (unionOpen && std::isfinite(ti))
                        for (int r : roots)
                        {
                            if (openC[(size_t)r] || cvol[(size_t)r] < P.minTrapVolumeMm3 || cnt[(size_t)r] < P.minTrapNodes) continue;
                            AirTrap a;
                            a.part = (int)k;
                            a.node = last[(size_t)r];
                            a.pos = m.nodes[(size_t)a.node];
                            a.volumeMm3 = (float)cvol[(size_t)r];
                            a.sealedS = (float)ti;
                            a.filledS = tf[(size_t)a.node];
                            a.nodeCount = cnt[(size_t)r];
                            partTraps.push_back(a);
                        }
                    for (int r : roots)
                    {
                        const int ri = uf.find(i);
                        if (r == ri) continue;
                        uf.p[(size_t)r] = ri;                    // i's root absorbs r (i added later: earlier fill)
                        openC[(size_t)ri] = openC[(size_t)ri] || openC[(size_t)r];
                        cvol[(size_t)ri] += cvol[(size_t)r];
                        cnt[(size_t)ri] += cnt[(size_t)r];
                        if (tEff((size_t)last[(size_t)r]) > tEff((size_t)last[(size_t)ri]) ||
                            (tEff((size_t)last[(size_t)r]) == tEff((size_t)last[(size_t)ri]) && last[(size_t)r] < last[(size_t)ri]))
                            last[(size_t)ri] = last[(size_t)r];
                    }
                }
            }

            // ---- Last to fill -------------------------------------------------------------
            {
                double meanEdge = 0.0; size_t ne = 0;
                for (size_t i = 0; i < nn; ++i)
                    for (int j : nb[i]) if ((int)i < j) { meanEdge += glm::length(m.nodes[(size_t)j] - m.nodes[i]); ++ne; }
                meanEdge = ne ? meanEdge / ne : 1.0;
                const double R = std::max(P.lastFillRadiusMm, 2.0 * meanEdge);
                Grid grid; grid.build(m.nodes, R);
                std::vector<LastFillPoint> cand;
                for (size_t i = 0; i < nn; ++i)
                {
                    if (tf[i] < 0.0f) continue;
                    bool isMax = true;
                    for (int j : nb[i]) if (tf[(size_t)j] < 0.0f || tf[(size_t)j] > tf[i]) { isMax = false; break; }
                    if (!isMax) continue;
                    grid.around(m.nodes[i], [&](int j)
                    {
                        if (!isMax || (size_t)j == i) return;
                        if (glm::length(m.nodes[(size_t)j] - m.nodes[i]) > R) return;
                        if (tf[(size_t)j] < 0.0f || tf[(size_t)j] > tf[i] || (tf[(size_t)j] == tf[i] && (size_t)j < i)) isMax = false;
                    });
                    if (!isMax) continue;
                    bool inTrap = false;
                    for (const AirTrap& a : partTraps)
                        if (glm::length(a.pos - m.nodes[i]) <= R) { inTrap = true; break; }
                    if (inTrap) continue;
                    LastFillPoint lp;
                    lp.part = (int)k; lp.node = (int)i; lp.pos = m.nodes[i]; lp.timeS = tf[i];
                    lp.onOutline = isOutline[i] != 0;
                    bool reach = false;
                    lp.ventDistMm = (float)ventDist(m.nodes[i], reach);
                    lp.vented = reach;
                    cand.push_back(lp);
                }
                std::sort(cand.begin(), cand.end(), [](const LastFillPoint& a, const LastFillPoint& b) { return a.timeS > b.timeS; });
                if ((int)cand.size() > P.maxLastFillPerPart) cand.resize((size_t)P.maxLastFillPerPart);
                for (LastFillPoint& lp : cand) D.lastFill.push_back(lp);
            }
            for (AirTrap& a : partTraps) D.traps.push_back(a);
        }
        std::stable_sort(D.traps.begin(), D.traps.end(), [](const AirTrap& a, const AirTrap& b) { return a.volumeMm3 > b.volumeMm3; });
        std::stable_sort(D.lastFill.begin(), D.lastFill.end(), [](const LastFillPoint& a, const LastFillPoint& b) { return a.timeS > b.timeS; });
        return D;
    }

} // namespace Flow
