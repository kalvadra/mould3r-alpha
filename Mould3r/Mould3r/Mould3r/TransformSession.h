#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

// =============================================================================
// TransformSession — the maths behind the Move / Rotate / Scale tools, shared
// by the Prepare canvas (GLCanvas) and the Fixture Editor (FixtureCanvas).
//
// A tool session snapshots the selection's poses when it starts. Everything
// after that is "snapshot + delta": the side panel's typed values (and, next,
// the on-canvas handles) only ever edit the Delta, and each update re-derives
// every pose from its snapshot, so the two input methods can never drift apart
// and Cancel is an exact restore.
//
// Rotation and scale act about one PIVOT for the whole selection — the centre
// of its world bounding box at session start — so a group turns / scales as a
// single rigid piece (positions move about the pivot too). Rotation is about
// the WORLD axes; the three angles apply in a fixed order: Z, then X, then Y
// (R = Ry * Rx * Rz — the same YXZ order every body stores its own pose in).
// Y is outermost on purpose: rotating about the vertical axis — the common
// case on a parting plane — is always a plain, full-range sum in the Y field,
// and the Rotate rings can always be written back into the fields exactly
// (see AddWorldRotation).
//
// Pure glm, no wx / GL, header-only.
// =============================================================================
namespace Xform
{
    enum class Tool { None, Move, Rotate, Scale };

    // A body's pose, in the YXZ Euler convention every SceneObject /
    // FixtureMesh uses: model = T * Ry(yaw) * Rx(pitch) * Rz(roll) * S(scale).
    struct Pose
    {
        glm::vec3 pos{ 0.0f };
        float     yawDeg = 0.0f;
        float     pitchDeg = 0.0f;
        float     rollDeg = 0.0f;
        float     scale = 1.0f;
    };

    // The change a session is applying. Identity by default.
    struct Delta
    {
        glm::vec3 move{ 0.0f };     // mm, world axes
        glm::vec3 rotDeg{ 0.0f };   // degrees about world X, Y, Z (applied Z, then X, then Y)
        float     scale = 1.0f;     // uniform factor

        bool IsIdentity() const
        {
            return move == glm::vec3(0.0f) && rotDeg == glm::vec3(0.0f) && scale == 1.0f;
        }
    };

    constexpr float kMinScale = 0.001f;   // same floor as the old ApplyScale

    inline glm::mat3 RotYXZ(float yawDeg, float pitchDeg, float rollDeg)
    {
        const glm::mat4 RY = glm::rotate(glm::mat4(1.0f), glm::radians(yawDeg),   glm::vec3(0, 1, 0));
        const glm::mat4 RX = glm::rotate(glm::mat4(1.0f), glm::radians(pitchDeg), glm::vec3(1, 0, 0));
        const glm::mat4 RZ = glm::rotate(glm::mat4(1.0f), glm::radians(rollDeg),  glm::vec3(0, 0, 1));
        return glm::mat3(RY * RX * RZ);
    }

    // Inverse of RotYXZ — a direct copy of GLCanvas::DecomposeYXZ (same
    // gimbal-lock convention: at pitch = +-90 roll is pinned to 0).
    inline void DecomposeYXZ(const glm::mat3& R, float& yawDeg, float& pitchDeg, float& rollDeg)
    {
        constexpr float kGimbalEps = 1.0e-5f;
        const float sinPitch = -R[2][1];
        const float pitch = std::asin(glm::clamp(sinPitch, -1.0f, 1.0f));
        const float cosPitch = std::cos(pitch);
        float yaw, roll;
        if (std::abs(cosPitch) > kGimbalEps)
        {
            yaw = std::atan2(R[2][0], R[2][2]);
            roll = std::atan2(R[0][1], R[1][1]);
        }
        else
        {
            roll = 0.0f;
            yaw = std::atan2(-R[0][2], R[0][0]);
        }
        yawDeg = glm::degrees(yaw);
        pitchDeg = glm::degrees(pitch);
        rollDeg = glm::degrees(roll);
    }

    // World-axis rotation for the three angles: Z first, then X, then Y.
    inline glm::mat3 DeltaRotation(const glm::vec3& rotDeg)
    {
        return RotYXZ(rotDeg.y, rotDeg.x, rotDeg.z);   // Ry * Rx * Rz
    }

    // An angle with whole turns taken off, keeping its sign: 370 -> 10,
    // -365 -> -5, +-360 -> 0. A ring drag's readout and sweep wedge reset this
    // way each time they pass a full turn (the pose is the same either way).
    inline float WrapTurn(float deg)
    {
        const float r = std::fmod(deg, 360.0f);
        return (r == 0.0f) ? 0.0f : r;   // also turns -0 into 0
    }
    inline glm::vec3 WrapTurns(const glm::vec3& deg)
    {
        return glm::vec3(WrapTurn(deg.x), WrapTurn(deg.y), WrapTurn(deg.z));
    }

    // The angles after a further rotation of `deg` about world axis `axis`
    // (0 = X, 1 = Y, 2 = Z) on top of `rotDeg` — what a Rotate ring drag
    // writes into the fields. Exact in every case (DeltaRotation of the result
    // is R_axis(deg) * DeltaRotation(rotDeg)):
    //   * Y is the outermost angle, so a Y turn is always just added;
    //   * an X or Z turn when that axis is the only one in use is just added
    //     too — so single-axis ring drags read naturally (0 -> 120 -> 400);
    //   * otherwise the combined rotation is re-expressed in the three fields
    //     (the numbers can change on every axis, but they reproduce exactly
    //     what the ring did, so editing any field afterwards behaves).
    inline glm::vec3 AddWorldRotation(const glm::vec3& rotDeg, int axis, float deg)
    {
        glm::vec3 out = rotDeg;
        if (axis == 1) { out.y += deg; return out; }
        if (axis == 0 && rotDeg.y == 0.0f && rotDeg.z == 0.0f) { out.x += deg; return out; }
        if (axis == 2 && rotDeg.x == 0.0f && rotDeg.y == 0.0f) { out.z += deg; return out; }

        glm::vec3 ax(0.0f);
        ax[axis] = 1.0f;
        const glm::mat3 R = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(deg), ax))
                          * DeltaRotation(rotDeg);
        DecomposeYXZ(R, out.y, out.x, out.z);   // yaw, pitch, roll
        return out;
    }

    // The pose a snapshot takes under `d` about `pivot`:
    //     pos' = pivot + R * (f * (pos - pivot)) + move
    //     rot' = R * rot,  scale' = scale * f
    // With no rotation the snapshot's Euler angles are kept verbatim (no
    // decompose round-trip), so a pure move / scale never rewrites them.
    inline Pose ApplyDelta(const Pose& s, const glm::vec3& pivot, const Delta& d)
    {
        Pose o = s;
        o.scale = std::max(kMinScale, s.scale * d.scale);
        // The factor actually applied (the floor can bite), so positions and
        // sizes stay in step.
        const float f = (s.scale > 0.0f) ? o.scale / s.scale : 1.0f;

        const bool rotates = d.rotDeg != glm::vec3(0.0f);
        const glm::mat3 R = rotates ? DeltaRotation(d.rotDeg) : glm::mat3(1.0f);

        o.pos = pivot + R * ((s.pos - pivot) * f) + d.move;
        if (rotates)
            DecomposeYXZ(R * RotYXZ(s.yawDeg, s.pitchDeg, s.rollDeg),
                o.yawDeg, o.pitchDeg, o.rollDeg);
        return o;
    }

    // Grow a world-space AABB by the 8 corners of a local box under `model`.
    inline void ExpandByBox(const glm::mat4& model, const glm::vec3& lo, const glm::vec3& hi,
                            glm::vec3& wlo, glm::vec3& whi)
    {
        for (int c = 0; c < 8; ++c)
        {
            const glm::vec3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
            const glm::vec3 w(model * glm::vec4(p, 1.0f));
            wlo = glm::min(wlo, w);
            whi = glm::max(whi, w);
        }
    }
}
