// MouldFeature.cpp — geometry builders for mould features.
// These are pure functions that produce GPU-ready SolidMesh objects.

#include "MouldFeature.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// BuildCylinderMesh — sweeps a circular cross-section from start to end to
// produce a closed frustum (tapered cylinder) mesh.
//
// draftAngleDeg controls the taper: the radius at 'start' equals 'radius',
// and the radius at 'end' equals radius + axisLen * tan(draftAngleDeg).
// When draftAngleDeg == 0 the result is a straight cylinder.
//
// Vertex layout: [px, py, pz, nx, ny, nz] — compatible with the vsLit shader.
//
// The solid has three parts:
//   - Start cap  : triangle fan, normal facing away from end
//   - End cap    : triangle fan, normal facing away from start
//   - Side quads : one quad per segment connecting the two rings
// ---------------------------------------------------------------------------
SolidMesh BuildCylinderMesh(const glm::vec3& start, const glm::vec3& end,
    float radius, float draftAngleDeg, int segments)
{
    SolidMesh solid;
    solid.valid = false;

    const glm::vec3 axis = end - start;
    const float     axisLen = glm::length(axis);
    if (axisLen < 1e-6f || radius < 1e-6f) return solid;

    // Sweep direction and two perpendicular axes (Gram-Schmidt)
    const glm::vec3 axisZ = axis / axisLen;
    glm::vec3 axisX = glm::abs(axisZ.x) < 0.9f
        ? glm::vec3(1.0f, 0.0f, 0.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);
    axisX = glm::normalize(axisX - glm::dot(axisX, axisZ) * axisZ);
    const glm::vec3 axisY = glm::cross(axisZ, axisX);

    // Draft taper: start ring has 'radius', end ring grows by the draft angle
    const float draftRad = glm::radians(glm::clamp(draftAngleDeg, 0.0f, 45.0f));
    const float startRadius = radius;
    const float endRadius = radius + axisLen * std::tan(draftRad);

    // Side-surface normal tilt
    const float tanDraft = std::tan(draftRad);

    std::vector<float>    verts;
    std::vector<uint32_t> idx;

    auto pushVert = [&](const glm::vec3& pos, const glm::vec3& norm)
        {
            verts.push_back(pos.x);  verts.push_back(pos.y);  verts.push_back(pos.z);
            verts.push_back(norm.x); verts.push_back(norm.y); verts.push_back(norm.z);
        };

    // ---- Side surface ------------------------------------------------------
    for (int cap = 0; cap < 2; ++cap)
    {
        const glm::vec3 centre = (cap == 0) ? start : end;
        const float     r = (cap == 0) ? startRadius : endRadius;
        for (int i = 0; i < segments; ++i)
        {
            const float theta = (float(i) / float(segments)) * glm::two_pi<float>();
            const glm::vec3 radial = std::cos(theta) * axisX + std::sin(theta) * axisY;
            const glm::vec3 sideNorm = glm::normalize(radial - tanDraft * axisZ);
            pushVert(centre + r * radial, sideNorm);
        }
    }
    for (int i = 0; i < segments; ++i)
    {
        const uint32_t s0 = (uint32_t)i;
        const uint32_t s1 = (uint32_t)((i + 1) % segments);
        const uint32_t e0 = (uint32_t)(segments + i);
        const uint32_t e1 = (uint32_t)(segments + (i + 1) % segments);
        idx.push_back(s0); idx.push_back(e0); idx.push_back(e1);
        idx.push_back(s0); idx.push_back(e1); idx.push_back(s1);
    }

    // ---- Caps --------------------------------------------------------------
    auto addCap = [&](const glm::vec3& centre, const glm::vec3& capNorm, float capRadius)
        {
            const uint32_t centreIdx = (uint32_t)(verts.size() / 6);
            pushVert(centre, capNorm);

            const uint32_t rimBase = (uint32_t)(verts.size() / 6);
            for (int i = 0; i < segments; ++i)
            {
                const float theta = (float(i) / float(segments)) * glm::two_pi<float>();
                const glm::vec3 radial = std::cos(theta) * axisX + std::sin(theta) * axisY;
                pushVert(centre + capRadius * radial, capNorm);
            }
            for (int i = 0; i < segments; ++i)
            {
                const uint32_t r0 = rimBase + (uint32_t)i;
                const uint32_t r1 = rimBase + (uint32_t)((i + 1) % segments);
                if (glm::dot(capNorm, axisZ) < 0.0f)
                {
                    idx.push_back(centreIdx); idx.push_back(r1); idx.push_back(r0);
                }
                else
                {
                    idx.push_back(centreIdx); idx.push_back(r0); idx.push_back(r1);
                }
            }
        };

    addCap(start, -axisZ, startRadius);
    addCap(end, +axisZ, endRadius);

    // ---- Upload to GPU ----
    glGenVertexArrays(1, &solid.vao);
    glGenBuffers(1, &solid.vbo);
    glGenBuffers(1, &solid.ebo);

    glBindVertexArray(solid.vao);
    glBindBuffer(GL_ARRAY_BUFFER, solid.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, solid.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)(idx.size() * sizeof(uint32_t)),
        idx.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
        (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    solid.indexCount = (GLsizei)idx.size();
    solid.valid = true;
    return solid;
}

// ---------------------------------------------------------------------------
// SamplePath — turn a FeaturePath into an ordered list of cross-section frames
// (stations). This is the single source of truth for a path's route: the
// preview sweep below consumes it, and the OCC cut (Part 3) will consume the
// same function, so the preview can never disagree with what gets cut.
//
// Everything is planar (constant Y), so each station only needs a position, a
// forward tangent, and the in-plane perpendicular — no Frenet frame.
// ---------------------------------------------------------------------------
namespace {

    // In-plane perpendicular of a forward tangent, matching the legacy
    // sideAxis handedness so the Simple case stays geometrically identical:
    // for tangent t in XZ, side = normalize(t.z, 0, -t.x).
    glm::vec3 SideAxisOf(const glm::vec3& tangent)
    {
        glm::vec3 s(tangent.z, 0.0f, -tangent.x);
        const float l = glm::length(s);
        return (l > 1e-6f) ? s / l : glm::vec3(1.0f, 0.0f, 0.0f);
    }

    glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback)
    {
        const float l = glm::length(v);
        return (l > 1e-6f) ? v / l : fallback;
    }

    // Cubic Bezier position / derivative at t in [0,1].
    glm::vec3 BezierPos(const glm::vec3& p0, const glm::vec3& p1,
        const glm::vec3& p2, const glm::vec3& p3, float t)
    {
        const float u = 1.0f - t;
        return u * u * u * p0
            + 3.0f * u * u * t * p1
            + 3.0f * u * t * t * p2
            + t * t * t * p3;
    }
    glm::vec3 BezierDeriv(const glm::vec3& p0, const glm::vec3& p1,
        const glm::vec3& p2, const glm::vec3& p3, float t)
    {
        const float u = 1.0f - t;
        return 3.0f * u * u * (p1 - p0)
            + 6.0f * u * t * (p2 - p1)
            + 3.0f * t * t * (p3 - p2);
    }

} // namespace

std::vector<PathStation> SamplePath(const FeaturePath& path, float spacing)
{
    std::vector<PathStation> out;
    if (spacing < 1e-3f) spacing = 1.5f;

    auto inPlaneTangent = [](glm::vec3 t) -> glm::vec3
        {
            t.y = 0.0f;
            return SafeNormalize(t, glm::vec3(0.0f, 0.0f, 1.0f));
        };

    // ---- Simple: two stations, straight channel (legacy behavior) ----------
    if (path.kind == PathKind::Simple)
    {
        const glm::vec3 d(path.end.x - path.start.x, 0.0f, path.end.z - path.start.z);
        if (glm::length(d) < 1e-6f) return out;           // degenerate
        const glm::vec3 fwd = inPlaneTangent(d);

        PathStation a, b;
        a.pos = path.start; a.tangent = fwd; a.sideAxis = SideAxisOf(fwd);
        b.pos = path.end;   b.tangent = fwd; b.sideAxis = SideAxisOf(fwd);
        out.push_back(a);
        out.push_back(b);
        return out;
    }

    // ---- Complex ----------------------------------------------------------
    const std::vector<PathNode>& nodes = path.nodes;
    if (nodes.size() < 2) return out;

    if (!path.smooth)
    {
        // Straight A->B->C polyline through the node positions. Interior nodes
        // are mitered (ring oriented to the averaged tangent) so the swept box
        // bends through a corner without tearing.
        const int n = (int)nodes.size();
        for (int i = 0; i < n; ++i)
        {
            glm::vec3 inDir(0.0f), outDir(0.0f);
            if (i > 0)     inDir = SafeNormalize(nodes[i].pos - nodes[i - 1].pos, glm::vec3(0.0f));
            if (i < n - 1) outDir = SafeNormalize(nodes[i + 1].pos - nodes[i].pos, glm::vec3(0.0f));

            glm::vec3 t;
            if (i == 0)          t = outDir;
            else if (i == n - 1) t = inDir;
            else                 t = SafeNormalize(inDir + outDir, outDir); // miter

            PathStation s;
            s.pos = nodes[i].pos;
            s.tangent = inPlaneTangent(t);
            s.sideAxis = SideAxisOf(s.tangent);
            out.push_back(s);
        }
        return out;
    }

    // ---- Complex + smooth: cubic Bezier per segment, arc-length resampled ---
    auto pushStation = [&](const glm::vec3& pos, const glm::vec3& tanRaw)
        {
            PathStation s;
            s.pos = pos;
            s.tangent = inPlaneTangent(tanRaw);
            s.sideAxis = SideAxisOf(s.tangent);
            out.push_back(s);
        };

    const int segCount = (int)nodes.size() - 1;
    for (int seg = 0; seg < segCount; ++seg)
    {
        const PathNode& n0 = nodes[seg];
        const PathNode& n1 = nodes[seg + 1];

        // Independent tangent handles: outgoing control point of n0 = pos +
        // handleOut, incoming control point of n1 = pos + handleIn. For a linked
        // (symmetric) node handleIn == -handleOut, which keeps the tangent
        // continuous across the node; a broken node can corner. Handles are
        // populated by AutoComputeComplexHandles before sampling.
        const glm::vec3 p0 = n0.pos;
        const glm::vec3 p3 = n1.pos;
        const glm::vec3 p1 = n0.pos + n0.handleOut;
        const glm::vec3 p2 = n1.pos + n1.handleIn;

        // Fine forward pass -> cumulative arc-length table for this segment.
        const int kFine = 32;
        std::vector<glm::vec3> fp(kFine + 1);
        std::vector<float>     fl(kFine + 1, 0.0f);
        for (int k = 0; k <= kFine; ++k)
            fp[k] = BezierPos(p0, p1, p2, p3, (float)k / (float)kFine);
        for (int k = 1; k <= kFine; ++k)
            fl[k] = fl[k - 1] + glm::length(fp[k] - fp[k - 1]);
        const float segLen = fl[kFine];

        // Place stations at uniform arc-length steps. Skip the shared start
        // node for every segment after the first (already emitted as the
        // previous segment's last station) to avoid duplicates.
        const int steps = std::max(1, (int)std::ceil(segLen / spacing));
        const int startStep = (seg == 0) ? 0 : 1;
        for (int st = startStep; st <= steps; ++st)
        {
            const float targetLen = segLen * ((float)st / (float)steps);
            int k = 1;
            while (k < kFine && fl[k] < targetLen) ++k;
            const float l0 = fl[k - 1], l1 = fl[k];
            const float frac = (l1 > l0) ? (targetLen - l0) / (l1 - l0) : 0.0f;
            const float t = ((float)(k - 1) + frac) / (float)kFine;
            pushStation(BezierPos(p0, p1, p2, p3, t),
                        BezierDeriv(p0, p1, p2, p3, t));
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// AutoComputeComplexHandles — derive symmetric Catmull-Rom tangent handles
// for every node from the node positions. See header for the rule.
// ---------------------------------------------------------------------------
void AutoComputeComplexHandles(FeaturePath& path)
{
    if (path.kind != PathKind::Complex) return;
    const int n = (int)path.nodes.size();
    if (n < 2) return;

    auto chordLen = [&](int a, int b) -> float
        {
            return glm::length(path.nodes[b].pos - path.nodes[a].pos);
        };

    for (int i = 0; i < n; ++i)
    {
        // Hand-edited nodes keep their explicit handles (which are offsets, so
        // they ride along when the node or its neighbours move).
        if (path.nodes[i].handlesManual) continue;

        glm::vec3 dir(0.0f);
        if (i == 0)
            dir = path.nodes[1].pos - path.nodes[0].pos;          // forward chord
        else if (i == n - 1)
            dir = path.nodes[n - 1].pos - path.nodes[n - 2].pos;  // backward chord
        else
            dir = path.nodes[i + 1].pos - path.nodes[i - 1].pos;  // centred

        dir.y = 0.0f;   // stay on the parting plane
        const float l = glm::length(dir);
        path.nodes[i].dir = (l > 1e-6f) ? dir / l : glm::vec3(0.0f, 0.0f, 1.0f);

        // 1/3 of the shorter adjacent chord keeps the spline taut.
        float arm;
        if (i == 0)            arm = chordLen(0, 1);
        else if (i == n - 1)   arm = chordLen(n - 2, n - 1);
        else                   arm = std::min(chordLen(i - 1, i), chordLen(i, i + 1));
        path.nodes[i].handleLen = arm / 3.0f;

        // Mirror the symmetric arms into the explicit-handle fields the sampler
        // reads. Linked, so handleIn == -handleOut.
        const glm::vec3 arm3 = path.nodes[i].dir * path.nodes[i].handleLen;
        path.nodes[i].handleOut = arm3;
        path.nodes[i].handleIn = -arm3;
        path.nodes[i].handlesLinked = true;
    }
}

// ---------------------------------------------------------------------------
// BuildBoxSweepMesh — sweeps a rectangular cross-section along a vent path.
// Vertex layout: [px, py, pz, nx, ny, nz] — compatible with the vsLit shader.
// ---------------------------------------------------------------------------
SolidMesh BuildBoxSweepMesh(const FeaturePath& path, float width, float depth,
    float overrunStart, float overrunEnd)
{
    SolidMesh solid;
    solid.valid = false;

    if (!path.valid) return solid;

    // One station list drives the whole sweep (Simple -> 2 stations, so this
    // stays geometrically identical to the old straight box).
    std::vector<PathStation> stations = SamplePath(path);
    if (stations.size() < 2) return solid;

    // Overruns extend the channel past its first/last station along the local
    // tangent — the same effect as the old extendedStart / extendedEnd. The
    // stations carry no overrun, so apply it here on this local copy.
    stations.front().pos -= stations.front().tangent * overrunStart;
    stations.back().pos += stations.back().tangent * overrunEnd;

    const glm::vec3 upAxis(0.0f, 1.0f, 0.0f);
    const float hw = width * 0.5f;
    const float hd = depth * 0.5f;

    // Ring of 4 corners at a station: BL, BR, TR, TL (matches legacy order).
    auto ringOf = [&](const PathStation& s) -> std::array<glm::vec3, 4>
        {
            return { {
                s.pos - s.sideAxis * hw - upAxis * hd,
                s.pos + s.sideAxis * hw - upAxis * hd,
                s.pos + s.sideAxis * hw + upAxis * hd,
                s.pos - s.sideAxis * hw + upAxis * hd
            } };
        };

    std::vector<float>    verts;
    std::vector<uint32_t> idx;

    auto addQuad = [&](const glm::vec3& a, const glm::vec3& b,
        const glm::vec3& c, const glm::vec3& d,
        const glm::vec3& n)
        {
            const uint32_t base = (uint32_t)(verts.size() / 6);
            for (const glm::vec3& p : { a, b, c, d })
            {
                verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
                verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
            }
            idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
            idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
        };

    // Start cap — faces backward along the first station's tangent.
    const auto firstRing = ringOf(stations.front());
    addQuad(firstRing[0], firstRing[3], firstRing[2], firstRing[1],
        -stations.front().tangent);

    // Side walls — four quads per segment between consecutive stations. For a
    // Simple path (one segment) these are exactly the old bottom/right/top/left.
    for (size_t i = 0; i + 1 < stations.size(); ++i)
    {
        const auto A = ringOf(stations[i]);
        const auto B = ringOf(stations[i + 1]);
        const glm::vec3 side = stations[i].sideAxis;

        addQuad(A[0], A[1], B[1], B[0], -upAxis);   // bottom
        addQuad(A[1], A[2], B[2], B[1], side);      // right
        addQuad(A[2], A[3], B[3], B[2], upAxis);    // top
        addQuad(A[3], A[0], B[0], B[3], -side);     // left
    }

    // End cap — faces forward along the last station's tangent.
    const auto lastRing = ringOf(stations.back());
    addQuad(lastRing[0], lastRing[1], lastRing[2], lastRing[3],
        stations.back().tangent);

    // ---- Upload to GPU ----
    glGenVertexArrays(1, &solid.vao);
    glGenBuffers(1, &solid.vbo);
    glGenBuffers(1, &solid.ebo);

    glBindVertexArray(solid.vao);
    glBindBuffer(GL_ARRAY_BUFFER, solid.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, solid.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)(idx.size() * sizeof(uint32_t)),
        idx.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
        (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    solid.indexCount = (GLsizei)idx.size();
    solid.valid = true;
    return solid;
}
