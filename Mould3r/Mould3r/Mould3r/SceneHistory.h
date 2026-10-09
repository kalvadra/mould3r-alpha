#pragma once
// ===========================================================================
// SceneHistory — undo / redo for the Prepare scene.
//
// The history records STATES, not operations. Mould3r's edits cascade (moving
// a part re-anchors and re-snaps its vents, moving the sprue drags runners and
// re-snaps gates, a Dynamic fixture refits around everything), so an inverse
// per operation would have to know every coupling and would drift silently
// whenever one was missed. Instead the canvas captures the authored scene as
// a SceneState, and undo puts an earlier SceneState back verbatim and rebuilds
// the preview geometry from it.
//
// What a SceneState holds:
//   - the fixture (its halves, kind, Dynamic clearance, injection points);
//   - every imported object and insert, by BODY UID + pose. The heavy body
//     data (CPU mesh, OCC shape, GPU mesh) is never copied: bodies still in
//     the scene are matched by uid, and a body removed from the scene is
//     parked in the canvas's body store (GLCanvas::RetireBody) for as long as
//     any history entry refers to it;
//   - every feature (sprue, runners, gates, vents, ejectors, indexers) copied
//     whole, with its GPU handles zeroed (StripGL);
//   - the object selection, restored with each step but never itself a step.
// Card settings (vent width, sprue diameter, ...) are deliberately NOT part of
// it: they only shape features placed afterwards.
//
// Steps are recorded by observation (MainFrame's idle handler): whenever the
// app is quiet — no mouse button held, no modal dialog, no deferred drag
// pending — the live state is captured and compared with the last recorded
// one; a difference becomes one undo step. So every way of changing the scene
// is covered without wiring each tool, and one user gesture is one step.
//
// MAINTENANCE: a field added to a feature struct, SceneObject's pose or
// InsertFeature must also be added to the comparison in SceneHistory.cpp
// (SameScene), or a change to only that field won't be seen as a step (it
// would fold into the next one). Capture/restore copy feature structs whole,
// so they pick up new feature fields automatically.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "FixtureFile.h"
#include "MouldFeature.h"

namespace History
{
    // A scene body (fixture half or imported object) by identity + pose.
    struct BodyRef
    {
        uint64_t  uid = 0;
        glm::vec3 pos{ 0.0f };
        float     yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
        float     scale = 1.0f;
        bool      mirrorX = false, mirrorZ = false;
    };

    // An insert: its body by uid + the parented placement.
    struct InsertRef
    {
        uint64_t  uid = 0;
        int       parentIndex = -1;
        glm::vec3 localOffset{ 0.0f };
        glm::vec3 localRotDeg{ 0.0f };
        float     localScale = 1.0f;
        int       id = -1;
        glm::mat4 worldMatrix{ 1.0f };
    };

    struct SceneState
    {
        // Fixture
        std::vector<BodyRef>        fixtures;
        FixtureKind                 fixtureKind = FixtureKind::Library;
        glm::vec3                   dynamicClearance{ 10.0f };
        std::vector<InjectionPoint> injectionPoints;
        InjectionPoint              activeInjectionPoint;
        bool                        hasActiveInjectionPoint = false;
        bool                        allowPerimeterInjection = false;
        TopInjection                topInjection = TopInjection::Off;

        // Bodies
        std::vector<BodyRef>   objects;
        std::vector<InsertRef> inserts;

        // Features (GPU handles always zero — see StripGL)
        SprueFeature                sprue;
        std::vector<RunnerFeature>  runners;
        std::vector<GateFeature>    gates;
        std::vector<VentInstance>   vents;
        std::vector<EjectorFeature> ejectors;
        std::vector<IndexerFeature> indexers;

        // Object selection (indices into objects). Restored, never compared.
        std::vector<int> selection;

        void CollectBodyUids(std::unordered_set<uint64_t>& out) const;
    };

    // Zero the GPU handles of a copied feature so the copy can never free or
    // draw the live scene's buffers.
    void StripGL(SolidMesh& m);
    void StripGL(SprueFeature& s);
    void StripGL(VentInstance& v);
    void StripGL(RunnerFeature& r);
    void StripGL(GateFeature& g);
    void StripGL(EjectorFeature& e);
    void StripGL(IndexerFeature& i);

    // True when the two scenes hold the same authored content. Ignores the
    // selection and the derived-at-Generate embedExtension fields. eps = 0
    // compares floats exactly (NaN equals NaN); eps > 0 allows a relative /
    // absolute tolerance. On a difference, *firstDiff (if given) names the
    // first differing field, e.g. "vents[2].path.nodes[1].pos.x".
    bool SameScene(const SceneState& a, const SceneState& b, float eps = 0.0f,
                   std::string* firstDiff = nullptr);

    // A short label for the change from `before` to `after` ("Move",
    // "Place Vent", "Delete Objects", "Change Fixture", ...), for the Edit
    // menu's "Undo <label>".
    std::string DescribeChange(const SceneState& before, const SceneState& after);

    // One history state: the canvas scene plus the frame's fixture definition
    // (needed to save the project and to edit a procedural fixture).
    struct Entry
    {
        SceneState        scene;
        FixtureDefinition fixture;
    };

    class Stack
    {
    public:
        explicit Stack(size_t maxSteps = 100) : m_maxSteps(maxSteps) {}

        // Forget everything; the next Observe sets the new baseline.
        void Reset();
        bool HasBaseline() const { return m_hasBaseline; }

        // Feed the live state at a quiet moment. Records a step (and clears
        // the redo stack) when it differs from the current state; otherwise
        // just refreshes the current selection. Returns true when a step was
        // recorded.
        bool Observe(Entry now);

        bool CanUndo() const { return m_hasBaseline && !m_undo.empty(); }
        bool CanRedo() const { return m_hasBaseline && !m_redo.empty(); }
        const std::string& UndoLabel() const;
        const std::string& RedoLabel() const;

        // Undo: restore UndoTarget() into the scene, capture the live result
        // and pass it to CompleteUndo (the live capture, not the target,
        // becomes current, so a restore that isn't bit-exact can't trigger a
        // spurious new step that would wipe the redo stack). Redo likewise.
        const Entry& UndoTarget() const { return m_undo.back().state; }
        const Entry& RedoTarget() const { return m_redo.back().state; }
        void CompleteUndo(Entry live);
        void CompleteRedo(Entry live);

        const Entry& Current() const { return m_current; }

        // Every body uid any entry refers to (for pruning the body store).
        void CollectBodyUids(std::unordered_set<uint64_t>& out) const;

        size_t UndoCount() const { return m_undo.size(); }
        size_t RedoCount() const { return m_redo.size(); }

    private:
        struct Step
        {
            Entry       state;   // undo: the state before the change; redo: the state after it
            std::string label;   // the change between this state and the next
        };

        size_t            m_maxSteps;
        bool              m_hasBaseline = false;
        Entry             m_current;
        std::vector<Step> m_undo;
        std::vector<Step> m_redo;
    };
}
