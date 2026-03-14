// GridRenderer.cpp
#include "GridRenderer.h"

#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <string>

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

        out vec3 vWorld;

        void main() {
            vWorld = aPos;
            gl_Position = uProj * uView * vec4(aPos, 1.0);
        }
    )GLSL";

    const char* fsSrc = R"GLSL(
        #version 330 core
        in vec3 vWorld;
        out vec4 FragColor;

        uniform float uMinorStep;    // 10mm
        uniform float uMajorStep;    // 50mm (or 100mm)
        uniform vec2  uHalfExtents;  // (100,100) for 200x200

        float lineAA(float coord, float step)
        {
            float x = coord / step;
            float d = abs(fract(x - 0.5) - 0.5);
            float w = fwidth(x);
            return 1.0 - smoothstep(0.0, w, d);
        }

        void main()
        {
            // Clip to bounds on XZ plane
            if (abs(vWorld.x) > uHalfExtents.x || abs(vWorld.z) > uHalfExtents.y)
                discard;

            float minor = max(lineAA(vWorld.x, uMinorStep),
                              lineAA(vWorld.z, uMinorStep));

            float major = max(lineAA(vWorld.x, uMajorStep),
                              lineAA(vWorld.z, uMajorStep));

            float intensity = minor * 0.25 + major * 0.75;

            // Subtle edge fade (optional)
            //float edgeX = 1.0 - smoothstep(uHalfExtents.x * 0.95, uHalfExtents.x, abs(vWorld.x));
            //float edgeZ = 1.0 - smoothstep(uHalfExtents.y * 0.95, uHalfExtents.y, abs(vWorld.z));
            //float edgeFade = min(edgeX, edgeZ);

            //vec3 color = vec3(intensity) * edgeFade;
            vec3 color = vec3(intensity);
            FragColor = vec4(color, intensity);
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
    m_locMinor = glGetUniformLocation(m_program, "uMinorStep");
    m_locMajor = glGetUniformLocation(m_program, "uMajorStep");
    m_locHalfExtents = glGetUniformLocation(m_program, "uHalfExtents");

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

void GridRenderer::RebuildGeometryIfReady()
{
    // Only rebuild if buffers exist
    if (!m_vao || !m_vbo || !m_ebo)
        return;

    const float hx = m_sizeX * 0.5f;
    const float hz = m_sizeZ * 0.5f;

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

    glUniform1f(m_locMinor, m_minorStep);
    glUniform1f(m_locMajor, m_majorStep);
    glUniform2f(m_locHalfExtents, hx, hz);

    // Avoid z-fighting with models on the plane
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);

    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)0);
    glBindVertexArray(0);

    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_BLEND);
}
