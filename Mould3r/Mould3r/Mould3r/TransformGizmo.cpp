#include "TransformGizmo.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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
    constexpr float kMinAxisPx  = 12.0f;   // shorter than this on screen: end-on, not pickable
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
        if (!ToScreen(vp, pivot + kAxes[i] * (kShaftStart * L), a) ||
            !ToScreen(vp, pivot + kAxes[i] * L, b)) continue;
        if (glm::length(b - a) < kMinAxisPx) continue;   // pointing at the camera
        const float d = SegmentDistance(m, a, b);
        if (d <= bestD) { bestD = d; best = axisHandles[i]; }
    }
    if (best != Handle::None) return best;

    // 3. Plane squares.
    const glm::vec3 view = glm::normalize(pivot - CameraPos(vp));
    const Handle planeHandles[3] = { Handle::PlaneYZ, Handle::PlaneXZ, Handle::PlaneXY };
    for (int n = 0; n < 3; ++n)
    {
        if (std::abs(glm::dot(view, kAxes[n])) < kMinPlaneDot) continue;   // edge-on
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
        if (!ToScreen(vp, pivot + kAxes[i] * (kShaftStart * L), a) ||
            !ToScreen(vp, pivot + kAxes[i] * ((kScaleCubePos + kScaleCubeR) * L), b)) continue;
        if (glm::length(b - a) < kMinAxisPx) continue;   // pointing at the camera
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
// GizmoRenderer
// =============================================================================
namespace
{
    const char* kVs = R"GLSL(
        #version 330 core
        layout(location = 0) in vec3 aPos;
        uniform mat4 uMVP;
        void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
    )GLSL";
    const char* kFs = R"GLSL(
        #version 330 core
        out vec4 FragColor;
        uniform vec4 uColor;
        void main() { FragColor = uColor; }
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
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        m.count = (GLsizei)idx.size();
    };

    const int seg = 16;
    // Cylinder (sides) and cone (sides + base), around +Y.
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        for (int i = 0; i < seg; ++i)
        {
            const float a = glm::two_pi<float>() * float(i) / float(seg);
            v.insert(v.end(), { std::cos(a), 0.0f, std::sin(a) });
            v.insert(v.end(), { std::cos(a), 1.0f, std::sin(a) });
        }
        for (uint32_t i = 0; i < (uint32_t)seg; ++i)
        {
            const uint32_t a0 = 2 * i, a1 = a0 + 1, b0 = 2 * ((i + 1) % seg), b1 = b0 + 1;
            idx.insert(idx.end(), { a0, b0, b1, a0, b1, a1 });
        }
        upload(m_cylinder, v, idx);
    }
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        v.insert(v.end(), { 0.0f, 1.0f, 0.0f });   // apex 0
        v.insert(v.end(), { 0.0f, 0.0f, 0.0f });   // base centre 1
        for (int i = 0; i < seg; ++i)
        {
            const float a = glm::two_pi<float>() * float(i) / float(seg);
            v.insert(v.end(), { std::cos(a), 0.0f, std::sin(a) });
        }
        for (uint32_t i = 0; i < (uint32_t)seg; ++i)
        {
            const uint32_t r0 = 2 + i, r1 = 2 + (i + 1) % seg;
            idx.insert(idx.end(), { 0u, r0, r1, 1u, r1, r0 });
        }
        upload(m_cone, v, idx);
    }
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        const int lat = 10, lon = 16;
        for (int i = 0; i <= lat; ++i)
        {
            const float ph = glm::pi<float>() * float(i) / float(lat);
            for (int j = 0; j <= lon; ++j)
            {
                const float th = glm::two_pi<float>() * float(j) / float(lon);
                v.insert(v.end(), { std::sin(ph) * std::cos(th), std::cos(ph), std::sin(ph) * std::sin(th) });
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
    {
        const std::vector<float> v = { 0, 0, 0,  1, 0, 0,  1, 1, 0,  0, 1, 0 };
        const std::vector<uint32_t> idx = { 0, 1, 2, 0, 2, 3 };
        upload(m_quad, v, idx);
    }
    {
        // Torus about +Y in the XZ plane: major radius 1, tube kRingTube.
        std::vector<float> v; std::vector<uint32_t> idx;
        const int maj = 96, mn = 8;
        for (int i = 0; i < maj; ++i)
        {
            const float u = glm::two_pi<float>() * float(i) / float(maj);
            const glm::vec3 c(std::cos(u), 0.0f, std::sin(u));
            for (int j = 0; j < mn; ++j)
            {
                const float w = glm::two_pi<float>() * float(j) / float(mn);
                const glm::vec3 p = c * (1.0f + kRingTube * std::cos(w)) + glm::vec3(0, kRingTube * std::sin(w), 0);
                v.insert(v.end(), { p.x, p.y, p.z });
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
    {
        std::vector<float> v;
        for (int c = 0; c < 8; ++c)
            v.insert(v.end(), { (c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : -1.0f });
        const std::vector<uint32_t> idx = {
            0, 2, 3, 0, 3, 1,   4, 5, 7, 4, 7, 6,   0, 1, 5, 0, 5, 4,
            2, 6, 7, 2, 7, 3,   0, 4, 6, 0, 6, 2,   1, 3, 7, 1, 7, 5 };
        upload(m_cube, v, idx);
    }
    glGenVertexArrays(1, &m_fanVao);
    glGenBuffers(1, &m_fanVbo);
    glBindVertexArray(m_fanVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_fanVbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    m_ready = true;
    return true;
}

void GizmoRenderer::DrawMesh(const Mesh& m, const glm::mat4& mvp, const glm::vec4& color)
{
    glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, &mvp[0][0]);
    glUniform4fv(m_uColor, 1, &color[0]);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr);
}

void GizmoRenderer::DrawMove(const Viewport& vp, const glm::vec3& pivot, Handle hover, Handle active)
{
    if (!Init()) return;

    const float L = HandleLength(vp, pivot);
    const glm::mat4 VP = vp.proj * vp.view;
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
    glUseProgram(m_program);

    // Guide line along the grabbed axis.
    const Handle axisH[3] = { Handle::X, Handle::Y, Handle::Z };
    if (IsAxis(active))
    {
        const int i = AxisIndex(active);
        glDepthMask(GL_FALSE);
        DrawMesh(m_cylinder, VP * AlongAxis(pivot - kAxes[i] * (40.0f * L), i, 80.0f * L, kShaftR * L * 0.6f),
                 glm::vec4(glm::vec3(kAxisColor[i]), 0.45f));
        glDepthMask(GL_TRUE);
    }

    // Axis arrows: shaft + cone head.
    for (int i = 0; i < 3; ++i)
    {
        const glm::vec4 c = colorFor(axisH[i], kAxisColor[i]);
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        DrawMesh(m_cylinder, VP * AlongAxis(pivot + kAxes[i] * (kShaftStart * L), i,
                                            (kHeadStart - kShaftStart) * L, kShaftR * L), c);
        DrawMesh(m_cone, VP * AlongAxis(pivot + kAxes[i] * (kHeadStart * L), i,
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
        DrawMesh(m_sphere, VP * M, c);
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
        glm::mat4 M(1.0f);
        const float s = (kPlaneHi - kPlaneLo) * L;
        M[0] = glm::vec4(kAxes[a] * s, 0.0f);
        M[1] = glm::vec4(kAxes[b] * s, 0.0f);
        M[2] = glm::vec4(kAxes[n], 0.0f);
        M[3] = glm::vec4(pivot + (kAxes[a] + kAxes[b]) * (kPlaneLo * L), 1.0f);
        DrawMesh(m_quad, VP * M, c);
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
    const glm::mat4 VP = vp.proj * vp.view;
    const bool dragging = drag && drag->valid;
    const Handle active = dragging ? drag->handle : Handle::None;
    const Handle rings[3] = { Handle::RotX, Handle::RotY, Handle::RotZ };

    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_program);

    // Rings: the ring about axis i lies in the plane normal to it, coloured
    // like that axis. Held ring lit, others faded; hover lit.
    for (int i = 0; i < 3; ++i)
    {
        glm::vec4 c = kAxisColor[i];
        if (dragging) c = (rings[i] == active) ? kHot : glm::vec4(glm::vec3(c), 0.25f);
        else if (rings[i] == hover) c = kHot;
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        DrawMesh(m_torus, VP * AlongAxis(pivot, i, R, R), c);
        glDepthMask(GL_TRUE);
    }

    // Small centre dot marking the pivot.
    {
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = kBallR * 0.6f * L;
        M[3] = glm::vec4(pivot, 1.0f);
        DrawMesh(m_sphere, VP * M, kBall);
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
        const glm::vec4 c(glm::vec3(kHot), 0.28f);
        glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, &VP[0][0]);
        glUniform4fv(m_uColor, 1, &c[0]);
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
    const glm::mat4 VP = vp.proj * vp.view;
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
    glUseProgram(m_program);

    const Handle axes[3] = { Handle::ScaleX, Handle::ScaleY, Handle::ScaleZ };
    for (int i = 0; i < 3; ++i)
    {
        const glm::vec4 c = colorFor(axes[i], kAxisColor[i]);
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        const float cubeAt = kScaleCubePos * k * L;
        const float stalk = cubeAt - kScaleCubeR * L - kShaftStart * L;
        if (stalk > 0.0f)
            DrawMesh(m_cylinder, VP * AlongAxis(pivot + kAxes[i] * (kShaftStart * L), i, stalk, kShaftR * L), c);
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = kScaleCubeR * L;
        M[3] = glm::vec4(pivot + kAxes[i] * cubeAt, 1.0f);
        DrawMesh(m_cube, VP * M, c);
        glDepthMask(GL_TRUE);
    }
    {
        const glm::vec4 c = colorFor(Handle::ScaleUniform, kBall);
        if (c.a < 1.0f) glDepthMask(GL_FALSE);
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = kCentreCubeR * L;
        M[3] = glm::vec4(pivot, 1.0f);
        DrawMesh(m_cube, VP * M, c);
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
    const glm::mat4 VP = vp.proj * vp.view;

    // Part of the scene: depth-tested like everything else, nothing cleared.
    // Lifted a hair off y = 0 so the X / Z shafts don't z-fight the floor.
    const glm::vec3 o = origin + glm::vec3(0.0f, 0.002f * L, 0.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(m_program);

    constexpr float kShaft = 0.028f, kHead = 0.075f, kHeadLen = 0.24f;
    for (int i = 0; i < 3; ++i)
    {
        const glm::vec4 c = kAxisColor[i];
        DrawMesh(m_cylinder, VP * AlongAxis(o, i, (1.0f - kHeadLen) * L, kShaft * L), c);
        DrawMesh(m_cone, VP * AlongAxis(o + kAxes[i] * ((1.0f - kHeadLen) * L), i,
                                        kHeadLen * L, kHead * L), c);
    }
    {
        glm::mat4 M(1.0f);
        M[0][0] = M[1][1] = M[2][2] = 0.055f * L;
        M[3] = glm::vec4(o, 1.0f);
        DrawMesh(m_sphere, VP * M, kBall);
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
    glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, &VP[0][0]);
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
        glUniform4fv(m_uColor, 1, &kAxisColor[i][0]);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(tri.size() / 3));
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void GizmoRenderer::Destroy()
{
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
