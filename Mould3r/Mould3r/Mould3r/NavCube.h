#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

class OrbitCamera;
#ifndef NAVCUBE_NO_WX
class wxMouseEvent;
class wxWindow;
#endif

// =============================================================================
// NavCube — the view cube pinned to the top-right corner of a 3D viewport,
// shared by the Prepare / Preview canvas (GLCanvas) and the Fixture Editor
// (FixtureCanvas).
//
// The cube turns with the camera, so the face toward you names the side of the
// model you're looking at. Faces are labelled in the world frame:
//
//     FRONT  +Z      BACK   -Z
//     RIGHT  +X      LEFT   -X
//     TOP    +Y      BOTTOM -Y
//
// Each face is a 3 x 3 grid of tiles. The middle tile is the face itself, the
// strips along its sides belong to the EDGES and the corner squares to the
// CORNERS, so every tile maps to a direction with components in {-1, 0, 1}:
// a face (one non-zero), an edge (two) or a corner (three) — 26 views. The
// tiles of one edge / corner light up together on hover.
//
//   * click a tile   -> the camera swings (animated) to look from that
//                       direction at the same target and distance;
//   * drag the cube  -> orbits the camera, like dragging the scene. The
//                       pointer is hidden and held at the cube while dragging,
//                       so the drag never runs out at the screen edge;
//   * the HOME icon  -> (top-left of the cube) swings back to the start-up
//                       view: default angle, target and distance.
//
// The cube is drawn 75 % opaque.
//
// The pure parts (layout, hit test, view angles, the click / drag / snap
// controller) are wx-free so they can be tested headless; the label textures
// and the wx mouse glue are compiled unless NAVCUBE_NO_WX is defined.
// =============================================================================
namespace NavCube
{
    constexpr int   kSizePx = 98;       // cube viewport, client pixels (square)
    constexpr int   kMarginPx = 14;     // from the canvas's top and right edges
    constexpr int   kHomePx = 20;       // home icon, client pixels (square)
    constexpr float kOpacity = 0.75f;   // the cube's (and home icon's) opacity

    // A square in client pixels (y down, like mouse events).
    struct Rect
    {
        int x = 0, y = 0, size = 0;
        bool Contains(int px, int py) const
        {
            return px >= x && px < x + size && py >= y && py < y + size;
        }
    };

    // Where the cube and its home icon sit for a given client size.
    struct Layout
    {
        Rect cube;
        Rect home;
        bool Contains(int px, int py) const { return cube.Contains(px, py) || home.Contains(px, py); }
    };
    Layout Placement(int clientW, int clientH);

    // Space a top-right overlay (the Move / Rotate / Scale panel) should leave
    // above itself so it sits clear below the cube.
    constexpr int kOverlayTopPx = kMarginPx + kSizePx + 6;

    // The view direction under (mx, my): components in {-1, 0, 1} pointing
    // from the cube's centre at the clicked face / edge / corner; (0,0,0) when
    // the cursor misses the cube. `view` is the camera's view matrix (only its
    // rotation is used).
    glm::ivec3 HitTest(const glm::mat4& view, const Rect& cube, int mx, int my);

    // OrbitCamera yaw / pitch that look at the target FROM direction `dir`.
    // Top / Bottom keep Front toward the bottom / top of the screen.
    void TargetAngles(const glm::ivec3& dir, float& yawDeg, float& pitchDeg);

    // Short name for a direction ("Front", "Top Front Right", ...), for tooltips.
    const char* DirectionName(const glm::ivec3& dir);

    // -------------------------------------------------------------------------
    // Controller — press / drag / click / snap-animation state. One per canvas.
    // -------------------------------------------------------------------------
    class Controller
    {
    public:
        Controller();

        // A left press: starts a cube / home gesture when (x, y) is on either.
        // Returns whether it did (the event is then the cube's).
        bool MouseDown(const glm::mat4& view, const Layout& l, int x, int y);
        // Motion with the button held during a cube gesture: past a few pixels
        // it becomes a drag that orbits `cam`. False when no gesture is live.
        bool MouseDrag(int x, int y, OrbitCamera& cam);
        // Release: a click (no drag) on a tile snaps to that view; on the home
        // icon, goes home.
        bool MouseUp(const glm::mat4& view, const Layout& l, int x, int y,
                     OrbitCamera& cam, long long nowMs);
        // Plain motion: tracks the hovered tile / home icon. True when the
        // highlight changed.
        bool Hover(const glm::mat4& view, const Layout& l, int x, int y);
        void ClearHover() { m_hover = glm::ivec3(0); m_homeHover = false; }

        // Advance a snap animation; call at the top of each paint. Returns true
        // while it's still running (the caller schedules another frame).
        bool Animate(OrbitCamera& cam, long long nowMs);
        void StopAnimation() { m_animating = false; }
        bool Animating() const { return m_animating; }

        bool Pressed() const { return m_pressed || m_homePressed; }
        bool Dragging() const { return m_dragging; }
        int  DownX() const { return m_downX; }
        int  DownY() const { return m_downY; }
        // The pointer was moved to (x, y) by the canvas (re-centred during a
        // drag): measure the next motion from there.
        void Rebase(int x, int y) { m_lastX = x; m_lastY = y; }
        // Abandon a gesture (mouse capture lost).
        void Cancel() { m_pressed = m_homePressed = m_dragging = false; m_pressDir = glm::ivec3(0); }

        // The tiles to light up: the hovered direction, or the pressed one
        // while a click is in progress.
        glm::ivec3 Highlight() const;
        bool HomeHot() const { return m_homeHover || m_homePressed; }

        // Start a snap to `dir` (also usable from a menu / shortcut).
        void SnapTo(const glm::ivec3& dir, const OrbitCamera& cam, long long nowMs);
        // Start the swing back to the home view.
        void GoHome(const OrbitCamera& cam, long long nowMs);
        // The home view's target / distance. Defaults to a fresh OrbitCamera's;
        // a canvas that frames its content on load sets that framing here.
        // The home angle is always the default view's (front-right-top).
        void SetHome(const glm::vec3& target, float distance);

    private:
        bool       m_pressed = false;
        bool       m_dragging = false;
        bool       m_homePressed = false;
        bool       m_homeHover = false;
        int        m_downX = 0, m_downY = 0, m_lastX = 0, m_lastY = 0;
        glm::ivec3 m_hover{ 0 };
        glm::ivec3 m_pressDir{ 0 };

        glm::vec3  m_homeTarget{ 0.0f };
        float      m_homeDist = 350.0f;
        float      m_homeYaw = 225.0f, m_homePitch = -35.0f;

        bool       m_animating = false;
        bool       m_animFrame = false;   // also moving target / distance (Home)
        float      m_yaw0 = 0.0f, m_pitch0 = 0.0f, m_yaw1 = 0.0f, m_pitch1 = 0.0f;
        glm::vec3  m_tgt0{ 0.0f }, m_tgt1{ 0.0f };
        float      m_dist0 = 1.0f, m_dist1 = 1.0f;
        long long  m_t0 = 0;
        void       StartAnim(const OrbitCamera& cam, float yaw1, float pitch1, long long nowMs);
    };

    // -------------------------------------------------------------------------
    // Renderer — draws the cube and home icon into their corner viewports,
    // last in the frame. GL resources are created lazily in whichever context
    // is current.
    // -------------------------------------------------------------------------
    class Renderer
    {
    public:
        // `highlight` = Controller::Highlight(), `homeHot` = HomeHot().
        // Restores the full-canvas viewport (clientW x clientH) before returning.
        void Draw(const glm::mat4& view, const Layout& l, int clientW, int clientH,
                  const glm::ivec3& highlight, bool homeHot);

        // Face label images: one 8-bit coverage mask per face (0 = clear,
        // 255 = ink), rows top to bottom. Face order: +X, -X, +Y, -Y, +Z, -Z.
        void SetLabel(int face, int w, int h, const unsigned char* coverage);
        bool HasLabels() const { return m_hasLabels; }

        void Destroy();   // call with the owning context current

    private:
        bool Init();
        void Quad(const glm::mat4& mvp, const glm::vec4& color, GLuint tex = 0);
        void DrawHome(const Rect& r, int clientH, bool hot);

        bool   m_ready = false, m_failed = false, m_hasLabels = false;
        GLuint m_program = 0, m_vao = 0, m_vbo = 0;
        GLuint m_iconVao = 0, m_iconVbo = 0;
        GLsizei m_iconPlate = 0, m_iconHouse = 0;   // vertex counts (plate first)
        GLint  m_uMVP = -1, m_uColor = -1, m_uUseTex = -1, m_uTex = -1;
        GLuint m_labelTex[6] = { 0, 0, 0, 0, 0, 0 };
        float  m_labelAspect[6] = { 4, 4, 4, 4, 4, 4 };   // width / height
    };

#ifndef NAVCUBE_NO_WX
    // Renders FRONT / BACK / ... with the system UI font and hands them to
    // `r`. Needs a GL context current (it uploads textures).
    void BuildLabels(Renderer& r);

    // wx glue: feed every mouse event (and left double-clicks) here first.
    // Returns true when the event belonged to the cube — the canvas then
    // ignores it. Handles capture, the hidden / re-centred pointer while
    // dragging, the hover tooltip and refresh.
    bool HandleMouse(Controller& c, wxMouseEvent& evt, wxWindow* canvas, OrbitCamera& cam);

    // From the canvas's wxEVT_MOUSE_CAPTURE_LOST: ends a cube drag cleanly
    // (pointer shown again).
    void CaptureLost(Controller& c, wxWindow* canvas);

    // At the top of OnPaint: advances a snap and keeps frames coming while
    // it runs.
    void AnimateFrame(Controller& c, wxWindow* canvas, OrbitCamera& cam);
#endif
}
