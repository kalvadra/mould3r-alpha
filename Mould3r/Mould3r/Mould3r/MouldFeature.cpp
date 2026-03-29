// MouldFeature.cpp — geometry builders for mould features.
// These are pure functions that produce GPU-ready SolidMesh objects.

#include "MouldFeature.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
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
// BuildBoxSweepMesh — sweeps a rectangular cross-section along a vent path.
// Vertex layout: [px, py, pz, nx, ny, nz] — compatible with the vsLit shader.
// ---------------------------------------------------------------------------
SolidMesh BuildBoxSweepMesh(const VentPath& path, float width, float depth,
    float overrunStart, float overrunEnd)
{
    SolidMesh solid;
    solid.valid = false;

    if (!path.valid) return solid;

    const glm::vec3 diff = path.end - path.start;
    const float     len = glm::length(glm::vec2(diff.x, diff.z));
    if (len < 1e-6f) return solid;

    const glm::vec3 pathDir(-diff.x / len, 0.0f, -diff.z / len);
    const glm::vec3 sideAxis(-pathDir.z, 0.0f, pathDir.x);
    const glm::vec3 upAxis(0.0f, 1.0f, 0.0f);

    const float hw = width * 0.5f;
    const float hd = depth * 0.5f;

    const glm::vec3 sweepDir = -pathDir;

    const glm::vec3 extendedStart = path.start - sweepDir * overrunStart;
    const glm::vec3 extendedEnd = path.end + sweepDir * overrunEnd;

    auto makeCorners = [&](const glm::vec3& centre) -> std::array<glm::vec3, 4>
        {
            return { {
                centre - sideAxis * hw - upAxis * hd,
                centre + sideAxis * hw - upAxis * hd,
                centre + sideAxis * hw + upAxis * hd,
                centre - sideAxis * hw + upAxis * hd
            } };
        };

    const auto startC = makeCorners(extendedStart);
    const auto endC = makeCorners(extendedEnd);

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

    addQuad(startC[0], startC[3], startC[2], startC[1], -sweepDir);
    addQuad(endC[0], endC[1], endC[2], endC[3], sweepDir);
    addQuad(startC[0], startC[1], endC[1], endC[0], -upAxis);
    addQuad(startC[1], startC[2], endC[2], endC[1], sideAxis);
    addQuad(startC[2], startC[3], endC[3], endC[2], upAxis);
    addQuad(startC[3], startC[0], endC[0], endC[3], -sideAxis);

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
