// Same as GLCanvas.cpp: the CRT headers (pulled in by glm / <cmath> below)
// come before any wx header, so wx's own opt-out arrives too late and the
// project's SDL checks turn its CRT calls into C4996 errors.
#define _CRT_SECURE_NO_WARNINGS

#include "NavCube.h"

#include "camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef NAVCUBE_NO_WX
#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/time.h>
#include <wx/window.h>
#endif

namespace NavCube
{
namespace
{
    // Half-extent of the orthographic box around the unit cube (half-size 1):
    // just over the corner radius sqrt(3), so the cube never clips as it turns.
    constexpr float kOrtho = 1.78f;
    // Tile split: the middle tile spans |u|, |v| <= kInner of the face [-1, 1].
    constexpr float kInner = 0.64f;
    constexpr float kGap = 0.018f;            // gap between tiles (shows the frame)
    constexpr long long kSnapMs = 280;        // snap animation length
    constexpr int kDragPx = 3;                // press-to-drag threshold

    // A face's frame: outward normal, and the label's right / up directions
    // as read when looking at that face in its own standard view.
    struct Face { glm::vec3 n, right, up; };
    const Face kFaces[6] = {
        { {  1, 0, 0 }, {  0, 0, -1 }, { 0, 1,  0 } },   // +X  RIGHT
        { { -1, 0, 0 }, {  0, 0,  1 }, { 0, 1,  0 } },   // -X  LEFT
        { {  0, 1, 0 }, {  1, 0,  0 }, { 0, 0, -1 } },   // +Y  TOP
        { {  0,-1, 0 }, {  1, 0,  0 }, { 0, 0,  1 } },   // -Y  BOTTOM
        { {  0, 0, 1 }, {  1, 0,  0 }, { 0, 1,  0 } },   // +Z  FRONT
        { {  0, 0,-1 }, { -1, 0,  0 }, { 0, 1,  0 } },   // -Z  BACK
    };
    const char* const kLabels[6] = { "RIGHT", "LEFT", "TOP", "BOTTOM", "FRONT", "BACK" };

    inline glm::ivec3 ToI(const glm::vec3& v)
    {
        return glm::ivec3(int(std::lround(v.x)), int(std::lround(v.y)), int(std::lround(v.z)));
    }

    // Tile index along one face axis: -1 / 0 / +1 for coordinate c in [-1, 1].
    inline int Band(float c) { return c > kInner ? 1 : (c < -kInner ? -1 : 0); }

    inline glm::mat4 Ortho()
    {
        return glm::ortho(-kOrtho, kOrtho, -kOrtho, kOrtho, -10.0f, 10.0f);
    }
    inline glm::mat3 ViewRot(const glm::mat4& view) { return glm::mat3(view); }

    float Ease(float t)   // smoothstep-ish ease in / out
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
    }
}

// =============================================================================
// Placement / hit test / angles
// =============================================================================
Rect Placement(int clientW, int /*clientH*/)
{
    Rect r;
    r.size = kSizePx;
    r.x = std::max(0, clientW - kSizePx - kMarginPx);
    r.y = kMarginPx;
    return r;
}

glm::ivec3 HitTest(const glm::mat4& view, const Rect& r, int mx, int my)
{
    if (!r.Contains(mx, my) || r.size <= 0) return glm::ivec3(0);

    // Orthographic: a ray straight into the screen through the cursor, in
    // view space, taken back into the cube's (world-aligned) frame.
    const float u = (2.0f * (float(mx - r.x) + 0.5f) / float(r.size) - 1.0f) * kOrtho;
    const float v = (1.0f - 2.0f * (float(my - r.y) + 0.5f) / float(r.size)) * kOrtho;
    const glm::mat3 Rt = glm::transpose(ViewRot(view));   // view -> world
    const glm::vec3 o = Rt * glm::vec3(u, v, 5.0f);
    const glm::vec3 d = Rt * glm::vec3(0.0f, 0.0f, -1.0f);

    // Slab test against [-1, 1]^3.
    float tNear = -1e9f, tFar = 1e9f;
    int axis = -1;
    for (int i = 0; i < 3; ++i)
    {
        if (std::abs(d[i]) < 1e-8f)
        {
            if (o[i] < -1.0f || o[i] > 1.0f) return glm::ivec3(0);
            continue;
        }
        float t0 = (-1.0f - o[i]) / d[i], t1 = (1.0f - o[i]) / d[i];
        if (t0 > t1) std::swap(t0, t1);
        if (t0 > tNear) { tNear = t0; axis = i; }
        tFar = std::min(tFar, t1);
    }
    if (axis < 0 || tNear > tFar) return glm::ivec3(0);

    const glm::vec3 p = o + d * tNear;
    glm::ivec3 dir(0);
    dir[axis] = p[axis] > 0.0f ? 1 : -1;
    for (int i = 0; i < 3; ++i)
        if (i != axis) dir[i] = Band(p[i]);
    return dir;
}

void TargetAngles(const glm::ivec3& dir, float& yawDeg, float& pitchDeg)
{
    // OrbitCamera puts the camera at target - D * dist with
    // D = (cos p cos y, sin p, cos p sin y), so looking FROM dir means D = -dir.
    const glm::vec3 v = glm::normalize(glm::vec3(dir));
    pitchDeg = glm::degrees(std::asin(glm::clamp(-v.y, -1.0f, 1.0f)));
    if (dir.x == 0 && dir.z == 0)
        yawDeg = 270.0f;   // Top / Bottom: Front (+Z) toward the bottom / top of the screen
    else
        yawDeg = glm::degrees(std::atan2(-v.z, -v.x));
}

const char* DirectionName(const glm::ivec3& d)
{
    static std::string s;
    s.clear();
    auto add = [&](const char* w) { if (!s.empty()) s += ' '; s += w; };
    if (d.y > 0) add("Top");
    if (d.y < 0) add("Bottom");
    if (d.z > 0) add("Front");
    if (d.z < 0) add("Back");
    if (d.x > 0) add("Right");
    if (d.x < 0) add("Left");
    return s.c_str();
}

// =============================================================================
// Controller
// =============================================================================
bool Controller::MouseDown(const glm::mat4& view, const Rect& r, int x, int y)
{
    if (!r.Contains(x, y)) return false;
    m_pressed = true;
    m_dragging = false;
    m_downX = m_lastX = x;
    m_downY = m_lastY = y;
    m_pressDir = HitTest(view, r, x, y);
    return true;
}

bool Controller::MouseDrag(int x, int y, OrbitCamera& cam)
{
    if (!m_pressed) return false;
    if (!m_dragging &&
        (std::abs(x - m_downX) > kDragPx || std::abs(y - m_downY) > kDragPx))
    {
        m_dragging = true;
        m_animating = false;            // a drag takes over from a snap
        m_hover = glm::ivec3(0);
        m_pressDir = glm::ivec3(0);
    }
    if (m_dragging)
        cam.Orbit(float(x - m_lastX), float(y - m_lastY));
    m_lastX = x;
    m_lastY = y;
    return true;
}

bool Controller::MouseUp(const glm::mat4& view, const Rect& r, int x, int y,
                         OrbitCamera& cam, long long nowMs)
{
    if (!m_pressed) return false;
    m_pressed = false;
    if (!m_dragging)
    {
        // A click: snap to what's under the release (normally what was pressed).
        const glm::ivec3 dir = HitTest(view, r, x, y);
        if (dir != glm::ivec3(0)) SnapTo(dir, cam, nowMs);
    }
    m_dragging = false;
    m_pressDir = glm::ivec3(0);
    m_hover = HitTest(view, r, x, y);
    return true;
}

bool Controller::Hover(const glm::mat4& view, const Rect& r, int x, int y)
{
    const glm::ivec3 h = HitTest(view, r, x, y);
    if (h == m_hover) return false;
    m_hover = h;
    return true;
}

glm::ivec3 Controller::Highlight() const
{
    if (m_pressed) return m_dragging ? glm::ivec3(0) : m_pressDir;
    return m_hover;
}

void Controller::SnapTo(const glm::ivec3& dir, const OrbitCamera& cam, long long nowMs)
{
    float yaw1, pitch1;
    TargetAngles(dir, yaw1, pitch1);
    m_yaw0 = cam.GetYawDegrees();
    m_pitch0 = cam.GetPitchDegrees();
    // The short way round in yaw (the camera's yaw is unbounded).
    float dy = std::fmod(yaw1 - m_yaw0, 360.0f);
    if (dy > 180.0f) dy -= 360.0f;
    if (dy < -180.0f) dy += 360.0f;
    // Straight down / up the yaw is only an orientation; still take the short way.
    m_yaw1 = m_yaw0 + dy;
    m_pitch1 = pitch1;
    m_t0 = nowMs;
    m_animating = true;
}

bool Controller::Animate(OrbitCamera& cam, long long nowMs)
{
    if (!m_animating) return false;
    const float t = float(nowMs - m_t0) / float(kSnapMs);
    if (t >= 1.0f)
    {
        cam.SetYawPitchDegrees(m_yaw1, m_pitch1);
        m_animating = false;
        return false;
    }
    const float e = Ease(t);
    cam.SetYawPitchDegrees(m_yaw0 + (m_yaw1 - m_yaw0) * e, m_pitch0 + (m_pitch1 - m_pitch0) * e);
    return true;
}

// =============================================================================
// Renderer
// =============================================================================
namespace
{
    const char* kVs = R"GLSL(
        #version 330 core
        layout(location = 0) in vec2 aUV;      // unit quad [0,1]^2
        uniform mat4 uMVP;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = uMVP * vec4(aUV, 0.0, 1.0); }
    )GLSL";
    const char* kFs = R"GLSL(
        #version 330 core
        in vec2 vUV;
        out vec4 FragColor;
        uniform vec4 uColor;
        uniform int uUseTex;
        uniform sampler2D uTex;
        void main()
        {
            if (uUseTex == 1)
            {
                float a = texture(uTex, vec2(vUV.x, 1.0 - vUV.y)).r;   // rows top-down
                FragColor = vec4(uColor.rgb, uColor.a * a);
            }
            else
                FragColor = uColor;
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

    // Colours (linear-ish sRGB, tuned against the 0.12 / 0.14 / 0.18 backdrop).
    const glm::vec3 kFaceCol(0.86f, 0.88f, 0.92f);   // middle tiles
    const glm::vec3 kRimCol(0.72f, 0.75f, 0.80f);    // edge / corner tiles
    const glm::vec3 kHotCol(0.38f, 0.64f, 1.00f);    // hovered / pressed
    const glm::vec3 kFrameCol(0.22f, 0.24f, 0.29f);  // the gaps between tiles
    const glm::vec3 kInkCol(0.16f, 0.18f, 0.22f);    // labels

    // Simple headlight-ish shading from the face's view-space normal.
    float Shade(const glm::mat3& R, const glm::vec3& n)
    {
        const glm::vec3 nv = R * n;
        const glm::vec3 L = glm::normalize(glm::vec3(0.35f, 0.55f, 1.0f));
        return 0.62f + 0.38f * std::max(0.0f, glm::dot(nv, L));
    }

    // Model matrix mapping the unit quad onto the rectangle [u0,u1] x [v0,v1]
    // of face f, pushed out along the normal to `lift`.
    glm::mat4 FaceRect(const Face& f, float u0, float u1, float v0, float v1, float lift)
    {
        glm::mat4 M(1.0f);
        M[0] = glm::vec4(f.right * (u1 - u0), 0.0f);
        M[1] = glm::vec4(f.up * (v1 - v0), 0.0f);
        M[2] = glm::vec4(f.n, 0.0f);
        M[3] = glm::vec4(f.n * lift + f.right * u0 + f.up * v0, 1.0f);
        return M;
    }
}

bool Renderer::Init()
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
    m_uUseTex = glGetUniformLocation(m_program, "uUseTex");
    m_uTex = glGetUniformLocation(m_program, "uTex");

    const float quad[] = { 0, 0,  1, 0,  1, 1,   0, 0,  1, 1,  0, 1 };
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    m_ready = true;
    return true;
}

void Renderer::SetLabel(int face, int w, int h, const unsigned char* coverage)
{
    if (face < 0 || face >= 6 || w <= 0 || h <= 0 || !coverage) return;
    if (!Init()) return;
    if (!m_labelTex[face]) glGenTextures(1, &m_labelTex[face]);
    glBindTexture(GL_TEXTURE_2D, m_labelTex[face]);
    GLint align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, coverage);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_labelAspect[face] = float(w) / float(h);
    m_hasLabels = true;
}

void Renderer::Quad(const glm::mat4& mvp, const glm::vec4& color, GLuint tex)
{
    glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, &mvp[0][0]);
    glUniform4fv(m_uColor, 1, &color[0]);
    glUniform1i(m_uUseTex, tex ? 1 : 0);
    if (tex)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(m_uTex, 0);
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Renderer::Draw(const glm::mat4& view, const Rect& r, int clientW, int clientH,
                    const glm::ivec3& highlight)
{
    if (r.size <= 0 || !Init()) return;

    // Own little viewport in the corner; only its depth is cleared, so the
    // scene shows through around the cube.
    const int glY = clientH - r.y - r.size;
    glViewport(r.x, glY, r.size, r.size);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r.x, glY, r.size, r.size);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    const glm::mat3 R = ViewRot(view);
    const glm::mat4 VP = Ortho() * glm::mat4(R);

    GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(m_program);
    glBindVertexArray(m_vao);

    const float bands[4] = { -1.0f, -kInner, kInner, 1.0f };
    for (int fi = 0; fi < 6; ++fi)
    {
        const Face& f = kFaces[fi];
        const float s = Shade(R, f.n);
        const glm::ivec3 n = ToI(f.n), ri = ToI(f.right), up = ToI(f.up);

        // The frame: a dark face just under the tiles; it shows in the gaps.
        Quad(VP * FaceRect(f, -1.0f, 1.0f, -1.0f, 1.0f, 0.997f), glm::vec4(kFrameCol * s, 1.0f));

        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j)
            {
                const glm::ivec3 dir = n + ri * i + up * j;
                glm::vec3 c = (i == 0 && j == 0) ? kFaceCol : kRimCol;
                if (highlight != glm::ivec3(0) && dir == highlight) c = kHotCol;
                const float u0 = bands[i + 1] + kGap, u1 = bands[i + 2] - kGap;
                const float v0 = bands[j + 1] + kGap, v1 = bands[j + 2] - kGap;
                Quad(VP * FaceRect(f, u0, u1, v0, v1, 1.0f), glm::vec4(c * s, 1.0f));
            }
    }

    // Labels on the middle tiles, blended over them.
    if (m_hasLabels)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (int fi = 0; fi < 6; ++fi)
        {
            if (!m_labelTex[fi]) continue;
            const Face& f = kFaces[fi];
            const float s = Shade(R, f.n);
            const float halfW = kInner * 0.97f;
            const float halfH = halfW / std::max(0.5f, m_labelAspect[fi]);
            const glm::ivec3 n = ToI(f.n);
            glm::vec3 ink = kInkCol;
            if (highlight == n) ink = glm::vec3(1.0f);   // white on the lit tile
            Quad(VP * FaceRect(f, -halfW, halfW, -halfH, halfH, 1.002f),
                 glm::vec4(ink * (0.6f + 0.4f * s), 1.0f), m_labelTex[fi]);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDepthFunc(GL_LESS);
    if (cullWas) glEnable(GL_CULL_FACE);
    glViewport(0, 0, clientW, clientH);
}

void Renderer::Destroy()
{
    for (GLuint& t : m_labelTex)
        if (t) { glDeleteTextures(1, &t); t = 0; }
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_program) glDeleteProgram(m_program);
    m_vbo = m_vao = m_program = 0;
    m_ready = m_failed = m_hasLabels = false;
}

// =============================================================================
// wx glue
// =============================================================================
#ifndef NAVCUBE_NO_WX
void BuildLabels(Renderer& r)
{
    // Rendered white-on-black with the UI font; the red channel is the
    // anti-aliased coverage. One font size for all six (fitted to the widest)
    // so the faces match.
    const int W = 256, H = 64;
    wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    font.SetWeight(wxFONTWEIGHT_BOLD);
    font.SetPixelSize(wxSize(0, 60));

    wxBitmap bmp(W, H, 24);
    wxMemoryDC dc(bmp);
    for (int tries = 0; tries < 8; ++tries)
    {
        dc.SetFont(font);
        int widest = 0;
        for (const char* l : kLabels)
            widest = std::max(widest, dc.GetTextExtent(wxString(l)).x);
        if (widest <= W - 8) break;
        font.SetPixelSize(wxSize(0, std::max(10, font.GetPixelSize().y * (W - 8) / std::max(1, widest))));
    }

    std::vector<unsigned char> cov(size_t(W) * H);
    for (int fi = 0; fi < 6; ++fi)
    {
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        dc.SetFont(font);
        dc.SetTextForeground(*wxWHITE);
        const wxString text(kLabels[fi]);
        const wxSize ext = dc.GetTextExtent(text);
        dc.DrawText(text, (W - ext.x) / 2, (H - ext.y) / 2);
        dc.SelectObject(wxNullBitmap);
        const wxImage img = bmp.ConvertToImage();
        const unsigned char* px = img.GetData();
        for (int i = 0; i < W * H; ++i) cov[i] = px[3 * i];
        r.SetLabel(fi, W, H, cov.data());
        dc.SelectObject(bmp);
    }
    dc.SelectObject(wxNullBitmap);
}

bool HandleMouse(Controller& c, wxMouseEvent& evt, wxWindow* canvas, OrbitCamera& cam)
{
    const wxSize sz = canvas->GetClientSize();
    const Rect r = Placement(sz.x, sz.y);
    const wxPoint p = evt.GetPosition();
    const glm::mat4 view = cam.View();

    if (evt.LeftDown() || evt.LeftDClick())
    {
        if (!c.MouseDown(view, r, p.x, p.y)) return false;
        if (!canvas->HasCapture()) canvas->CaptureMouse();
        canvas->Refresh(false);
        return true;
    }
    if (evt.LeftUp())
    {
        if (!c.Pressed()) return false;
        if (canvas->HasCapture()) canvas->ReleaseMouse();
        c.MouseUp(view, r, p.x, p.y, cam, wxGetLocalTimeMillis().GetValue());
        canvas->Refresh(false);
        return true;
    }
    if (evt.Dragging() && c.Pressed())
    {
        if (evt.LeftIsDown()) c.MouseDrag(p.x, p.y, cam);
        canvas->Refresh(false);
        return true;
    }
    if (evt.Moving() || evt.Leaving())
    {
        const bool inside = !evt.Leaving() && r.Contains(p.x, p.y);
        const bool changed = inside ? c.Hover(view, r, p.x, p.y)
                                    : (c.Highlight() != glm::ivec3(0) ? (c.ClearHover(), true) : false);
        if (changed)
        {
            const glm::ivec3 h = c.Highlight();
            if (h != glm::ivec3(0)) canvas->SetToolTip(wxString(DirectionName(h)));
            else                    canvas->UnsetToolTip();
            canvas->Refresh(false);
        }
        return inside;
    }
    return false;
}

void AnimateFrame(Controller& c, wxWindow* canvas, OrbitCamera& cam)
{
    if (!c.Animating()) return;
    if (c.Animate(cam, wxGetLocalTimeMillis().GetValue()))
        canvas->CallAfter([canvas] { canvas->Refresh(false); });
}
#endif
}
