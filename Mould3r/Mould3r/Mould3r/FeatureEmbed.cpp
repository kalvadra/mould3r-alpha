#include "FeatureEmbed.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>

namespace FeatureEmbed
{
namespace
{
    using dvec3 = glm::dvec3;

    struct Interval { double a, b; };

    // A body's triangles pre-filtered to the neighbourhood of the line bundle,
    // in the body's OBJECT space (the lines are carried into object space
    // instead of transforming every vertex into world space).
    struct LocalBody
    {
        std::vector<dvec3> tri;     // 3 per candidate triangle
        glm::dmat4         inv{ 1.0 };
        dvec3              axis{ 0.0 };   // world intoPart carried to object space
    };

    // Unrestricted line / triangle intersection (Moller-Trumbore without the
    // t > 0 gate): the whole infinite line is needed for exact parity. `dir`
    // need not be unit — t is in the caller's parameterisation. Double-sided,
    // so inconsistently wound imports are handled.
    bool LineTriangle(const dvec3& orig, const dvec3& dir,
                      const dvec3& v0, const dvec3& v1, const dvec3& v2,
                      double& outT)
    {
        const dvec3  e1 = v1 - v0;
        const dvec3  e2 = v2 - v0;
        const dvec3  h  = glm::cross(dir, e2);
        const double a  = glm::dot(e1, h);
        if (std::abs(a) < 1e-14) return false;          // parallel / degenerate
        const double f = 1.0 / a;
        const dvec3  s = orig - v0;
        const double u = f * glm::dot(s, h);
        if (u < 0.0 || u > 1.0) return false;
        const dvec3  q = glm::cross(s, e1);
        const double v = f * glm::dot(dir, q);
        if (v < 0.0 || u + v > 1.0) return false;
        outT = f * glm::dot(e2, q);
        return true;
    }

    // Inside intervals of one body along the line p(t) = worldPt + t*intoPart.
    // Returns false when the crossing count is odd (open / non-manifold mesh
    // on this line) — the caller nudges and retries.
    bool BodyIntervals(const LocalBody& body, const dvec3& worldPt,
                       double mergeTol, std::vector<Interval>& out)
    {
        const dvec3 o = dvec3(body.inv * glm::dvec4(worldPt, 1.0));

        std::vector<double> hits;
        for (size_t i = 0; i + 2 < body.tri.size(); i += 3)
        {
            double t;
            if (LineTriangle(o, body.axis, body.tri[i], body.tri[i + 1],
                             body.tri[i + 2], t))
                hits.push_back(t);
        }
        std::sort(hits.begin(), hits.end());

        // Collapse repeats: a line through a shared edge / vertex reports the
        // one physical crossing once per touching triangle.
        std::vector<double> xs;
        for (double t : hits)
            if (xs.empty() || t - xs.back() > mergeTol) xs.push_back(t);

        if (xs.size() % 2 != 0) return false;
        for (size_t i = 0; i + 1 < xs.size(); i += 2)
            out.push_back({ xs[i], xs[i + 1] });
        return true;
    }

    LocalBody PrepareBody(const BodyMesh& bm, const dvec3& origin,
                          const dvec3& intoPart, double bundleRadius)
    {
        LocalBody lb;
        if (!bm.verts || !bm.indices) return lb;
        const std::vector<float>&    V = *bm.verts;
        const std::vector<uint32_t>& I = *bm.indices;

        lb.inv  = glm::inverse(glm::dmat4(bm.model));
        lb.axis = glm::dmat3(lb.inv) * intoPart;

        // The bundle is an infinite cylinder of radius bundleRadius about the
        // axis line. Carry it into object space; the radius is scaled by the
        // largest stretch of the inverse linear part so non-uniform / mirror
        // transforms stay conservative.
        const glm::dmat3 L(lb.inv);
        const double stretch = std::max({ glm::length(L[0]),
                                          glm::length(L[1]),
                                          glm::length(L[2]) });
        const dvec3  oL   = dvec3(lb.inv * glm::dvec4(origin, 1.0));
        const double aLen = glm::length(lb.axis);
        if (aLen < 1e-12) return lb;
        const dvec3  aHat = lb.axis / aLen;
        const double rL   = bundleRadius * stretch;

        const size_t vcount = V.size() / 3;
        for (size_t i = 0; i + 2 < I.size(); i += 3)
        {
            const uint32_t i0 = I[i], i1 = I[i + 1], i2 = I[i + 2];
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
            const dvec3 v0(V[i0 * 3], V[i0 * 3 + 1], V[i0 * 3 + 2]);
            const dvec3 v1(V[i1 * 3], V[i1 * 3 + 1], V[i1 * 3 + 2]);
            const dvec3 v2(V[i2 * 3], V[i2 * 3 + 1], V[i2 * 3 + 2]);

            // Bounding sphere of the triangle vs. the bundle cylinder.
            const dvec3  c  = (v0 + v1 + v2) / 3.0;
            const double rb = std::sqrt(std::max({ glm::dot(v0 - c, v0 - c),
                                                   glm::dot(v1 - c, v1 - c),
                                                   glm::dot(v2 - c, v2 - c) }));
            const double d  = glm::length(glm::cross(c - oL, aHat));
            if (d > rL + rb) continue;

            lb.tri.push_back(v0);
            lb.tri.push_back(v1);
            lb.tri.push_back(v2);
        }
        return lb;
    }

    // Union of intervals, sorted.
    void MergeIntervals(std::vector<Interval>& iv)
    {
        std::sort(iv.begin(), iv.end(),
                  [](const Interval& x, const Interval& y) { return x.a < y.a; });
        std::vector<Interval> out;
        for (const Interval& x : iv)
        {
            if (!out.empty() && x.a <= out.back().b)
                out.back().b = std::max(out.back().b, x.b);
            else
                out.push_back(x);
        }
        iv.swap(out);
    }

    // Two unit vectors spanning the plane perpendicular to `n`.
    void PerpBasis(const glm::vec3& n, glm::vec3& u, glm::vec3& v)
    {
        const glm::vec3 ref = (std::abs(n.y) < 0.9f) ? glm::vec3(0, 1, 0)
                                                    : glm::vec3(1, 0, 0);
        u = glm::normalize(glm::cross(n, ref));
        v = glm::cross(n, u);
    }
} // namespace

Result Analyze(const glm::vec3& origin,
               const glm::vec3& intoPart,
               const std::vector<glm::vec3>& offsets,
               const std::vector<BodyMesh>& bodies,
               const Params& params)
{
    Result res;
    if (offsets.empty() || bodies.empty()) return res;
    const float axLen = glm::length(intoPart);
    if (axLen < 1e-6f) return res;
    const glm::vec3 axis = intoPart / axLen;

    // Nudge used when a line hits an edge / vertex badly enough to give an odd
    // crossing count: a tiny sideways shift, well inside any tolerance that
    // matters for a moulded feature.
    glm::vec3 nu, nv;
    PerpBasis(axis, nu, nv);
    const float     kNudge = 2.0e-3f;
    const glm::vec3 nudges[3] = { glm::vec3(0.0f),
                                  (nu * 0.8f + nv * 0.6f) * kNudge,
                                  (nu * -0.6f + nv * 0.8f) * kNudge };

    float bundleR = 0.0f;
    for (const glm::vec3& o : offsets) bundleR = std::max(bundleR, glm::length(o));
    bundleR += 2.0f * kNudge + 1.0e-3f;

    std::vector<LocalBody> local;
    local.reserve(bodies.size());
    for (const BodyMesh& bm : bodies)
    {
        LocalBody lb = PrepareBody(bm, dvec3(origin), dvec3(axis), bundleR);
        if (!lb.tri.empty()) local.push_back(std::move(lb));
    }

    const double maxExt = std::max(0.0f, params.maxExtension);
    res.analysed = true;
    res.samples  = (int)offsets.size();
    res.lines.reserve(offsets.size());

    double maxGap   = 0.0;
    double minExit  = std::numeric_limits<double>::max();
    bool   anyReach = false;

    for (const glm::vec3& off : offsets)
    {
        LineResult lr;
        lr.offset = off;

        // Union of every body's inside intervals along this line.
        std::vector<Interval> iv;
        bool ok = true;
        for (const glm::vec3& nudge : nudges)
        {
            iv.clear();
            ok = true;
            const dvec3 p = dvec3(origin + off + nudge);
            for (const LocalBody& lb : local)
                if (!BodyIntervals(lb, p, params.mergeTol, iv)) { ok = false; break; }
            if (ok) break;
        }

        if (!ok)
        {
            lr.state = LineState::Unreliable;
            ++res.unreliable;
            res.lines.push_back(lr);
            continue;
        }
        MergeIntervals(iv);

        // The interval that matters is the first one still extending behind
        // the mouth plane (b > 0). One that ends in front of the mouth is part
        // of the body bulging past it into the channel — irrelevant.
        const Interval* hit = nullptr;
        for (const Interval& x : iv)
            if (x.b > 0.0) { hit = &x; break; }

        if (!hit || hit->a > maxExt)
        {
            lr.state = LineState::Unreachable;
            ++res.unreachable;
            res.lines.push_back(lr);
            continue;
        }

        lr.entry = (float)hit->a;
        lr.exit  = (float)hit->b;
        if (hit->a <= 0.0) { lr.state = LineState::Embedded; ++res.embedded; }
        else               { lr.state = LineState::Gap;      ++res.gap;      }

        anyReach = true;
        if (hit->a > maxGap) { maxGap = hit->a; res.worstOffset = off; }
        minExit = std::min(minExit, hit->b);
        res.lines.push_back(lr);
    }

    if (anyReach)
    {
        res.maxGap  = (float)maxGap;
        res.ceiling = (float)std::clamp(minExit - params.margin, 0.0, maxExt);

        if (maxGap > 0.0)
        {
            const double want  = maxGap + params.margin;
            const double limit = params.respectCeiling ? (double)res.ceiling : maxExt;
            double ext = std::min(want, limit);

            // The margins on both sides don't fit (thin wall behind a curved
            // mouth) but the deepest entry is still short of the thinnest
            // exit: every line CAN land inside, so split what's left rather
            // than giving up a line to protect a margin.
            if (params.respectCeiling && ext < maxGap && maxGap < minExit)
                ext = 0.5 * (maxGap + std::min(minExit, maxExt));

            res.extension = (float)std::min(ext, maxExt);

            // Only a genuine conflict counts: some line would still be short
            // of the part at a depth where another has already punched out
            // the back of it (or the cap bit first).
            res.ceilingLimited = (double)res.extension < maxGap - 1e-6;
        }
    }

    // Coverage at the recommended depth: a line is connected once the
    // extension reaches its entry.
    int connected = 0;
    for (const LineResult& lr : res.lines)
        if ((lr.state == LineState::Embedded || lr.state == LineState::Gap) &&
            lr.entry <= res.extension + 1e-5f)
            ++connected;
    res.coverage = res.samples ? float(connected) / float(res.samples) : 0.0f;

    res.fullyEmbedded = anyReach && res.unreachable == 0 &&
                        res.unreliable == 0 && !res.ceilingLimited;
    return res;
}

std::vector<glm::vec3> RectSamples(const glm::vec3& sideAxis,
                                   const glm::vec3& upAxis,
                                   float halfWidth, float halfDepth,
                                   int n, int m)
{
    n = std::max(n, 2);
    m = std::max(m, 2);
    std::vector<glm::vec3> out;
    out.reserve(size_t(n) * size_t(m));
    for (int i = 0; i < n; ++i)
    {
        const float s = -1.0f + 2.0f * float(i) / float(n - 1);
        for (int j = 0; j < m; ++j)
        {
            const float t = -1.0f + 2.0f * float(j) / float(m - 1);
            out.push_back(sideAxis * (s * halfWidth) + upAxis * (t * halfDepth));
        }
    }
    return out;
}

std::vector<glm::vec3> DiscSamples(const glm::vec3& axis, float radius,
                                   int rings, int spokes)
{
    std::vector<glm::vec3> out;
    out.push_back(glm::vec3(0.0f));
    const float len = glm::length(axis);
    if (len < 1e-6f || radius <= 0.0f) return out;

    glm::vec3 u, v;
    PerpBasis(axis / len, u, v);
    rings  = std::max(rings, 1);
    spokes = std::max(spokes, 3);
    for (int r = 1; r <= rings; ++r)
    {
        const float rr = radius * float(r) / float(rings);
        for (int k = 0; k < spokes; ++k)
        {
            const float a = 6.28318530718f * float(k) / float(spokes);
            out.push_back(u * (rr * std::cos(a)) + v * (rr * std::sin(a)));
        }
    }
    return out;
}

} // namespace FeatureEmbed
