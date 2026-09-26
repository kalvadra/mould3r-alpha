#pragma once
// ===========================================================================
// FeedNetwork — the 1D nodal representation of the feed system for the
// Hele-Shaw flow analysis.
//
// Rather than meshing the sprue / runners / gates as solid geometry, the feed
// system is a graph: each feature path contributes nodes at its ends (and at
// any junction where another feature attaches part-way along it), and an edge
// between consecutive nodes carries the path LENGTH and a simplified CROSS
// SECTION. Per the modelling rule, the feature's specified section is assumed
// over the whole length of that edge, which fixes its hydraulic resistance.
//
// Each moulded object is (for now) a single PART node: the feed system joins
// it on one side (gate origins / a direct-injection sprue) and the vents leave
// it on the other. The part's own flow field is a later step (a mid-surface /
// dual-domain mesh of the cavity rebuilt from the source geometry); the part
// node is where that mesh will plug in.
//
// Pure data + compute — no wx, no GL, no app types (like FlowMesh /
// FlowSolver). Built by GLCanvas::BuildFeedNetwork at Generate Mould and handed
// to PreviewPanel through ShotPreviewInput.
//
// Units: millimetres throughout. With Q in mm^3/s, L in mm, G in mm^4 and eta
// in Pa.s, the pipe law  dP = eta * L * Q / G  comes out directly in Pa, so the
// network can be solved in mm without SI conversion.
// ===========================================================================

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <functional>

namespace Flow
{
    enum class FeedNodeKind
    {
        SprueInlet,      // machine nozzle — the injection source
        SprueParting,    // sprue crosses the parting plane (runner/gate feed)
        SprueEnd,        // sprue terminus (on the part for direct injection)
        RunnerEnd,       // end of a runner path
        Junction,        // a gate/sub-runner attaches part-way along a runner
        GateTransition,  // gate frustum -> sub-runner change of section
        GateOrigin,      // gate mouth on the cavity (melt enters the part here)
        Part,            // a moulded object, lumped as one node for now
        VentStart,       // vent mouth on the cavity (air leaves the part here)
        VentOutlet       // vent exit to atmosphere
    };

    enum class FeedEdgeKind
    {
        Sprue,
        Runner,
        SubRunner,
        Gate,        // the gate frustum (orifice section)
        Vent,        // air channel — never carries melt
        CavityIn,    // feed -> part link (gate mouth / direct sprue into the part)
        CavityOut    // part -> vent link
    };

    enum class SectionShape { None, Circle, Rect };

    // Simplified cross-section of an edge. `hydraulicDiameterMm` (4A/P) is the
    // single "thickness" figure for the section; `conductanceMm4` is the shape
    // factor G in the laminar law  Q = G * dP / (eta * L).
    struct FeedSection
    {
        SectionShape shape = SectionShape::None;
        float diameterMm = 0.0f;          // Circle
        float widthMm    = 0.0f;          // Rect (in-plane)
        float heightMm   = 0.0f;          // Rect (depth)
        float areaMm2    = 0.0f;
        float perimeterMm = 0.0f;
        float hydraulicDiameterMm = 0.0f;
        double conductanceMm4 = 0.0;
    };

    FeedSection MakeCircleSection(float diameterMm);
    FeedSection MakeRectSection(float widthMm, float heightMm);

    // Apparent (Newtonian) wall shear rate for a volumetric flow through the
    // section: 32Q/(pi D^3) for a circle, 6Q/(w h^2) (h = the thinner side) for
    // a rectangle. Used to evaluate a shear-thinning viscosity per edge. [1/s]
    double ApparentWallShearRate(const FeedSection& s, double flowMm3s);

    struct FeedNode
    {
        glm::vec3    pos{ 0.0f };
        FeedNodeKind kind = FeedNodeKind::Junction;
        int          objectIndex = -1;   // Part / GateOrigin / VentStart: owning object
        std::string  label;
    };

    struct FeedEdge
    {
        int          a = -1, b = -1;     // node indices (a = upstream as built)
        FeedEdgeKind kind = FeedEdgeKind::Runner;
        float        lengthMm = 0.0f;
        FeedSection  section;
        int          featureIndex = -1;  // index into the source feature list
        std::string  label;

        bool carriesMelt() const
        {
            return kind == FeedEdgeKind::Sprue || kind == FeedEdgeKind::Runner ||
                   kind == FeedEdgeKind::SubRunner || kind == FeedEdgeKind::Gate;
        }
        // Geometric resistance L / G [mm^-3]; times eta gives Pa.s/mm^3.
        double geometricResistance() const
        {
            return (section.conductanceMm4 > 0.0) ? (double)lengthMm / section.conductanceMm4 : 0.0;
        }
    };

    struct FeedNetwork
    {
        std::vector<FeedNode> nodes;
        std::vector<FeedEdge> edges;
        int inletNode = -1;
        std::vector<std::string> warnings;

        bool empty() const { return nodes.empty(); }

        // Add a node, or reuse an existing one within `mergeTolMm` (> 0) so
        // coincident feature ends become one junction. A reused node keeps its
        // original kind. Returns the node index.
        int addNode(const glm::vec3& pos, FeedNodeKind kind, int objectIndex = -1,
                    float mergeTolMm = 0.0f, const std::string& label = {});

        // Add an edge; returns its index (or -1 for a self-loop / bad index).
        int addEdge(int a, int b, FeedEdgeKind kind, float lengthMm,
                    const FeedSection& section, int featureIndex = -1,
                    const std::string& label = {});

        int countNodes(FeedNodeKind k) const;
        int countEdges(FeedEdgeKind k) const;
    };

    // Arc length of a polyline (mm).
    float PolylineLengthMm(const std::vector<glm::vec3>& pts);

    // Steady isothermal feed-system solve: inject `flowRateMm3s` at the inlet,
    // hold every cavity-entry node (a node with a CavityIn link to a part) at
    // 0 gauge — the melt reaching the cavity — and solve the melt-carrying
    // edges. `viscosity(gammaDot)` returns eta [Pa.s] at a wall shear rate
    // [1/s]; each edge's viscosity is iterated (Picard, log-relaxed) to its own
    // shear rate, so a shear-thinning melt gets a realistic pressure drop and
    // gate split. Vents and part nodes take no part in the melt solve.
    struct FeedSolveResult
    {
        bool ok = false;
        std::string message;
        int  iterations = 0;
        bool converged  = false;

        std::vector<float> nodePressureMPa;  // per node; <0 = not in the melt solve
        std::vector<float> edgeFlowMm3s;     // per edge, signed a->b (0 for non-melt)
        std::vector<float> edgeShearRate;    // per edge [1/s]
        std::vector<float> edgeViscosityPaS; // per edge
        std::vector<float> edgeDeltaPMPa;    // per edge |dP|

        float inletPressureMPa = 0.0f;       // total feed pressure drop
        std::vector<int>   entryNodes;       // cavity-entry nodes, in node order
        std::vector<float> entryFlowMm3s;    // melt delivered through each
        std::vector<int>   entryPartNode;    // the part node each entry feeds
    };

    FeedSolveResult SolveFeedNetwork(const FeedNetwork& net, double flowRateMm3s,
                                     const std::function<double(double)>& viscosity,
                                     int maxIterations = 200, double tol = 1.0e-6);

    const char* FeedNodeKindName(FeedNodeKind k);
    const char* FeedEdgeKindName(FeedEdgeKind k);

} // namespace Flow
