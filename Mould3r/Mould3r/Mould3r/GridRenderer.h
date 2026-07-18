// GridRenderer.h
#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include "GridSettings.h"   // GridShape / GridSettings — drives the grid layout

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

    // Apply a full grid configuration (shape / size / spacing / major divisions)
    // authored from the Grid menu. This is the primary entry point; it derives
    // the minor/major steps and rebuilds geometry as needed.
    void ApplySettings(const GridSettings& s);

private:
    void RebuildGeometryIfReady();

private:
    bool m_ready = false;

    // Settings (mm)
    GridShape m_shape = GridShape::Rectangular;
    float m_sizeX = 200.0f;      // rectangular full extent X
    float m_sizeZ = 200.0f;      // rectangular full extent Z
    float m_radius = 100.0f;     // circular radius
    float m_minorStep = 10.0f;
    float m_majorStep = 50.0f;

    // Circular (polar) radial spokes. The minor spoke count is authored from
    // the Grid dialog; the 4 cardinal axes are always the major spokes.
    int m_spokes = 12;                                // minor radial divisions
    static constexpr int kCircularMajorSpokes = 4;    // cardinal axes (90 deg)

    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_ebo = 0;

    // uniform locations
    GLint m_locView = -1;
    GLint m_locProj = -1;
    GLint m_locYLift = -1;
    GLint m_locMinor = -1;
    GLint m_locMajor = -1;
    GLint m_locHalfExtents = -1;
    GLint m_locShape = -1;
    GLint m_locRadius = -1;
    GLint m_locAngular = -1;
    GLint m_locMajorAngular = -1;
};
