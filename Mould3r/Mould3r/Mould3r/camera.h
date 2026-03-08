#pragma once
#include <glm/glm.hpp>

class OrbitCamera
{
public:
    OrbitCamera();

    // --- Matrices ---
    glm::mat4 View() const;
    glm::mat4 Projection() const;
    glm::mat4 RotationOnlyView() const; // for view-cube overlays

    // --- Configuration ---
    void SetAspect(float aspect);
    void SetClip(float nearPlane, float farPlane);
    void SetFovDegrees(float fovDeg);

    void SetTarget(const glm::vec3& target);
    void SetDistance(float distance);
    void SetYawPitchDegrees(float yawDeg, float pitchDeg);

    // --- Interaction (call from mouse/scroll) ---
    // dx/dy are usually mouse deltas in pixels
    void Orbit(float dx, float dy);
    void Pan(float dx, float dy);
    // delta > 0 should move closer (feel free to invert in caller)
    void Dolly(float delta);

    // --- Convenience ---
    glm::vec3 Position() const;
    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;

    // Useful for "zoom to fit"
    void FrameSphere(const glm::vec3& center, float radius);

    // Sensitivities (tweak to taste)
    void SetOrbitSensitivity(float degPerPixel);
    void SetPanSensitivity(float unitsPerPixelAtDist1);
    void SetDollySensitivity(float dollyStrength);

private:
    glm::vec3 m_target{ 0.0f, 0.0f, 0.0f };
    float m_distance = 5.0f;

    float m_yawDeg = 45.0f;     // rotation around world up
    float m_pitchDeg = -20.0f;  // rotation around camera right

    glm::vec3 m_worldUp{ 0.0f, 1.0f, 0.0f };

    float m_fovDeg = 45.0f;
    float m_aspect = 16.0f / 9.0f;
    float m_near = 0.1f;
    float m_far = 1000.0f;

    float m_orbitSensDegPerPx = 0.15f;
    float m_panSensUnitsPerPxAtDist1 = 0.0025f;
    float m_dollySens = 0.08f;

    float m_minDistance = 0.02f;
    float m_maxDistance = 5000.0f;

private:
    void ClampPitch();
    void ClampDistance();
};
