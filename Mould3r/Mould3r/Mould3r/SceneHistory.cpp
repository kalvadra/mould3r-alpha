// SceneHistory — see SceneHistory.h.

#include "SceneHistory.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace History
{
    // -----------------------------------------------------------------------
    // SceneState
    // -----------------------------------------------------------------------
    void SceneState::CollectBodyUids(std::unordered_set<uint64_t>& out) const
    {
        for (const BodyRef& b : fixtures) if (b.uid) out.insert(b.uid);
        for (const BodyRef& b : objects)  if (b.uid) out.insert(b.uid);
        for (const InsertRef& r : inserts) if (r.uid) out.insert(r.uid);
    }

    // -----------------------------------------------------------------------
    // StripGL
    // -----------------------------------------------------------------------
    void StripGL(SolidMesh& m)
    {
        m.vao = 0;
        m.vbo = 0;
        m.ebo = 0;
        m.indexCount = 0;
        m.valid = false;
    }

    void StripGL(SprueFeature& s)
    {
        StripGL(s.solid);
        StripGL(s.coldSlugSolid);
        s.pathVAO = 0;
        s.pathVBO = 0;
        s.pathVertexCount = 0;
        s.xsecVAO = 0;
        s.xsecVBO = 0;
        s.xsecVertexCount = 0;
    }

    void StripGL(VentInstance& v)   { StripGL(v.solid); }
    void StripGL(RunnerFeature& r)  { StripGL(r.solid); StripGL(r.coldPlugSolid); }
    void StripGL(GateFeature& g)    { StripGL(g.solid); StripGL(g.subRunnerSolid); }
    void StripGL(EjectorFeature& e) { StripGL(e.solid); }
    void StripGL(IndexerFeature& i) { StripGL(i.solid); }

    // -----------------------------------------------------------------------
    // Comparison
    // -----------------------------------------------------------------------
    namespace
    {
        // Walks two scenes field by field and stops at the first difference.
        // Allocation-free on the common (equal) path: field names are string
        // literals, and the nesting ("vents[2].path.nodes[1]") is a small
        // stack of (name, index) frames that is only turned into a string
        // when a difference is found. This runs on every idle event.
        class Cmp
        {
        public:
            Cmp(float eps, std::string* out) : m_eps(eps), m_out(out) { m_ctx.reserve(8); }

            bool Same() const { return m_same; }

            // RAII nesting frame: Scope s(c, "path"); or Scope s(c, "nodes", i);
            class Scope
            {
            public:
                Scope(Cmp& c, const char* name, int index = -1) : m_c(c) { m_c.m_ctx.push_back({ name, index }); }
                ~Scope() { m_c.m_ctx.pop_back(); }
                Scope(const Scope&) = delete;
                Scope& operator=(const Scope&) = delete;
            private:
                Cmp& m_c;
            };

            void Fail(const char* field)
            {
                if (!m_same) return;
                m_same = false;
                if (!m_out) return;
                std::string w;
                for (const Frame& f : m_ctx)
                {
                    if (!f.name || !*f.name) continue;
                    if (!w.empty()) w += '.';
                    w += f.name;
                    if (f.index >= 0) w += "[" + std::to_string(f.index) + "]";
                }
                if (field && *field)
                {
                    if (!w.empty() && field[0] != '[') w += '.';
                    w += field;
                }
                *m_out = w;
            }

            void F(const char* field, float a, float b)
            {
                if (!m_same) return;
                if (a == b) return;
                if (std::isnan(a) && std::isnan(b)) return;
                if (m_eps > 0.0f)
                {
                    const float scale = std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
                    if (std::fabs(a - b) <= m_eps * scale) return;
                }
                Fail(field);
            }

            void V(const char* field, const glm::vec3& a, const glm::vec3& b)
            {
                if (!m_same) return;
                Scope s(*this, field);
                F("x", a.x, b.x);
                F("y", a.y, b.y);
                F("z", a.z, b.z);
            }

            void M(const char* field, const glm::mat4& a, const glm::mat4& b)
            {
                if (!m_same) return;
                for (int col = 0; col < 4 && m_same; ++col)
                {
                    Scope s(*this, field, col);
                    static const char* const kRow[4] = { "[0]", "[1]", "[2]", "[3]" };
                    for (int r = 0; r < 4 && m_same; ++r)
                        F(kRow[r], a[col][r], b[col][r]);
                }
            }

            template <typename T>
            void E(const char* field, const T& a, const T& b)   // exact (bool, int, enum, string, uid)
            {
                if (m_same && !(a == b)) Fail(field);
            }

            // Vectors: the sizes, then each element through `each(c, x, y)`.
            template <typename T, typename Fn>
            void Vec(const char* field, const std::vector<T>& a, const std::vector<T>& b, Fn each)
            {
                if (!m_same) return;
                if (a.size() != b.size())
                {
                    Scope s(*this, field);
                    Fail("size");
                    return;
                }
                for (size_t i = 0; i < a.size() && m_same; ++i)
                {
                    Scope s(*this, field, (int)i);
                    each(*this, a[i], b[i]);
                }
            }

        private:
            struct Frame { const char* name; int index; };
            float              m_eps;
            std::string*       m_out;
            bool               m_same = true;
            std::vector<Frame> m_ctx;
        };

        void ComparePath(Cmp& c, const char* field, const FeaturePath& a, const FeaturePath& b)
        {
            Cmp::Scope s(c, field);
            c.E("kind", a.kind, b.kind);
            c.V("start", a.start, b.start);
            c.V("end", a.end, b.end);
            c.E("smooth", a.smooth, b.smooth);
            c.E("valid", a.valid, b.valid);
            c.F("overrunStart", a.overrunStart, b.overrunStart);
            c.F("overrunEnd", a.overrunEnd, b.overrunEnd);
            c.Vec("nodes", a.nodes, b.nodes,
                [](Cmp& k, const PathNode& x, const PathNode& y) {
                    k.V("pos", x.pos, y.pos);
                    k.V("dir", x.dir, y.dir);
                    k.F("handleLen", x.handleLen, y.handleLen);
                    k.V("handleIn", x.handleIn, y.handleIn);
                    k.V("handleOut", x.handleOut, y.handleOut);
                    k.E("handlesLinked", x.handlesLinked, y.handlesLinked);
                    k.E("handlesManual", x.handlesManual, y.handlesManual);
                });
        }

        void CompareInjection(Cmp& c, const InjectionPoint& a, const InjectionPoint& b)
        {
            c.E("label", a.label, b.label);
            c.F("x", a.x, b.x);
            c.F("y", a.y, b.y);
            c.F("z", a.z, b.z);
            c.E("type", a.type, b.type);
            c.E("perimeter", a.perimeter, b.perimeter);
            c.E("topPlane", a.topPlane, b.topPlane);
        }

        void CompareBody(Cmp& c, const BodyRef& a, const BodyRef& b)
        {
            c.E("uid", a.uid, b.uid);
            c.V("pos", a.pos, b.pos);
            c.F("yawDeg", a.yawDeg, b.yawDeg);
            c.F("pitchDeg", a.pitchDeg, b.pitchDeg);
            c.F("rollDeg", a.rollDeg, b.rollDeg);
            c.F("scale", a.scale, b.scale);
            c.E("mirrorX", a.mirrorX, b.mirrorX);
            c.E("mirrorZ", a.mirrorZ, b.mirrorZ);
        }

        void CompareInsert(Cmp& c, const InsertRef& x, const InsertRef& y)
        {
            c.E("uid", x.uid, y.uid);
            c.E("parentIndex", x.parentIndex, y.parentIndex);
            c.V("localOffset", x.localOffset, y.localOffset);
            c.V("localRotDeg", x.localRotDeg, y.localRotDeg);
            c.F("localScale", x.localScale, y.localScale);
            c.E("id", x.id, y.id);
            c.M("worldMatrix", x.worldMatrix, y.worldMatrix);
        }

        void CompareSprue(Cmp& c, const SprueFeature& a, const SprueFeature& b)
        {
            c.E("hasPoint", a.hasPoint, b.hasPoint);
            c.V("worldPos", a.worldPos, b.worldPos);
            c.V("pathStart", a.pathStart, b.pathStart);
            c.V("pathEnd", a.pathEnd, b.pathEnd);
            c.V("partingPos", a.partingPos, b.partingPos);
            c.E("hasPartingPoint", a.hasPartingPoint, b.hasPartingPoint);
            c.E("isDirectInjection", a.isDirectInjection, b.isDirectInjection);
            c.E("hasEndpointOverride", a.hasEndpointOverride, b.hasEndpointOverride);
            c.V("endpointOverride", a.endpointOverride, b.endpointOverride);
            c.E("overrideDirectInjection", a.overrideDirectInjection, b.overrideDirectInjection);
            c.F("radius", a.radius, b.radius);
            c.F("draftAngleDeg", a.draftAngleDeg, b.draftAngleDeg);
            c.F("coldSlugDepth", a.coldSlugDepth, b.coldSlugDepth);
        }

        void CompareRunner(Cmp& c, const RunnerFeature& a, const RunnerFeature& b)
        {
            c.V("point", a.point, b.point);
            ComparePath(c, "path", a.path, b.path);
        }

        void CompareVent(Cmp& c, const VentInstance& a, const VentInstance& b)
        {
            c.V("point.worldPos", a.point.worldPos, b.point.worldPos);
            c.V("point.worldNormal", a.point.worldNormal, b.point.worldNormal);
            ComparePath(c, "path", a.path, b.path);
            c.E("crossSection.valid", a.crossSection.valid, b.crossSection.valid);
            for (int k = 0; k < 4 && c.Same(); ++k)
            {
                Cmp::Scope s(c, "crossSection.corners", k);
                c.V("", a.crossSection.corners[k], b.crossSection.corners[k]);
            }
            c.E("parentIndex", a.parentIndex, b.parentIndex);
            c.V("localPos", a.localPos, b.localPos);
            c.V("localNormal", a.localNormal, b.localNormal);
            // embedExtension: derived at Generate Mould, not authored.
        }

        void CompareGate(Cmp& c, const GateFeature& a, const GateFeature& b)
        {
            c.V("point.worldPos", a.point.worldPos, b.point.worldPos);
            c.V("point.worldNormal", a.point.worldNormal, b.point.worldNormal);
            c.V("pathEnd", a.pathEnd, b.pathEnd);
            c.E("hasPath", a.hasPath, b.hasPath);
            c.E("parentIndex", a.parentIndex, b.parentIndex);
            c.V("localPos", a.localPos, b.localPos);
            c.V("localNormal", a.localNormal, b.localNormal);
            ComparePath(c, "subPath", a.subPath, b.subPath);
            // embedExtension: derived at Generate Mould, not authored.
        }

        void ComparePoint(Cmp& c, const glm::vec3& a, const glm::vec3& b) { c.V("point", a, b); }
    }

    bool SameScene(const SceneState& a, const SceneState& b, float eps, std::string* firstDiff)
    {
        Cmp c(eps, firstDiff);

        // Fixture
        c.Vec("fixtures", a.fixtures, b.fixtures, CompareBody);
        c.E("fixtureKind", a.fixtureKind, b.fixtureKind);
        c.V("dynamicClearance", a.dynamicClearance, b.dynamicClearance);
        c.Vec("injectionPoints", a.injectionPoints, b.injectionPoints, CompareInjection);
        c.E("hasActiveInjectionPoint", a.hasActiveInjectionPoint, b.hasActiveInjectionPoint);
        if (a.hasActiveInjectionPoint && b.hasActiveInjectionPoint)
        {
            Cmp::Scope s(c, "activeInjectionPoint");
            CompareInjection(c, a.activeInjectionPoint, b.activeInjectionPoint);
        }
        c.E("allowPerimeterInjection", a.allowPerimeterInjection, b.allowPerimeterInjection);
        c.E("topInjection", a.topInjection, b.topInjection);

        // Bodies
        c.Vec("objects", a.objects, b.objects, CompareBody);
        c.Vec("inserts", a.inserts, b.inserts, CompareInsert);

        // Features
        {
            Cmp::Scope s(c, "sprue");
            CompareSprue(c, a.sprue, b.sprue);
        }
        c.Vec("runners", a.runners, b.runners, CompareRunner);
        c.Vec("gates", a.gates, b.gates, CompareGate);
        c.Vec("vents", a.vents, b.vents, CompareVent);
        c.Vec("ejectors", a.ejectors, b.ejectors,
            [](Cmp& k, const EjectorFeature& x, const EjectorFeature& y) { ComparePoint(k, x.point, y.point); });
        c.Vec("indexers", a.indexers, b.indexers,
            [](Cmp& k, const IndexerFeature& x, const IndexerFeature& y) { ComparePoint(k, x.point, y.point); });

        return c.Same();
    }

    // -----------------------------------------------------------------------
    // DescribeChange
    // -----------------------------------------------------------------------
    namespace
    {
        std::vector<uint64_t> Uids(const std::vector<BodyRef>& v)
        {
            std::vector<uint64_t> out;
            out.reserve(v.size());
            for (const BodyRef& b : v) out.push_back(b.uid);
            return out;
        }

        // "Place Vent" / "Place Vents" / "Remove Vent" / "Edit Vent" for a
        // feature list, or "" when the lists are the same.
        template <typename T, typename SameFn>
        std::string FeatureChange(const std::vector<T>& a, const std::vector<T>& b,
                                  const char* one, const char* many, SameFn same)
        {
            if (b.size() > a.size())
                return std::string("Place ") + (b.size() - a.size() > 1 ? many : one);
            if (b.size() < a.size())
                return std::string("Remove ") + (a.size() - b.size() > 1 ? many : one);
            for (size_t i = 0; i < a.size(); ++i)
                if (!same(a[i], b[i])) return std::string("Edit ") + one;
            return std::string();
        }

        template <typename T, typename Fn>
        bool SameEach(const T& a, const T& b, Fn fn)
        {
            Cmp c(0.0f, nullptr);
            fn(c, a, b);
            return c.Same();
        }
    }

    std::string DescribeChange(const SceneState& a, const SceneState& b)
    {
        // Fixture swap / rebuild (new halves get new uids).
        if (Uids(a.fixtures) != Uids(b.fixtures) || a.fixtureKind != b.fixtureKind)
            return "Change Fixture";

        // Objects added / removed.
        {
            std::unordered_set<uint64_t> ua, ub;
            for (const BodyRef& r : a.objects) ua.insert(r.uid);
            for (const BodyRef& r : b.objects) ub.insert(r.uid);
            size_t added = 0, removed = 0;
            for (uint64_t u : ub) if (!ua.count(u)) ++added;
            for (uint64_t u : ua) if (!ub.count(u)) ++removed;
            if (added && removed) return "Change Objects";
            if (added)   return added > 1 ? "Add Objects" : "Add Object";
            if (removed) return removed > 1 ? "Delete Objects" : "Delete Object";
        }

        // Same objects: what kind of transform?
        if (a.objects.size() == b.objects.size())
        {
            bool moved = false, rotated = false, scaled = false, mirrored = false, reordered = false;
            for (size_t i = 0; i < a.objects.size(); ++i)
            {
                const BodyRef& x = a.objects[i];
                const BodyRef& y = b.objects[i];
                if (x.uid != y.uid) { reordered = true; continue; }
                if (x.pos != y.pos) moved = true;
                if (x.yawDeg != y.yawDeg || x.pitchDeg != y.pitchDeg || x.rollDeg != y.rollDeg) rotated = true;
                if (x.scale != y.scale) scaled = true;
                if (x.mirrorX != y.mirrorX || x.mirrorZ != y.mirrorZ) mirrored = true;
            }
            const int kinds = (int)moved + (int)rotated + (int)scaled + (int)mirrored;
            if (reordered) return "Change Objects";
            if (kinds > 1 || mirrored) return "Transform";
            if (moved)   return "Move";
            if (rotated) return "Rotate";
            if (scaled)  return "Scale";
        }

        // Inserts
        if (b.inserts.size() > a.inserts.size()) return "Place Insert";
        if (b.inserts.size() < a.inserts.size())
            return a.inserts.size() - b.inserts.size() > 1 ? "Remove Inserts" : "Remove Insert";

        // Injection point
        if (a.hasActiveInjectionPoint != b.hasActiveInjectionPoint ||
            (a.hasActiveInjectionPoint &&
             !SameEach(a.activeInjectionPoint, b.activeInjectionPoint,
                       CompareInjection)))
        {
            const bool alongPerimeter = a.hasActiveInjectionPoint && b.hasActiveInjectionPoint &&
                                        ((a.activeInjectionPoint.perimeter && b.activeInjectionPoint.perimeter) ||
                                         (a.activeInjectionPoint.topPlane && b.activeInjectionPoint.topPlane)) &&
                                        a.sprue.hasPoint && b.sprue.hasPoint;
            return alongPerimeter ? "Edit Sprue" : "Select Injection Point";
        }

        // Sprue
        if (!a.sprue.hasPoint && b.sprue.hasPoint) return "Place Sprue";
        if (a.sprue.hasPoint && !b.sprue.hasPoint) return "Remove Sprue";
        if (!SameEach(a.sprue, b.sprue, CompareSprue))
            return "Edit Sprue";

        // Feed system, then the rest, in the order a change cascades: a runner
        // edit also moves gates that attach to it, so runners are asked first.
        std::string s;
        s = FeatureChange(a.runners, b.runners, "Runner", "Runners",
            [](const RunnerFeature& x, const RunnerFeature& y) { return SameEach(x, y, CompareRunner); });
        if (!s.empty()) return s;
        s = FeatureChange(a.gates, b.gates, "Gate", "Gates",
            [](const GateFeature& x, const GateFeature& y) { return SameEach(x, y, CompareGate); });
        if (!s.empty()) return s;
        s = FeatureChange(a.vents, b.vents, "Vent", "Vents",
            [](const VentInstance& x, const VentInstance& y) { return SameEach(x, y, CompareVent); });
        if (!s.empty()) return s;
        s = FeatureChange(a.ejectors, b.ejectors, "Ejector", "Ejectors",
            [](const EjectorFeature& x, const EjectorFeature& y) { return x.point == y.point; });
        if (!s.empty()) return s;
        s = FeatureChange(a.indexers, b.indexers, "Indexer", "Indexers",
            [](const IndexerFeature& x, const IndexerFeature& y) { return x.point == y.point; });
        if (!s.empty()) return s;

        // Inserts edited in place (same count).
        for (size_t i = 0; i < a.inserts.size() && i < b.inserts.size(); ++i)
        {
            const InsertRef& x = a.inserts[i];
            const InsertRef& y = b.inserts[i];
            if (x.localOffset != y.localOffset || x.localRotDeg != y.localRotDeg ||
                x.localScale != y.localScale || x.parentIndex != y.parentIndex || x.uid != y.uid)
                return "Edit Insert";
        }

        return "Edit";
    }

    // -----------------------------------------------------------------------
    // Stack
    // -----------------------------------------------------------------------
    void Stack::Reset()
    {
        m_hasBaseline = false;
        m_current = Entry{};
        m_undo.clear();
        m_redo.clear();
    }

    bool Stack::Observe(Entry now)
    {
        if (!m_hasBaseline)
        {
            m_current = std::move(now);
            m_hasBaseline = true;
            return false;
        }

        // A hair of tolerance (relative 1e-6, a few float ulps) so a rebuild
        // that reproduces a value only to the last bit isn't taken for an edit
        // — that would record an empty-looking step and wipe the redo stack.
        // Any real edit is many orders of magnitude larger.
        if (SameScene(m_current.scene, now.scene, 1.0e-6f, nullptr))
        {
            // Nothing authored changed. Keep the selection fresh so the step
            // that follows records the selection the user had just before it.
            m_current.scene.selection = std::move(now.scene.selection);
            return false;
        }

        Step step;
        step.label = DescribeChange(m_current.scene, now.scene);
        step.state = std::move(m_current);
        m_undo.push_back(std::move(step));
        m_redo.clear();
        if (m_undo.size() > m_maxSteps)
            m_undo.erase(m_undo.begin(), m_undo.begin() + (m_undo.size() - m_maxSteps));
        m_current = std::move(now);
        return true;
    }

    const std::string& Stack::UndoLabel() const
    {
        static const std::string kEmpty;
        return m_undo.empty() ? kEmpty : m_undo.back().label;
    }

    const std::string& Stack::RedoLabel() const
    {
        static const std::string kEmpty;
        return m_redo.empty() ? kEmpty : m_redo.back().label;
    }

    void Stack::CompleteUndo(Entry live)
    {
        if (m_undo.empty()) return;
        Step undone = std::move(m_undo.back());
        m_undo.pop_back();

        Step redo;
        redo.label = std::move(undone.label);
        redo.state = std::move(m_current);
        m_redo.push_back(std::move(redo));

        m_current = std::move(live);
    }

    void Stack::CompleteRedo(Entry live)
    {
        if (m_redo.empty()) return;
        Step redone = std::move(m_redo.back());
        m_redo.pop_back();

        Step undo;
        undo.label = std::move(redone.label);
        undo.state = std::move(m_current);
        m_undo.push_back(std::move(undo));
        if (m_undo.size() > m_maxSteps)
            m_undo.erase(m_undo.begin(), m_undo.begin() + (m_undo.size() - m_maxSteps));

        m_current = std::move(live);
    }

    void Stack::CollectBodyUids(std::unordered_set<uint64_t>& out) const
    {
        if (m_hasBaseline) m_current.scene.CollectBodyUids(out);
        for (const Step& s : m_undo) s.state.scene.CollectBodyUids(out);
        for (const Step& s : m_redo) s.state.scene.CollectBodyUids(out);
    }
}
