#pragma once
#include <vector>
#include <cstdint>

struct CreaseSplitResult
{
    std::vector<float>    posNorm;   // [px,py,pz,nx,ny,nz] repeated
    std::vector<uint32_t> indices;   // remapped triangle indices
};

// pos3: [px,py,pz] repeated
CreaseSplitResult SplitByCreaseAngle_Pos3(
    const std::vector<float>& pos3,
    const std::vector<uint32_t>& indices,
    float creaseAngleDeg
);
