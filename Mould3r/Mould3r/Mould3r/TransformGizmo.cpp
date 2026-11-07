// The CRT headers (via glm / <cmath>) arrive before the wx ones used for the
// readout text, so wx's own opt-out would come too late for the project's SDL
// checks (C4996) -- same as GLCanvas.cpp / NavCube.cpp.
#define _CRT_SECURE_NO_WARNINGS

#include "TransformGizmo.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>   // glm::ortho (readout)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#ifndef GIZMO_NO_WX
#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/settings.h>
#endif

namespace Gizmo
{
namespace
{
    // Handle layout, as fractions of the arrow length L.
    constexpr float kShaftStart = 0.18f;   // clear of the centre ball
    constexpr float kHeadStart  = 0.80f;   // cone from here to the tip
    constexpr float kShaftR     = 0.0145f; // ~1.6 px
    constexpr float kHeadR      = 0.055f;
    constexpr float kPlaneLo    = 0.28f;   // plane square spans [lo, hi] on both axes
    constexpr float kPlaneHi    = 0.48f;
    constexpr float kBallR      = 0.065f;  // ~7 px
    constexpr float kRingTube   = kShaftR / kRingRadius;   // tube radius on a unit ring
    constexpr int   kRingSamples = 96;                     // hit-test polyline
    constexpr float kMinRingDot = 0.12f;   // |cos| between ring normal and ray: edge-on below
    constexpr float kScaleCubePos = 0.94f; // axis cube centre along its axis
    constexpr float kScaleCubeR   = 0.055f;// axis cube half-size (~6 px)
    constexpr float kCentreCubeR  = 0.07f; // centre cube half-size (~8 px)

    // Hit tolerances, pixels.
    constexpr float kAxisTolPx  = 7.0f;
    constexpr float kFreeTolPx  = 10.0f;
    constexpr float kMinPlaneDot = 0.2f;   // |cos| between plane normal and view: edge-on below

    const glm::vec3 kAxes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

    int AxisIndex(Handle h)
    {
        switch (h)
        {
        case Handle::X: case Handle::PlaneYZ: return 0;
        case Handle::Y: case Handle::PlaneXZ: return 1;
        case Handle::Z: case Handle::PlaneXY: return 2;
        default: return -1;
        }
    }
    bool IsAxis(Handle h)  { return h == Handle::X || h == Handle::Y || h == Handle::Z; }
    bool IsPlane(Handle h) { return h == Handle::PlaneYZ || h == Handle::PlaneXZ || h == Handle::PlaneXY; }

    // The two in-plane axes of a plane handle (normal axis n).
    void PlaneAxes(int n, int& a, int& b) { a = (n + 1) % 3; b = (n + 2) % 3; if (a > b) std::swap(a, b); }

    glm::vec3 CameraPos(const Viewport& vp) { return glm::vec3(glm::inverse(vp.view)[3]); }

    float Ramp(float x, float lo, float hi) { return glm::clamp((x - lo) / (hi - lo), 0.0f, 1.0f); }

    // How usable an axis handle (arrow / scale stalk) is from here: 0 when it
    // points (nearly) at the camera -- drawn hidden and not hittable -- rising
    // to 1 once it is clearly side-on. The hit tests and the renderer share
    // it, so whatever is visible can be grabbed and nothing hidden can be.
    constexpr float kAxisHideSin = 0.11f;   // ~12 px of a 110 px arrow
    float AxisVisibility(const Viewport& vp, const glm::vec3& pivot, int axis)
    {
        const glm::vec3 view = glm::normalize(pivot - CameraPos(vp));
        const float c = glm::dot(view, kAxes[axis]);
        return Ramp(std::sqrt(std::max(0.0f, 1.0f - c * c)), kAxisHideSin, kAxisHideSin + 0.17f);
    }
    // Same for a plane square (normal axis n): 0 when seen edge-on.
    float PlaneVisibility(const Viewport& vp, const glm::vec3& pivot, int n)
    {
        const glm::vec3 view = glm::normalize(pivot - CameraPos(vp));
        return Ramp(std::abs(glm::dot(view, kAxes[n])), kMinPlaneDot, kMinPlaneDot + 0.15f);
    }

    bool ToScreen(const Viewport& vp, const glm::vec3& p, glm::vec2& out)
    {
        const glm::vec4 c = vp.proj * vp.view * glm::vec4(p, 1.0f);
        if (c.w <= 1e-6f) return false;   // behind the camera
        const glm::vec3 ndc = glm::vec3(c) / c.w;
        out = glm::vec2((ndc.x + 1.0f) * 0.5f * float(vp.width),
                        (1.0f - ndc.y) * 0.5f * float(vp.height));
        return true;
    }

    float SegmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b)
    {
        const glm::vec2 ab = b - a;
        const float l2 = glm::dot(ab, ab);
        const float t = l2 > 1e-9f ? glm::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
        return glm::length(p - (a + ab * t));
    }

    // Convex quad (any winding) contains p.
    bool InQuad(const glm::vec2& p, const glm::vec2 q[4])
    {
        int pos = 0, neg = 0;
        for (int i = 0; i < 4; ++i)
        {
            const glm::vec2 e = q[(i + 1) % 4] - q[i];
            const glm::vec2 v = p - q[i];
            const float c = e.x * v.y - e.y * v.x;
            if (c > 0.0f) ++pos; else if (c < 0.0f) ++neg;
        }
        return pos == 0 || neg == 0;
    }

    // Closest point on the line o + t*u to the ray ro + s*v (u, v unit).
    bool LineRayClosest(const glm::vec3& o, const glm::vec3& u,
                        const glm::vec3& ro, const glm::vec3& v, float& t)
    {
        const float b = glm::dot(u, v);
        const float denom = 1.0f - b * b;
        if (denom < 1e-4f) return false;   // ray (nearly) along the axis
        const glm::vec3 w0 = o - ro;
        const float d = glm::dot(u, w0), e = glm::dot(v, w0);
        t = (b * e - d) / denom;
        return true;
    }

    bool RayPlane(const glm::vec3& po, const glm::vec3& n,
                  const glm::vec3& ro, const glm::vec3& v, glm::vec3& hit)
    {
        const float dn = glm::dot(v, n);
        if (std::abs(dn) < 0.02f) return false;   // grazing: tiny moves would leap
        const float s = glm::dot(po - ro, n) / dn;
        if (s <= 0.0f) return false;              // behind the camera
        hit = ro + v * s;
        return true;
    }

    float RoundTo(float v, float step) { return std::round(v / step) * step; }
}

float WorldPerPixel(const Viewport& vp, const glm::vec3& at)
{
    // Perspective: one pixel spans 2 * depth * tan(fov/2) / height at that
    // depth, and proj[1][1] = 1 / tan(fov/2).
    const float depth = std::max(1e-4f, -(vp.view * glm::vec4(at, 1.0f)).z);
    const float f = std::max(1e-6f, vp.proj[1][1]);
    return 2.0f * depth / (f * float(std::max(1, vp.height)));
}

void MouseRay(const Viewport& vp, float mx, float my, glm::vec3& origin, glm::vec3& dir)
{
    // Same unprojection as GLCanvas::BuildMouseRay.
    const float ndcX = (2.0f * mx) / float(std::max(1, vp.width)) - 1.0f;
    const float ndcY = 1.0f - (2.0f * my) / float(std::max(1, vp.height));
    const glm::mat4 inv = glm::inverse(vp.proj * vp.view);
    glm::vec4 n = inv * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 f = inv * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    n /= n.w; f /= f.w;
    origin = glm::vec3(n);
    dir = glm::normalize(glm::vec3(f) - glm::vec3(n));
}

Handle HitTestMove(const Viewport& vp, const glm::vec3& pivot, float mx, float my)
{
    const glm::vec2 m(mx, my);
    const float L = HandleLength(vp, pivot);

    glm::vec2 sp;
    if (!ToScreen(vp, pivot, sp)) return Handle::None;

    // 1. Centre ball.
    if (glm::length(m - sp) <= kFreeTolPx) return Handle::Free;

    // 2. Nearest axis arrow within tolerance.
    Handle best = Handle::None;
    float bestD = kAxisTolPx;
    const Handle axisHandles[3] = { Handle::X, Handle::Y, Handle::Z };
    for (int i = 0; i < 3; ++i)
    {
        glm::vec2 a, b;
        if (AxisVisibility(vp, pivot, i) <= 0.0f) continue;   // pointing at the camera
        if (!ToScreen(vp, pivot + kAxes[i] * (kShaftStart * L), a) ||
            !ToScreen(vp, pivot + kAxes[i] * L, b)) continue;
        const float d = SegmentDistance(m, a, b);
        if (d <= bestD) { bestD = d; best = axisHandles[i]; }
    }
    if (best != Handle::None) return best;

    // 3. Plane squares.
    const Handle planeHandles[3] = { Handle::PlaneYZ, Handle::PlaneXZ, Handle::PlaneXY };
    for (int n = 0; n < 3; ++n)
    {
        if (PlaneVisibility(vp, pivot, n) <= 0.0f) continue;   // edge-on
        int a, b;
        PlaneAxes(n, a, b);
        const glm::vec3 A = kAxes[a] * L, B = kAxes[b] * L;
        const glm::vec3 c[4] = { pivot + A * kPlaneLo + B * kPlaneLo, pivot + A * kPlaneHi + B * kPlaneLo,
                                 pivot + A * kPlaneHi + B * kPlaneHi, pivot + A * kPlaneLo + B * kPlaneHi };
        glm::vec2 q[4];
        bool ok = true;
        for (int k = 0; k < 4 && ok; ++k) ok = ToScreen(vp, c[k], q[k]);
        if (ok && InQuad(m, q)) return planeHandles[n];
    }
    return Handle::None;
}

bool BeginMoveDrag(const Viewport& vp, const glm::vec3& pivot, Handle h,
                   float mx, float my, MoveDrag& out)
{
    out = MoveDrag();
    if (h == Handle::None) return false;
    glm::vec3 ro, rd;
    MouseRay(vp, mx, my, ro, rd);

    out.handle = h;
    out.origin = pivot;
    if (IsAxis(h))
    {
        out.dir = kAxes[AxisIndex(h)];
        float t;
        if (!LineRayClosest(pivot, out.dir, ro, rd, t)) return false;
        out.grab = pivot + out.dir * t;
    }
    else
    {
        out.dir = IsPlane(h) ? kAxes[AxisIndex(h)]
                             : glm::normalize(pivot - CameraPos(vp));   // Free: faces the camera
        if (!RayPlane(pivot, out.dir, ro, rd, out.grab)) return false;
    }
    out.valid = true;
    return true;
}

bool MoveDragOffset(const Viewport& vp, const MoveDrag& d, float mx, float my, glm::vec3& offset)
{
    if (!d.valid) return false;
    glm::vec3 ro, rd;
    MouseRay(vp, mx, my, ro, rd);
    if (IsAxis(d.handle))
    {
        float t;
        if (!LineRayClosest(d.origin, d.dir, ro, rd, t)) return false;
        offset = (d.origin + d.dir * t) - d.grab;
        return true;
    }
    glm::vec3 hit;
    if (!RayPlane(d.origin, d.dir, ro, rd, hit)) return false;
    offset = hit - d.grab;
    return true;
}

// ---- Rotate ------------------------------------------------------------------
namespace
{
    // In-plane basis of ring `i` with P(t) = cos t * a + sin t * b turning
    // right-handedly about the axis (cross(a, b) = axis).
    void RingBasis(int i, glm::vec3& a, glm::vec3& b) { a = kAxes[(i + 1) % 3]; b = kAxes[(i + 2) % 3]; }

    int RingAxis(Handle h)
    {
        return h == Handle::RotX ? 0 : h == Handle::RotY ? 1 : h == Handle::RotZ ? 2 : -1;
    }

    float WrapPi(float a)
    {
        const float tp = glm::two_pi<float>();
        while (a > glm::pi<float>()) a -= tp;
        while (a <= -glm::pi<float>()) a += tp;
        return a;
    }
}

Handle HitTestRotate(const Viewport& vp, const glm::vec3& pivot, float mx, float my)
{
    const glm::vec2 m(mx, my);
    const float R = kRingRadius * HandleLength(vp, pivot);
    const Handle rings[3] = { Handle::RotX, Handle::RotY, Handle::RotZ };
    Handle best = Handle::None;
    float bestD = kAxisTolPx;
    for (int i = 0; i < 3; ++i)
    {
        glm::vec3 a, b;
        RingBasis(i, a, b);
        glm::vec2 prev(0.0f);
        bool havePrev = false;
        for (int k = 0; k <= kRingSamples; ++k)
        {
            const float t = glm::two_pi<float>() * float(k) / float(kRingSamples);
            glm::vec2 sp;
            if (!ToScreen(vp, pivot + (a * std::cos(t) + b * std::sin(t)) * R, sp)) { havePrev = false; continue; }
            if (havePrev)
            {
                const float d = SegmentDistance(m, prev, sp);
                if (d <= bestD) { bestD = d; best = rings[i]; }
            }
            prev = sp;
            havePrev = true;
        }
    }
    return best;
}

bool BeginRotateDrag(const Viewport& vp, const glm::vec3& pivot, Handle h,
                     float mx, float my, RotateDrag& out)
{
    out = RotateDrag();
    const int i = RingAxis(h);
    if (i < 0) return false;
    const glm::vec3 n = kAxes[i];
    const float R = kRingRadius * HandleLength(vp, pivot);

    glm::vec3 ro, rd;
    MouseRay(vp, mx, my, ro, rd);

    out.handle = h;
    out.axis = i;
    out.pivot = pivot;
    out.grabPx = glm::vec2(mx, my);

    // Normal case: angles are read where the mouse ray meets the ring plane.
    glm::vec3 hit;
    if (std::abs(glm::dot(rd, n)) >= kMinRingDot && RayPlane(pivot, n, ro, rd, hit) &&
        glm::length(hit - pivot) > 1e-4f * R)
    {
        out.v0 = glm::normalize(hit - pivot);
        out.tangentMode = false;
        out.valid = true;
        return true;
    }

    // Edge-on ring: the plane is useless, so read the drag along the ring's
    // on-screen tangent at the point grabbed.
    glm::vec3 a, b;
    RingBasis(i, a, b);
    float bestD = std::numeric_limits<float>::max();
    glm::vec3 P(0.0f);
    for (int k = 0; k < kRingSamples; ++k)
    {
        const float t = glm::two_pi<float>() * float(k) / float(kRingSamples);
        const glm::vec3 q = pivot + (a * std::cos(t) + b * std::sin(t)) * R;
        glm::vec2 sp;
        if (!ToScreen(vp, q, sp)) continue;
        const float d = glm::length(sp - out.grabPx);
        if (d < bestD) { bestD = d; P = q; }
    }
    if (bestD == std::numeric_limits<float>::max()) return false;
    out.v0 = glm::normalize(P - pivot);
    const glm::vec3 T = glm::cross(n, out.v0);           // +angle moves P along T
    const float step = 0.01f;                            // radians
    glm::vec2 s0, s1;
    if (!ToScreen(vp, P, s0) || !ToScreen(vp, P + T * (R * step), s1)) return false;
    const glm::vec2 dv = s1 - s0;
    const float len = glm::length(dv);
    if (len < 1e-4f) return false;
    out.tanScreen = dv / len;
    out.pxPerRad = len / step;
    out.tangentMode = true;
    out.valid = true;
    return true;
}

bool RotateDragAngle(const Viewport& vp, RotateDrag& d, float mx, float my, float& totalDeg)
{
    if (!d.valid) return false;
    if (d.tangentMode)
    {
        d.totalRad = glm::dot(glm::vec2(mx, my) - d.grabPx, d.tanScreen) / d.pxPerRad;
    }
    else
    {
        const glm::vec3 n = kAxes[d.axis];
        glm::vec3 ro, rd, hit;
        MouseRay(vp, mx, my, ro, rd);
        if (!RayPlane(d.pivot, n, ro, rd, hit)) return false;
        const glm::vec3 v = hit - d.pivot;
        if (glm::length(v) < 1e-6f) return false;
        const glm::vec3 vn = glm::normalize(v);
        const float raw = std::atan2(glm::dot(n, glm::cross(d.v0, vn)), glm::dot(d.v0, vn));
        d.totalRad += WrapPi(raw - d.lastRaw);   // unwrap: keeps counting past +-180
        d.lastRaw = raw;
    }
    totalDeg = glm::degrees(d.totalRad);
    return true;
}

// ---- Scale -------------------------------------------------------------------
namespace
{
    int ScaleAxis(Handle h)
    {
        return h == Handle::ScaleX ? 0 : h == Handle::ScaleY ? 1 : h == Handle::ScaleZ ? 2 : -1;
    }
}

Handle HitTestScale(const Viewport& vp, const glm::vec3& pivot, float mx, float my)
{
    const glm::vec2 m(mx, my);
    const float L = HandleLength(vp, pivot);
    glm::vec2 sp;
    if (!ToScreen(vp, pivot, sp)) return Handle::None;
    if (glm::length(m - sp) <= kFreeTolPx) return Handle::ScaleUniform;

    const Handle axes[3] = { Handle::ScaleX, Handle::ScaleY, Handle::ScaleZ };
    Handle best = Handle::None;
    float bestD = kAxisTolPx;
    for (int i = 0; i < 3; ++i)
    {
        glm::vec2 a, b;
        if (AxisVisibility(vp, pivot, i) <= 0.0f) continue;   // pointing at the camera
        if (!ToScreen(vp, pivot + kAxes[i] * (kShaftStart * L), a) ||
            !ToScreen(vp, pivot + kAxes[i] * ((kScaleCubePos + kScaleCubeR) * L), b)) continue;
        const float d = SegmentDistance(m, a, b);
        if (d <= bestD) { bestD = d; best = axes[i]; }
    }
    return best;
}

bool BeginScaleDrag(const Viewport& vp, const glm::vec3& pivot, Handle h,
                    float mx, float my, ScaleDrag& out)
{
    out = ScaleDrag();
    out.handle = h;
    out.pivot = pivot;
    out.grabPx = glm::vec2(mx, my);
    out.pxPerDouble = kArrowPx;
    if (h == Handle::ScaleUniform)
    {
        out.axis = -1;
        out.valid = true;
        return true;
    }
    const int i = ScaleAxis(h);
    if (i < 0) return false;
    glm::vec3 ro, rd;
    MouseRay(vp, mx, my, ro, rd);
    float t;
    if (!LineRayClosest(pivot, kAxes[i], ro, rd, t)) return false;
    // A grab right at the pivot would make every later ratio explode.
    if (t < 0.05f * HandleLength(vp, pivot)) return false;
    out.axis = i;
    out.t0 = t;
    out.valid = true;
    return true;
}

bool ScaleDragFactor(const Viewport& vp, const ScaleDrag& d, float mx, float my, float& factor)
{
    if (!d.valid) return false;
    if (d.axis < 0)
    {
        // Centre cube: right / up grows, left / down shrinks, exponentially so
        // equal travel either way undoes itself.
        const float travel = (mx - d.grabPx.x) - (my - d.grabPx.y);
        factor = std::max(kMinFactor, std::pow(2.0f, travel / d.pxPerDouble));
        return true;
    }
    glm::vec3 ro, rd;
    MouseRay(vp, mx, my, ro, rd);
    float t;
    if (!LineRayClosest(d.pivot, kAxes[d.axis], ro, rd, t)) return false;
    factor = std::max(kMinFactor, t / d.t0);
    return true;
}

glm::vec2 GridSnapXZ(const GridSettings& g, const glm::vec2& p)
{
    const float step = std::max(0.0001f, g.spacing);
    if (g.shape == GridShape::Circular)
    {
        const float r = glm::length(p);
        float rs = std::round(r / step) * step;
        rs = std::min(rs, g.radius);
        if (rs <= 0.0f) return glm::vec2(0.0f);
        const int   spokes = std::max(1, g.spokes);
        const float angStep = glm::two_pi<float>() / float(spokes);
        const float ts = std::round(std::atan2(p.y, p.x) / angStep) * angStep;
        return glm::vec2(rs * std::cos(ts), rs * std::sin(ts));
    }
    float x = std::round(p.x / step) * step;
    float z = std::round(p.y / step) * step;
    x = std::clamp(x, -g.sizeX * 0.5f, g.sizeX * 0.5f);
    z = std::clamp(z, -g.sizeY * 0.5f, g.sizeY * 0.5f);
    return glm::vec2(x, z);
}

glm::vec3 SnapMoveTarget(const glm::vec3& target, Handle h, const GridSettings& g)
{
    bool fx = false, fy = false, fz = false;
    switch (h)
    {
    case Handle::X:       fx = true; break;
    case Handle::Y:       fy = true; break;
    case Handle::Z:       fz = true; break;
    case Handle::PlaneXZ: fx = fz = true; break;
    case Handle::PlaneXY: fx = fy = true; break;
    case Handle::PlaneYZ: fy = fz = true; break;
    case Handle::Free:    fx = fy = fz = true; break;
    default: return target;
    }
    const float step = std::max(0.0001f, g.spacing);
    glm::vec3 out = target;
    if (fx && fz)
    {
        const glm::vec2 s = GridSnapXZ(g, glm::vec2(target.x, target.z));
        out.x = s.x; out.z = s.y;
    }
    else if (fx) out.x = RoundTo(target.x, step);
    else if (fz) out.z = RoundTo(target.z, step);
    if (fy) out.y = RoundTo(target.y, step);
    return out;
}

void GridTriadPlacement(const GridSettings& g, glm::vec3& origin, float& length)
{
    float extent;   // the grid's smaller full extent
    if (g.shape == GridShape::Circular)
    {
        const float r = std::max(1.0f, g.radius);
        const float k = std::sqrt(0.5f);
        origin = glm::vec3(-r * k, 0.0f, -r * k);   // the rim at 225 deg
        extent = 2.0f * r;
    }
    else
    {
        const float sx = std::max(1.0f, g.sizeX), sz = std::max(1.0f, g.sizeY);
        origin = glm::vec3(-0.5f * sx, 0.0f, -0.5f * sz);
        extent = std::min(sx, sz);
    }
    // 7 % of the grid, at least one grid square, never more than a third (so
    // a small grid with a coarse spacing still looks sensible).
    length = std::min(std::max(0.07f * extent, g.spacing), 0.3f * extent);
}

// =============================================================================
// Drag readout text
// =============================================================================
namespace
{
    const char* kMinus = "\xE2\x88\x92";   // U+2212, so +/- columns line up

    std::string Signed(double v, int places)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.*f", places, std::abs(v));
        const bool zero = std::string(buf).find_first_not_of("0.") == std::string::npos;
        return std::string(zero ? "" : (v < 0.0 ? kMinus : "+")) + buf;
    }
}

std::string DragReadout(Handle h, const glm::vec3& moveMM, float rotDeg, float factor,
                        bool imperial, bool snapping)
{
    static const char* kAxisName[3] = { "X", "Y", "Z" };
    std::string out;
    switch (h)
    {
    case Handle::X: case Handle::Y: case Handle::Z:
    case Handle::PlaneYZ: case Handle::PlaneXZ: case Handle::PlaneXY: case Handle::Free:
    {
        // The components this handle frees.
        bool use[3] = { false, false, false };
        if (h == Handle::X) use[0] = true;
        else if (h == Handle::Y) use[1] = true;
        else if (h == Handle::Z) use[2] = true;
        else if (h == Handle::Free) use[0] = use[1] = use[2] = true;
        else { const int n = AxisIndex(h); use[(n + 1) % 3] = use[(n + 2) % 3] = true; }
        const double k = imperial ? 1.0 / 25.4 : 1.0;
        const int places = imperial ? 3 : 2;
        for (int i = 0; i < 3; ++i)
        {
            if (!use[i]) continue;
            if (!out.empty()) out += "   ";
            out += std::string(kAxisName[i]) + " " + Signed(moveMM[i] * k, places);
        }
        out += imperial ? " in" : " mm";
        break;
    }
    case Handle::RotX: case Handle::RotY: case Handle::RotZ:
    {
        const int i = (h == Handle::RotX) ? 0 : (h == Handle::RotY) ? 1 : 2;
        out = std::string(kAxisName[i]) + " " + Signed(rotDeg, 1) + "\xC2\xB0";   // degree sign
        break;
    }
    case Handle::ScaleX: case Handle::ScaleY: case Handle::ScaleZ: case Handle::ScaleUniform:
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", factor);
        out = std::string("\xC3\x97") + buf;   // multiplication sign
        break;
    }
    default:
        return std::string();
    }
    if (snapping) out += "  \xC2\xB7 snap";   // middle dot
    return out;
}

#ifndef GIZMO_NO_WX
bool RasterizeText(const std::string& utf8, std::vector<unsigned char>& coverage,
                   int& width, int& height)
{
    const wxString text = wxString::FromUTF8(utf8.c_str());
    wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    font.SetWeight(wxFONTWEIGHT_BOLD);

    // Measure, then draw white on black; the red channel is the coverage.
    wxBitmap probe(1, 1, 24);
    wxMemoryDC dc(probe);
    dc.SetFont(font);
    const wxSize ext = dc.GetTextExtent(text);
    if (ext.x <= 0 || ext.y <= 0) return false;
    const int padX = 9, padY = 4;
    width = ext.x + 2 * padX;
    height = ext.y + 2 * padY;

    wxBitmap bmp(width, height, 24);
    dc.SelectObject(bmp);
    dc.SetBackground(*wxBLACK_BRUSH);
    dc.Clear();
    dc.SetFont(font);
    dc.SetTextForeground(*wxWHITE);
    dc.DrawText(text, padX, padY);
    dc.SelectObject(wxNullBitmap);

    const wxImage img = bmp.ConvertToImage();
    if (!img.IsOk()) return false;
    const unsigned char* px = img.GetData();
    coverage.assign(size_t(width) * height, 0);
    for (int i = 0; i < width * height; ++i) coverage[i] = px[3 * i];
    return true;
}
#endif

// =============================================================================
// GizmoRenderer
// =============================================================================
namespace
{
    // Handles draw in flat colour. The shader keeps an optional headlight
    // (uLit, off by default -- see DrawMesh).
    // uDimBack darkens whatever lies on the far side of the pivot from the
    // camera -- the back halves of the rotate rings.
    const char* kVs = R"GLSL(
        #version 330 core
        layout(location = 0) in vec3 aPos;
        layout(location = 1) in vec3 aNormal;
        uniform mat4 uMVP;
        uniform mat4 uModel;
        uniform mat3 uNormalMat;   // model -> view, inverse-transpose
        out vec3 vN;
        out vec3 vWorld;
        out vec2 vUV;              // the unit quad's own position (readout texture)
        void main()
        {
            vUV = aPos.xy;
            vN = uNormalMat * aNormal;
            vWorld = vec3(uModel * vec4(aPos, 1.0));
            gl_Position = uMVP * vec4(aPos, 1.0);
        }
    )GLSL";
    const char* kFs = R"GLSL(
        #version 330 core
        in vec3 vN;
        in vec3 vWorld;
        in vec2 vUV;
        out vec4 FragColor;
        uniform vec4 uColor;
        uniform int  uUseTex;
        uniform sampler2D uTex;
        uniform int  uLit;
        uniform int  uDimBack;
        uniform vec3 uPivot;
        uniform vec3 uCam;
        void main()
        {
            vec3 c = uColor.rgb;
            float a = uColor.a;
            if (uLit == 1)
            {
                vec3 n = normalize(vN);
                vec3 L = normalize(vec3(-0.35, 0.55, 0.75));
                float d = max(dot(n, L), 0.0);
                float spec = pow(max(dot(reflect(-L, n), vec3(0.0, 0.0, 1.0)), 0.0), 24.0);
                c = c * (0.50 + 0.55 * d) + vec3(0.22 * spec);
            }
            if (uDimBack == 1 && dot(vWorld - uPivot, uCam - uPivot) < 0.0)
            {
                c *= 0.55;
                a *= 0.55;
            }
            if (uUseTex == 1)
                a *= texture(uTex, vec2(vUV.x, 1.0 - vUV.y)).r;   // rows top-down
            FragColor = vec4(c, a);
        }
    )GLSL";

    GLuint Compile(GLenum type, const char* src)
    {
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { glDeleteShader(s); return 0; }
        return s;
    }

    const glm::vec4 kAxisColor[3] = { { 0.92f, 0.27f, 0.27f, 1.0f },    // X red
                                      { 0.42f, 0.82f, 0.32f, 1.0f },    // Y green
                                      { 0.32f, 0.54f, 0.98f, 1.0f } };  // Z blue
    const glm::vec4 kHot(1.00f, 0.84f, 0.22f, 1.0f);                    // hover / dragging
    const glm::vec4 kBall(0.92f, 0.93f, 0.96f, 1.0f);

    // Model matrix mapping the unit meshes' +Y onto `axis` (length `len`) with
    // radius `r` across, based at `base`.
    glm::mat4 AlongAxis(const glm::vec3& base, int axis, float len, float r)
    {
        const glm::vec3 e = kAxes[axis];
        const glm::vec3 p = kAxes[(axis + 1) % 3];
        const glm::vec3 q = kAxes[(axis + 2) % 3];
        glm::mat4 M(1.0f);
        M[0] = glm::vec4(p * r, 0.0f);
        M[1] = glm::vec4(e * len, 0.0f);
        M[2] = glm::vec4(q * r, 0.0f);
        M[3] = glm::vec4(base, 1.0f);
        return M;
    }
}

bool GizmoRenderer::Init()
{
    if (m_ready) return true;
    if (m_failed) return false;

    const GLuint vs = Compile(GL_VERTEX_SHADER, kVs);
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs)
    {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        m_failed = true;
        return false;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        glDeleteProgram(m_program);
        m_program = 0;
        m_failed = true;
        return false;
    }
    m_uMVP = glGetUniformLocation(m_program, "uMVP");
    m_uColor = glGetUniformLocation(m_program, "uColor");
    m_uModel = glGetUniformLocation(m_program, "uModel");
    m_uNormalMat = glGetUniformLocation(m_program, "uNormalMat");
    m_uLit = glGetUniformLocation(m_program, "uLit");
    m_uDimBack = glGetUniformLocation(m_program, "uDimBack");
    m_uPivot = glGetUniformLocation(m_program, "uPivot");
    m_uCam = glGetUniformLocation(m_program, "uCam");
    m_uUseTex = glGetUniformLocation(m_program, "uUseTex");
    m_uTex = glGetUniformLocation(m_program, "uTex");

    // Interleaved position + normal (6 floats per vertex).
    auto upload = [](Mesh& m, const std::vector<float>& v, const std::vector<uint32_t>& idx)
    {
        glGenVertexArrays(1, &m.vao);
        glGenBuffers(1, &m.vbo);
        glGenBuffers(1, &m.ebo);
        glBindVertexArray(m.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(float)), v.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint32_t)), idx.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        m.count = (GLsizei)idx.size();
    };
    auto vert = [](std::vector<float>& v, const glm::vec3& p, const glm::vec3& n)
    {
        v.insert(v.end(), { p.x, p.y, p.z, n.x, n.y, n.z });
    };

    const int seg = 20;
    // Cylinder sides around +Y, radius 1, y in [0, 1].
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        for (int i = 0; i < seg; ++i)
        {
            const float a = glm::two_pi<float>() * float(i) / float(seg);
            const glm::vec3 n(std::cos(a), 0.0f, std::sin(a));
            vert(v, glm::vec3(n.x, 0.0f, n.z), n);
            vert(v, glm::vec3(n.x, 1.0f, n.z), n);
        }
        for (uint32_t i = 0; i < (uint32_t)seg; ++i)
        {
            const uint32_t a0 = 2 * i, a1 = a0 + 1, b0 = 2 * ((i + 1) % seg), b1 = b0 + 1;
            idx.insert(idx.end(), { a0, b0, b1, a0, b1, a1 });
        }
        upload(m_cylinder, v, idx);
    }
    // Cone: base radius 1 at y = 0, apex at y = 1. Each side facet has its own
    // apex vertex so the shading stays smooth around the cone; the base is a
    // separate flat disc.
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        auto sideN = [](float a) { return glm::normalize(glm::vec3(std::cos(a), 1.0f, std::sin(a))); };
        for (int i = 0; i < seg; ++i)
        {
            const float a0 = glm::two_pi<float>() * float(i) / float(seg);
            const float a1 = glm::two_pi<float>() * float(i + 1) / float(seg);
            const uint32_t base = (uint32_t)(v.size() / 6);
            vert(v, glm::vec3(0.0f, 1.0f, 0.0f), sideN(0.5f * (a0 + a1)));
            vert(v, glm::vec3(std::cos(a0), 0.0f, std::sin(a0)), sideN(a0));
            vert(v, glm::vec3(std::cos(a1), 0.0f, std::sin(a1)), sideN(a1));
            idx.insert(idx.end(), { base, base + 1, base + 2 });
        }
        const uint32_t centre = (uint32_t)(v.size() / 6);
        vert(v, glm::vec3(0.0f), glm::vec3(0, -1, 0));
        for (int i = 0; i < seg; ++i)
        {
            const float a = glm::two_pi<float>() * float(i) / float(seg);
            vert(v, glm::vec3(std::cos(a), 0.0f, std::sin(a)), glm::vec3(0, -1, 0));
        }
        for (uint32_t i = 0; i < (uint32_t)seg; ++i)
            idx.insert(idx.end(), { centre, centre + 1 + (i + 1) % seg, centre + 1 + i });
        upload(m_cone, v, idx);
    }
    // Sphere, radius 1.
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        const int lat = 12, lon = 20;
        for (int i = 0; i <= lat; ++i)
        {
            const float ph = glm::pi<float>() * float(i) / float(lat);
            for (int j = 0; j <= lon; ++j)
            {
                const float th = glm::two_pi<float>() * float(j) / float(lon);
                const glm::vec3 p(std::sin(ph) * std::cos(th), std::cos(ph), std::sin(ph) * std::sin(th));
                vert(v, p, p);
            }
        }
        for (int i = 0; i < lat; ++i)
            for (int j = 0; j < lon; ++j)
            {
                const uint32_t a = i * (lon + 1) + j, b = a + lon + 1;
                idx.insert(idx.end(), { a, b, a + 1, a + 1, b, b + 1 });
            }
        upload(m_sphere, v, idx);
    }
    // Unit square [0,1]^2 in the XY plane (drawn unlit).
    {
        std::vector<float> v;
        for (const glm::vec3& p : { glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(1, 1, 0), glm::vec3(0, 1, 0) })
            vert(v, p, glm::vec3(0, 0, 1));
        upload(m_quad, v, { 0, 1, 2, 0, 2, 3 });
    }
    // Torus about +Y in the XZ plane: major radius 1, tube kRingTube.
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        const int maj = 96, mn = 10;
        for (int i = 0; i < maj; ++i)
        {
            const float u = glm::two_pi<float>() * float(i) / float(maj);
            const glm::vec3 c(std::cos(u), 0.0f, std::sin(u));
            for (int j = 0; j < mn; ++j)
            {
                const float w = glm::two_pi<float>() * float(j) / float(mn);
                const glm::vec3 n = c * std::cos(w) + glm::vec3(0, std::sin(w), 0);
                vert(v, c + n * kRingTube, n);
            }
        }
        for (uint32_t i = 0; i < (uint32_t)maj; ++i)
            for (uint32_t j = 0; j < (uint32_t)mn; ++j)
            {
                const uint32_t a = i * mn + j, b = i * mn + (j + 1) % mn;
                const uint32_t c = ((i + 1) % maj) * mn + j, d = ((i + 1) % maj) * mn + (j + 1) % mn;
                idx.insert(idx.end(), { a, c, d, a, d, b });
            }
        upload(m_torus, v, idx);
    }
    // Cube [-1, 1]^3 with flat faces (4 vertices per face).
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        for (int f = 0; f < 6; ++f)
        {
            const int ax = f / 2;
            const float sgn = (f % 2) ? 1.0f : -1.0f;
            glm::vec3 n(0.0f); n[ax] = sgn;
            const glm::vec3 u = kAxes[(ax + 1) % 3], w = kAxes[(ax + 2) % 3];
            const uint32_t base = (uint32_t)(v.size() / 6);
            const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
            for (int k = 0; k < 4; ++k) vert(v, n + u * sx[k] + w * sy[k], n);
            if (sgn > 0) idx.insert(idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
            else         idx.insert(idx.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
        }
        upload(m_cube, v, idx);
    }
    glGenVertexArrays(1, &m_fanVao);
    glGenBuffers(1, &m_fanVbo);
    glBindVertexArray(m_fanVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_fanVbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);   // flat pieces only: no normals needed
    glBindVertexArray(0);

    m_ready = true;
    return true;
}

// Per-draw setup shared by every Draw*: camera matrices for DrawMesh, and the
// uniforms that default the same for every piece.
void GizmoRenderer::BeginFrame(const Viewport& vp, const glm::vec3& pivot)
{
    m_view = vp.view;
    m_VP = vp.proj * vp.view;
    glUseProgram(m_program);
    glUniform1i(m_uLit, 0);
    glUniform1i(m_uDimBack, 0);
    glUniform3fv(m_uPivot, 1, &pivot[0]);
    const glm::vec3 cam = CameraPos(vp);
    glUniform3fv(m_uCam, 1, &cam[0]);
    glUniform1i(m_uUseTex, 0);
}

void GizmoRenderer::DrawMesh(const Mesh& m, const glm::mat4& model, const glm::vec4& color, bool lit)
{
    const glm::mat4 mvp = m_VP * model;
    // Inverse-transpose so normals survive the meshes' non-uniform scaling
    // (thin shafts, flat cones).
    const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m_view * model)));
    glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, &mvp[0][0]);
    glUniformMatrix4fv(m_uModel, 1, GL_FALSE, &model[0][0]);
    glUniformMatrix3fv(m_uNormalMat, 1, GL_FALSE, &nm[0][0]);
    glUniform4fv(m_uColor, 1, &color[0]);
    glUniform1i(m_uLit, lit ? 1 : 0);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr);
}

// For the dynamic, already-world-space pieces (wedge, labels): unlit.
void GizmoRenderer::SetFlat(const glm::vec4& color)
{
    const glm::mat4 I(1.0f);
    glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, &m_VP[0][0]);
    glUniformMatrix4fv(m_uModel, 1, GL_FALSE, &I[0][0]);
    glUniform4fv(m_uColor, 1, &color[0]);
    glUniform1i(m_uLit, 0);
}

void GizmoRenderer::DrawMove(const Viewport& vp, const glm::vec3& pivot, Handle hover, Handle active)
{
    if (!Init()) return;

    const float L = HandleLength(vp, pivot);
    const bool dragging = active != Handle::None;

    // While dragging, the grabbed handle is lit and the rest fade back.
    auto colorFor = [&](Handle h, const glm::vec4& base) -> glm::vec4
    {
        if (dragging)
            return h == active ? kHot : glm::vec4(glm::vec3(base), 0.25f);
        return h == hover ? kHot : base;
    };

    // On top of the scene, but the handles still occlude each other.
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    BeginFrame(vp, pivot);

    // Guide line along the grabbed axis.
    const Handle axisH[3] = { Handle::X, Handle::Y, Handle::Z };
    if (IsAxis(active))
    {
        const int i = AxisIndex(active);
        glDepthMask(GL_FALSE);
        DrawMesh(m_cylinder, AlongAxis(pivot - kAxes[i] * (40.0f * L), i, 80.0f * L, kShaftR * L * 0.6f),
                 glm::vec4(glm::vec3(kAxisColor[i]), 0.45f), /*lit=*/false);
        glDepthMask(GL_TRUE);
    }

    // Axis arrows: shaft + cone head. One pointing at the camera fades out
    // (it can't be dragged from this view -- see AxisVisibility).
    for (int i = 0; i < 3; ++i)
    {
        glm::vec4 c = colorFor(axisH[i], kAxisColor[i]);
        if (axisH[i] != active) c.a *= AxisVisibility(vp, pivot, i);
        if (c.a <= 0.0f) continue;
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        DrawMesh(m_cylinder, AlongAxis(pivot + kAxes[i] * (kShaftStart * L), i,
                                            (kHeadStart - kShaftStart) * L, kShaftR * L), c);
        DrawMesh(m_cone, AlongAxis(pivot + kAxes[i] * (kHeadStart * L), i,
                                        (1.0f - kHeadStart) * L, kHeadR * L), c);
        glDepthMask(GL_TRUE);
    }

    // Centre ball (Free).
    {
        const glm::vec4 c = colorFor(Handle::Free, kBall);
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = kBallR * L;
        M[3] = glm::vec4(pivot, 1.0f);
        DrawMesh(m_sphere, M, c);
        glDepthMask(GL_TRUE);
    }

    // Plane squares last: translucent, tinted by their normal axis.
    glDepthMask(GL_FALSE);
    const Handle planeH[3] = { Handle::PlaneYZ, Handle::PlaneXZ, Handle::PlaneXY };
    for (int n = 0; n < 3; ++n)
    {
        int a, b;
        PlaneAxes(n, a, b);
        glm::vec4 c = colorFor(planeH[n], kAxisColor[n]);
        if (!dragging) c.a = (planeH[n] == hover) ? 0.75f : 0.40f;
        else if (planeH[n] == active) c.a = 0.75f;
        if (planeH[n] != active) c.a *= PlaneVisibility(vp, pivot, n);   // fades edge-on
        if (c.a <= 0.0f) continue;
        glm::mat4 M(1.0f);
        const float s = (kPlaneHi - kPlaneLo) * L;
        M[0] = glm::vec4(kAxes[a] * s, 0.0f);
        M[1] = glm::vec4(kAxes[b] * s, 0.0f);
        M[2] = glm::vec4(kAxes[n], 0.0f);
        M[3] = glm::vec4(pivot + (kAxes[a] + kAxes[b]) * (kPlaneLo * L), 1.0f);
        DrawMesh(m_quad, M, c, /*lit=*/false);
    }
    glDepthMask(GL_TRUE);

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

void GizmoRenderer::DrawRotate(const Viewport& vp, const glm::vec3& pivot,
                               Handle hover, const RotateDrag* drag, float sweepDeg)
{
    if (!Init()) return;

    const float L = HandleLength(vp, pivot);
    const float R = kRingRadius * L;
    const bool dragging = drag && drag->valid;
    const Handle active = dragging ? drag->handle : Handle::None;
    const Handle rings[3] = { Handle::RotX, Handle::RotY, Handle::RotZ };

    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    BeginFrame(vp, pivot);

    // Rings: the ring about axis i lies in the plane normal to it, coloured
    // like that axis. Held ring lit, others faded; hover lit. The half of each
    // ring behind the pivot is dimmed, so front and back read apart.
    glUniform1i(m_uDimBack, 1);
    for (int i = 0; i < 3; ++i)
    {
        glm::vec4 c = kAxisColor[i];
        if (dragging) c = (rings[i] == active) ? kHot : glm::vec4(glm::vec3(c), 0.25f);
        else if (rings[i] == hover) c = kHot;
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        DrawMesh(m_torus, AlongAxis(pivot, i, R, R), c);
        glDepthMask(GL_TRUE);
    }
    glUniform1i(m_uDimBack, 0);

    // Small centre dot marking the pivot.
    {
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = kBallR * 0.6f * L;
        M[3] = glm::vec4(pivot, 1.0f);
        DrawMesh(m_sphere, M, kBall);
    }

    // Sweep wedge from the grab direction through the angle turned so far
    // (capped at one full turn).
    const float sweepRad = glm::radians(sweepDeg);
    if (dragging && std::abs(sweepRad) > 1e-4f)
    {
        const glm::vec3 n = kAxes[drag->axis];
        const glm::vec3 w = glm::cross(n, drag->v0);
        const float sweep = glm::clamp(sweepRad, -glm::two_pi<float>(), glm::two_pi<float>());
        const int segs = std::max(2, int(std::abs(sweep) / glm::two_pi<float>() * 96.0f));
        std::vector<float> fan;
        fan.reserve(size_t(segs + 2) * 3);
        fan.insert(fan.end(), { pivot.x, pivot.y, pivot.z });
        for (int k = 0; k <= segs; ++k)
        {
            const float t = sweep * float(k) / float(segs);
            const glm::vec3 p = pivot + (drag->v0 * std::cos(t) + w * std::sin(t)) * R;
            fan.insert(fan.end(), { p.x, p.y, p.z });
        }
        glBindVertexArray(m_fanVao);
        glBindBuffer(GL_ARRAY_BUFFER, m_fanVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(fan.size() * sizeof(float)), fan.data(), GL_STREAM_DRAW);
        SetFlat(glm::vec4(glm::vec3(kHot), 0.28f));
        glDepthMask(GL_FALSE);
        glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)(fan.size() / 3));
        glDepthMask(GL_TRUE);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

void GizmoRenderer::DrawScale(const Viewport& vp, const glm::vec3& pivot,
                              Handle hover, Handle active, float stretch)
{
    if (!Init()) return;

    const float L = HandleLength(vp, pivot);
    const bool dragging = active != Handle::None;
    // Stalks stretch with the factor (uniform, so all three), within reason so
    // they stay on screen and grabbable-looking.
    const float k = dragging ? glm::clamp(stretch, 0.15f, 4.0f) : 1.0f;

    auto colorFor = [&](Handle h, const glm::vec4& base) -> glm::vec4
    {
        if (dragging)
            return h == active ? kHot : glm::vec4(glm::vec3(base), 0.35f);
        return h == hover ? kHot : base;
    };

    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    BeginFrame(vp, pivot);

    const Handle axes[3] = { Handle::ScaleX, Handle::ScaleY, Handle::ScaleZ };
    for (int i = 0; i < 3; ++i)
    {
        glm::vec4 c = colorFor(axes[i], kAxisColor[i]);
        if (axes[i] != active) c.a *= AxisVisibility(vp, pivot, i);   // end-on: fades
        if (c.a <= 0.0f) continue;
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        const float cubeAt = kScaleCubePos * k * L;
        const float stalk = cubeAt - kScaleCubeR * L - kShaftStart * L;
        if (stalk > 0.0f)
            DrawMesh(m_cylinder, AlongAxis(pivot + kAxes[i] * (kShaftStart * L), i, stalk, kShaftR * L), c);
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = kScaleCubeR * L;
        M[3] = glm::vec4(pivot + kAxes[i] * cubeAt, 1.0f);
        DrawMesh(m_cube, M, c);
        glDepthMask(GL_TRUE);
    }
    {
        const glm::vec4 c = colorFor(Handle::ScaleUniform, kBall);
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = kCentreCubeR * L;
        M[3] = glm::vec4(pivot, 1.0f);
        DrawMesh(m_cube, M, c);
        glDepthMask(GL_TRUE);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

namespace
{
    // Stroke glyphs for the triad labels, in a unit cell (x in [-0.35, 0.35],
    // y in [-0.5, 0.5]): segment end points.
    struct Stroke { glm::vec2 a, b; };
    const Stroke kGlyphX[] = { { { -0.35f, -0.5f }, {  0.35f,  0.5f } },
                               { { -0.35f,  0.5f }, {  0.35f, -0.5f } } };
    const Stroke kGlyphY[] = { { { -0.35f,  0.5f }, {  0.0f,   0.0f } },
                               { {  0.35f,  0.5f }, {  0.0f,   0.0f } },
                               { {  0.0f,   0.0f }, {  0.0f,  -0.5f } } };
    const Stroke kGlyphZ[] = { { { -0.35f,  0.5f }, {  0.35f,  0.5f } },
                               { {  0.35f,  0.5f }, { -0.35f, -0.5f } },
                               { { -0.35f, -0.5f }, {  0.35f, -0.5f } } };
}

void GizmoRenderer::DrawAxisTriad(const Viewport& vp, const glm::vec3& origin, float length)
{
    if (!Init() || !(length > 0.0f)) return;

    const float L = length;

    // Part of the scene: depth-tested like everything else, nothing cleared.
    // Lifted a hair off y = 0 so the X / Z shafts don't z-fight the floor.
    const glm::vec3 o = origin + glm::vec3(0.0f, 0.002f * L, 0.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    BeginFrame(vp, o);

    constexpr float kShaft = 0.028f, kHead = 0.075f, kHeadLen = 0.24f;
    for (int i = 0; i < 3; ++i)
    {
        const glm::vec4 c = kAxisColor[i];
        DrawMesh(m_cylinder, AlongAxis(o, i, (1.0f - kHeadLen) * L, kShaft * L), c);
        DrawMesh(m_cone, AlongAxis(o + kAxes[i] * ((1.0f - kHeadLen) * L), i,
                                        kHeadLen * L, kHead * L), c);
    }
    {
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = 0.055f * L;
        M[3] = glm::vec4(o, 1.0f);
        DrawMesh(m_sphere, M, kBall);
    }

    // Labels just past each tip, as camera-facing strokes (the view matrix's
    // rows are the camera's right / up in world space).
    const glm::vec3 right(vp.view[0][0], vp.view[1][0], vp.view[2][0]);
    const glm::vec3 up(vp.view[0][1], vp.view[1][1], vp.view[2][1]);
    const float size = 0.2f * L;        // glyph height
    const float w = 0.065f;             // stroke half width, glyph units
    struct Glyph { const Stroke* s; int n; };
    const Glyph glyphs[3] = { { kGlyphX, 2 }, { kGlyphY, 3 }, { kGlyphZ, 3 } };

    glBindVertexArray(m_fanVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_fanVbo);
    for (int i = 0; i < 3; ++i)
    {
        // Raised by half a glyph so the X / Z labels stand on the floor
        // instead of dipping through it.
        const glm::vec3 centre = o + kAxes[i] * (1.2f * L) + glm::vec3(0.0f, 0.55f * size, 0.0f);
        auto world = [&](const glm::vec2& p) { return centre + (right * p.x + up * p.y) * size; };
        std::vector<float> tri;
        for (int k = 0; k < glyphs[i].n; ++k)
        {
            const Stroke& st = glyphs[i].s[k];
            glm::vec2 d = st.b - st.a;
            const float len = glm::length(d);
            if (len <= 0.0f) continue;
            d /= len;
            const glm::vec2 n(-d.y, d.x);
            // Square caps (extended by the half width) close the joints.
            const glm::vec2 a = st.a - d * w, b = st.b + d * w;
            const glm::vec3 q[4] = { world(a - n * w), world(a + n * w),
                                     world(b + n * w), world(b - n * w) };
            for (int j : { 0, 1, 2, 0, 2, 3 }) tri.insert(tri.end(), { q[j].x, q[j].y, q[j].z });
        }
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tri.size() * sizeof(float)), tri.data(), GL_DYNAMIC_DRAW);
        SetFlat(kAxisColor[i]);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(tri.size() / 3));
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void GizmoRenderer::SetReadout(const unsigned char* coverage, int width, int height)
{
    if (!Init() || !coverage || width <= 0 || height <= 0) return;
    if (!m_readoutTex) glGenTextures(1, &m_readoutTex);
    glBindTexture(GL_TEXTURE_2D, m_readoutTex);
    GLint align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, coverage);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    // Drawn 1:1 with screen pixels, so nearest keeps the text crisp.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_readoutW = width;
    m_readoutH = height;
}

void GizmoRenderer::DrawReadout(const Viewport& vp, float mx, float my)
{
    if (!Init() || !m_readoutTex || m_readoutW <= 0) return;
    const float w = float(m_readoutW), h = float(m_readoutH);
    const float W = float(vp.width), H = float(vp.height);

    // Below-right of the cursor; flipped to the other side near an edge.
    float x = mx + 18.0f, y = my + 20.0f;
    if (x + w > W - 4.0f) x = mx - 18.0f - w;
    if (y + h > H - 4.0f) y = my - 20.0f - h;
    x = std::floor(std::max(4.0f, x));
    y = std::floor(std::max(4.0f, y));

    // Screen space, y down, one unit per pixel.
    glUseProgram(m_program);
    m_VP = glm::ortho(0.0f, W, H, 0.0f, -1.0f, 1.0f);
    m_view = glm::mat4(1.0f);
    glUniform1i(m_uDimBack, 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto rect = [](float rx, float ry, float rw, float rh)
    {
        // Unit quad (0,0)-(1,1) onto the pixel rect, v = 0 at its bottom edge
        // so the texture (rows top-down) reads upright.
        glm::mat4 M(1.0f);
        M[0] = glm::vec4(rw, 0.0f, 0.0f, 0.0f);
        M[1] = glm::vec4(0.0f, -rh, 0.0f, 0.0f);
        M[3] = glm::vec4(rx, ry + rh, 0.0f, 1.0f);
        return M;
    };
    glUniform1i(m_uUseTex, 0);
    DrawMesh(m_quad, rect(x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.16f), false);
    DrawMesh(m_quad, rect(x, y, w, h), glm::vec4(0.10f, 0.11f, 0.14f, 0.88f), false);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_readoutTex);
    glUniform1i(m_uTex, 0);
    glUniform1i(m_uUseTex, 1);
    DrawMesh(m_quad, rect(x, y, w, h), glm::vec4(kHot.r, kHot.g, kHot.b, 1.0f), false);
    glUniform1i(m_uUseTex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void GizmoRenderer::Destroy()
{
    if (m_readoutTex) glDeleteTextures(1, &m_readoutTex);
    m_readoutTex = 0;
    m_readoutW = m_readoutH = 0;
    if (m_fanVbo) glDeleteBuffers(1, &m_fanVbo);
    if (m_fanVao) glDeleteVertexArrays(1, &m_fanVao);
    m_fanVbo = m_fanVao = 0;
    for (Mesh* m : { &m_cylinder, &m_cone, &m_sphere, &m_quad, &m_torus, &m_cube })
    {
        if (m->ebo) glDeleteBuffers(1, &m->ebo);
        if (m->vbo) glDeleteBuffers(1, &m->vbo);
        if (m->vao) glDeleteVertexArrays(1, &m->vao);
        *m = Mesh();
    }
    if (m_program) glDeleteProgram(m_program);
    m_program = 0;
    m_ready = false;
}
} // namespace Gizmo
