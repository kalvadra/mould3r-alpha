// ===========================================================================
// FlowSolver.cpp — isothermal Newtonian Hele-Shaw fill (P2). See FlowSolver.h.
//
// Discretisation: linear-triangle FEM for div(S grad p)=0 on the mid-surface,
// with the cotangent stiffness L_e (so it is correct on a curved surface mesh,
// not just a flat one) scaled by the element conductance S_e = h_e^3/(12*eta).
// Filling is a Control-Volume march: each pseudo-step solves the pressure on the
// filled control volumes (gate = flow-rate source, melt front held at p=0),
// reads the inflow into each front control volume from the solved field, and
// advances the fill factors so exactly the next control volume tops out. This
// is the classic CVFEM filling scheme (Hieber & Shen / Voller).
//
// Units: the public interface is in millimetres; everything below is converted
// to SI (m, Pa, s) on entry and converted back on the way out.
// ===========================================================================

#include "FlowSolver.h"

// Eigen's headers land at <...>/include/eigen3/Eigen/... under vcpkg, but the
// vcpkg MSBuild integration only adds <...>/include to the search path (the
// eigen3 prefix is normally supplied by Eigen's CMake config, which a raw
// .vcxproj doesn't consume). Resolve both layouts so no include-dir edit is
// needed in the project file.
#if __has_include(<Eigen/Sparse>)
  #include <Eigen/Sparse>
#elif __has_include(<eigen3/Eigen/Sparse>)
  #include <eigen3/Eigen/Sparse>
#else
  #error "Eigen not found. Ensure vcpkg installed eigen3 (see vcpkg.json)."
#endif
#include <cmath>
#include <vector>
#include <algorithm>
#include <limits>

namespace Flow
{
    namespace
    {
        constexpr double kMM   = 1.0e-3;      // mm -> m
        constexpr double kG    = 9.80665;     // gravity, for kgf -> N (tonne-force)
        constexpr double kTonN = 1000.0 * kG; // 1 tonne-force in newtons

        struct Elem
        {
            int    n[3]  = { 0, 0, 0 };
            double area  = 0.0;   // m^2
            double h     = 0.0;   // full gap, m
            double S     = 0.0;   // conductance h^3/(12 eta), m^3/(Pa.s)
            double L[3][3] = { {0,0,0},{0,0,0},{0,0,0} };  // cotangent stiffness
            double projArea = 0.0; // m^2, projected normal to the pull axis
        };
    } // namespace

    FlowResult SolveIsothermalFill(const SolveMesh& mesh,
                                   const FlowSolverParams& params,
                                   const glm::vec3& projectAxis)
    {
        FlowResult R;
        R.totalNodes = (int)mesh.nodes.size();

        if (mesh.empty())
        { R.message = "Empty solve mesh."; return R; }
        if (mesh.gateNodes.empty())
        { R.message = "No gate node — nothing to inject through."; return R; }
        if (mesh.halfGapMm.size() != mesh.nodes.size())
        { R.message = "Half-gap array does not match node count."; return R; }
        const double eta = std::max(1.0e-6, params.viscosityPaS);
        const double fillTime = std::max(1.0e-6, params.fillTimeS);

        const int N = (int)mesh.nodes.size();

        // Node positions in metres; half-gaps in metres.
        std::vector<glm::dvec3> P(N);
        std::vector<double> halfGapM(N);
        for (int i = 0; i < N; ++i)
        {
            P[i] = glm::dvec3(mesh.nodes[i]) * kMM;
            halfGapM[i] = std::max(0.0, (double)mesh.halfGapMm[i]) * kMM;
        }

        glm::dvec3 axis(projectAxis);
        { const double L = std::sqrt(glm::dot(axis, axis));
          axis = (L > 1e-12) ? axis / L : glm::dvec3(0, 1, 0); }

        // ---- Precompute element geometry, conductance, cotangent stiffness ----
        std::vector<Elem> E;
        E.reserve(mesh.tris.size());
        std::vector<double> V(N, 0.0);        // nodal control volume, m^3
        std::vector<double> projA(N, 0.0);    // nodal projected area, m^2
        double cavityVol = 0.0;               // m^3
        double cavityProj = 0.0;              // m^2

        for (const glm::ivec3& tri : mesh.tris)
        {
            const int a = tri.x, b = tri.y, c = tri.z;
            if (a < 0 || b < 0 || c < 0 || a >= N || b >= N || c >= N) continue;

            const glm::dvec3 p0 = P[a], p1 = P[b], p2 = P[c];
            const glm::dvec3 e01 = p1 - p0, e02 = p2 - p0;
            const glm::dvec3 cr = glm::cross(e01, e02);
            const double twoA = std::sqrt(glm::dot(cr, cr));
            if (twoA <= 1.0e-18) continue;                 // degenerate
            const double area = 0.5 * twoA;
            const glm::dvec3 nrm = cr / twoA;

            const double hE = ( (2.0*halfGapM[a]) + (2.0*halfGapM[b]) + (2.0*halfGapM[c]) ) / 3.0;
            if (hE <= 0.0) continue;                        // no gap -> no flow

            Elem el;
            el.n[0] = a; el.n[1] = b; el.n[2] = c;
            el.area = area;
            el.h    = hE;
            el.S    = (hE * hE * hE) / (12.0 * eta);
            el.projArea = area * std::fabs(glm::dot(nrm, axis));

            // Cotangents of the three interior angles (cot = (u.v)/|u x v|,
            // and |u x v| = 2A for any vertex of this triangle).
            const double cot0 = glm::dot(p1 - p0, p2 - p0) / twoA; // at vertex a
            const double cot1 = glm::dot(p0 - p1, p2 - p1) / twoA; // at vertex b
            const double cot2 = glm::dot(p0 - p2, p1 - p2) / twoA; // at vertex c

            const double L01 = -0.5 * cot2;   // edge (a,b) opposite vertex c
            const double L12 = -0.5 * cot0;   // edge (b,c) opposite vertex a
            const double L20 = -0.5 * cot1;   // edge (c,a) opposite vertex b
            el.L[0][1] = el.L[1][0] = L01;
            el.L[1][2] = el.L[2][1] = L12;
            el.L[2][0] = el.L[0][2] = L20;
            el.L[0][0] = -(L01 + L20);
            el.L[1][1] = -(L01 + L12);
            el.L[2][2] = -(L12 + L20);

            const double vThird = area * hE / 3.0;
            V[a] += vThird; V[b] += vThird; V[c] += vThird;
            const double aThird = el.projArea / 3.0;
            projA[a] += aThird; projA[b] += aThird; projA[c] += aThird;
            cavityVol  += area * hE;
            cavityProj += el.projArea;

            E.push_back(el);
        }

        if (E.empty() || cavityVol <= 0.0)
        { R.message = "No fillable elements (zero gap or degenerate mesh)."; return R; }

        // Node -> incident element list (for the front and the active set).
        std::vector<std::vector<int>> node2elem(N);
        for (int e = 0; e < (int)E.size(); ++e)
            for (int k = 0; k < 3; ++k) node2elem[E[e].n[k]].push_back(e);

        // ---- Fill march state ----
        std::vector<double> f(N, 0.0);          // fill factor 0..1
        std::vector<char>   filled(N, 0);
        std::vector<double> tfill(N, -1.0);
        std::vector<char>   isGate(N, 0);
        std::vector<double> lastP(N, 0.0);      // Pa, last solved field

        const double Qtotal = cavityVol / fillTime;             // m^3/s
        int nGates = 0;
        for (int g : mesh.gateNodes) if (g >= 0 && g < N) ++nGates;
        if (nGates == 0) { R.message = "Gate nodes are out of range."; return R; }
        const double Qgate = Qtotal / (double)nGates;

        for (int g : mesh.gateNodes)
            if (g >= 0 && g < N)
            { isGate[g] = 1; filled[g] = 1; f[g] = 1.0; tfill[g] = 0.0; }

        const double eps = 1.0e-9;
        double t = 0.0;
        double maxGateP = 0.0;
        bool   shortShot = false;
        int    step = 0;

        std::vector<int>    compact(N, -1);
        std::vector<Eigen::Triplet<double>> tFull, tFF;
        Eigen::SparseMatrix<double> Kfull(N, N);

        for (; step < params.maxSteps; ++step)
        {
            int filledCount = 0;
            for (int i = 0; i < N; ++i) filledCount += filled[i] ? 1 : 0;
            if (filledCount >= N) break;                       // fully filled

            // Front = empty node adjacent (via an element) to a filled node.
            std::vector<char> isFront(N, 0);
            std::vector<int>  front;
            for (int e = 0; e < (int)E.size(); ++e)
            {
                bool anyFilled = filled[E[e].n[0]] || filled[E[e].n[1]] || filled[E[e].n[2]];
                if (!anyFilled) continue;
                for (int k = 0; k < 3; ++k)
                {
                    const int i = E[e].n[k];
                    if (!filled[i] && !isFront[i]) { isFront[i] = 1; front.push_back(i); }
                }
            }
            if (front.empty()) break;   // remaining empties unreachable -> short shot

            // Compact map over filled nodes (the pressure unknowns).
            int nf = 0;
            for (int i = 0; i < N; ++i) compact[i] = filled[i] ? nf++ : -1;

            // Assemble over active elements (>=1 filled node): full K (for front
            // inflow) and the filled x filled block (the solve).
            tFull.clear(); tFF.clear();
            for (int e = 0; e < (int)E.size(); ++e)
            {
                const Elem& el = E[e];
                const bool active = filled[el.n[0]] || filled[el.n[1]] || filled[el.n[2]];
                if (!active) continue;
                for (int A = 0; A < 3; ++A)
                    for (int B = 0; B < 3; ++B)
                    {
                        const double kij = el.S * el.L[A][B];
                        if (kij == 0.0) continue;
                        const int i = el.n[A], j = el.n[B];
                        tFull.emplace_back(i, j, kij);
                        if (filled[i] && filled[j])
                            tFF.emplace_back(compact[i], compact[j], kij);
                    }
            }

            Kfull.setZero();
            Kfull.setFromTriplets(tFull.begin(), tFull.end());

            Eigen::SparseMatrix<double> Kff(nf, nf);
            Kff.setFromTriplets(tFF.begin(), tFF.end());

            Eigen::VectorXd bf = Eigen::VectorXd::Zero(nf);
            for (int i = 0; i < N; ++i)
                if (filled[i] && isGate[i]) bf[compact[i]] += Qgate;

            Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> chol;
            chol.compute(Kff);
            if (chol.info() != Eigen::Success)
            { R.message = "Pressure solve failed (matrix not factorable)."; return R; }
            Eigen::VectorXd xf = chol.solve(bf);
            if (chol.info() != Eigen::Success)
            { R.message = "Pressure solve failed (solve step)."; return R; }

            Eigen::VectorXd pfull = Eigen::VectorXd::Zero(N);
            for (int i = 0; i < N; ++i)
                if (filled[i]) pfull[i] = xf[compact[i]];

            for (int i = 0; i < N; ++i) lastP[i] = pfull[i];

            // Inflow into each front control volume: q_i = -(K p)_i.
            Eigen::VectorXd r = Kfull * pfull;

            double dtMin = std::numeric_limits<double>::infinity();
            for (int i : front)
            {
                const double q = -r[i];
                if (q > 1.0e-30 && V[i] > 0.0)
                {
                    const double dti = (1.0 - f[i]) * V[i] / q;
                    if (dti < dtMin) dtMin = dti;
                }
            }
            if (!(dtMin < std::numeric_limits<double>::infinity()))
                break;   // no inflow anywhere -> trapped remainder (short shot)

            t += dtMin;
            for (int i : front)
            {
                const double q = -r[i];
                if (q <= 0.0 || V[i] <= 0.0) continue;
                f[i] += q * dtMin / V[i];
                if (f[i] >= 1.0 - eps) { f[i] = 1.0; filled[i] = 1; tfill[i] = t; }
            }

            double gp = 0.0;
            for (int g : mesh.gateNodes)
                if (g >= 0 && g < N) gp = std::max(gp, pfull[g]);
            maxGateP = std::max(maxGateP, gp);

            if (params.maxPressureMPa > 0.0 && (gp / 1.0e6) > params.maxPressureMPa)
            { shortShot = true; ++step; break; }
        }

        // ---- Assemble results (convert SI -> reporting units) ----
        R.ok = true;
        R.steps = step;
        R.fillTimeS.assign(N, -1.0f);
        R.pressureMPa.assign(N, 0.0f);

        int filledNodes = 0;
        double filledVol = 0.0;
        double clampN = 0.0;
        double maxP = 0.0;
        double lastFill = 0.0;
        for (int i = 0; i < N; ++i)
        {
            R.fillTimeS[i]   = (tfill[i] >= 0.0) ? (float)tfill[i] : -1.0f;
            R.pressureMPa[i] = (float)(lastP[i] / 1.0e6);
            if (filled[i]) { ++filledNodes; filledVol += V[i]; lastFill = std::max(lastFill, tfill[i]); }
            maxP   = std::max(maxP, lastP[i]);
            clampN += lastP[i] * projA[i];
        }

        R.filledNodes      = filledNodes;
        R.fillTimeTotalS   = (float)lastFill;
        R.maxPressureMPa   = (float)(std::max(maxP, maxGateP) / 1.0e6);
        R.clampForceTonne  = (float)(clampN / kTonN);
        R.cavityVolumeMm3  = (float)(cavityVol * 1.0e9);    // m^3 -> mm^3
        R.projectedAreaMm2 = (float)(cavityProj * 1.0e6);   // m^2 -> mm^2
        R.shortShotPct     = (cavityVol > 0.0)
                             ? (float)(100.0 * (1.0 - filledVol / cavityVol)) : 0.0f;

        if (shortShot)
            R.message = "Short shot — injection pressure hit the machine limit before fill.";
        else if (filledNodes < N)
            R.message = "Incomplete fill — an unreachable / trapped region remained.";
        else
            R.message = "Fill complete.";
        return R;
    }

} // namespace Flow
