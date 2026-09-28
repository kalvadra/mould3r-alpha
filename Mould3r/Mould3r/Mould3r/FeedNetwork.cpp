// ===========================================================================
// FeedNetwork.cpp — see FeedNetwork.h.
//
// Section conductance G (laminar, fully developed, Q = G dP / (eta L)):
//   circle    G = pi R^4 / 8                                 (Hagen-Poiseuille)
//   rectangle G = (w h^3 / 12) * [1 - (192 h / (pi^5 w)) * sum_{n odd} tanh(n pi w / 2h) / n^5]
//             with h <= w (the exact series solution for a rectangular duct).
//
// Network solve: nodal pressures on the melt-carrying edges with the inlet as a
// flow-rate source and the cavity-entry nodes held at 0 gauge. Each edge's
// viscosity is evaluated at its own apparent wall shear rate and iterated to a
// fixed point (Picard, relaxed in log space) — a generalised-Newtonian
// "representative viscosity" solve, standard for runner balancing.
// ===========================================================================

#include "FeedNetwork.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <queue>

namespace Flow
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // Solve A x = b in place (dense, partial pivoting). Returns false if
        // the matrix is singular to working precision.
        bool SolveDense(std::vector<double>& A, std::vector<double>& b, int n)
        {
            for (int col = 0; col < n; ++col)
            {
                int piv = col;
                double best = std::fabs(A[(size_t)col * n + col]);
                for (int r = col + 1; r < n; ++r)
                {
                    const double v = std::fabs(A[(size_t)r * n + col]);
                    if (v > best) { best = v; piv = r; }
                }
                if (best < 1.0e-300) return false;
                if (piv != col)
                {
                    for (int c = 0; c < n; ++c)
                        std::swap(A[(size_t)col * n + c], A[(size_t)piv * n + c]);
                    std::swap(b[(size_t)col], b[(size_t)piv]);
                }
                const double inv = 1.0 / A[(size_t)col * n + col];
                for (int r = col + 1; r < n; ++r)
                {
                    const double f = A[(size_t)r * n + col] * inv;
                    if (f == 0.0) continue;
                    for (int c = col; c < n; ++c)
                        A[(size_t)r * n + c] -= f * A[(size_t)col * n + c];
                    b[(size_t)r] -= f * b[(size_t)col];
                }
            }
            for (int r = n - 1; r >= 0; --r)
            {
                double s = b[(size_t)r];
                for (int c = r + 1; c < n; ++c) s -= A[(size_t)r * n + c] * b[(size_t)c];
                b[(size_t)r] = s / A[(size_t)r * n + r];
            }
            return true;
        }
    } // namespace

    // ---------------------------------------------------------------------
    // Sections
    // ---------------------------------------------------------------------
    FeedSection MakeCircleSection(float diameterMm)
    {
        FeedSection s;
        const double D = std::max(0.0f, diameterMm);
        s.shape = SectionShape::Circle;
        s.diameterMm = (float)D;
        const double R = 0.5 * D;
        s.areaMm2 = (float)(kPi * R * R);
        s.perimeterMm = (float)(kPi * D);
        s.hydraulicDiameterMm = (float)D;
        s.conductanceMm4 = kPi * R * R * R * R / 8.0;
        return s;
    }

    FeedSection MakeRectSection(float widthMm, float heightMm)
    {
        FeedSection s;
        s.shape = SectionShape::Rect;
        s.widthMm = std::max(0.0f, widthMm);
        s.heightMm = std::max(0.0f, heightMm);
        const double w = std::max(s.widthMm, s.heightMm);   // long side
        const double h = std::min(s.widthMm, s.heightMm);   // short side
        s.areaMm2 = (float)(w * h);
        s.perimeterMm = (float)(2.0 * (w + h));
        s.hydraulicDiameterMm = (w + h > 0.0) ? (float)(2.0 * w * h / (w + h)) : 0.0f;
        if (w > 0.0 && h > 0.0)
        {
            double sum = 0.0;
            for (int n = 1; n <= 41; n += 2)
                sum += std::tanh(n * kPi * w / (2.0 * h)) / std::pow((double)n, 5.0);
            const double factor = 1.0 - (192.0 * h / (std::pow(kPi, 5.0) * w)) * sum;
            s.conductanceMm4 = (w * h * h * h / 12.0) * factor;
        }
        return s;
    }

    double ApparentWallShearRate(const FeedSection& s, double flowMm3s)
    {
        const double Q = std::fabs(flowMm3s);
        if (s.shape == SectionShape::Circle && s.diameterMm > 0.0f)
        {
            const double D = s.diameterMm;
            return 32.0 * Q / (kPi * D * D * D);
        }
        if (s.shape == SectionShape::Rect && s.widthMm > 0.0f && s.heightMm > 0.0f)
        {
            const double w = std::max(s.widthMm, s.heightMm);
            const double h = std::min(s.widthMm, s.heightMm);
            return 6.0 * Q / (w * h * h);
        }
        return 0.0;
    }

    // ---------------------------------------------------------------------
    // Network construction
    // ---------------------------------------------------------------------
    int FeedNetwork::addNode(const glm::vec3& pos, FeedNodeKind kind, int objectIndex,
                             float mergeTolMm, const std::string& label)
    {
        if (mergeTolMm > 0.0f)
        {
            const float tol2 = mergeTolMm * mergeTolMm;
            for (int i = 0; i < (int)nodes.size(); ++i)
            {
                // Parts are never merged into (a part node is not a feed point).
                if (nodes[i].kind == FeedNodeKind::Part) continue;
                const glm::vec3 d = nodes[i].pos - pos;
                if (glm::dot(d, d) <= tol2) return i;
            }
        }
        FeedNode n;
        n.pos = pos; n.kind = kind; n.objectIndex = objectIndex; n.label = label;
        nodes.push_back(n);
        return (int)nodes.size() - 1;
    }

    int FeedNetwork::addEdge(int a, int b, FeedEdgeKind kind, float lengthMm,
                             const FeedSection& section, int featureIndex,
                             const std::string& label)
    {
        if (a < 0 || b < 0 || a >= (int)nodes.size() || b >= (int)nodes.size() || a == b)
            return -1;
        FeedEdge e;
        e.a = a; e.b = b; e.kind = kind; e.lengthMm = std::max(0.0f, lengthMm);
        e.section = section; e.featureIndex = featureIndex; e.label = label;
        edges.push_back(e);
        return (int)edges.size() - 1;
    }

    int FeedNetwork::countNodes(FeedNodeKind k) const
    {
        int c = 0; for (const FeedNode& n : nodes) c += (n.kind == k) ? 1 : 0; return c;
    }
    int FeedNetwork::countEdges(FeedEdgeKind k) const
    {
        int c = 0; for (const FeedEdge& e : edges) c += (e.kind == k) ? 1 : 0; return c;
    }

    float PolylineLengthMm(const std::vector<glm::vec3>& pts)
    {
        double L = 0.0;
        for (size_t i = 1; i < pts.size(); ++i) L += glm::length(pts[i] - pts[i - 1]);
        return (float)L;
    }

    // ---------------------------------------------------------------------
    // Steady feed solve
    // ---------------------------------------------------------------------
    FeedSolveResult SolveFeedNetwork(const FeedNetwork& net, double flowRateMm3s,
                                     const std::function<double(double)>& viscosity,
                                     int maxIterations, double tol)
    {
        FeedSolveResult R;
        const int N = (int)net.nodes.size();
        const int E = (int)net.edges.size();
        R.nodePressureMPa.assign(N, -1.0f);
        R.edgeFlowMm3s.assign(E, 0.0f);
        R.edgeShearRate.assign(E, 0.0f);
        R.edgeViscosityPaS.assign(E, 0.0f);
        R.edgeDeltaPMPa.assign(E, 0.0f);

        if (N == 0 || net.inletNode < 0 || net.inletNode >= N)
        { R.message = "No sprue inlet in the feed network."; return R; }
        if (!viscosity)
        { R.message = "No viscosity model supplied."; return R; }

        // Cavity-entry nodes: the feed end of every CavityIn link.
        std::vector<int> entryPart(N, -1);
        for (const FeedEdge& e : net.edges)
        {
            if (e.kind != FeedEdgeKind::CavityIn) continue;
            const bool aPart = net.nodes[e.a].kind == FeedNodeKind::Part;
            const bool bPart = net.nodes[e.b].kind == FeedNodeKind::Part;
            if (aPart == bPart) continue;
            entryPart[aPart ? e.b : e.a] = aPart ? e.a : e.b;
        }

        // Reachability from the inlet over melt-carrying edges.
        std::vector<std::vector<int>> adj(N);
        for (int i = 0; i < E; ++i)
            if (net.edges[i].carriesMelt())
            { adj[net.edges[i].a].push_back(i); adj[net.edges[i].b].push_back(i); }
        std::vector<char> reach(N, 0);
        {
            std::queue<int> q; q.push(net.inletNode); reach[net.inletNode] = 1;
            while (!q.empty())
            {
                const int u = q.front(); q.pop();
                for (int ei : adj[u])
                {
                    const int v = (net.edges[ei].a == u) ? net.edges[ei].b : net.edges[ei].a;
                    if (!reach[v]) { reach[v] = 1; q.push(v); }
                }
            }
        }

        int reachableEntries = 0;
        for (int i = 0; i < N; ++i) if (reach[i] && entryPart[i] >= 0) ++reachableEntries;
        if (reachableEntries == 0)
        { R.message = "No melt path from the sprue to any part (no gate / direct-injection link reached)."; return R; }

        // Unknowns: reachable, non-entry nodes.
        std::vector<int> idx(N, -1);
        int m = 0;
        for (int i = 0; i < N; ++i) if (reach[i] && entryPart[i] < 0) idx[i] = m++;

        // Per-edge viscosity, iterated.
        std::vector<double> eta(E, viscosity(0.0));
        std::vector<double> pNode(N, 0.0);
        std::vector<double> qEdge(E, 0.0);
        const double Q = std::max(0.0, flowRateMm3s);

        auto conductance = [&](int ei) -> double
        {
            const FeedEdge& e = net.edges[ei];
            const double L = std::max(1.0e-3, (double)e.lengthMm);
            const double G = e.section.conductanceMm4;
            if (G <= 0.0 || eta[ei] <= 0.0) return 0.0;
            return G / (eta[ei] * L);                    // mm^3 / (Pa.s)
        };

        const int iterCap = std::max(1, maxIterations);
        for (int it = 1; it <= iterCap; ++it)
        {
            R.iterations = it;
            std::vector<double> A((size_t)m * m, 0.0), b((size_t)m, 0.0);
            for (int ei = 0; ei < E; ++ei)
            {
                const FeedEdge& e = net.edges[ei];
                if (!e.carriesMelt() || !reach[e.a] || !reach[e.b]) continue;
                const double c = conductance(ei);
                if (c <= 0.0) continue;
                const int ia = idx[e.a], ib = idx[e.b];
                if (ia >= 0) { A[(size_t)ia * m + ia] += c; if (ib >= 0) A[(size_t)ia * m + ib] -= c; }
                if (ib >= 0) { A[(size_t)ib * m + ib] += c; if (ia >= 0) A[(size_t)ib * m + ia] -= c; }
            }
            if (idx[net.inletNode] >= 0) b[(size_t)idx[net.inletNode]] += Q;

            if (m > 0 && !SolveDense(A, b, m))
            { R.message = "Feed network matrix is singular (disconnected or zero-section edge)."; return R; }

            for (int i = 0; i < N; ++i) pNode[i] = (idx[i] >= 0) ? b[(size_t)idx[i]] : 0.0;

            double maxRel = 0.0;
            std::vector<double> target(E, 0.0);
            for (int ei = 0; ei < E; ++ei)
            {
                const FeedEdge& e = net.edges[ei];
                if (!e.carriesMelt() || !reach[e.a] || !reach[e.b]) continue;
                qEdge[ei] = conductance(ei) * (pNode[e.a] - pNode[e.b]);
                const double gd = ApparentWallShearRate(e.section, qEdge[ei]);
                target[ei] = std::max(1.0e-6, viscosity(gd));
                maxRel = std::max(maxRel, std::fabs(target[ei] - eta[ei]) / eta[ei]);
            }
            if (maxRel < tol) { R.converged = true; break; }
            for (int ei = 0; ei < E; ++ei)          // relax in log space
                if (target[ei] > 0.0)
                    eta[ei] = std::exp(0.5 * std::log(eta[ei]) + 0.5 * std::log(target[ei]));
        }

        // Outputs from the final solve.
        for (int i = 0; i < N; ++i)
            if (reach[i]) R.nodePressureMPa[i] = (float)(pNode[i] / 1.0e6);
        for (int ei = 0; ei < E; ++ei)
        {
            const FeedEdge& e = net.edges[ei];
            if (!e.carriesMelt() || !reach[e.a] || !reach[e.b]) continue;
            R.edgeFlowMm3s[ei]     = (float)qEdge[ei];
            R.edgeShearRate[ei]    = (float)ApparentWallShearRate(e.section, qEdge[ei]);
            R.edgeViscosityPaS[ei] = (float)eta[ei];
            R.edgeDeltaPMPa[ei]    = (float)(std::fabs(pNode[e.a] - pNode[e.b]) / 1.0e6);
        }
        R.inletPressureMPa = (float)(pNode[net.inletNode] / 1.0e6);

        for (int i = 0; i < N; ++i)
        {
            if (!reach[i] || entryPart[i] < 0) continue;
            double inflow = 0.0;
            for (int ei : adj[i])
            {
                const FeedEdge& e = net.edges[ei];
                inflow += (e.b == i) ? qEdge[ei] : -qEdge[ei];
            }
            R.entryNodes.push_back(i);
            R.entryFlowMm3s.push_back((float)inflow);
            R.entryPartNode.push_back(entryPart[i]);
        }

        R.ok = true;
        R.message = R.converged ? "Feed network solved."
                                : "Feed network solved (viscosity iteration hit the cap; result approximate).";
        return R;
    }

    const char* FeedNodeKindName(FeedNodeKind k)
    {
        switch (k)
        {
        case FeedNodeKind::SprueInlet:     return "sprue inlet";
        case FeedNodeKind::SprueParting:   return "sprue @ parting";
        case FeedNodeKind::SprueEnd:       return "sprue end";
        case FeedNodeKind::RunnerEnd:      return "runner end";
        case FeedNodeKind::Junction:       return "junction";
        case FeedNodeKind::GateTransition: return "gate transition";
        case FeedNodeKind::GateOrigin:     return "gate mouth";
        case FeedNodeKind::Part:           return "part";
        case FeedNodeKind::VentStart:      return "vent mouth";
        case FeedNodeKind::VentOutlet:     return "vent outlet";
        }
        return "?";
    }

    const char* FeedEdgeKindName(FeedEdgeKind k)
    {
        switch (k)
        {
        case FeedEdgeKind::Sprue:     return "sprue";
        case FeedEdgeKind::Runner:    return "runner";
        case FeedEdgeKind::SubRunner: return "sub-runner";
        case FeedEdgeKind::Gate:      return "gate";
        case FeedEdgeKind::Vent:      return "vent";
        case FeedEdgeKind::CavityIn:  return "cavity in";
        case FeedEdgeKind::CavityOut: return "cavity out";
        }
        return "?";
    }

} // namespace Flow
