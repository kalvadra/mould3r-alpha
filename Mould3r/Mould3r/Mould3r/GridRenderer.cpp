// GridRenderer.cpp
#include "GridRenderer.h"

#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <string>
#include <cmath>

// pi (avoid relying on M_PI being defined on MSVC without _USE_MATH_DEFINES)
static constexpr float kPi = 3.14159265358979323846f;

// Small lift (mm) applied to the grid quad in +Y so it sits just above the
// y=0 parting plane instead of z-fighting with coplanar mould faces. Tiny
// relative to typical mould dimensions, so it reads as flush-but-visible.
static constexpr float kGridYLift = 0.1f;

// --- Small local shader helpers ---
static GLuint CompileShader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetShaderInfoLog(s, len, &len, log.data());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint LinkProgram(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetProgramInfoLog(p, len, &len, log.data());
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool GridRenderer::Init()
{
    if (m_ready) return true;

    const char* vsSrc = R"GLSL(
        #version 330 core
        layout(location=0) in vec3 aPos;

        uniform mat4 uView;
        uniform mat4 uProj;
        uniform float uYLift;   // small lift above y=0 to clear the parting plane

        out vec3 vWorld;

        void main() {
            vWorld = aPos;   // line math uses XZ only; the lift doesn't affect it
            vec3 p = aPos + vec3(0.0, uYLift, 0.0);
            gl_Position = uProj * uView * vec4(p, 1.0);
        }
    )GLSL";

    const char* fsSrc = R"GLSL(
        #version 330 core
        in vec3 vWorld;
        out vec4 FragColor;

        uniform int   uShape;            // 0 = rectangular, 1 = circular
        uniform float uMinorStep;        // minor spacing (mm)
        uniform float uMajorStep;        // major spacing (mm)
        uniform vec2  uHalfExtents;      // rectangular half extents (mm)
        uniform float uRadius;           // circular radius (mm)
        uniform float uAngularStep;      // minor spoke spacing (radians)
        uniform float uMajorAngularStep; // major spoke spacing (radians)

        // Line pixel widths. "A point or two" thicker than the old ~1px lines
        // so the grid reads clearly; minor lines stay a touch thinner than
        // major ones. Tunable.
        const float MINOR_PX = 2.0;
        const float MAJOR_PX = 3.0;

        // Anti-aliased line of `widthPx` pixels wherever `coord` is a multiple
        // of `step`. d/w is the distance from the line centre in pixels; the
        // line is full for the inner half-width and fades over ~1px.
        float lineAA(float coord, float step, float widthPx)
        {
            float x = coord / step;
            float d = abs(fract(x - 0.5) - 0.5);
            float w = fwidth(x);
            return 1.0 - smoothstep(widthPx * 0.5 - 0.5,
                                    widthPx * 0.5 + 0.5, d / w);
        }

        // Spoke variant: clamps the derivative so the atan() seam and the
        // converging centre don't smear into a solid line.
        float spokeAA(float ang, float step, float widthPx)
        {
            float x = ang / step;
            float d = abs(fract(x - 0.5) - 0.5);
            float w = min(fwidth(x), 2.0);
            return 1.0 - smoothstep(widthPx * 0.5 - 0.5,
                                    widthPx * 0.5 + 0.5, d / w);
        }

        void main()
        {
            float minor = 0.0;
            float major = 0.0;

            if (uShape == 0)
            {
                // Rectangular: clip to bounds, lines along X and Z.
                if (abs(vWorld.x) > uHalfExtents.x || abs(vWorld.z) > uHalfExtents.y)
                    discard;

                minor = max(lineAA(vWorld.x, uMinorStep, MINOR_PX),
                            lineAA(vWorld.z, uMinorStep, MINOR_PX));
                major = max(lineAA(vWorld.x, uMajorStep, MAJOR_PX),
                            lineAA(vWorld.z, uMajorStep, MAJOR_PX));
            }
            else
            {
                // Circular (polar): concentric rings + radial spokes, clipped
                // to a disc of uRadius.
                float r = length(vWorld.xz);
                if (r > uRadius)
                    discard;

                float ang = atan(vWorld.z, vWorld.x);

                minor = max(lineAA(r, uMinorStep, MINOR_PX),
                            spokeAA(ang, uAngularStep, MINOR_PX));
                major = max(lineAA(r, uMajorStep, MAJOR_PX),
                            spokeAA(ang, uMajorAngularStep, MAJOR_PX));

                // Crisp outer boundary ring at the radius (major weight/width).
                float we = fwidth(r);
                float edge = 1.0 - smoothstep(MAJOR_PX * 0.5 - 0.5,
                                              MAJOR_PX * 0.5 + 0.5,
                                              abs(r - uRadius) / we);
                major = max(major, edge);
            }

            float intensity = minor * 0.25 + major * 0.75;

            // Output premultiplied coverage. Combined with the inversion blend
            // (ONE_MINUS_DST_COLOR / ONE_MINUS_SRC_ALPHA) this resolves to
            // mix(background, 1 - background, intensity): each line pixel
            // becomes the inverse of whatever is behind it — dark over the
            // light mould, light over the dark background.
            FragColor = vec4(vec3(intensity), intensity);
        }
    )GLSL";


    GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) return false;

    m_program = LinkProgram(vs, fs);
    if (!m_program) return false;

    // Create buffers for a 200x200 (default) grid quad on XZ plane
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    RebuildGeometryIfReady();

    // Cache uniform locations
    m_locView = glGetUniformLocation(m_program, "uView");
    m_locProj = glGetUniformLocation(m_program, "uProj");
    m_locYLift = glGetUniformLocation(m_program, "uYLift");
    m_locMinor = glGetUniformLocation(m_program, "uMinorStep");
    m_locMajor = glGetUniformLocation(m_program, "uMajorStep");
    m_locHalfExtents = glGetUniformLocation(m_program, "uHalfExtents");
    m_locShape = glGetUniformLocation(m_program, "uShape");
    m_locRadius = glGetUniformLocation(m_program, "uRadius");
    m_locAngular = glGetUniformLocation(m_program, "uAngularStep");
    m_locMajorAngular = glGetUniformLocation(m_program, "uMajorAngularStep");

    m_ready = true;
    return true;
}

void GridRenderer::Destroy()
{
    if (m_ebo) { glDeleteBuffers(1, &m_ebo); m_ebo = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_program) { glDeleteProgram(m_program); m_program = 0; }
    m_ready = false;
}

void GridRenderer::SetSizeMM(float sizeX, float sizeZ)
{
    m_sizeX = std::max(0.001f, sizeX);
    m_sizeZ = std::max(0.001f, sizeZ);
    RebuildGeometryIfReady();
}

void GridRenderer::SetStepsMM(float minor, float major)
{
    m_minorStep = std::max(0.0001f, minor);
    m_majorStep = std::max(m_minorStep, major);
}

void GridRenderer::ApplySettings(const GridSettings& s)
{
    m_shape = s.shape;
    m_sizeX = std::max(0.001f, s.sizeX);
    m_sizeZ = std::max(0.001f, s.sizeY);   // authored "Y" is the world-Z extent
    m_radius = std::max(0.001f, s.radius);

    m_minorStep = std::max(0.0001f, s.spacing);
    const int every = std::max(1, s.majorEvery);
    m_majorStep = m_minorStep * static_cast<float>(every);

    m_spokes = std::max(1, s.spokes);

    // Quad extents depend on shape, so refresh the geometry.
    RebuildGeometryIfReady();
}

void GridRenderer::RebuildGeometryIfReady()
{
    // Only rebuild if buffers exist
    if (!m_vao || !m_vbo || !m_ebo)
        return;

    // Quad must cover the drawn region: the rectangle for rectangular grids,
    // or the bounding square of the disc (radius each way) for circular ones.
    const float hx = (m_shape == GridShape::Circular) ? m_radius : m_sizeX * 0.5f;
    const float hz = (m_shape == GridShape::Circular) ? m_radius : m_sizeZ * 0.5f;

    float verts[] = {
        -hx, 0.0f, -hz,
         hx, 0.0f, -hz,
         hx, 0.0f,  hz,
        -hx, 0.0f,  hz
    };

    uint32_t idx[] = { 0, 1, 2, 0, 2, 3 };

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * (GLsizei)sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void GridRenderer::Draw(const glm::mat4& view, const glm::mat4& proj)
{
    if (!m_ready) return;

    const float hx = m_sizeX * 0.5f;
    const float hz = m_sizeZ * 0.5f;

    glUseProgram(m_program);

    glUniformMatrix4fv(m_locView, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(m_locProj, 1, GL_FALSE, glm::value_ptr(proj));

    // Lift the grid toward the viewer, not a fixed +Y. A fixed +Y lift only
    // reads as "above the surface" from above; viewed from the -Y side it
    // pushes the grid away from the camera, behind the y=0 parting face, so
    // the mould occludes it. Sign the lift by which side of y=0 the camera is
    // on (camera world position = translation of the inverse view matrix).
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    const float yLift = (camPos.y >= 0.0f) ? kGridYLift : -kGridYLift;
    glUniform1f(m_locYLift, yLift);

    glUniform1i(m_locShape, (m_shape == GridShape::Circular) ? 1 : 0);
    glUniform1f(m_locMinor, m_minorStep);
    glUniform1f(m_locMajor, m_majorStep);
    glUniform2f(m_locHalfExtents, hx, hz);
    glUniform1f(m_locRadius, m_radius);
    glUniform1f(m_locAngular, 2.0f * kPi / static_cast<float>(std::max(1, m_spokes)));
    glUniform1f(m_locMajorAngular, 2.0f * kPi / static_cast<float>(kCircularMajorSpokes));

    // Contrast-adaptive lines: the inversion blend makes each line pixel the
    // inverse of the colour already in the buffer (mix(dst, 1-dst, coverage)),
    // so the grid reads dark over the light mould and light over the dark
    // background. NEGATIVE polygon offset pulls the grid toward the camera so
    // it wins z-fights with the coplanar y=0 parting face (works with the
    // vertex Y-lift to make the grid sit ON the surface rather than behind it);
    // it's still occluded normally by mould bulk well above the plane.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)0);
    glBindVertexArray(0);

    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_BLEND);

    // Restore the conventional alpha blend for any later blended draws.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
