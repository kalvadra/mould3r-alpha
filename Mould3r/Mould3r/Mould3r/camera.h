#pragma once
#include <glm/glm.hpp>

class OrbitCamera
{
public:
    OrbitCamera();

    // --- Matrices ---
    glm::mat4 View() const;
    glm::mat4 Projection() const;
    glm::mat4 RotationOnlyView() const;

    // --- Configuration ---
    void SetAspect(float aspect);
    void SetClip(float nearPlane, float farPlane);
    void SetFovDegrees(float fovDeg);

    void SetTarget(const glm::vec3& target);
    void SetDistance(float distance);
    void SetYawPitchDegrees(float yawDeg, float pitchDeg);

    // --- Interaction ---
    void Orbit(float dx, float dy);
    void Pan(float dx, float dy);
    void Dolly(float delta);

    // --- Getters ---
    glm::vec3 Position() const;
    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;

    float GetDistance() const { return m_distance; }   // used by GLCanvas translate

    // Zoom to fit
    void FrameSphere(const glm::vec3& center, float radius);

    // Sensitivities
    void SetOrbitSensitivity(float degPerPixel);
    void SetPanSensitivity(float unitsPerPixelAtDist1);
    void SetDollySensitivity(float dollyStrength);

private:
    glm::vec3 m_target{ 0.0f, 0.0f, 0.0f };
    float m_distance = 350.0f;

    float m_yawDeg = 45.0f;
    float m_pitchDeg = -35.0f;

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
