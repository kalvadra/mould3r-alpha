// MouldFeature.cpp — geometry builders for mould features.
// These are pure functions that produce GPU-ready SolidMesh objects.

#include "MouldFeature.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <string>
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
// BuildSphereMesh — UV sphere positioned at `center` with the given radius.
// Same stack/slice/vertex-generation math as GLCanvas::BuildSphereGPU, but
// baked to world position (that one is a shared unit sphere at the origin,
// scaled per-instance for marker rendering) — see the header comment.
//
// Vertex layout: [px, py, pz, nx, ny, nz] — compatible with the vsLit shader,
// matching BuildCylinderMesh.
// ---------------------------------------------------------------------------
SolidMesh BuildSphereMesh(const glm::vec3& center, float radius,
    int stacks, int slices)
{
    SolidMesh solid;
    solid.valid = false;

    if (radius < 1e-6f || stacks < 2 || slices < 3) return solid;

    std::vector<float>    verts;
    std::vector<uint32_t> idx;

    for (int i = 0; i <= stacks; ++i)
    {
        const float phi = glm::pi<float>() * float(i) / float(stacks);
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);

        for (int j = 0; j <= slices; ++j)
        {
            const float theta = 2.0f * glm::pi<float>() * float(j) / float(slices);
            const glm::vec3 n(sinPhi * std::cos(theta), cosPhi, sinPhi * std::sin(theta));

            verts.push_back(center.x + n.x * radius);
            verts.push_back(center.y + n.y * radius);
            verts.push_back(center.z + n.z * radius);
            verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
        }
    }

    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            const uint32_t a = uint32_t(i * (slices + 1) + j);
            const uint32_t b = uint32_t(a + slices + 1);
            idx.push_back(a);     idx.push_back(b);     idx.push_back(a + 1);
            idx.push_back(b);     idx.push_back(b + 1); idx.push_back(a + 1);
        }
    }

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
        // Straight A->B->C polyline, but emitted as one INDEPENDENT run per
        // segment rather than a single mitered chain. Each leg gets two
        // stations — both square to that leg's own direction — so the swept
        // box keeps a constant cross-section the whole way down the leg and
        // never shears or changes width through a corner. The first station of
        // every leg is flagged `startsRun`, so the sweeper caps each leg
        // separately instead of trying to bridge the orientation change at a
        // node. (Adjacent legs therefore leave a small wedge of empty space on
        // the outside of each bend; that gap is filled in a later step.)
        const int n = (int)nodes.size();
        for (int seg = 0; seg + 1 < n; ++seg)
        {
            const glm::vec3 d = nodes[seg + 1].pos - nodes[seg].pos;
            if (glm::length(glm::vec3(d.x, 0.0f, d.z)) < 1e-6f) continue; // skip degenerate leg
            const glm::vec3 fwd = inPlaneTangent(d);
            const glm::vec3 side = SideAxisOf(fwd);

            PathStation a;
            a.pos = nodes[seg].pos;
            a.tangent = fwd;
            a.sideAxis = side;
            a.startsRun = true;            // open a fresh prism for this leg

            PathStation b;
            b.pos = nodes[seg + 1].pos;
            b.tangent = fwd;
            b.sideAxis = side;             // same orientation along the whole leg

            out.push_back(a);
            out.push_back(b);
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
// ---------------------------------------------------------------------------
// MakeTwoNodePath - see the header for the contract (callers must only promote
// an already-resolved path).
// ---------------------------------------------------------------------------
void MakeTwoNodePath(FeaturePath& path, const glm::vec3& a, const glm::vec3& b)
{
    path.kind = PathKind::Complex;
    path.nodes.clear();

    PathNode n0; n0.pos = a;
    PathNode n1; n1.pos = b;
    path.nodes.push_back(n0);
    path.nodes.push_back(n1);

    path.start = a;
    path.end   = b;
    path.valid = true;

    // smooth is left as-is (a freshly derived path has it false). Handles are
    // only meaningful when smooth, and Catmull-Rom over two nodes yields the
    // straight chord either way.
    if (path.smooth) AutoComputeComplexHandles(path);
}

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

    // A joint fills the corner where two legs meet: the rectangular section
    // revolved 360 degrees about the vertical (Y) axis through the node. For a
    // rectangle that revolution is a cylinder of radius hw (half-width) and
    // height 2*hd (full depth), centred on the node. Being rotationally
    // symmetric it meets both legs flush at any bend angle and fills the open
    // wedge the per-leg prisms leave on the outside of the corner. Round-shaded
    // sides, flat caps.
    const int kJointSegments = 24;
    auto addJoint = [&](const glm::vec3& centre)
        {
            const glm::vec3 e1(1.0f, 0.0f, 0.0f);
            const glm::vec3 e2(0.0f, 0.0f, 1.0f);
            const glm::vec3 top = centre + upAxis * hd;
            const glm::vec3 bot = centre - upAxis * hd;

            auto pv = [&](const glm::vec3& p, const glm::vec3& nrm)
                {
                    verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
                    verts.push_back(nrm.x); verts.push_back(nrm.y); verts.push_back(nrm.z);
                };

            // Side wall - bottom ring then top ring, radial (round) normals.
            const uint32_t sideBase = (uint32_t)(verts.size() / 6);
            for (int ring = 0; ring < 2; ++ring)
            {
                const glm::vec3 c = (ring == 0) ? bot : top;
                for (int i = 0; i < kJointSegments; ++i)
                {
                    const float th =
                        (float(i) / float(kJointSegments)) * glm::two_pi<float>();
                    const glm::vec3 radial = std::cos(th) * e1 + std::sin(th) * e2;
                    pv(c + radial * hw, radial);
                }
            }
            for (int i = 0; i < kJointSegments; ++i)
            {
                const uint32_t b0 = sideBase + (uint32_t)i;
                const uint32_t b1 = sideBase + (uint32_t)((i + 1) % kJointSegments);
                const uint32_t t0 = sideBase + (uint32_t)(kJointSegments + i);
                const uint32_t t1 =
                    sideBase + (uint32_t)(kJointSegments + (i + 1) % kJointSegments);
                idx.push_back(b0); idx.push_back(b1); idx.push_back(t1);
                idx.push_back(b0); idx.push_back(t1); idx.push_back(t0);
            }

            // Caps - flat disks closing the top and bottom of the joint.
            auto addCap = [&](const glm::vec3& c, const glm::vec3& nrm)
                {
                    const uint32_t centreIdx = (uint32_t)(verts.size() / 6);
                    pv(c, nrm);
                    const uint32_t rimBase = (uint32_t)(verts.size() / 6);
                    for (int i = 0; i < kJointSegments; ++i)
                    {
                        const float th =
                            (float(i) / float(kJointSegments)) * glm::two_pi<float>();
                        const glm::vec3 radial =
                            std::cos(th) * e1 + std::sin(th) * e2;
                        pv(c + radial * hw, nrm);
                    }
                    for (int i = 0; i < kJointSegments; ++i)
                    {
                        const uint32_t r0 = rimBase + (uint32_t)i;
                        const uint32_t r1 =
                            rimBase + (uint32_t)((i + 1) % kJointSegments);
                        idx.push_back(centreIdx); idx.push_back(r0); idx.push_back(r1);
                    }
                };
            addCap(top, upAxis);
            addCap(bot, -upAxis);
        };

    // Sweep each run as its own capped prism. A run break (`startsRun`, plus
    // the implicit run start at index 0) is where one swept piece ends and the
    // next begins: we cap the start of a run, connect side walls only between
    // stations that belong to the SAME run, and cap the end of a run. For a
    // Simple or smooth-complex path the whole station list is one run, so this
    // reduces to exactly the old start-cap / side-walls / end-cap and the mesh
    // is unchanged. A straight complex path is one run per leg, so each leg
    // comes out as an independent constant-width box.
    const size_t N = stations.size();
    for (size_t i = 0; i < N; ++i)
    {
        const bool runStart = (i == 0) || stations[i].startsRun;
        const bool runEnd   = (i + 1 >= N) || stations[i + 1].startsRun;

        const auto A = ringOf(stations[i]);

        // Start cap — faces backward along this run's leading tangent.
        if (runStart)
            addQuad(A[0], A[3], A[2], A[1], -stations[i].tangent);

        // Side walls to the next station, but only within the same run.
        if (!runEnd)
        {
            const auto B = ringOf(stations[i + 1]);
            const glm::vec3 side = stations[i].sideAxis;

            addQuad(A[0], A[1], B[1], B[0], -upAxis);   // bottom
            addQuad(A[1], A[2], B[2], B[1], side);      // right
            addQuad(A[2], A[3], B[3], B[2], upAxis);    // top
            addQuad(A[3], A[0], B[0], B[3], -side);     // left
        }

        // End cap — faces forward along this run's trailing tangent.
        if (runEnd)
            addQuad(A[0], A[1], A[2], A[3], stations[i].tangent);

        // Interior node (an internal run boundary): drop a revolved joint so
        // the two legs meet through a filled, rounded corner. Endpoints - the
        // start of the first run and the end of the last - are not run
        // boundaries, so they keep their square caps.
        if (runEnd && (i + 1 < N))
            addJoint(stations[i].pos);
    }

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
// BuildTubeSweepMesh — sweeps a CIRCULAR cross-section along a feature path,
// the round-profile analogue of BuildBoxSweepMesh (used for runners).  It is
// driven by the same SamplePath stations, so a Simple path yields a plain
// straight cylinder visually identical to BuildCylinderMesh; a non-smooth
// complex path yields one independent constant-radius cylinder per leg with a
// SPHERE joint dropped at each interior node to fill the corner; a smooth
// complex path yields a single continuous swept tube.
//
// The joint is a sphere of the tube radius centred on the node: every point of
// an adjoining leg's circular end rim is exactly `radius` from the node, so the
// rim lies on the sphere and the sphere mates flush with both legs at any bend
// angle in all three axes — the circular counterpart of the vent's
// revolved-rectangle (cylinder) joint.  Interior nodes only; the two endpoints
// keep their flat disk caps.
//
// Vertex layout: [px, py, pz, nx, ny, nz] — compatible with vsLit.  Normals are
// the true outward directions (radial on the walls, spherical on the joints),
// so lighting is correct regardless of triangle winding (culling is disabled).
// ---------------------------------------------------------------------------
SolidMesh BuildTubeSweepMesh(const FeaturePath& path, float radius, int segments,
    float overrunStart, float overrunEnd, bool sphereAtStart, bool sphereAtEnd)
{
    SolidMesh solid;
    solid.valid = false;

    if (!path.valid || radius < 1e-6f || segments < 3) return solid;

    // One station list drives the whole sweep (Simple -> 2 stations, so this
    // stays a plain cylinder).  Matches BuildBoxSweepMesh's use of SamplePath so
    // the preview can never disagree with the OCC cut that consumes the same
    // stations.
    std::vector<PathStation> stations = SamplePath(path);
    if (stations.size() < 2) return solid;

    // Overruns extend the tube past its first/last station along the local
    // tangent (unused for runners today; kept for parity with BuildBoxSweepMesh).
    stations.front().pos -= stations.front().tangent * overrunStart;
    stations.back().pos  += stations.back().tangent  * overrunEnd;

    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    std::vector<float>    verts;
    std::vector<uint32_t> idx;

    auto pv = [&](const glm::vec3& p, const glm::vec3& n)
        {
            verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
            verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
        };

    // Outward radial direction of a station's cross-section at angle th.  The
    // section plane is spanned by the in-plane perpendicular (sideAxis) and the
    // vertical (up); because the path is planar these vary continuously along a
    // run, so consecutive rings stay aligned and the tube never twists.
    auto radialAt = [&](const PathStation& s, float th)
        {
            return std::cos(th) * s.sideAxis + std::sin(th) * up;
        };

    // Push a ring of `segments` verts (radial normals) at a station; return the
    // index of the ring's first vertex.
    auto pushRing = [&](const PathStation& s) -> uint32_t
        {
            const uint32_t base = (uint32_t)(verts.size() / 6);
            for (int i = 0; i < segments; ++i)
            {
                const float th = (float(i) / float(segments)) * glm::two_pi<float>();
                const glm::vec3 rad = radialAt(s, th);
                pv(s.pos + radius * rad, rad);
            }
            return base;
        };

    // Connect two rings (same segment count) with side quads.  Winding matches
    // BuildCylinderMesh; culling is off so only the normals matter.
    auto connectRings = [&](uint32_t a, uint32_t b)
        {
            for (int i = 0; i < segments; ++i)
            {
                const uint32_t s0 = a + (uint32_t)i;
                const uint32_t s1 = a + (uint32_t)((i + 1) % segments);
                const uint32_t e0 = b + (uint32_t)i;
                const uint32_t e1 = b + (uint32_t)((i + 1) % segments);
                idx.push_back(s0); idx.push_back(e0); idx.push_back(e1);
                idx.push_back(s0); idx.push_back(e1); idx.push_back(s1);
            }
        };

    // Flat disk cap at a station, facing capNorm (the run's leading/trailing
    // tangent, negated at the start).
    auto addCap = [&](const PathStation& s, const glm::vec3& capNorm)
        {
            const uint32_t centreIdx = (uint32_t)(verts.size() / 6);
            pv(s.pos, capNorm);
            const uint32_t rimBase = (uint32_t)(verts.size() / 6);
            for (int i = 0; i < segments; ++i)
            {
                const float th = (float(i) / float(segments)) * glm::two_pi<float>();
                pv(s.pos + radius * radialAt(s, th), capNorm);
            }
            for (int i = 0; i < segments; ++i)
            {
                const uint32_t r0 = rimBase + (uint32_t)i;
                const uint32_t r1 = rimBase + (uint32_t)((i + 1) % segments);
                idx.push_back(centreIdx); idx.push_back(r0); idx.push_back(r1);
            }
        };

    // Sphere joint (radius == tube radius) centred on an interior node.  A UV
    // sphere: `lon` longitude columns, `lat` latitude bands, spherical normals.
    auto addSphere = [&](const glm::vec3& c)
        {
            const int lon = segments;
            const int lat = std::max(6, segments / 2);
            const uint32_t base = (uint32_t)(verts.size() / 6);
            for (int i = 0; i <= lat; ++i)
            {
                const float phi = (float(i) / float(lat)) * glm::pi<float>();
                const float cy = std::cos(phi);
                const float sy = std::sin(phi);
                for (int j = 0; j <= lon; ++j)
                {
                    const float th = (float(j) / float(lon)) * glm::two_pi<float>();
                    const glm::vec3 n(sy * std::cos(th), cy, sy * std::sin(th));
                    pv(c + radius * n, n);
                }
            }
            const int stride = lon + 1;
            for (int i = 0; i < lat; ++i)
                for (int j = 0; j < lon; ++j)
                {
                    const uint32_t a = base + (uint32_t)(i * stride + j);
                    const uint32_t b = base + (uint32_t)((i + 1) * stride + j);
                    idx.push_back(a);     idx.push_back(b); idx.push_back(a + 1);
                    idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
                }
        };

    // Walk the stations run by run (a run break is `startsRun`, plus the
    // implicit run start at index 0).  Push a ring at every station, connect
    // consecutive rings WITHIN a run, cap the two ends of each run, and drop a
    // sphere joint at every interior node (an internal run boundary).  A Simple
    // path is a single 2-station run -> one capped cylinder identical to
    // BuildCylinderMesh; a non-smooth complex path is one run per leg.
    const size_t N = stations.size();

    std::vector<uint32_t> ringBase(N);
    for (size_t i = 0; i < N; ++i)
        ringBase[i] = pushRing(stations[i]);

    for (size_t i = 0; i + 1 < N; ++i)
        if (!stations[i + 1].startsRun)
            connectRings(ringBase[i], ringBase[i + 1]);

    for (size_t i = 0; i < N; ++i)
    {
        const bool runStart = (i == 0) || stations[i].startsRun;
        const bool runEnd   = (i + 1 >= N) || stations[i + 1].startsRun;

        if (runStart) addCap(stations[i], -stations[i].tangent);
        if (runEnd)   addCap(stations[i], +stations[i].tangent);
        if (runEnd && (i + 1 < N)) addSphere(stations[i].pos);
    }

    // Optional joint sphere at the very first station.  Used by gates: it rounds
    // the junction where the straight frustum cone meets a sub-runner that
    // leaves at an angle (i.e. a smooth sub-runner), filling the gap the flat
    // start cap would otherwise leave.  The disk cap underneath stays harmlessly
    // inside the sphere.
    if (sphereAtStart) addSphere(stations.front().pos);

    // Optional joint sphere at the very last station.  Used by gates so the
    // feed end blends into the sprue/runner it meets — a sphere at every
    // sub-runner node except the gate origin.
    if (sphereAtEnd)   addSphere(stations.back().pos);

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

// ===========================================================================
// Gate types — Tab / Fan gate block (see MouldFeature.h)
// ===========================================================================
const char* GateTypeLabel(GateType t)
{
    switch (t)
    {
    case GateType::Tab: return "Tab";
    case GateType::Fan: return "Fan";
    case GateType::TaperedCylinder:
    default:            return "Tapered Cylinder";
    }
}

GateType GateTypeFromLabel(const char* label)
{
    if (!label) return GateType::TaperedCylinder;
    const std::string s(label);
    if (s == "Tab") return GateType::Tab;
    if (s == "Fan") return GateType::Fan;
    return GateType::TaperedCylinder;
}

const char* GateSideLabel(GateSide s)
{
    switch (s)
    {
    case GateSide::A: return "A side";
    case GateSide::B: return "B side";
    case GateSide::Both:
    default:          return "Both";
    }
}

GateSide GateSideFromLabel(const char* label)
{
    if (!label) return GateSide::Both;
    const std::string s(label);
    if (s == "A side") return GateSide::A;
    if (s == "B side") return GateSide::B;
    return GateSide::Both;
}

void GateBlockVertical(GateSide side, float height, float crossEps,
                       float& outHeight, float& outUpOffset)
{
    crossEps = std::max(crossEps, 0.0f);
    switch (side)
    {
    case GateSide::A:   // spans [-crossEps, +height] along up
        outHeight   = height + crossEps;
        outUpOffset = 0.5f * (height - crossEps);
        break;
    case GateSide::B:   // spans [-height, +crossEps]
        outHeight   = height + crossEps;
        outUpOffset = -0.5f * (height - crossEps);
        break;
    case GateSide::Both:
    default:
        outHeight   = height;
        outUpOffset = 0.0f;
        break;
    }
}

float GateFanSlope(float width, float length, float taperDeg)
{
    if (!(taperDeg > 0.0f) || !(length > 1e-6f)) return 0.0f;
    const float wanted = std::tan(glm::radians(std::min(taperDeg, 80.0f)));
    // Largest slope that still leaves kGateFanMinFrontWidth at the far end.
    const float allowed = (width - kGateFanMinFrontWidth) / (2.0f * length);
    if (!(allowed > 0.0f)) return 0.0f;
    return std::min(wanted, allowed);
}

void GateBlockAxes(const glm::vec3& dir, glm::vec3& sideAxis, glm::vec3& upAxis)
{
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    glm::vec3 up = worldUp - glm::dot(worldUp, dir) * dir;
    if (glm::length(up) < 1e-4f)
    {
        // A (near-)vertical leg has no "up" — fall back to world X.
        const glm::vec3 x(1.0f, 0.0f, 0.0f);
        up = x - glm::dot(x, dir) * dir;
    }
    upAxis = glm::normalize(up);
    // cross(dir, up): for a level leg this is (-d.z, 0, d.x), the same side
    // axis BuildVentCrossSection uses.
    sideAxis = glm::normalize(glm::cross(dir, upAxis));
}

GateBlock MakeGateBlock(const glm::vec3& origin, const glm::vec3& dir,
    float width, float height, float slope, float backExt, float frontLen,
    float upOffset)
{
    GateBlock b;
    const float dLen = glm::length(dir);
    if (dLen < 1e-6f || !(width > 1e-6f) || !(height > 1e-6f)) return b;
    if (!(backExt + frontLen > 1e-6f)) return b;

    b.dir = dir / dLen;
    GateBlockAxes(b.dir, b.sideAxis, b.upAxis);

    const float hh  = 0.5f * height;
    const float hwB = 0.5f * width + slope * backExt;
    // GateFanSlope keeps the nominal far end >= kGateFanMinFrontWidth; the
    // floor here only guards callers that pass their own slope.
    const float hwF = std::max(0.5f * width - slope * frontLen,
                               0.5f * kGateFanMinFrontWidth);

    const glm::vec3 lift = b.upAxis * upOffset;
    const glm::vec3 cB = origin - b.dir * backExt + lift;
    const glm::vec3 cF = origin + b.dir * frontLen + lift;
    auto rect = [&](const glm::vec3& c, float hw, glm::vec3* out)
    {
        out[0] = c - b.sideAxis * hw - b.upAxis * hh;   // BL
        out[1] = c + b.sideAxis * hw - b.upAxis * hh;   // BR
        out[2] = c + b.sideAxis * hw + b.upAxis * hh;   // TR
        out[3] = c - b.sideAxis * hw + b.upAxis * hh;   // TL
    };
    rect(cB, hwB, b.back);
    rect(cF, hwF, b.front);

    b.backWidth  = 2.0f * hwB;
    b.frontWidth = 2.0f * hwF;
    b.height     = height;
    b.axialLen   = backExt + frontLen;
    b.valid      = true;
    return b;
}

// ---------------------------------------------------------------------------
// BuildGateBlockMesh — six flat-shaded quads (4 unshared vertices each so the
// edges stay crisp). Each quad's winding is checked against the outward
// direction from the block centre, so the result is outward-facing whatever
// handedness the side / up axes came out with.
// ---------------------------------------------------------------------------
SolidMesh BuildGateBlockMesh(const GateBlock& block)
{
    SolidMesh solid;
    solid.valid = false;
    if (!block.valid) return solid;

    const glm::vec3* B = block.back;
    const glm::vec3* F = block.front;
    glm::vec3 centre(0.0f);
    for (int i = 0; i < 4; ++i) centre += B[i] + F[i];
    centre /= 8.0f;

    std::vector<float>    verts;
    std::vector<uint32_t> idx;
    verts.reserve(6 * 4 * 6);
    idx.reserve(6 * 6);

    auto quad = [&](const glm::vec3& a, const glm::vec3& b,
                    const glm::vec3& c, const glm::vec3& d)
    {
        glm::vec3 n = glm::cross(b - a, c - a);
        if (glm::length(n) < 1e-12f) n = glm::cross(c - a, d - a);
        if (glm::length(n) < 1e-12f) return;   // collapsed face
        n = glm::normalize(n);
        const glm::vec3 faceC = 0.25f * (a + b + c + d);
        const bool flip = glm::dot(n, faceC - centre) < 0.0f;
        if (flip) n = -n;

        const uint32_t base = (uint32_t)(verts.size() / 6);
        for (const glm::vec3* p : { &a, &b, &c, &d })
        {
            verts.push_back(p->x); verts.push_back(p->y); verts.push_back(p->z);
            verts.push_back(n.x);  verts.push_back(n.y);  verts.push_back(n.z);
        }
        if (!flip)
        {
            idx.push_back(base); idx.push_back(base + 1); idx.push_back(base + 2);
            idx.push_back(base); idx.push_back(base + 2); idx.push_back(base + 3);
        }
        else
        {
            idx.push_back(base); idx.push_back(base + 2); idx.push_back(base + 1);
            idx.push_back(base); idx.push_back(base + 3); idx.push_back(base + 2);
        }
    };

    quad(B[0], B[1], B[2], B[3]);                 // back (part side)
    quad(F[0], F[1], F[2], F[3]);                 // front (sub-runner side)
    for (int i = 0; i < 4; ++i)                   // bottom, right, top, left
    {
        const int j = (i + 1) % 4;
        quad(B[i], B[j], F[j], F[i]);
    }
    if (idx.empty()) return solid;

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

// ===========================================================================
// Channel profiles — runner / sub-runner cross-sections (see MouldFeature.h)
// ===========================================================================
namespace
{
    constexpr float kChannelMaxDraftDeg = 60.0f;
    constexpr float kChannelMinHalfFloor = 0.05f;   // trapezoid floor half-width floor

    // Uploads [pos, normal] triangles as a SolidMesh (same attribute layout as
    // every other builder in this file).
    SolidMesh UploadSolidMesh(const std::vector<float>& verts,
                              const std::vector<uint32_t>& idx)
    {
        SolidMesh solid;
        solid.valid = false;
        if (verts.empty() || idx.empty()) return solid;

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

    float ChannelDraftRad(const ChannelProfile& p)
    {
        return glm::radians(glm::clamp(p.draftDeg, 0.0f, kChannelMaxDraftDeg));
    }

    // Circumcentre of three 2D points (the arc's centre).
    glm::vec2 Circumcentre(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c)
    {
        const float d = 2.0f * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (std::abs(d) < 1e-12f) return 0.5f * (a + c);
        const float a2 = glm::dot(a, a), b2 = glm::dot(b, b), c2 = glm::dot(c, c);
        return glm::vec2((a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d,
                         (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d);
    }

    // Tessellated outline for the mesh: one entry per straight piece, each
    // with its own end normals (so corners stay crisp and the arc shades
    // round). Normals point out of the section.
    struct MeshEdge { glm::vec2 a, b, na, nb; };

    std::vector<MeshEdge> TessellateOutline(const std::vector<ChannelSeg>& segs, int arcSegs)
    {
        std::vector<MeshEdge> out;
        for (const ChannelSeg& sg : segs)
        {
            if (!sg.arc)
            {
                const glm::vec2 d = sg.b - sg.a;
                const float l = glm::length(d);
                if (l < 1e-7f) continue;
                const glm::vec2 n(d.y / l, -d.x / l);   // right-hand normal: outward for CCW
                out.push_back({ sg.a, sg.b, n, n });
                continue;
            }
            const glm::vec2 c = Circumcentre(sg.a, sg.mid, sg.b);
            const float r = glm::length(sg.a - c);
            const float t0 = std::atan2(sg.a.y - c.y, sg.a.x - c.x);
            const float tm = std::atan2(sg.mid.y - c.y, sg.mid.x - c.x);
            const float t1 = std::atan2(sg.b.y - c.y, sg.b.x - c.x);
            auto wrap = [](float x) { const float tp = glm::two_pi<float>();
                                      while (x < 0.0f) x += tp; while (x >= tp) x -= tp; return x; };
            // Counter-clockwise sweep a->b; take the clockwise one instead when
            // the mid point isn't on the CCW side.
            float sweep = wrap(t1 - t0);
            if (wrap(tm - t0) > sweep) sweep -= glm::two_pi<float>();
            const int n = std::max(2, arcSegs);
            for (int k = 0; k < n; ++k)
            {
                const float u0 = t0 + sweep * float(k) / float(n);
                const float u1 = t0 + sweep * float(k + 1) / float(n);
                const glm::vec2 n0(std::cos(u0), std::sin(u0)), n1(std::cos(u1), std::sin(u1));
                const glm::vec2 p0 = (k == 0) ? sg.a : c + r * n0;
                const glm::vec2 p1 = (k == n - 1) ? sg.b : c + r * n1;
                out.push_back({ p0, p1, n0, n1 });
            }
        }
        return out;
    }
}

ChannelType ChannelTypeFromLabel(const char* label)
{
    if (!label) return ChannelType::Cylinder;
    const std::string s(label);
    if (s == "Parabolic")   return ChannelType::Parabolic;
    if (s == "Trapezoidal") return ChannelType::Trapezoidal;
    return ChannelType::Cylinder;   // "Cylinder", "Cylindrical", unknown
}

float ChannelHalfWidth(const ChannelProfile& p)
{
    switch (p.type)
    {
    case ChannelType::Parabolic:
    {
        const float a = ChannelDraftRad(p);
        return 0.5f * p.diameter * (1.0f + std::sin(a)) / std::cos(a);
    }
    case ChannelType::Trapezoidal: return 0.5f * p.width;
    case ChannelType::Cylinder:
    default:                       return 0.5f * p.diameter;
    }
}

float ChannelConeRadius(const ChannelProfile& p)
{
    switch (p.type)
    {
    case ChannelType::Trapezoidal: return 0.5f * p.height;
    case ChannelType::Parabolic:
    case ChannelType::Cylinder:
    default:                       return 0.5f * p.diameter;
    }
}

void ChannelAreaPerimeter(const ChannelProfile& p, double& area, double& perimeter)
{
    const double pi = glm::pi<double>();
    switch (p.type)
    {
    case ChannelType::Parabolic:
    {
        const double R = 0.5 * std::max(0.0f, p.diameter);
        const double a = ChannelDraftRad(p), s = std::sin(a), c = std::cos(a);
        const double w0 = R * (1.0 + s) / c;        // half-width at the plane
        const double uT = R * (1.0 + s);            // wall tangent height
        // Trapezoid up to the tangent points + the circular cap above them.
        area = (w0 + R * c) * uT + R * R * (0.5 * pi - a - s * c);
        const double wall = std::sqrt((w0 - R * c) * (w0 - R * c) + uT * uT);
        perimeter = 2.0 * w0 + 2.0 * wall + R * (pi - 2.0 * a);
        return;
    }
    case ChannelType::Trapezoidal:
    {
        const double hw = 0.5 * std::max(0.0f, p.width);
        const double H  = std::max(0.0f, p.height);
        const double hf = std::max((double)kChannelMinHalfFloor,
                                   hw - H * std::tan(ChannelDraftRad(p)));
        area = (hw + hf) * H;
        perimeter = 2.0 * hw + 2.0 * hf + 2.0 * std::sqrt((hw - hf) * (hw - hf) + H * H);
        return;
    }
    case ChannelType::Cylinder:
    default:
    {
        const double R = 0.5 * std::max(0.0f, p.diameter);
        area = pi * R * R;
        perimeter = 2.0 * pi * R;
        return;
    }
    }
}

std::vector<ChannelSeg> ChannelExactOutline(const ChannelProfile& p, float crossEps)
{
    std::vector<ChannelSeg> out;
    const float e = std::max(0.0f, crossEps);
    const float a = ChannelDraftRad(p);
    const float t = std::tan(a);
    auto line = [&](const glm::vec2& p0, const glm::vec2& p1)
    {
        ChannelSeg sg; sg.a = p0; sg.b = p1; out.push_back(sg);
    };

    if (p.type == ChannelType::Parabolic)
    {
        const float R = 0.5f * p.diameter;
        if (!(R > 1e-6f)) return out;
        const float s = std::sin(a), c = std::cos(a);
        const float wB = R * (1.0f + s) / c + e * t;          // base half-width at u = -e
        const glm::vec2 Tr( R * c, R * (1.0f + s));            // right wall tangent point
        const glm::vec2 Tl(-R * c, R * (1.0f + s));
        line(glm::vec2(-wB, -e), glm::vec2(wB, -e));            // base
        line(glm::vec2(wB, -e), Tr);                            // right wall
        ChannelSeg arc; arc.arc = true; arc.a = Tr; arc.mid = glm::vec2(0.0f, 2.0f * R); arc.b = Tl;
        out.push_back(arc);                                     // top of the circle
        line(Tl, glm::vec2(-wB, -e));                           // left wall
        return out;
    }
    if (p.type == ChannelType::Trapezoidal)
    {
        const float hw = 0.5f * p.width, H = p.height;
        if (!(hw > 1e-6f) || !(H > 1e-6f)) return out;
        const float hB = hw + e * t;
        const float hF = std::max(kChannelMinHalfFloor, hw - H * t);
        line(glm::vec2(-hB, -e), glm::vec2(hB, -e));            // base
        line(glm::vec2(hB, -e), glm::vec2(hF, H));              // right wall
        line(glm::vec2(hF, H), glm::vec2(-hF, H));              // floor
        line(glm::vec2(-hF, H), glm::vec2(-hB, -e));            // left wall
        return out;
    }
    return out;   // Cylinder: no one-sided outline
}

ChannelJointShape ChannelJoint(const ChannelProfile& p, float crossEps)
{
    ChannelJointShape j;
    const float e = std::max(0.0f, crossEps);
    const float a = ChannelDraftRad(p);
    const float t = std::tan(a);
    if (p.type == ChannelType::Parabolic)
    {
        const float R = 0.5f * p.diameter;
        const float s = std::sin(a), c = std::cos(a);
        j.u0 = -e;                j.r0 = R * (1.0f + s) / c + e * t;
        j.u1 = R * (1.0f + s);    j.r1 = R * c;
        j.sphere = true;          j.sphereU = R;  j.sphereR = R;
    }
    else if (p.type == ChannelType::Trapezoidal)
    {
        const float hw = 0.5f * p.width;
        j.u0 = -e;                j.r0 = hw + e * t;
        j.u1 = p.height;          j.r1 = std::max(kChannelMinHalfFloor, hw - p.height * t);
    }
    return j;
}

void ChannelSweepGeometry(const FeaturePath& path, const ChannelProfile& profile,
    bool jointAtStart, bool jointAtEnd,
    std::vector<float>& verts, std::vector<uint32_t>& idx)
{
    if (!path.valid || profile.IsRound()) return;
    const std::vector<ChannelSeg> segs = ChannelExactOutline(profile, 0.0f);
    if (segs.empty()) return;
    const std::vector<MeshEdge> edges = TessellateOutline(segs, 24);
    if (edges.empty()) return;

    const std::vector<PathStation> stations = SamplePath(path);
    if (stations.size() < 2) return;

    const glm::vec3 Y(0.0f, 1.0f, 0.0f);
    const float sign = profile.SideSign();

    auto pv = [&](const glm::vec3& p, const glm::vec3& n)
    {
        verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
        verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
    };
    auto toWorld = [&](const PathStation& st, const glm::vec2& q)
    {
        return st.pos + st.sideAxis * q.x + Y * (sign * q.y);
    };
    auto dirWorld = [&](const PathStation& st, const glm::vec2& n)
    {
        return glm::normalize(st.sideAxis * n.x + Y * (sign * n.y));
    };

    // Section centroid (vertex average) for the end-cap fans.
    glm::vec2 centroid(0.0f);
    for (const MeshEdge& me : edges) centroid += me.a;
    centroid /= float(edges.size());

    // Ring: two verts per edge (its own end normals).
    auto pushRing = [&](const PathStation& st) -> uint32_t
    {
        const uint32_t base = (uint32_t)(verts.size() / 6);
        for (const MeshEdge& me : edges)
        {
            pv(toWorld(st, me.a), dirWorld(st, me.na));
            pv(toWorld(st, me.b), dirWorld(st, me.nb));
        }
        return base;
    };
    auto connectRings = [&](uint32_t r0, uint32_t r1)
    {
        for (uint32_t k = 0; k < (uint32_t)edges.size(); ++k)
        {
            const uint32_t a0 = r0 + 2 * k, b0 = a0 + 1;
            const uint32_t a1 = r1 + 2 * k, b1 = a1 + 1;
            idx.push_back(a0); idx.push_back(b0); idx.push_back(b1);
            idx.push_back(a0); idx.push_back(b1); idx.push_back(a1);
        }
    };
    auto addCap = [&](const PathStation& st, const glm::vec3& n)
    {
        const uint32_t c = (uint32_t)(verts.size() / 6);
        pv(toWorld(st, centroid), n);
        const uint32_t rim = (uint32_t)(verts.size() / 6);
        for (const MeshEdge& me : edges) pv(toWorld(st, me.a), n);
        const uint32_t E = (uint32_t)edges.size();
        for (uint32_t k = 0; k < E; ++k)
        {
            idx.push_back(c); idx.push_back(rim + k); idx.push_back(rim + (k + 1) % E);
        }
    };

    // Revolved joint about the vertical axis through `centre` (ChannelJoint):
    // a cone frustum with both end discs, plus the profile's circle as a
    // sphere for a Parabolic channel.
    const ChannelJointShape J = ChannelJoint(profile, 0.0f);
    const int kRev = 32;
    auto addJoint = [&](const glm::vec3& centre)
    {
        auto at = [&](float r, float u, float phi)
        {
            return centre + glm::vec3(std::cos(phi) * r, sign * u, std::sin(phi) * r);
        };
        // Frustum wall: outward normal tilts by the wall slope.
        const float dr = J.r1 - J.r0, du = J.u1 - J.u0;
        const float wl = std::sqrt(dr * dr + du * du);
        const float nr = (wl > 1e-9f) ? du / wl : 1.0f;     // radial component
        const float nu = (wl > 1e-9f) ? -dr / wl : 0.0f;    // axial component
        const uint32_t wall = (uint32_t)(verts.size() / 6);
        for (int ring = 0; ring < 2; ++ring)
            for (int k = 0; k < kRev; ++k)
            {
                const float phi = glm::two_pi<float>() * float(k) / float(kRev);
                const glm::vec3 n = glm::normalize(glm::vec3(std::cos(phi) * nr, sign * nu,
                                                             std::sin(phi) * nr));
                pv(ring == 0 ? at(J.r0, J.u0, phi) : at(J.r1, J.u1, phi), n);
            }
        for (int k = 0; k < kRev; ++k)
        {
            const uint32_t a0 = wall + k, a1 = wall + (k + 1) % kRev;
            const uint32_t b0 = a0 + kRev, b1 = a1 + kRev;
            idx.push_back(a0); idx.push_back(b0); idx.push_back(b1);
            idx.push_back(a0); idx.push_back(b1); idx.push_back(a1);
        }
        // End discs (the base at the parting plane, the floor / top).
        for (int disc = 0; disc < 2; ++disc)
        {
            const float r = disc == 0 ? J.r0 : J.r1;
            const float u = disc == 0 ? J.u0 : J.u1;
            const glm::vec3 n(0.0f, disc == 0 ? -sign : sign, 0.0f);
            const uint32_t c = (uint32_t)(verts.size() / 6);
            pv(centre + glm::vec3(0.0f, sign * u, 0.0f), n);
            for (int k = 0; k < kRev; ++k)
                pv(at(r, u, glm::two_pi<float>() * float(k) / float(kRev)), n);
            for (int k = 0; k < kRev; ++k)
            {
                idx.push_back(c); idx.push_back(c + 1 + k); idx.push_back(c + 1 + (k + 1) % kRev);
            }
        }
        if (J.sphere && J.sphereR > 1e-6f)
        {
            const glm::vec3 sc = centre + glm::vec3(0.0f, sign * J.sphereU, 0.0f);
            const int lon = kRev, lat = kRev / 2;
            const uint32_t base = (uint32_t)(verts.size() / 6);
            for (int i = 0; i <= lat; ++i)
            {
                const float ph = glm::pi<float>() * float(i) / float(lat);
                for (int k = 0; k <= lon; ++k)
                {
                    const float th = glm::two_pi<float>() * float(k) / float(lon);
                    const glm::vec3 n(std::sin(ph) * std::cos(th), std::cos(ph),
                                      std::sin(ph) * std::sin(th));
                    pv(sc + J.sphereR * n, n);
                }
            }
            const int stride = lon + 1;
            for (int i = 0; i < lat; ++i)
                for (int k = 0; k < lon; ++k)
                {
                    const uint32_t a = base + i * stride + k, b = a + stride;
                    idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
                    idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
                }
        }
    };

    // Same run walk as BuildTubeSweepMesh: rings at every station, sides
    // within a run, caps at run ends, a joint at every interior node.
    const size_t N = stations.size();
    std::vector<uint32_t> ring(N);
    for (size_t i = 0; i < N; ++i) ring[i] = pushRing(stations[i]);
    for (size_t i = 0; i + 1 < N; ++i)
        if (!stations[i + 1].startsRun) connectRings(ring[i], ring[i + 1]);
    for (size_t i = 0; i < N; ++i)
    {
        const bool runStart = (i == 0) || stations[i].startsRun;
        const bool runEnd   = (i + 1 >= N) || stations[i + 1].startsRun;
        if (runStart) addCap(stations[i], -stations[i].tangent);
        if (runEnd)   addCap(stations[i], +stations[i].tangent);
        if (runEnd && (i + 1 < N)) addJoint(stations[i].pos);
    }
    if (jointAtStart) addJoint(stations.front().pos);
    if (jointAtEnd)   addJoint(stations.back().pos);
}

SolidMesh BuildChannelSweepMesh(const FeaturePath& path, const ChannelProfile& profile,
    bool jointAtStart, bool jointAtEnd)
{
    if (profile.IsRound())
        return BuildTubeSweepMesh(path, 0.5f * profile.diameter, /*segments=*/32,
            /*overrunStart=*/0.0f, /*overrunEnd=*/0.0f, jointAtStart, jointAtEnd);

    std::vector<float>    verts;
    std::vector<uint32_t> idx;
    ChannelSweepGeometry(path, profile, jointAtStart, jointAtEnd, verts, idx);
    return UploadSolidMesh(verts, idx);
}
