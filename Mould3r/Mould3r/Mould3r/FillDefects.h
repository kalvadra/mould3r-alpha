#pragma once
// ===========================================================================
// FillDefects — weld / meld lines, air traps and last-to-fill points read from
// a finished coupled fill (CoupledFillResult) on the part midplanes. Pure
// post-processing of the fill-time field; no wx, no GL.
//
// Weld and meld lines. Each midplane triangle's flow direction is its fill-
// time gradient. Across an interior edge, two triangles whose flows both run
// INTO the edge (converging, not diverging as around a gate) and meet at a
// large angle mark where two melt fronts joined. The meeting angle is the
// angle between the two flow directions: >= weldAngleDeg (default 135, near
// head-on) is weld-type — the weakest and most visible; between
// weldMinAngleDeg (75) and that, meld-type (fronts merging while still
// flowing; stronger). A line is a weld line when at least a third of its
// length meets weld-type (fronts from point gates meet head-on in the middle
// and at an angle towards the ends), else a meld line. Limitation: only where
// fronts actually meet is traced — once two streams have merged they flow in
// parallel, so the meld line they leave trailing downstream is not followed. Edges join into lines through shared nodes; each line
// reports its length, when it formed, its mean meeting angle and (thermal) the
// coldest melt-front temperature along it — colder fronts weld worse.
//
// Air traps. Air can leave the cavity at the part outline (parting line) and
// through vents. Walking the fill backwards in time (nodes in decreasing fill
// time, joined with union-find), an unfilled region that is cut off from the
// outline and from every vent while it still holds air is a sealed pocket:
// the air in it is trapped and compressed where it fills last (burn marks,
// short shots, voids). Each pocket reports its air volume when it sealed, when
// that was, and where it ends up (its last node to fill). Only outermost
// pockets are reported (a pocket that later splits is one trap).
//
// Last to fill. The latest-filling points (local fill-time maxima within
// lastFillRadiusMm) that are not in a pocket: where the air leaves last, i.e.
// where vents belong. Each says whether a vent mouth is within reach.
// ===========================================================================

#include "CoupledFill.h"
#include "FeedNetwork.h"
#include "Midplane.h"

#include <glm/glm.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Flow
{
    struct FillDefectParams
    {
        double weldMinAngleDeg  = 75.0;   // below: ordinary flow, not a line
        double weldAngleDeg     = 135.0;  // at or above: weld line; below: meld line
        double minLineLengthMm  = 2.0;    // shorter lines are dropped (mesh noise)
        double ventCaptureMm    = 3.0;    // a vent mouth vents nodes within this (plan) + half its width
        double minTrapVolumeMm3 = 0.5;    // smaller pockets are dropped
        int    minTrapNodes     = 3;      // single-node "pockets" are fill-time noise
        double lastFillRadiusMm = 5.0;    // a last-to-fill point is the latest within this radius
        int    maxLastFillPerPart = 6;
    };

    struct WeldLine
    {
        int   part = -1;                              // index into the midplanes / CoupledFillResult::parts
        bool  weld = true;                            // false: meld line
        std::vector<std::pair<int, int>> edges;       // midplane node pairs
        float lengthMm = 0.0f;
        float meanAngleDeg = 0.0f;                    // length-weighted meeting angle
        float maxAngleDeg = 0.0f;
        float weldFraction = 0.0f;                    // share of the length meeting at >= weldAngleDeg
        float formedS = 0.0f;                         // when the fronts first met on it
        float minFrontTempC = 0.0f;                   // thermal: coldest front along it (else 0)
        bool  hasFrontTemp = false;
        glm::vec3 anchor{ 0.0f };                     // a point on the line (labels / markers)
    };

    struct AirTrap
    {
        int   part = -1;
        int   node = -1;                              // midplane node where it fills last
        glm::vec3 pos{ 0.0f };
        float volumeMm3 = 0.0f;                       // air cut off when it sealed
        float sealedS = 0.0f;                         // when it was cut off
        float filledS = 0.0f;                         // when its last node filled (< 0: never — short shot)
        int   nodeCount = 0;
    };

    struct LastFillPoint
    {
        int   part = -1;
        int   node = -1;
        glm::vec3 pos{ 0.0f };
        float timeS = 0.0f;
        bool  onOutline = false;                      // on the part outline (parting line)
        bool  vented = false;                         // a vent mouth reaches it
        float ventDistMm = -1.0f;                     // plan distance to the nearest vent mouth (< 0: no vents)
    };

    struct FillDefects
    {
        std::vector<WeldLine>      lines;             // welds and melds, longest first per part
        std::vector<AirTrap>       traps;             // largest first
        std::vector<LastFillPoint> lastFill;          // latest first
        int weldCount = 0, meldCount = 0;
    };

    FillDefects DetectFillDefects(const FeedNetwork& net,
                                  const std::vector<MidplaneMesh>& parts,
                                  const CoupledFillResult& fill,
                                  const FillDefectParams& params = FillDefectParams());

} // namespace Flow
