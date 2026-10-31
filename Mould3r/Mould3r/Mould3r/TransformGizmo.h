#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

#include "GridSettings.h"

// =============================================================================
// TransformGizmo — the on-canvas handles for the Move / Rotate / Scale tools,
// shared by the Prepare canvas (GLCanvas) and the Fixture Editor
// (FixtureCanvas) so both draw and behave identically.
//
// Everything is drawn at the session pivot (the centre of the selection's
// bounding box, plus the move so far) and aligned to the world axes (X red,
// Y green, Z blue).
//
// MOVE handles:
//
//   * three axis arrows   — drag along that one axis;
//   * three plane squares — drag in that plane (XY / XZ / YZ), each tinted
//                           by the axis normal to it, the usual convention;
//   * a centre ball       — free drag in the plane facing the camera.
//
// ROTATE rings: one per world axis, in the plane that axis is normal to. A drag
// turns the selection about that axis through the pivot; the angle follows the
// cursor around the ring (several turns are fine), and a translucent wedge
// shows the sweep. A ring seen edge-on falls back to dragging along it.
//
// SCALE handles: a cube on a stalk along each axis plus a centre cube. Every
// one scales UNIFORMLY (a body carries a single scale factor), about the
// pivot:
//   * an axis cube follows the cursor along its axis — drag it out to grow,
//     in to shrink; the factor is how far it now sits from the pivot over how
//     far it sat at the grab;
//   * the centre cube grows dragging right / up and shrinks left / down,
//     doubling every arrow-length (110 px) of travel.
// While dragging, the stalks stretch with the factor.
//
// The handles keep a constant on-screen size whatever the zoom. Hit-testing is
// done in screen space against the projected handles, so there's no picking
// pass and the tolerance is in pixels.
//
// The pure parts (hit test, drag maths, snapping) are plain glm functions so
// they can be unit-tested without GL; GizmoRenderer is the only GL code and
// owns its own tiny shader, so it works in either canvas's context.
// =============================================================================
namespace Gizmo
{
    enum class Handle { None, X, Y, Z, PlaneYZ, PlaneXZ, PlaneXY, Free,
                        RotX, RotY, RotZ,
                        ScaleX, ScaleY, ScaleZ, ScaleUniform };

    // A canvas's camera matrices and client size in the same pixels as its
    // mouse events.
    struct Viewport
    {
        glm::mat4 view{ 1.0f };
        glm::mat4 proj{ 1.0f };
        int       width = 1;
        int       height = 1;
    };

    // On-screen length of an axis arrow, pixels.
    constexpr float kArrowPx = 110.0f;

    // World size of one pixel at `at` (perspective), and the arrow length there.
    float WorldPerPixel(const Viewport& vp, const glm::vec3& at);
    inline float HandleLength(const Viewport& vp, const glm::vec3& pivot)
    {
        return kArrowPx * WorldPerPixel(vp, pivot);
    }

    // World-space mouse ray for client pixel (mx, my).
    void MouseRay(const Viewport& vp, float mx, float my, glm::vec3& origin, glm::vec3& dir);

    // Move handle under the cursor at (mx, my), or None.
    Handle HitTestMove(const Viewport& vp, const glm::vec3& pivot, float mx, float my);

    // A Move drag in progress: the constraint is frozen at the grab, so the
    // result only depends on where the cursor is now.
    struct MoveDrag
    {
        Handle    handle = Handle::None;
        glm::vec3 origin{ 0.0f };   // pivot at the grab
        glm::vec3 dir{ 0.0f };      // axis (X/Y/Z) or plane normal (planes, Free)
        glm::vec3 grab{ 0.0f };     // where the cursor first met the constraint
        bool      valid = false;
    };

    // Start a drag on `h`; false when the constraint can't be grabbed from this
    // view (an axis pointing straight at the camera, a plane seen edge-on).
    bool BeginMoveDrag(const Viewport& vp, const glm::vec3& pivot, Handle h,
                       float mx, float my, MoveDrag& out);

    // World offset from the grab to the cursor now, along the constraint.
    // False (offset untouched) when the ray can't meet it this frame.
    bool MoveDragOffset(const Viewport& vp, const MoveDrag& d, float mx, float my,
                        glm::vec3& offset);

    // Ctrl-snapping for a Move drag. `target` is where the pivot would land;
    // the components the handle frees are snapped, the rest kept:
    //   * X and Z both free (XZ plane, Free)   -> the grid's own XZ snap
    //                                            (rectangular lattice or
    //                                            circular rings / spokes);
    //   * only one of X / Z free (an axis, or the XY / YZ plane)
    //                                          -> rounded to the grid spacing;
    //   * Y free                               -> rounded to the grid spacing.
    glm::vec3 SnapMoveTarget(const glm::vec3& target, Handle h, const GridSettings& g);

    // ---- Rotate ----------------------------------------------------------------
    // Ring radius as a fraction of the arrow length.
    constexpr float kRingRadius = 0.85f;

    // Rotate ring under the cursor (RotX / RotY / RotZ), or None.
    Handle HitTestRotate(const Viewport& vp, const glm::vec3& pivot, float mx, float my);

    // A Rotate drag in progress. The angle is accumulated frame to frame so it
    // can run past +-180 (several turns).
    struct RotateDrag
    {
        Handle    handle = Handle::None;
        int       axis = 0;                // 0 = X, 1 = Y, 2 = Z
        glm::vec3 pivot{ 0.0f };
        glm::vec3 v0{ 0.0f };              // unit, in the ring plane: the grab direction
        bool      tangentMode = false;     // ring edge-on: drag along it on screen
        glm::vec2 grabPx{ 0.0f };
        glm::vec2 tanScreen{ 0.0f };       // screen direction of +angle at the grab
        float     pxPerRad = 1.0f;
        float     lastRaw = 0.0f;          // plane mode: previous wrapped angle
        float     totalRad = 0.0f;         // accumulated, signed (right-hand about the axis)
        bool      valid = false;
    };

    bool BeginRotateDrag(const Viewport& vp, const glm::vec3& pivot, Handle h,
                         float mx, float my, RotateDrag& out);
    // Updates the accumulated angle; returns it in degrees. False (drag
    // unchanged) when the cursor can't be read against the ring this frame.
    bool RotateDragAngle(const Viewport& vp, RotateDrag& d, float mx, float my, float& totalDeg);

    // Ctrl-snapping for a Rotate drag: the turn rounded to `stepDeg`.
    inline float SnapAngle(float deg, float stepDeg = 15.0f)
    {
        return stepDeg > 0.0f ? std::round(deg / stepDeg) * stepDeg : deg;
    }

    // ---- Scale -----------------------------------------------------------------
    // Scale handle under the cursor (ScaleX / ScaleY / ScaleZ / ScaleUniform).
    Handle HitTestScale(const Viewport& vp, const glm::vec3& pivot, float mx, float my);

    struct ScaleDrag
    {
        Handle    handle = Handle::None;
        int       axis = -1;               // 0..2 for an axis cube, -1 for the centre
        glm::vec3 pivot{ 0.0f };
        float     t0 = 1.0f;               // axis cube: grab distance along the axis
        glm::vec2 grabPx{ 0.0f };          // centre cube: grab position on screen
        float     pxPerDouble = 110.0f;    // centre cube: travel that doubles the size
        bool      valid = false;
    };

    bool BeginScaleDrag(const Viewport& vp, const glm::vec3& pivot, Handle h,
                        float mx, float my, ScaleDrag& out);
    // The factor relative to the grab (1 = unchanged), floored at kMinFactor.
    // False (keep the last factor) when the cursor can't be read this frame.
    bool ScaleDragFactor(const Viewport& vp, const ScaleDrag& d, float mx, float my, float& factor);

    constexpr float kMinFactor = 0.01f;

    // Ctrl-snapping for a Scale drag: the RESULTING scale (relative to the
    // tool's start) rounded to `step` (0.1 = 10 % steps), never below `step`.
    inline float SnapFactor(float f, float step = 0.1f)
    {
        return std::max(step, std::round(f / step) * step);
    }

    // The grid's XZ snap (a copy of GLCanvas::SnapToGrid, so the Fixture
    // Editor, which has no snap of its own, snaps the same way).
    glm::vec2 GridSnapXZ(const GridSettings& g, const glm::vec2& p);

    // ---- Reference axes on the grid ------------------------------------------
    // A small labelled X / Y / Z triad standing on the grid's front-left corner
    // -- the (-X, -Z) corner, nearest the camera in the default view, where the
    // X and Z arrows run along the grid's two front edges (a circular grid
    // uses the matching point on its rim). It is sized with the grid, not the
    // zoom, so it reads as part of the floor.
    void GridTriadPlacement(const GridSettings& g, glm::vec3& origin, float& length);

    // -------------------------------------------------------------------------
    // GizmoRenderer — draws the handles. Create one per canvas; GL resources are
    // made lazily on the first Draw, in whichever context is current.
    // Drawn last in the frame: it clears the depth buffer so the handles sit
    // on top of the scene but still occlude each other correctly.
    // -------------------------------------------------------------------------
    class GizmoRenderer
    {
    public:
        void DrawMove(const Viewport& vp, const glm::vec3& pivot,
                      Handle hover, Handle active);
        // `drag` (may be null) is the ring being held; `sweepDeg` the turn
        // applied so far (after any Ctrl snap), drawn as a wedge from the grab.
        void DrawRotate(const Viewport& vp, const glm::vec3& pivot,
                        Handle hover, const RotateDrag* drag, float sweepDeg);
        // `active` is the handle being dragged (or None); `stretch` the factor
        // applied by that drag so far, used to lengthen the stalks.
        void DrawScale(const Viewport& vp, const glm::vec3& pivot,
                       Handle hover, Handle active, float stretch);
        // The grid's reference triad (GridTriadPlacement). Unlike the handles
        // it is drawn IN the scene (depth-tested, no depth clear), right after
        // the grid; the X / Y / Z labels face the camera.
        void DrawAxisTriad(const Viewport& vp, const glm::vec3& origin, float length);
        void Destroy();   // call with the owning context current

    private:
        struct Mesh { GLuint vao = 0, vbo = 0, ebo = 0; GLsizei count = 0; };
        bool Init();
        void DrawMesh(const Mesh& m, const glm::mat4& mvp, const glm::vec4& color);

        bool   m_ready = false;
        bool   m_failed = false;
        GLuint m_program = 0;
        GLint  m_uMVP = -1;
        GLint  m_uColor = -1;
        Mesh   m_cylinder;   // radius 1, y in [0, 1]
        Mesh   m_cone;       // base radius 1 at y = 0, apex at y = 1
        Mesh   m_sphere;     // radius 1
        Mesh   m_quad;       // unit square [0,1]^2 in the XY plane
        Mesh   m_torus;      // ring in the XZ plane about +Y: radius 1, tube kRingTube
        Mesh   m_cube;       // [-1, 1]^3
        GLuint m_fanVao = 0, m_fanVbo = 0;   // dynamic sweep wedge (rotate drags)
    };
}
