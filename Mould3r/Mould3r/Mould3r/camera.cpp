#include "Camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

OrbitCamera::OrbitCamera() = default;

void OrbitCamera::SetAspect(float aspect)
{
    m_aspect = std::max(0.001f, aspect);
}

void OrbitCamera::SetClip(float nearPlane, float farPlane)
{
    m_near = std::max(0.0001f, nearPlane);
    m_far = std::max(m_near + 0.001f, farPlane);
}

void OrbitCamera::SetFovDegrees(float fovDeg)
{
    m_fovDeg = std::clamp(fovDeg, 10.0f, 90.0f);
}

void OrbitCamera::SetTarget(const glm::vec3& target)
{
    m_target = target;
}

void OrbitCamera::SetDistance(float distance)
{
    m_distance = distance;
    ClampDistance();
}

void OrbitCamera::SetYawPitchDegrees(float yawDeg, float pitchDeg)
{
    m_yawDeg = yawDeg;
    m_pitchDeg = pitchDeg;
    ClampPitch();
}

void OrbitCamera::SetOrbitSensitivity(float degPerPixel)
{
    m_orbitSensDegPerPx = std::max(0.0001f, degPerPixel);
}

void OrbitCamera::SetPanSensitivity(float unitsPerPixelAtDist1)
{
    m_panSensUnitsPerPxAtDist1 = std::max(0.000001f, unitsPerPixelAtDist1);
}

void OrbitCamera::SetDollySensitivity(float dollyStrength)
{
    m_dollySens = std::max(0.0001f, dollyStrength);
}

glm::vec3 OrbitCamera::Position() const
{
    const float yaw = glm::radians(m_yawDeg);
    const float pitch = glm::radians(m_pitchDeg);

    // Direction from target to camera (spherical coords)
    glm::vec3 dir = { 0.0f,0.0f,0.0f };//Initialize to zeros
    dir.x = std::cos(pitch) * std::cos(yaw);
    dir.y = std::sin(pitch);
    dir.z = std::cos(pitch) * std::sin(yaw);

    dir = glm::normalize(dir);
    return m_target - dir * m_distance; // camera position
}

glm::vec3 OrbitCamera::Forward() const
{
    return glm::normalize(m_target - Position());
}

glm::vec3 OrbitCamera::Right() const
{
    return glm::normalize(glm::cross(Forward(), m_worldUp));
}

glm::vec3 OrbitCamera::Up() const
{
    return glm::normalize(glm::cross(Right(), Forward()));
}

glm::mat4 OrbitCamera::View() const
{
    return glm::lookAt(Position(), m_target, Up());
}

glm::mat4 OrbitCamera::RotationOnlyView() const
{
    glm::mat4 v = View();
    return glm::mat4(glm::mat3(v)); // strips translation
}

glm::mat4 OrbitCamera::Projection() const
{
    return glm::perspective(glm::radians(m_fovDeg), m_aspect, m_near, m_far);
}

void OrbitCamera::Orbit(float dx, float dy)
{
    m_yawDeg += dx * m_orbitSensDegPerPx;
    m_pitchDeg += dy * m_orbitSensDegPerPx;
    ClampPitch();
}

void OrbitCamera::Pan(float dx, float dy)
{
    // Pan should feel consistent at different zoom levels
    const float scale = m_panSensUnitsPerPxAtDist1 * m_distance;

    // Screen-space convention: +dx = move mouse right, so scene pans right
    // Typical CAD feel: dragging right moves the target left (camera plane), so use -Right * dx
    m_target += (-Right() * dx + Up() * dy) * scale;
}

void OrbitCamera::Dolly(float delta)
{
    // Exponential dolly feels CAD-like and avoids clipping through target
    const float factor = std::exp(-delta * m_dollySens);
    m_distance *= factor;
    ClampDistance();
}

void OrbitCamera::FrameSphere(const glm::vec3& center, float radius)
{
    m_target = center;
    radius = std::max(radius, 0.0001f);

    const float fovRad = glm::radians(m_fovDeg);
    const float dist = radius / std::sin(fovRad * 0.5f);

    m_distance = dist * 1.1f; // padding
    ClampDistance();
}

void OrbitCamera::ClampPitch()
{
    // Avoid looking straight up/down to prevent flips
    m_pitchDeg = std::clamp(m_pitchDeg, -89.0f, 89.0f);
}

void OrbitCamera::ClampDistance()
{
    m_distance = std::clamp(m_distance, m_minDistance, m_maxDistance);
}
