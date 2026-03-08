// GridRenderer.h
#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

class GridRenderer
{
public:
    GridRenderer() = default;
    ~GridRenderer() = default;

    // Call once after GLAD is loaded and a context is current
    bool Init();

    // Call when shutting down (or before recreating GL context)
    void Destroy();

    // Draw the bounded grid. Provide view/proj.
    void Draw(const glm::mat4& view, const glm::mat4& proj);

    bool IsReady() const { return m_ready; }

    // Units assume your world is in millimeters.
    void SetSizeMM(float sizeX, float sizeZ);     // e.g. 200,200
    void SetStepsMM(float minor, float major);    // e.g. 10,50

private:
    void RebuildGeometryIfReady();

private:
    bool m_ready = false;

    // Settings (mm)
    float m_sizeX = 200.0f;
    float m_sizeZ = 200.0f;
    float m_minorStep = 10.0f;
    float m_majorStep = 50.0f;

    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_ebo = 0;

    // uniform locations
    GLint m_locView = -1;
    GLint m_locProj = -1;
    GLint m_locMinor = -1;
    GLint m_locMajor = -1;
    GLint m_locHalfExtents = -1;
};
