#pragma once
#include <cstdint>

// Settings applied to mesh-format imports (STL, OBJ). STEP imports ignore
// these — STEP already has its own tessellation control via deflection.
//
// The Quality presets map to target triangle counts; "Off" disables
// simplification entirely (the mesh is imported at its full triangle count).
// Values are persisted via AppConfig so they survive between sessions.
namespace MeshImportSettings
{
    enum class Quality : int
    {
        Off    = 0,   // no decimation
        Draft  = 1,   // ~2,000 triangles
        Normal = 2,   // ~10,000 triangles  (default)
        High   = 3    // ~50,000 triangles
    };

    // Returns the cached current quality. First call initializes from AppConfig.
    Quality  GetQuality();

    // Sets and persists the current quality.
    void     SetQuality(Quality q);

    // Returns the target triangle count that the decimator should aim for.
    // Returns 0 for Quality::Off (= "don't simplify").
    uint32_t TargetTriangleCount(Quality q);
}

