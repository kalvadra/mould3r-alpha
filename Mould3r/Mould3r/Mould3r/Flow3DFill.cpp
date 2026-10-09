// Flow3DFill.cpp — see Flow3DFill.h.
#include "Flow3DFill.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numeric>
#include <tuple>

#include "GapThermal.h"   // Flow::MeltViscosity (tabulated Cross-WLF, no-flow cut-off)

// Eigen: vcpkg installs under include/eigen3, which a raw .vcxproj doesn't
// add to the search path (see FlowSolver.cpp).
#if __has_include(<Eigen/Sparse>)
#include <Eigen/Sparse>
#elif __has_include(<eigen3/Eigen/Sparse>)
#include <eigen3/Eigen/Sparse>
#else
#error "Eigen not found. Ensure vcpkg installed eigen3 (see vcpkg.json)."
#endif
#if __has_include(<Eigen/IterativeLinearSolvers>)
#include <Eigen/IterativeLinearSolvers>
#else
#include <eigen3/Eigen/IterativeLinearSolvers>
#endif

// AMGCL (header-only, MIT; third_party/amgcl). Built without Boost: the
// parameter structs are set in code, never read from property trees.
#ifndef AMGCL_NO_BOOST
#define AMGCL_NO_BOOST
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100 4127 4146 4244 4267 4456 4457 4458 4459 4701 4702 4996)
#endif
#include <amgcl/adapter/crs_tuple.hpp>
#include <amgcl/amg.hpp>
#include <amgcl/backend/builtin.hpp>
#include <amgcl/coarsening/smoothed_aggregation.hpp>
#include <amgcl/make_solver.hpp>
#include <amgcl/preconditioner/schur_pressure_correction.hpp>
#include <amgcl/relaxation/as_preconditioner.hpp>
#include <amgcl/relaxation/spai0.hpp>
#include <amgcl/solver/fgmres.hpp>
#include <amgcl/solver/preonly.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Flow3D
{
    namespace
    {
        using Vec3 = std::array<double, 3>;
        inline double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        inline Vec3 Sub(const double* a, const double* b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
        inline Vec3 Cross(const Vec3& a, const Vec3& b)
        {
            return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
        }

        using Backend = amgcl::backend::builtin<double>;
        using Solver = amgcl::make_solver<
            amgcl::preconditioner::schur_pressure_correction<
                amgcl::make_solver<
                    amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::spai0>,
                    amgcl::solver::preonly<Backend>>,
                amgcl::make_solver<
                    amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::spai0>,
                    amgcl::solver::preonly<Backend>>>,
            amgcl::solver::fgmres<Backend>>;

        // Per-tet geometry.
        struct TetGeo
        {
            double V = 0.0;           // volume
            double h = 0.0;           // size (edge of the regular tet of this volume)
            Vec3   g[4];              // shape-function gradients
            bool   ok = false;
        };

        // One inlet face.
        struct InletFace
        {
            int32_t v[3];
            double  area;
            Vec3    n;                // outward unit normal
            int     region;
        };
    }

    FillResult RunIsothermalFill(const TetMesh::Mesh& mesh, const std::vector<int32_t>& nb,
                                 const Boundary& bc, const FillSetup& S)
    {
        using clock = std::chrono::steady_clock;
        const auto tStart = clock::now();
        FillResult R;
        const size_t nt = mesh.TetCount(), nn = mesh.VertexCount();
        if (nt == 0 || nb.size() != 4 * nt || !bc.Ready() || bc.slotTag.size() != 4 * nt)
        {
            R.message = "There is no tagged 3D mesh to fill.";
            return R;
        }
        if (!S.viscosity || !(S.flowRateMm3s > 0.0))
        {
            R.message = "The fill needs a viscosity model and an injection rate.";
            return R;
        }

        // ---- Geometry -------------------------------------------------------------
        std::vector<TetGeo> geo(nt);
        std::vector<double> cvVol(nn, 0.0);
        const double kRegular = 6.0 * std::sqrt(2.0);
        for (size_t t = 0; t < nt; ++t)
        {
            const int32_t* T = &mesh.tets[4 * t];
            const double* p0 = &mesh.verts[3 * (size_t)T[0]];
            const double* p1 = &mesh.verts[3 * (size_t)T[1]];
            const double* p2 = &mesh.verts[3 * (size_t)T[2]];
            const double* p3 = &mesh.verts[3 * (size_t)T[3]];
            const Vec3 e1 = Sub(p1, p0), e2 = Sub(p2, p0), e3 = Sub(p3, p0);
            const double sixV = Dot(e1, Cross(e2, e3));
            TetGeo& G = geo[t];
            if (!(sixV > 0.0)) continue;   // degenerate / inverted: left out of the solve
            G.V = sixV / 6.0;
            G.h = std::cbrt(kRegular * G.V);
            const Vec3 a = Cross(e2, e3), b = Cross(e3, e1), c = Cross(e1, e2);
            for (int k = 0; k < 3; ++k)
            {
                G.g[1][k] = a[k] / sixV;
                G.g[2][k] = b[k] / sixV;
                G.g[3][k] = c[k] / sixV;
                G.g[0][k] = -(G.g[1][k] + G.g[2][k] + G.g[3][k]);
            }
            G.ok = true;
            for (int k = 0; k < 4; ++k) cvVol[(size_t)T[k]] += 0.25 * G.V;
        }
        double totalVol = 0.0;
        for (double v : cvVol) totalVol += v;
        R.cavityVolumeMm3 = totalVol;

        // Node adjacency (for passing on the overfill of a node).
        std::vector<std::vector<int32_t>> adj(nn);
        for (size_t t = 0; t < nt; ++t)
        {
            const int32_t* T = &mesh.tets[4 * t];
            for (int a = 0; a < 4; ++a)
                for (int b = 0; b < 4; ++b)
                    if (a != b) adj[(size_t)T[a]].push_back(T[b]);
        }
        for (auto& v : adj)
        {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        // ---- Network --------------------------------------------------------------
        const int nNet = std::max(0, S.netNodes);
        std::vector<char> netReach((size_t)nNet, 0);
        if (S.sourceNode >= 0 && S.sourceNode < nNet)
        {
            std::vector<int> stack{ S.sourceNode };
            netReach[(size_t)S.sourceNode] = 1;
            while (!stack.empty())
            {
                const int k = stack.back();
                stack.pop_back();
                for (const FeedBeam& e : S.beams)
                {
                    int o = -1;
                    if (e.a == k) o = e.b;
                    else if (e.b == k) o = e.a;
                    if (o >= 0 && o < nNet && !netReach[(size_t)o]) { netReach[(size_t)o] = 1; stack.push_back(o); }
                }
            }
        }
        else
        {
            R.message = "The feed system has no machine inlet (sprue).";
            return R;
        }

        // Regions that can feed melt: an inlet region mapped to a reachable node.
        const size_t nReg = bc.regions.size();
        std::vector<int> regNet(nReg, -1);
        for (size_t r = 0; r < nReg; ++r)
        {
            if (bc.regions[r].tag != TagInlet) continue;
            const int k = r < S.regionNetNode.size() ? S.regionNetNode[r] : -1;
            if (k >= 0 && k < nNet && netReach[(size_t)k]) regNet[r] = k;
            else
                R.warnings.push_back(bc.regions[r].label + " is not connected to the machine inlet; it is "
                                     "treated as a closed wall.");
        }

        // ---- Boundary nodes -----------------------------------------------------------
        std::vector<char> wallNode(nn, 0), inletNode(nn, 0);
        std::vector<InletFace> inletFaces;
        for (size_t s = 0; s < 4 * nt; ++s)
        {
            const uint8_t tag = bc.slotTag[s];
            if (tag == TagInterior) continue;
            const int32_t* T = &mesh.tets[4 * (s / 4)];
            const int f = (int)(s % 4);
            const int32_t v[3] = { T[TetMesh::kTetFace[f][0]], T[TetMesh::kTetFace[f][1]], T[TetMesh::kTetFace[f][2]] };
            const int reg = bc.slotRegion[s];
            if (tag == TagInlet && reg >= 0 && (size_t)reg < nReg && regNet[(size_t)reg] >= 0)
            {
                const double* a = &mesh.verts[3 * (size_t)v[0]];
                const double* b = &mesh.verts[3 * (size_t)v[1]];
                const double* c = &mesh.verts[3 * (size_t)v[2]];
                Vec3 n = Cross(Sub(b, a), Sub(c, a));
                const double L = std::sqrt(Dot(n, n));
                if (!(L > 0.0)) continue;
                for (double& x : n) x /= L;
                inletFaces.push_back({ { v[0], v[1], v[2] }, 0.5 * L, n, reg });
                for (int32_t i : v) inletNode[(size_t)i] = 1;
            }
            else
                for (int32_t i : v) wallNode[(size_t)i] = 1;   // wall, parting line, vent, cut-off inlet
        }
        if (inletFaces.empty())
        {
            R.message = "No inlet on the mesh is connected to the machine, so nothing can fill it.";
            return R;
        }
        // The ring of wall faces around each inlet touches the inlet's rim
        // nodes, whose velocity is free: melt crosses those faces too, so they
        // join the inlet (same pressure, counted in its flux). Without them
        // the flux through the ring would leave the mesh unaccounted.
        {
            std::vector<int> nodeRegion(nn, -1);
            for (const InletFace& f : inletFaces)
                for (int32_t v : f.v) if (nodeRegion[(size_t)v] < 0) nodeRegion[(size_t)v] = f.region;
            for (size_t s = 0; s < 4 * nt; ++s)
            {
                const uint8_t tag = bc.slotTag[s];
                if (tag == TagInterior) continue;
                const int reg0 = bc.slotRegion[s];
                if (tag == TagInlet && reg0 >= 0 && (size_t)reg0 < nReg && regNet[(size_t)reg0] >= 0) continue;
                const int32_t* T = &mesh.tets[4 * (s / 4)];
                const int f = (int)(s % 4);
                const int32_t v[3] = { T[TetMesh::kTetFace[f][0]], T[TetMesh::kTetFace[f][1]], T[TetMesh::kTetFace[f][2]] };
                int reg = -1;
                for (int32_t i : v) if (inletNode[(size_t)i]) { reg = nodeRegion[(size_t)i]; break; }
                if (reg < 0) continue;
                const double* a = &mesh.verts[3 * (size_t)v[0]];
                const double* b = &mesh.verts[3 * (size_t)v[1]];
                const double* c = &mesh.verts[3 * (size_t)v[2]];
                Vec3 n = Cross(Sub(b, a), Sub(c, a));
                const double L = std::sqrt(Dot(n, n));
                if (!(L > 0.0)) continue;
                for (double& x : n) x /= L;
                inletFaces.push_back({ { v[0], v[1], v[2] }, 0.5 * L, n, reg });
            }
        }
        // Velocity is fixed (no slip) on walls; an inlet's rim stays free so a
        // gate only a few elements across still passes melt.
        std::vector<char> fixedU(nn, 0);
        for (size_t i = 0; i < nn; ++i) fixedU[i] = wallNode[i] && !inletNode[i];

        // ---- Thermal set-up ---------------------------------------------------------------
        const bool thermal = S.thermal.enabled;
        R.thermal = thermal;
        const double kMelt = S.thermal.k, rhoCp = std::max(1.0, S.thermal.rhoCp);
        const double alpha = kMelt / rhoCp;                      // m^2/s
        const double meltK = S.thermal.meltK, wallK = S.thermal.wallK, noFlowK = S.thermal.noFlowK;
        R.wallTempC = wallK - TestMaterial::kZeroC;
        Flow::MeltViscosity melt;
        double etaFrozen = 1.0e6;
        if (thermal)
        {
            melt.init(S.thermal.viscosity, noFlowK, std::min(wallK, noFlowK) - 20.0, meltK + 80.0);
            // Melt below the no-flow temperature: 100x the stiffest melt there
            // is. Effectively solid, without a contrast the solver can't take.
            etaFrozen = 100.0 * S.thermal.viscosity.eta0(noFlowK);
        }
        // Wall area per node (m^2) — the faces against the mould (wall, parting
        // line, vents), a third of each — and each tet's depth across its wall
        // layer (mm), for the frozen skin.
        std::vector<double> wallArea(nn, 0.0);
        for (size_t s = 0; s < 4 * nt; ++s)
        {
            const uint8_t tag = bc.slotTag[s];
            if (tag == TagInterior || tag == TagInlet) continue;
            const int32_t* T = &mesh.tets[4 * (s / 4)];
            const int f = (int)(s % 4);
            const int32_t v[3] = { T[TetMesh::kTetFace[f][0]], T[TetMesh::kTetFace[f][1]], T[TetMesh::kTetFace[f][2]] };
            const Vec3 n = Cross(Sub(&mesh.verts[3 * (size_t)v[1]], &mesh.verts[3 * (size_t)v[0]]),
                                 Sub(&mesh.verts[3 * (size_t)v[2]], &mesh.verts[3 * (size_t)v[0]]));
            const double areaM2 = 0.5 * std::sqrt(Dot(n, n)) * 1.0e-6;
            for (int32_t i : v) wallArea[(size_t)i] += areaM2 / 3.0;
        }
        // Each node's element depth (mm): 0.8 x the mean size of its tets — the
        // thickness of the layer a wall node stands for.
        std::vector<double> nodeEllMm(nn, 0.0);
        {
            std::vector<int> cnt(nn, 0);
            for (size_t tt = 0; tt < nt; ++tt)
            {
                if (!geo[tt].ok) continue;
                for (int k = 0; k < 4; ++k)
                {
                    nodeEllMm[(size_t)mesh.tets[4 * tt + (size_t)k]] += geo[tt].h;
                    cnt[(size_t)mesh.tets[4 * tt + (size_t)k]] += 1;
                }
            }
            for (size_t i = 0; i < nn; ++i) nodeEllMm[i] = cnt[i] ? 0.8 * nodeEllMm[i] / cnt[i] : 1.0;
        }
        std::vector<double> Tk(nn, meltK);           // bulk melt temperature per node [K]
        std::vector<double> skinMm(nn, 0.0);         // frozen skin at wall nodes [mm]
        std::vector<double> skinMult(nt, 1.0);       // viscosity factor from the skin
        if (thermal)
        {
            R.frontTempC.assign(nn, FillResult::kNotFilledC);
            for (size_t i = 0; i < nn; ++i)
                if (inletNode[i]) R.frontTempC[i] = (float)(meltK - TestMaterial::kZeroC);
        }
        // Frozen skin depth (mm) after contact time tc (s) of melt at Tb against
        // the wall: where Tw + (Tb - Tw) erf(z / 2 sqrt(alpha tc)) = Tnf.
        auto skinDepth = [&](double Tb, double tc) -> double
        {
            if (!(tc > 0.0)) return 0.0;
            if (Tb <= noFlowK) return 1.0e9;                        // frozen through
            const double y = std::clamp((noFlowK - wallK) / (Tb - wallK), 0.0, 0.999999);
            // erf^-1(y) by Newton from a good start.
            double x = y < 0.7 ? y * (0.886226925 + 0.232013666 * y * y) : std::sqrt(-std::log((1.0 - y) / 2.0));
            for (int it = 0; it < 30; ++it)
            {
                const double e = std::erf(x) - y;
                const double d = 1.1283791670955126 * std::exp(-x * x);
                const double step = e / d;
                x -= step;
                if (std::fabs(step) < 1e-12) break;
            }
            return 2.0 * std::sqrt(alpha * tc) * std::max(0.0, x) * 1.0e3;   // m -> mm
        };
        // The frozen-layer value reported for a filled node (see frameFrozenMm).
        auto frozenValue = [&](size_t i) -> float
        {
            if (Tk[i] < noFlowK) return FillResult::kFrozenThrough;
            return wallArea[i] > 0.0 ? (float)std::min(skinMm[i], 1.0e3) : 0.0f;
        };

        // ---- State ----------------------------------------------------------------------
        std::vector<double> F(nn, 0.0);                  // fill fraction of each node's volume
        R.fillTimeS.assign(nn, -1.0f);
        for (size_t i = 0; i < nn; ++i)
            if (inletNode[i]) { F[i] = 1.0; R.fillTimeS[i] = 0.0f; }
        auto filled = [&F](size_t i) { return F[i] >= 1.0 - 1e-9; };

        std::vector<double> U(3 * nn, 0.0), P(nn, 0.0);   // last solution (mm/s, Pa)
        std::vector<double> netP((size_t)nNet, 0.0);
        std::vector<double> tetEta(nt, 0.0), tetGamma(nt, 0.0), tetTau(nt, 0.0);
        std::vector<char> tetSeen(nt, 0);                // has a solved velocity field
        std::vector<double> beamFlow(S.beams.size(), S.flowRateMm3s);
        const double gamma0 = 300.0;                      // first-guess shear rate [1/s]

        // Viscosity. Feed beams run at the melt temperature (the 1D feed is
        // isothermal); tets at their mean filled-node temperature, frozen below
        // the no-flow temperature and stiffened by their frozen skin.
        auto beamEta = [&](double gd)
        {
            const double e = thermal ? melt.eta(std::max(gd, 1.0e-3), meltK) : S.viscosity(std::max(gd, 1.0e-3));
            return std::isfinite(e) && e > 0.0 ? e : 1.0e3;
        };
        auto tetEtaAt = [&](size_t tt, double gd)
        {
            if (!thermal) return beamEta(gd);
            const int32_t* Tn = &mesh.tets[4 * tt];
            double sum = 0.0;
            int cnt = 0;
            for (int k = 0; k < 4; ++k)
                if (F[(size_t)Tn[k]] > 0.0) { sum += Tk[(size_t)Tn[k]]; ++cnt; }
            const double Tt = cnt ? sum / cnt : meltK;
            if (Tt < noFlowK) return etaFrozen;
            const double e = melt.eta(std::max(gd, 1.0e-3), Tt) * skinMult[tt];
            return std::isfinite(e) && e > 0.0 ? std::min(e, etaFrozen) : 1.0e3;
        };

        const double Q = S.flowRateMm3s;
        const double pMax = S.maxPressureMPa * 1.0e6;
        // Progress: the fill's share of the bar (packing takes the rest).
        const double progFill = (thermal && S.pack.enabled) ? 0.5 : 1.0;
        bool pressureMode = false;
        double t = 0.0;
        double filledVol = 0.0;
        for (size_t i = 0; i < nn; ++i) if (filled(i)) filledVol += cvVol[i];
        const int maxSteps = std::max(50, 40 * S.targetSteps);
        const double dtVolTarget = totalVol / (std::max(10, S.targetSteps) * Q);
        int nextSnapshot = 0;
        double itersSum = 0.0, negFlux = 0.0, posFlux = 0.0;

        std::vector<int> uIdx(nn), pIdx(nn), netIdx((size_t)nNet);
        std::vector<char> tetActive(nt, 0);
        std::vector<double> x;          // warm start / solution
        std::vector<double> lastQreg(nReg, 0.0);
        double pool = 0.0;              // overfill not yet placed (volume)
        int stallSteps = 0;             // consecutive steps with (almost) no flow
        std::vector<double> inflowSum(nn, 0.0), inflowT(nn, 0.0);   // melt arriving at front nodes

        // ---- Thermal step -------------------------------------------------------------------
        // Implicit over dt from tNow: capacity, upwind advection through the
        // element-pair fluxes of the last flow solve, conduction between filled
        // nodes, shear heating (shearHeat), the sub-grid wall loss and the skin.
        // Inlet nodes hold the melt temperature, or (inletFree, packing, when
        // the inflow is slow) take the melt entering at it.
        std::vector<double> inletIn(nn, 0.0);   // inflow per inlet node [mm^3/s] (inletFree)
        auto thermalStep = [&](double dt, double tNow, bool shearHeat, bool inletFree)
        {
            const auto tTherm = clock::now();
            std::vector<int> eIdx(nn, -1);
            int nE = 0;
            for (size_t i = 0; i < nn; ++i)
                if (filled(i) && (inletFree || !inletNode[i])) eIdx[i] = nE++;
            std::fill(inflowSum.begin(), inflowSum.end(), 0.0);
            std::fill(inflowT.begin(), inflowT.end(), 0.0);
            std::vector<Eigen::Triplet<double>> et;
            et.reserve((size_t)nE * 20);
            std::vector<double> eRhs((size_t)nE, 0.0), eDiag((size_t)nE, 0.0);
            const double c9 = rhoCp * 1.0e-9;   // J/(mm^3 K)
            for (size_t tt = 0; tt < nt; ++tt)
            {
                if (!tetActive[tt]) continue;
                const TetGeo& G = geo[tt];
                const int32_t* Tn = &mesh.tets[4 * tt];
                Vec3 w{ 0, 0, 0 };   // ubar - tau grad p: the conservative flux velocity [mm/s]
                for (int k = 0; k < 4; ++k)
                    for (int c = 0; c < 3; ++c)
                        w[(size_t)c] += 0.25 * U[3 * (size_t)Tn[k] + (size_t)c] - tetTau[tt] * P[(size_t)Tn[k]] * G.g[k][c];
                bool allFilled = true;
                for (int k = 0; k < 4; ++k) allFilled = allFilled && filled((size_t)Tn[k]);
                // Shear heating eta gammaDot^2, shared by the tet's filled nodes:
                // the melt's own viscosity (not the skin's stiffening, which
                // stands for lost channel, not work), none in frozen melt.
                int nFill = 0;
                double Tsum = 0.0;
                for (int k = 0; k < 4; ++k)
                    if (filled((size_t)Tn[k])) { ++nFill; Tsum += Tk[(size_t)Tn[k]]; }
                const double Tt = nFill ? Tsum / nFill : meltK;
                const double heat = (shearHeat && nFill && Tt >= noFlowK)
                    ? melt.eta(std::max(tetGamma[tt], 1.0e-3), Tt) * tetGamma[tt] * tetGamma[tt] * G.V * 1.0e-9   // W
                    : 0.0;
                for (int a = 0; a < 4; ++a)
                {
                    const size_t ia = (size_t)Tn[a];
                    const int ea = eIdx[ia];
                    if (ea >= 0 && nFill > 0) eRhs[(size_t)ea] += heat / nFill;
                    for (int b = 0; b < 4; ++b)
                    {
                        if (b == a) continue;
                        const size_t ib = (size_t)Tn[b];
                        Vec3 dg{ G.g[a][0] - G.g[b][0], G.g[a][1] - G.g[b][1], G.g[a][2] - G.g[b][2] };
                        const double fab = 0.25 * G.V * Dot(dg, w);   // mm^3/s into a from b
                        if (fab > 0.0)
                        {
                            if (ea >= 0)
                            {
                                eDiag[(size_t)ea] += c9 * fab;
                                if (eIdx[ib] >= 0) et.emplace_back(ea, eIdx[ib], -c9 * fab);
                                else eRhs[(size_t)ea] += c9 * fab * Tk[ib];
                            }
                            else if (!filled(ia) && F[ib] > 0.0)
                            {
                                inflowSum[ia] += fab;
                                inflowT[ia] += fab * Tk[ib];
                            }
                        }
                        if (allFilled && ea >= 0)
                        {
                            const double kab = kMelt * G.V * Dot(G.g[a], G.g[b]) * 1.0e-3;   // W/K
                            if (eIdx[ib] >= 0) et.emplace_back(ea, eIdx[ib], kab);
                            else eRhs[(size_t)ea] -= kab * Tk[ib];
                        }
                    }
                    if (allFilled && ea >= 0)
                        eDiag[(size_t)ea] += kMelt * G.V * Dot(G.g[a], G.g[a]) * 1.0e-3;
                }
            }
            for (size_t i = 0; i < nn; ++i)
            {
                const int e = eIdx[i];
                if (e < 0) continue;
                const double cap = c9 * cvVol[i] / dt;
                eDiag[(size_t)e] += cap;
                eRhs[(size_t)e] += cap * Tk[i];
                if (inletFree && inletIn[i] > 0.0)   // melt entering at the melt temperature
                {
                    eDiag[(size_t)e] += c9 * inletIn[i];
                    eRhs[(size_t)e] += c9 * inletIn[i] * meltK;
                }
                if (wallArea[i] > 0.0)
                {
                    // Semi-infinite melt against the wall since the melt arrived:
                    // flux k (Tb - Tw) / sqrt(pi alpha t). The node stands for the
                    // layer 0..d from the wall (d = half its element depth), so
                    // its temperature is that layer's mean of the erf profile,
                    // Tw + (Tb - Tw) A(x), x = d / (2 sqrt(alpha t)): the flux in
                    // terms of it is h (T - Tw) with h = k / (A sqrt(pi alpha t)).
                    // Early (thin boundary layer) A -> 1; late it tends to the
                    // conduction across the layer, 2k/d.
                    const double tc = std::max(tNow + dt - (double)R.fillTimeS[i], 0.5 * dt);
                    const double sq = std::sqrt(alpha * tc);
                    const double x = 0.5 * nodeEllMm[i] * 1.0e-3 / (2.0 * sq);
                    const double A = x > 1.0e-6 ? (x * std::erf(x) + (std::exp(-x * x) - 1.0) / 1.7724538509055159) / x
                                                : x / 1.7724538509055159;
                    const double hW = kMelt / (std::max(A, 1.0e-9) * 1.7724538509055159 * sq);   // W/(m^2 K)
                    eDiag[(size_t)e] += wallArea[i] * hW;
                    eRhs[(size_t)e] += wallArea[i] * hW * wallK;
                }
                et.emplace_back(e, e, eDiag[(size_t)e]);
            }
            if (nE > 0)
            {
                Eigen::SparseMatrix<double> EA(nE, nE);
                EA.setFromTriplets(et.begin(), et.end());
                Eigen::Map<Eigen::VectorXd> b(eRhs.data(), nE);
                Eigen::VectorXd x0(nE);
                for (size_t i = 0; i < nn; ++i) if (eIdx[i] >= 0) x0[eIdx[i]] = Tk[i];
                Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::DiagonalPreconditioner<double>> bicg;
                bicg.setTolerance(1.0e-10);
                bicg.setMaxIterations(500);
                bicg.compute(EA);
                const Eigen::VectorXd xs = bicg.solveWithGuess(b, x0);
                for (size_t i = 0; i < nn; ++i)
                    if (eIdx[i] >= 0 && std::isfinite(xs[eIdx[i]]))
                        Tk[i] = std::clamp(xs[eIdx[i]], std::min(wallK, meltK) - 1.0, meltK + 150.0);
            }
            // Frozen skin at the wall nodes, and the stiffening it gives the
            // tets along the wall (the conductance lost, ~ (1 - skin/depth)^3).
            // The skin grows against the melt core: the wall node's own
            // temperature already averages in the cooled layer, so take the
            // interior neighbours' when they are hotter.
            for (size_t i = 0; i < nn; ++i)
            {
                skinMm[i] = 0.0;
                if (!(wallArea[i] > 0.0) || !filled(i) || R.fillTimeS[i] < 0.0f) continue;
                double core = 0.0;
                int nc = 0;
                for (int32_t j : adj[i])
                    if (wallArea[(size_t)j] <= 0.0 && filled((size_t)j)) { core += Tk[(size_t)j]; ++nc; }
                const double Tfar = nc ? std::max(Tk[i], core / nc) : Tk[i];
                skinMm[i] = skinDepth(Tfar, tNow + dt - (double)R.fillTimeS[i]);
            }
            for (size_t tt = 0; tt < nt; ++tt)
            {
                skinMult[tt] = 1.0;
                if (!geo[tt].ok) continue;
                const int32_t* Tn = &mesh.tets[4 * tt];
                double sk = 0.0;
                for (int k = 0; k < 4; ++k) sk += std::min(skinMm[(size_t)Tn[k]], 1.0e3);
                if (sk <= 0.0) continue;
                const double r = std::min(0.95, sk / (3.0 * 0.8 * geo[tt].h));
                skinMult[tt] = std::min(1.0e3, 1.0 / ((1.0 - r) * (1.0 - r) * (1.0 - r)));
            }
            R.stats.thermalSeconds += std::chrono::duration<double>(clock::now() - tTherm).count();
        };

        for (int step = 0; step < maxSteps; ++step)
        {
            // ---- Active domain: tets touching a filled node -----------------------------
            size_t nActive = 0;
            for (size_t tt = 0; tt < nt; ++tt)
            {
                tetActive[tt] = 0;
                if (!geo[tt].ok) continue;
                const int32_t* T = &mesh.tets[4 * tt];
                for (int k = 0; k < 4; ++k)
                    if (filled((size_t)T[k])) { tetActive[tt] = 1; break; }
                nActive += tetActive[tt];
            }
            if (nActive == 0) { R.message = "The melt couldn't enter the mesh."; return R; }

            std::fill(uIdx.begin(), uIdx.end(), -1);
            std::fill(pIdx.begin(), pIdx.end(), -1);
            int nU = 0, nP = 0;
            std::vector<char> nodeActive(nn, 0);
            for (size_t tt = 0; tt < nt; ++tt)
                if (tetActive[tt])
                    for (int k = 0; k < 4; ++k) nodeActive[(size_t)mesh.tets[4 * tt + (size_t)k]] = 1;
            for (size_t i = 0; i < nn; ++i)
            {
                if (!nodeActive[i]) continue;
                if (!fixedU[i]) uIdx[i] = nU++;
                if (filled(i)) pIdx[i] = nP++;
            }

            // Viscosity per tet: lagged from the last solution (first guess where new).
            double etaSum = 0.0, hSum = 0.0;
            for (size_t tt = 0; tt < nt; ++tt)
            {
                if (!tetActive[tt]) continue;
                if (!tetSeen[tt]) tetGamma[tt] = std::max(tetGamma[tt], gamma0);
                tetEta[tt] = tetEtaAt(tt, tetGamma[tt]);
                etaSum += std::log(tetEta[tt]);
                hSum += geo[tt].h;
            }
            // Geometric mean: frozen tets mustn't swamp the scale.
            const double etaRef = std::exp(etaSum / (double)nActive), hRef = hSum / (double)nActive;
            const double sig = etaRef / hRef;   // pressure scaling: balances the blocks

            // Picard passes on the viscosity: several on the first step (no
            // field to lag from); after that the viscosity lags one step (the
            // flow changes little between steps) unless it moved a lot.
            const int maxPasses = (step == 0) ? 6 : 2;
            double Qin = 0.0, machineP = 0.0;
            std::vector<double> rate(nn, 0.0);
            for (int pass = 0; pass < maxPasses; ++pass)
            {
                // Network unknowns: reachable nodes; the source is fixed in pressure mode.
                std::fill(netIdx.begin(), netIdx.end(), -1);
                int nN = 0;
                for (int k = 0; k < nNet; ++k)
                    if (netReach[(size_t)k] && !(pressureMode && k == S.sourceNode)) netIdx[(size_t)k] = nN++;
                const size_t nU3 = 3 * (size_t)nU;
                const size_t nDof = nU3 + (size_t)nP + (size_t)nN;
                R.stats.maxUnknowns = std::max(R.stats.maxUnknowns, nDof);
                auto uD = [&](size_t i, int c) { return (ptrdiff_t)(3 * (size_t)uIdx[i] + (size_t)c); };
                auto pD = [&](size_t i) { return (ptrdiff_t)(nU3 + (size_t)pIdx[i]); };
                auto nD = [&](int k) { return (ptrdiff_t)(nU3 + (size_t)nP + (size_t)netIdx[(size_t)k]); };

                std::vector<Eigen::Triplet<double, ptrdiff_t>> trip;
                trip.reserve(nActive * 120);
                std::vector<double> rhs(nDof, 0.0);

                for (size_t tt = 0; tt < nt; ++tt)
                {
                    if (!tetActive[tt]) continue;
                    const TetGeo& G = geo[tt];
                    const int32_t* T = &mesh.tets[4 * tt];
                    const double eta = tetEta[tt];
                    const double tau = S.pspgAlpha * G.h * G.h / eta;
                    tetTau[tt] = tau;   // the fluxes below must use the same tau
                    for (int a = 0; a < 4; ++a)
                    {
                        const size_t ia = (size_t)T[a];
                        for (int b = 0; b < 4; ++b)
                        {
                            const size_t ib = (size_t)T[b];
                            const double gg = Dot(G.g[a], G.g[b]) * G.V;
                            if (uIdx[ia] >= 0 && uIdx[ib] >= 0)
                                for (int c = 0; c < 3; ++c) trip.emplace_back(uD(ia, c), uD(ib, c), eta * gg);
                            if (pIdx[ia] >= 0 && pIdx[ib] >= 0)
                                trip.emplace_back(pD(ia), pD(ib), -tau * gg * sig * sig);
                            if (pIdx[ia] >= 0 && uIdx[ib] >= 0)
                                for (int c = 0; c < 3; ++c)
                                {
                                    const double v = -0.25 * G.V * G.g[b][c] * sig;
                                    trip.emplace_back(pD(ia), uD(ib, c), v);
                                    trip.emplace_back(uD(ib, c), pD(ia), v);
                                }
                        }
                    }
                }
                // Inlet flux coupling: each region's pressure acts on its faces.
                for (const InletFace& f : inletFaces)
                {
                    const int k = regNet[(size_t)f.region];
                    for (int32_t vi : f.v)
                    {
                        const size_t i = (size_t)vi;
                        if (uIdx[i] < 0) continue;
                        for (int c = 0; c < 3; ++c)
                        {
                            const double e = f.area / 3.0 * f.n[(size_t)c] * sig;
                            if (netIdx[(size_t)k] >= 0)
                            {
                                trip.emplace_back(uD(i, c), nD(k), e);
                                trip.emplace_back(nD(k), uD(i, c), e);
                            }
                            else
                                rhs[(size_t)uD(i, c)] -= e * (pMax / sig);
                        }
                    }
                }
                // Feed beams: -K block (conductance G / (eta L)).
                for (size_t e = 0; e < S.beams.size(); ++e)
                {
                    const FeedBeam& fb = S.beams[e];
                    if (fb.a < 0 || fb.b < 0 || fb.a >= nNet || fb.b >= nNet) continue;
                    if (!netReach[(size_t)fb.a] || !netReach[(size_t)fb.b]) continue;
                    const double gd = Flow::ApparentWallShearRate(fb.section, std::fabs(beamFlow[e]));
                    const double eta = beamEta(gd);
                    const double cond = fb.section.conductanceMm4 / (eta * std::max(fb.lengthMm, 1.0e-3)) * sig * sig;
                    const int ends[2] = { fb.a, fb.b };
                    for (int s = 0; s < 2; ++s)
                    {
                        const int me = ends[s], other = ends[1 - s];
                        if (netIdx[(size_t)me] < 0) continue;
                        trip.emplace_back(nD(me), nD(me), -cond);
                        if (netIdx[(size_t)other] >= 0) trip.emplace_back(nD(me), nD(other), cond);
                        else rhs[(size_t)nD(me)] -= cond * (pMax / sig);
                    }
                }
                if (!pressureMode) rhs[(size_t)nD(S.sourceNode)] -= Q * sig;
                // Every pressure-like row keeps a stored diagonal (even 0): the
                // Schur preconditioner adjusts it in place, and an inlet node with
                // no feed beams (full shot) would otherwise have none.
                for (size_t d = nU3; d < nDof; ++d) trip.emplace_back((ptrdiff_t)d, (ptrdiff_t)d, 0.0);

                Eigen::SparseMatrix<double, Eigen::RowMajor, ptrdiff_t> K((ptrdiff_t)nDof, (ptrdiff_t)nDof);
                K.setFromTriplets(trip.begin(), trip.end());
                trip.clear();
                trip.shrink_to_fit();
                K.makeCompressed();
                const ptrdiff_t rows = (ptrdiff_t)nDof;
                std::vector<ptrdiff_t> ptr(K.outerIndexPtr(), K.outerIndexPtr() + rows + 1);
                std::vector<ptrdiff_t> col(K.innerIndexPtr(), K.innerIndexPtr() + K.nonZeros());
                std::vector<double> val(K.valuePtr(), K.valuePtr() + K.nonZeros());
                K.resize(0, 0);
                K.data().squeeze();

                // Warm start from the last solution.
                x.assign(nDof, 0.0);
                for (size_t i = 0; i < nn; ++i)
                {
                    if (uIdx[i] >= 0) for (int c = 0; c < 3; ++c) x[(size_t)uD(i, c)] = U[3 * i + (size_t)c];
                    if (pIdx[i] >= 0) x[(size_t)pD(i)] = P[i] / sig;
                }
                for (int k = 0; k < nNet; ++k) if (netIdx[(size_t)k] >= 0) x[(size_t)nD(k)] = netP[(size_t)k] / sig;

                Solver::params prm;
                prm.solver.tol = S.solverTolerance;
                prm.solver.maxiter = S.solverMaxIterations;
                prm.solver.M = 60;
                prm.precond.adjust_p = 2;   // the Schur approximation keeps Kpu dia(Kuu)^-1 Kup whole
                prm.precond.pmask.assign(nDof, 0);
                for (size_t d = nU3; d < nDof; ++d) prm.precond.pmask[d] = 1;
                const auto tSolve = clock::now();
                int iters = 0;
                double resid = 0.0;
                {
                    auto A = std::tie(rows, ptr, col, val);
                    Solver solve(A, prm);
                    std::tie(iters, resid) = solve(A, rhs, x);
                }
                R.stats.solveSeconds += std::chrono::duration<double>(clock::now() - tSolve).count();
                R.stats.solves += 1;
                itersSum += iters;
                R.stats.maxIterations = std::max(R.stats.maxIterations, iters);
                R.stats.worstResidual = std::max(R.stats.worstResidual, resid);
                if (!std::isfinite(resid))
                {
                    R.message = "The flow solve broke down (non-finite residual).";
                    return R;
                }

                // Unpack.
                std::fill(P.begin(), P.end(), 0.0);
                for (size_t i = 0; i < nn; ++i)
                {
                    for (int c = 0; c < 3; ++c) U[3 * i + (size_t)c] = uIdx[i] >= 0 ? x[(size_t)uD(i, c)] : 0.0;
                    if (pIdx[i] >= 0) P[i] = x[(size_t)pD(i)] * sig;
                }
                for (int k = 0; k < nNet; ++k)
                    netP[(size_t)k] = netIdx[(size_t)k] >= 0 ? x[(size_t)nD(k)] * sig : (k == S.sourceNode ? pMax : 0.0);
                for (size_t e = 0; e < S.beams.size(); ++e)
                {
                    const FeedBeam& fb = S.beams[e];
                    if (fb.a < 0 || fb.b < 0 || fb.a >= nNet || fb.b >= nNet) continue;
                    const double gd = Flow::ApparentWallShearRate(fb.section, std::fabs(beamFlow[e]));
                    const double cond = fb.section.conductanceMm4 / (beamEta(gd) * std::max(fb.lengthMm, 1.0e-3));
                    beamFlow[e] = cond * (netP[(size_t)fb.a] - netP[(size_t)fb.b]);
                }
                machineP = netP[(size_t)S.sourceNode];

                // New shear rates -> viscosity change.
                double maxRel = 0.0;
                for (size_t tt = 0; tt < nt; ++tt)
                {
                    if (!tetActive[tt]) continue;
                    const TetGeo& G = geo[tt];
                    const int32_t* T = &mesh.tets[4 * tt];
                    double L[3][3] = {};   // velocity gradient du_i/dx_j
                    for (int k = 0; k < 4; ++k)
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j) L[i][j] += U[3 * (size_t)T[k] + (size_t)i] * G.g[k][j];
                    double dd = 0.0;   // 2 D:D
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                        {
                            const double d = 0.5 * (L[i][j] + L[j][i]);
                            dd += 2.0 * d * d;
                        }
                    tetGamma[tt] = std::sqrt(dd);
                    const double eNew = tetEtaAt(tt, tetGamma[tt]);
                    // Tets new to the melt this step started from a guess; only
                    // the ones with a lagged value decide whether to re-solve.
                    if (tetSeen[tt] || step == 0)
                        maxRel = std::max(maxRel, std::fabs(eNew - tetEta[tt]) / tetEta[tt]);
                    tetSeen[tt] = 1;
                    tetEta[tt] = eNew;
                }

                // Pressure control: past the machine limit, hold the limit.
                if (!pressureMode && machineP > pMax)
                {
                    pressureMode = true;
                    R.pressureLimited = true;
                    continue;
                }

                // Front fluxes from the discrete continuity (conservative).
                std::fill(rate.begin(), rate.end(), 0.0);
                for (size_t tt = 0; tt < nt; ++tt)
                {
                    if (!tetActive[tt]) continue;
                    const TetGeo& G = geo[tt];
                    const int32_t* T = &mesh.tets[4 * tt];
                    Vec3 ubar{ 0, 0, 0 }, gp{ 0, 0, 0 };
                    for (int k = 0; k < 4; ++k)
                        for (int c = 0; c < 3; ++c)
                        {
                            ubar[(size_t)c] += 0.25 * U[3 * (size_t)T[k] + (size_t)c];
                            gp[(size_t)c] += P[(size_t)T[k]] * G.g[k][c];
                        }
                    const double tau = tetTau[tt];
                    for (int a = 0; a < 4; ++a)
                    {
                        const size_t ia = (size_t)T[a];
                        if (filled(ia)) continue;
                        rate[ia] += G.V * (Dot(G.g[a], ubar) - tau * Dot(G.g[a], gp));
                    }
                }
                Qin = 0.0;
                for (double r : rate) Qin += r;

                // In pressure control the rate may climb back above the target.
                if (pressureMode && Qin > Q * 1.001)
                {
                    pressureMode = false;
                    continue;
                }
                if (maxRel < (step == 0 ? 0.1 : 0.5)) break;   // viscosity settled (enough)
            }
            if (pressureMode && Qin > Q) Qin = Q;
            R.maxInletMPa = std::max(R.maxInletMPa, machineP * 1.0e-6);
            for (size_t r = 0; r < nReg; ++r) lastQreg[r] = 0.0;
            for (const InletFace& f : inletFaces)
            {
                double qn = 0.0;
                for (int32_t vi : f.v)
                    qn -= Dot(Vec3{ U[3 * (size_t)vi], U[3 * (size_t)vi + 1], U[3 * (size_t)vi + 2] }, f.n) / 3.0;
                lastQreg[(size_t)f.region] += qn * f.area;
            }

            // ---- Advance the front -----------------------------------------------------
            double sumPos = 0.0, sumNeg = 0.0;
            for (size_t i = 0; i < nn; ++i)
            {
                if (rate[i] > 0.0) sumPos += rate[i];
                else if (rate[i] < 0.0) sumNeg -= rate[i];
            }
            posFlux += sumPos;
            negFlux += sumNeg;
            if (!(sumPos > 0.0) || !(Qin > 0.0))
            {
                R.incomplete = filledVol < totalVol * (1.0 - 1e-6);
                break;
            }
            // Backflow into already-empty front nodes is dropped; the rest is
            // scaled so the front takes exactly the inflow.
            const double scale = Qin / sumPos;
            double dtNode = 1e300;
            for (size_t i = 0; i < nn; ++i)
            {
                if (rate[i] <= 0.0) { rate[i] = 0.0; continue; }
                rate[i] *= scale;
                dtNode = std::min(dtNode, (1.0 - F[i]) * cvVol[i] / rate[i]);
            }
            const double dt = std::max(dtNode, dtVolTarget);

            // A fill that has all but stopped at the pressure limit (frozen off),
            // or crawls on past four times its target time, is a short shot.
            if (pressureMode && Qin < 0.01 * Q) ++stallSteps;
            else stallSteps = 0;
            if (stallSteps >= 3 || (pressureMode && t > 4.0 * totalVol / Q) || t > 20.0 * totalVol / Q)
            {
                R.stalled = true;
                R.incomplete = true;
                break;
            }

            // ---- Thermal step: the filled melt over dt ----------------------------------
            if (thermal) thermalStep(dt, t, true, false);
            // The melt arriving at a front node: its upstream mix (first arrival
            // takes it outright; after that it mixes by volume).
            auto arrivingT = [&](size_t i) { return inflowSum[i] > 0.0 ? inflowT[i] / inflowSum[i] : meltK; };
            auto mixInto = [&](size_t i, double addVol, double Tin)
            {
                if (!thermal || addVol <= 0.0) return;
                const double have = F[i] * cvVol[i];
                Tk[i] = have > 0.0 ? (have * Tk[i] + addVol * Tin) / (have + addVol) : Tin;
            };

            std::vector<double> overflow;   // (node, excess volume) pairs flattened
            const double poolShare = pool / (Qin * dt);   // last step's leftover, spread over the front
            pool = 0.0;
            for (size_t i = 0; i < nn; ++i)
            {
                if (rate[i] <= 0.0) continue;
                const double before = F[i];
                const double add = rate[i] * dt * (1.0 + poolShare);
                const double room = (1.0 - before) * cvVol[i];
                mixInto(i, std::min(add, room), arrivingT(i));
                if (add >= room)
                {
                    F[i] = 1.0;
                    const double tf = t + room / rate[i];
                    R.fillTimeS[i] = (float)tf;
                    if (thermal) R.frontTempC[i] = (float)(Tk[i] - TestMaterial::kZeroC);
                    if (add > room) { overflow.push_back((double)i); overflow.push_back(add - room); }
                    filledVol += room;
                }
                else
                {
                    F[i] = before + add / cvVol[i];
                    filledVol += add;
                }
            }
            // Pass each overfilled node's excess on to its unfilled neighbours
            // (a few rings out), so the front keeps moving where the flow is.
            for (int ring = 0; ring < 4 && !overflow.empty(); ++ring)
            {
                std::vector<double> next;
                for (size_t k = 0; k + 1 < overflow.size(); k += 2)
                {
                    const size_t i = (size_t)overflow[k];
                    double excess = overflow[k + 1];
                    std::vector<int32_t> open;
                    for (int32_t j : adj[i]) if (!filled((size_t)j)) open.push_back(j);
                    if (open.empty())
                    {
                        // Nowhere next to it: hand it to a filled neighbour to pass on.
                        for (int32_t j : adj[i]) { next.push_back((double)j); next.push_back(excess); break; }
                        continue;
                    }
                    double roomSum = 0.0;
                    for (int32_t j : open) roomSum += (1.0 - F[(size_t)j]) * cvVol[(size_t)j];
                    const double give = std::min(excess, roomSum);
                    for (int32_t j : open)
                    {
                        const size_t jj = (size_t)j;
                        const double room = (1.0 - F[jj]) * cvVol[jj];
                        const double part = roomSum > 0.0 ? give * room / roomSum : 0.0;
                        mixInto(jj, part, Tk[i]);
                        F[jj] += part / cvVol[jj];
                        filledVol += part;
                        if (F[jj] >= 1.0 - 1e-9)
                        {
                            F[jj] = 1.0;
                            R.fillTimeS[jj] = (float)(t + dt);
                            if (thermal) R.frontTempC[jj] = (float)(Tk[jj] - TestMaterial::kZeroC);
                        }
                    }
                    excess -= give;
                    if (excess > 1e-12 * totalVol) { next.push_back((double)i); next.push_back(excess); }
                }
                overflow.swap(next);
            }
            for (size_t k = 0; k + 1 < overflow.size(); k += 2) pool += overflow[k + 1];
            t += dt;
            R.stats.steps += 1;

            // ---- Record ------------------------------------------------------------------
            FillFrame fr;
            fr.timeS = (float)t;
            fr.filledPct = (float)(100.0 * std::min(1.0, filledVol / totalVol));
            fr.inletMPa = (float)(machineP * 1.0e-6);
            fr.flowMm3s = (float)Qin;
            if (filledVol / totalVol >= (double)nextSnapshot / std::max(1, S.snapshots))
            {
                std::vector<float> snap(nn, -1.0f);
                for (size_t i = 0; i < nn; ++i)
                    if (R.fillTimeS[i] >= 0.0f) snap[i] = (float)(P[i] * 1.0e-6);
                fr.snapshot = (int)R.framePressureMPa.size();
                R.framePressureMPa.push_back(std::move(snap));
                if (thermal)
                {
                    std::vector<float> ts(nn, FillResult::kNotFilledC), fz(nn, 0.0f);
                    for (size_t i = 0; i < nn; ++i)
                    {
                        if (R.fillTimeS[i] < 0.0f) continue;
                        ts[i] = (float)(Tk[i] - TestMaterial::kZeroC);
                        fz[i] = frozenValue(i);
                    }
                    R.frameTempC.push_back(std::move(ts));
                    R.frameFrozenMm.push_back(std::move(fz));
                }
                while ((double)nextSnapshot / std::max(1, S.snapshots) <= filledVol / totalVol) ++nextSnapshot;
            }
            R.frames.push_back(fr);

            if (S.progress && !S.progress(progFill * std::min(1.0, filledVol / totalVol), t))
            {
                R.cancelled = true;
                R.message = "Cancelled.";
                return R;
            }
            bool any = false;
            for (size_t i = 0; i < nn && !any; ++i) any = !filled(i);
            if (!any) break;
        }

        // ---- Results ----------------------------------------------------------------------
        size_t unfilled = 0;
        double tMax = 0.0;
        for (size_t i = 0; i < nn; ++i)
        {
            if (R.fillTimeS[i] < 0.0f) ++unfilled;
            else tMax = std::max(tMax, (double)R.fillTimeS[i]);
        }
        if (unfilled > 0)
        {
            R.incomplete = true;
            R.warnings.push_back(std::to_string(unfilled) + " of " + std::to_string(nn) + " nodes were never reached" +
                                 (R.stalled ? std::string(" before the flow stopped.")
                                            : std::string(" (a region with no inlet, or cut off).")));
        }
        R.endPressureMPa.assign(nn, -1.0f);
        R.endSpeedMmS.assign(nn, 0.0f);
        for (size_t i = 0; i < nn; ++i)
        {
            if (R.fillTimeS[i] >= 0.0f) R.endPressureMPa[i] = (float)(P[i] * 1.0e-6);
            const double* u = &U[3 * i];
            R.endSpeedMmS[i] = (float)std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        }
        R.tetShearRate.assign(nt, 0.0f);
        for (size_t tt = 0; tt < nt; ++tt)
        {
            R.tetShearRate[tt] = (float)tetGamma[tt];
            R.maxShearRate = std::max(R.maxShearRate, tetGamma[tt]);
        }
        if (thermal)
        {
            R.endTempC.assign(nn, FillResult::kNotFilledC);
            R.endFrozenMm.assign(nn, 0.0f);
            R.minFrontTempC = 1e9;
            R.maxTempC = -1e9;
            for (size_t i = 0; i < nn; ++i)
            {
                if (R.fillTimeS[i] < 0.0f) continue;
                const double c = Tk[i] - TestMaterial::kZeroC;
                R.endTempC[i] = (float)c;
                R.endFrozenMm[i] = frozenValue(i);
                R.maxTempC = std::max(R.maxTempC, c);
                if (R.frontTempC[i] > FillResult::kNotFilledC) R.minFrontTempC = std::min(R.minFrontTempC, (double)R.frontTempC[i]);
                if (R.endFrozenMm[i] < FillResult::kFrozenThrough) R.maxFrozenMm = std::max(R.maxFrozenMm, (double)R.endFrozenMm[i]);
            }
            if (R.minFrontTempC > 1e8) R.minFrontTempC = 0.0;
        }
        if (R.stalled)
            R.warnings.push_back(thermal
                ? "At the machine's pressure limit the melt all but stopped before the cavity was full: the "
                  "flow froze off (short shot). A hotter melt or mould, a faster fill, bigger gates or more "
                  "pressure would help."
                : "At the machine's pressure limit the fill slowed to a crawl (over four times the target time) "
                  "before the cavity was full. More pressure, bigger gates or thicker walls would help.");
        R.regionFlowMm3s.assign(nReg, 0.0f);
        for (size_t r = 0; r < nReg; ++r) R.regionFlowMm3s[r] = (float)lastQreg[r];
        R.netPressureMPa.assign((size_t)nNet, 0.0f);
        for (int k = 0; k < nNet; ++k) R.netPressureMPa[(size_t)k] = (float)(netP[(size_t)k] * 1.0e-6);


        // ==== Packing, holding and cooling =========================================
        // Continues from the end-of-fill state. Each node conserves its melt
        // mass M; while full, M = V rho(T, p) (Tait), so cooling at fixed mass
        // drops the pressure and draws melt in. Each step:
        //   1. temperatures over dt with the last step's flows (no shear heating);
        //   2. viscosities at the new temperatures (frozen melt carries nothing);
        //   3. the Stokes solve with compressible continuity, linearised in p:
        //        (B u - C p)_i - V_i kappa_i (p_i - p_i^n) / dt = (V_i rho(T, p^n) - M_i) / (rho_i dt)
        //      the nozzle at the pack pressure while holding (0 after); nodes
        //      that would go into tension held at p = 0 (active set);
        //   4. masses from the solved (conservative) node inflows.
        if (thermal && S.pack.enabled && !R.incomplete && !R.cancelled)
        {
            const auto& PK = S.pack;
            FillResult::PackResult& K = R.pack;
            K.ran = true;
            const TestMaterial::TaitPVT& pvt = PK.pvt;
            const double Teject = PK.ejectionTempC + TestMaterial::kZeroC;
            const double rhoRoom = 1.0 / pvt.specificVolume(PK.roomTempC + TestMaterial::kZeroC, 0.0);
            auto rhoOf = [&](double TK, double pPa) { return 1.0 / pvt.specificVolume(TK, std::max(0.0, pPa)); };
            auto drhoOf = [&](double TK, double pPa)   // d rho / d p within the current (melt / solid) domain
            {
                pPa = std::max(0.0, pPa);
                const bool meltDom = TK > pvt.b5 + pvt.b6 * pPa;
                const double b1 = meltDom ? pvt.b1m : pvt.b1s, b2 = meltDom ? pvt.b2m : pvt.b2s;
                const double b3 = meltDom ? pvt.b3m : pvt.b3s, b4 = meltDom ? pvt.b4m : pvt.b4s;
                const double dT = TK - pvt.b5;
                const double v0 = b1 + b2 * dT, Bt = b3 * std::exp(-b4 * dT);
                const double v = v0 * (1.0 - pvt.C * std::log(1.0 + pPa / Bt));
                return v0 * pvt.C / ((Bt + pPa) * v * v);
            };
            const double pPack = (PK.packPressureMPa > 0.0 ? PK.packPressureMPa : PK.packFraction * R.maxInletMPa) * 1.0e6;
            K.packPressureMPa = pPack * 1.0e-6;
            const double t0 = t;

            // Masses (kg/m^3 x mm^3) and the state they start from.
            std::vector<double> Pn(nn), M(nn);
            for (size_t i = 0; i < nn; ++i)
            {
                Pn[i] = std::max(0.0, P[i]);
                M[i] = cvVol[i] * rhoOf(Tk[i], Pn[i]);
            }
            double mass0 = 0.0;
            for (double m : M) mass0 += m;

            // Seals. Cavity-only meshes: each fed gate of the 1D feed is a
            // cylinder of melt cooling to the wall (layered, as the 2.5D
            // gates); it seals when frozen through, and its beam narrows as its
            // skin grows. Full-shot meshes: the gates are in the mesh, so the
            // hold ends when melt has all but stopped coming in.
            Flow::LayerGrid lg;
            lg.build(12);
            struct GateCol { int net = -1; double radiusM = 0.0; std::vector<double> T; double frozen = 0.0; double sealedAt = -1.0; };
            std::vector<GateCol> cols;
            std::vector<int> colOfNet((size_t)nNet, -1);
            for (int k = 0; k < nNet; ++k)
            {
                if (!netReach[(size_t)k] || (size_t)k >= PK.gateRadiusMm.size() || !(PK.gateRadiusMm[(size_t)k] > 0.0)) continue;
                bool used = false;
                for (size_t r = 0; r < nReg; ++r) used = used || regNet[r] == k;
                if (!used) continue;
                GateCol c;
                c.net = k;
                c.radiusM = PK.gateRadiusMm[(size_t)k] * 1.0e-3;
                c.T.assign((size_t)lg.n, meltK);
                colOfNet[(size_t)k] = (int)cols.size();
                cols.push_back(std::move(c));
            }
            std::vector<double> beamScale(S.beams.size(), 1.0);
            const bool sealByInflow = cols.empty();
            int lowInflowSteps = 0;
            double sealedAtInflow = -1.0;

            // Ejection tracking and frames.
            K.ejectTimeS.assign(nn, -1.0f);
            std::vector<double> tPrevK(Tk);
            size_t left = 0;
            for (size_t i = 0; i < nn; ++i) if (R.fillTimeS[i] >= 0.0f) ++left;
            const size_t total = std::max<size_t>(1, left);
            const size_t fillFrames = R.frames.size();
            auto recordPackFrame = [&](double tAbs, double pinPa, int phase)
            {
                FillFrame fr;
                fr.timeS = (float)tAbs;
                fr.filledPct = 100.0f;
                fr.inletMPa = (float)(pinPa * 1.0e-6);
                fr.flowMm3s = 0.0f;
                fr.phase = phase;
                fr.snapshot = (int)R.framePressureMPa.size();
                std::vector<float> ps(nn, -1.0f), ts(nn, FillResult::kNotFilledC), fz(nn, 0.0f);
                for (size_t i = 0; i < nn; ++i)
                {
                    if (R.fillTimeS[i] < 0.0f) continue;
                    ps[i] = (float)(std::max(0.0, P[i]) * 1.0e-6);
                    ts[i] = (float)(Tk[i] - TestMaterial::kZeroC);
                    fz[i] = frozenValue(i);
                }
                R.framePressureMPa.push_back(std::move(ps));
                R.frameTempC.push_back(std::move(ts));
                R.frameFrozenMm.push_back(std::move(fz));
                R.frames.push_back(fr);
                // Thin the pack frames: keep every other (and the latest) once
                // there are too many, compacting the snapshots they point at.
                const size_t nPack = R.frames.size() - fillFrames;
                if (PK.frames > 0 && (int)nPack > 2 * PK.frames)
                {
                    std::vector<FillFrame> kept;
                    for (size_t r = fillFrames; r < R.frames.size(); r += 2) kept.push_back(R.frames[r]);
                    if ((nPack - 1) % 2 != 0) kept.push_back(R.frames.back());
                    R.frames.resize(fillFrames);
                    int next = kept.empty() ? 0 : kept.front().snapshot;
                    for (FillFrame& kf : kept)
                    {
                        const int from = kf.snapshot;
                        if (from != next)
                        {
                            R.framePressureMPa[(size_t)next] = std::move(R.framePressureMPa[(size_t)from]);
                            R.frameTempC[(size_t)next] = std::move(R.frameTempC[(size_t)from]);
                            R.frameFrozenMm[(size_t)next] = std::move(R.frameFrozenMm[(size_t)from]);
                        }
                        kf.snapshot = next++;
                        R.frames.push_back(kf);
                    }
                    R.framePressureMPa.resize((size_t)next);
                    R.frameTempC.resize((size_t)next);
                    R.frameFrozenMm.resize((size_t)next);
                }
            };

            // Everything is in the melt now: all (good) tets active.
            for (size_t tt = 0; tt < nt; ++tt) tetActive[tt] = geo[tt].ok ? 1 : 0;
            std::vector<char> clamp0(nn, 0);
            std::vector<double> divN(nn, 0.0);
            double tp = 0.0, dt = std::max(1.0e-4, PK.dtInitialS);
            bool holding = true;
            const bool autoHold = PK.holdTimeS <= 0.0;
            const double holdLimit = autoHold ? std::max(0.0, PK.holdMaxS) : PK.holdTimeS;
            double inMass = 0.0;
            int steps = 0;
            std::vector<double> xp;   // warm start
            while (true)
            {
                if (holding && !autoHold && tp + dt > holdLimit - 1.0e-9) dt = std::max(1.0e-4, holdLimit - tp);
                const double pin = holding ? pPack : 0.0;

                // ---- 1. temperatures (last step's flows; inlet nodes take the melt coming in)
                std::fill(inletIn.begin(), inletIn.end(), 0.0);
                for (const InletFace& f : inletFaces)
                    for (int32_t vi : f.v)
                    {
                        const double un = Dot(Vec3{ U[3 * (size_t)vi], U[3 * (size_t)vi + 1], U[3 * (size_t)vi + 2] }, f.n);
                        if (un < 0.0) inletIn[(size_t)vi] += -un * f.area / 3.0;
                    }
                thermalStep(dt, t0 + tp, false, true);
                for (GateCol& c : cols)
                {
                    Flow::ConductStep(lg, Flow::GapGeom::Cylinder, c.radiusM, kMelt, rhoCp, dt, wallK, nullptr, c.T.data());
                    c.frozen = Flow::FrozenFraction(lg, c.T.data(), wallK, noFlowK);
                    if (c.sealedAt < 0.0 && c.frozen >= 0.999) c.sealedAt = t0 + tp + dt;
                }
                for (size_t e = 0; e < S.beams.size(); ++e)
                {
                    const FeedBeam& fb = S.beams[e];
                    double sc = 1.0;
                    for (int end : { fb.a, fb.b })
                        if (end >= 0 && end < nNet && colOfNet[(size_t)end] >= 0)
                        {
                            const double open = std::max(0.0, 1.0 - cols[(size_t)colOfNet[(size_t)end]].frozen);
                            sc = std::min(sc, std::max(1.0e-9, open * open * open * open));   // pipe: radius^4
                        }
                    beamScale[e] = sc;
                }

                // ---- 2. viscosities at the new temperatures, per-node density
                for (size_t tt = 0; tt < nt; ++tt)
                    if (tetActive[tt]) tetEta[tt] = tetEtaAt(tt, std::max(tetGamma[tt], 1.0e-3));
                std::vector<double> rhoN(nn), kapN(nn), mT(nn);
                for (size_t i = 0; i < nn; ++i)
                {
                    mT[i] = cvVol[i] * rhoOf(Tk[i], Pn[i]);
                    rhoN[i] = mT[i] / std::max(1e-30, cvVol[i]);
                    kapN[i] = drhoOf(Tk[i], Pn[i]) / rhoN[i];
                }

                // ---- 3. compressible Stokes, active set p >= 0
                double etaLog = 0.0, hSum = 0.0;
                size_t nAct = 0;
                for (size_t tt = 0; tt < nt; ++tt)
                    if (tetActive[tt]) { etaLog += std::log(tetEta[tt]); hSum += geo[tt].h; ++nAct; }
                const double sig = std::exp(etaLog / std::max<size_t>(1, nAct)) / (hSum / std::max<size_t>(1, nAct));
                std::fill(clamp0.begin(), clamp0.end(), 0);
                bool failed = false;
                for (int pass = 0; pass < 6; ++pass)
                {
                    std::fill(uIdx.begin(), uIdx.end(), -1);
                    std::fill(pIdx.begin(), pIdx.end(), -1);
                    int nU = 0, nP = 0;
                    for (size_t i = 0; i < nn; ++i)
                    {
                        if (R.fillTimeS[i] < 0.0f) continue;
                        if (!fixedU[i]) uIdx[i] = nU++;
                        if (!clamp0[i]) pIdx[i] = nP++;
                    }
                    std::fill(netIdx.begin(), netIdx.end(), -1);
                    int nN = 0;
                    for (int k = 0; k < nNet; ++k)
                        if (netReach[(size_t)k] && k != S.sourceNode) netIdx[(size_t)k] = nN++;
                    const size_t nU3 = 3 * (size_t)nU, nDof = nU3 + (size_t)nP + (size_t)nN;
                    auto uD = [&](size_t i, int c) { return (ptrdiff_t)(3 * (size_t)uIdx[i] + (size_t)c); };
                    auto pD = [&](size_t i) { return (ptrdiff_t)(nU3 + (size_t)pIdx[i]); };
                    auto nD = [&](int k) { return (ptrdiff_t)(nU3 + (size_t)nP + (size_t)netIdx[(size_t)k]); };
                    std::vector<Eigen::Triplet<double, ptrdiff_t>> trip;
                    trip.reserve(nAct * 120);
                    std::vector<double> rhs(nDof, 0.0);
                    for (size_t tt = 0; tt < nt; ++tt)
                    {
                        if (!tetActive[tt]) continue;
                        const TetGeo& G = geo[tt];
                        const int32_t* Tn = &mesh.tets[4 * tt];
                        const double eta = tetEta[tt];
                        const double tau = S.pspgAlpha * G.h * G.h / eta;
                        tetTau[tt] = tau;
                        for (int a = 0; a < 4; ++a)
                        {
                            const size_t ia = (size_t)Tn[a];
                            for (int b = 0; b < 4; ++b)
                            {
                                const size_t ib = (size_t)Tn[b];
                                const double gg = Dot(G.g[a], G.g[b]) * G.V;
                                if (uIdx[ia] >= 0 && uIdx[ib] >= 0)
                                    for (int c = 0; c < 3; ++c) trip.emplace_back(uD(ia, c), uD(ib, c), eta * gg);
                                if (pIdx[ia] >= 0 && pIdx[ib] >= 0)
                                    trip.emplace_back(pD(ia), pD(ib), -tau * gg * sig * sig);
                                if (pIdx[ia] >= 0 && uIdx[ib] >= 0)
                                    for (int c = 0; c < 3; ++c)
                                    {
                                        const double v = -0.25 * G.V * G.g[b][c] * sig;
                                        trip.emplace_back(pD(ia), uD(ib, c), v);
                                        trip.emplace_back(uD(ib, c), pD(ia), v);
                                    }
                            }
                        }
                    }
                    // Compressibility (pressure rows).
                    for (size_t i = 0; i < nn; ++i)
                    {
                        if (pIdx[i] < 0) continue;
                        const double c = cvVol[i] * kapN[i] / dt;   // mm^3 / (Pa s)
                        trip.emplace_back(pD(i), pD(i), -c * sig * sig);
                        rhs[(size_t)pD(i)] += sig * ((mT[i] - M[i]) / (rhoN[i] * dt) - c * Pn[i]);
                    }
                    for (const InletFace& f : inletFaces)
                    {
                        const int k = regNet[(size_t)f.region];
                        for (int32_t vi : f.v)
                        {
                            const size_t i = (size_t)vi;
                            if (uIdx[i] < 0) continue;
                            for (int c = 0; c < 3; ++c)
                            {
                                const double e = f.area / 3.0 * f.n[(size_t)c] * sig;
                                if (netIdx[(size_t)k] >= 0)
                                {
                                    trip.emplace_back(uD(i, c), nD(k), e);
                                    trip.emplace_back(nD(k), uD(i, c), e);
                                }
                                else
                                    rhs[(size_t)uD(i, c)] -= e * (pin / sig);
                            }
                        }
                    }
                    for (size_t e = 0; e < S.beams.size(); ++e)
                    {
                        const FeedBeam& fb = S.beams[e];
                        if (fb.a < 0 || fb.b < 0 || fb.a >= nNet || fb.b >= nNet) continue;
                        if (!netReach[(size_t)fb.a] || !netReach[(size_t)fb.b]) continue;
                        const double gd = Flow::ApparentWallShearRate(fb.section, std::fabs(beamFlow[e]));
                        const double cond = beamScale[e] * fb.section.conductanceMm4 /
                                            (beamEta(gd) * std::max(fb.lengthMm, 1.0e-3)) * sig * sig;
                        const int ends[2] = { fb.a, fb.b };
                        for (int sd = 0; sd < 2; ++sd)
                        {
                            const int me = ends[sd], other = ends[1 - sd];
                            if (netIdx[(size_t)me] < 0) continue;
                            trip.emplace_back(nD(me), nD(me), -cond);
                            if (netIdx[(size_t)other] >= 0) trip.emplace_back(nD(me), nD(other), cond);
                            else rhs[(size_t)nD(me)] -= cond * (pin / sig);
                        }
                    }
                    for (size_t d = nU3; d < nDof; ++d) trip.emplace_back((ptrdiff_t)d, (ptrdiff_t)d, 0.0);

                    Eigen::SparseMatrix<double, Eigen::RowMajor, ptrdiff_t> Km((ptrdiff_t)nDof, (ptrdiff_t)nDof);
                    Km.setFromTriplets(trip.begin(), trip.end());
                    trip.clear();
                    trip.shrink_to_fit();
                    Km.makeCompressed();
                    const ptrdiff_t rows = (ptrdiff_t)nDof;
                    std::vector<ptrdiff_t> ptr(Km.outerIndexPtr(), Km.outerIndexPtr() + rows + 1);
                    std::vector<ptrdiff_t> col(Km.innerIndexPtr(), Km.innerIndexPtr() + Km.nonZeros());
                    std::vector<double> val(Km.valuePtr(), Km.valuePtr() + Km.nonZeros());
                    Km.resize(0, 0);
                    Km.data().squeeze();
                    xp.assign(nDof, 0.0);
                    for (size_t i = 0; i < nn; ++i)
                    {
                        if (uIdx[i] >= 0) for (int c = 0; c < 3; ++c) xp[(size_t)uD(i, c)] = U[3 * i + (size_t)c];
                        if (pIdx[i] >= 0) xp[(size_t)pD(i)] = Pn[i] / sig;
                    }
                    for (int k = 0; k < nNet; ++k) if (netIdx[(size_t)k] >= 0) xp[(size_t)nD(k)] = netP[(size_t)k] / sig;
                    Solver::params prm;
                    prm.solver.tol = S.solverTolerance;
                    prm.solver.maxiter = S.solverMaxIterations;
                    prm.solver.M = 60;
                    prm.precond.adjust_p = 2;
                    prm.precond.pmask.assign(nDof, 0);
                    for (size_t d = nU3; d < nDof; ++d) prm.precond.pmask[d] = 1;
                    const auto tSolve = clock::now();
                    int iters = 0;
                    double resid = 0.0;
                    {
                        auto A = std::tie(rows, ptr, col, val);
                        Solver solve(A, prm);
                        std::tie(iters, resid) = solve(A, rhs, xp);
                    }
                    R.stats.solveSeconds += std::chrono::duration<double>(clock::now() - tSolve).count();
                    ++R.stats.solves;
                    ++K.solves;
                    if (!std::isfinite(resid)) { K.message = "The packing solve broke down."; failed = true; break; }

                    for (size_t i = 0; i < nn; ++i)
                    {
                        for (int c = 0; c < 3; ++c) U[3 * i + (size_t)c] = uIdx[i] >= 0 ? xp[(size_t)uD(i, c)] : 0.0;
                        P[i] = pIdx[i] >= 0 ? xp[(size_t)pD(i)] * sig : 0.0;
                    }
                    for (int k = 0; k < nNet; ++k)
                        netP[(size_t)k] = netIdx[(size_t)k] >= 0 ? xp[(size_t)nD(k)] * sig : (k == S.sourceNode ? pin : 0.0);

                    // Node inflows from the solved continuity (conservative).
                    std::fill(divN.begin(), divN.end(), 0.0);
                    for (size_t tt = 0; tt < nt; ++tt)
                    {
                        if (!tetActive[tt]) continue;
                        const TetGeo& G = geo[tt];
                        const int32_t* Tn = &mesh.tets[4 * tt];
                        double divu = 0.0;
                        Vec3 gp{ 0, 0, 0 };
                        for (int k = 0; k < 4; ++k)
                            for (int c = 0; c < 3; ++c)
                            {
                                divu += U[3 * (size_t)Tn[k] + (size_t)c] * G.g[k][c];
                                gp[(size_t)c] += P[(size_t)Tn[k]] * G.g[k][c];
                            }
                        for (int a = 0; a < 4; ++a)
                            divN[(size_t)Tn[a]] += -0.25 * G.V * divu - tetTau[tt] * G.V * Dot(G.g[a], gp);
                    }
                    // Active set: tension -> hold at zero; held but receiving more
                    // than fits at zero -> release.
                    bool changed = false;
                    for (size_t i = 0; i < nn; ++i)
                    {
                        if (R.fillTimeS[i] < 0.0f) continue;
                        if (pIdx[i] >= 0 && P[i] < 0.0) { clamp0[i] = 1; changed = true; }
                        else if (clamp0[i] && pIdx[i] < 0)
                        {
                            const double capacity = cvVol[i] * rhoOf(Tk[i], 0.0);
                            if (M[i] + dt * rhoN[i] * divN[i] > capacity * (1.0 + 1.0e-9)) { clamp0[i] = 0; changed = true; }
                        }
                    }
                    if (!changed) break;
                }
                if (failed) break;
                for (double& v : P) v = std::max(0.0, v);

                // ---- 4. masses from the solved continuity: each node gains its
                // own density times its net inflow — exactly what the linearised
                // solve balanced, so the pressure and the mass stay in step (an
                // upwind exchange of densities would be explicit, and unstable
                // at the large Courant numbers of the first pack steps).
                std::vector<double> dM(nn, 0.0);
                for (size_t i = 0; i < nn; ++i) dM[i] = dt * rhoN[i] * divN[i];
                double qIn = 0.0;   // volume rate in through the inlets (for the seal test and the mass check)
                for (const InletFace& f : inletFaces)
                    for (int32_t vi : f.v)
                    {
                        const size_t i = (size_t)vi;
                        const double q = -Dot(Vec3{ U[3 * i], U[3 * i + 1], U[3 * i + 2] }, f.n) * f.area / 3.0;
                        inMass += (q > 0.0 ? rhoOf(meltK, P[i]) : rhoN[i]) * q * dt;
                        qIn += q;
                    }
                for (size_t i = 0; i < nn; ++i)
                    if (R.fillTimeS[i] >= 0.0f) M[i] += dM[i];
                Pn = P;
                // Beam flows at the new pressures (their shear rates next step).
                for (size_t e = 0; e < S.beams.size(); ++e)
                {
                    const FeedBeam& fb = S.beams[e];
                    if (fb.a < 0 || fb.b < 0 || fb.a >= nNet || fb.b >= nNet) continue;
                    const double gd = Flow::ApparentWallShearRate(fb.section, std::fabs(beamFlow[e]));
                    const double cond = beamScale[e] * fb.section.conductanceMm4 / (beamEta(gd) * std::max(fb.lengthMm, 1.0e-3));
                    beamFlow[e] = cond * (netP[(size_t)fb.a] - netP[(size_t)fb.b]);
                }
                // Shear rates of the (slow) packing flow, for the next viscosity pass.
                for (size_t tt = 0; tt < nt; ++tt)
                {
                    if (!tetActive[tt]) continue;
                    const TetGeo& G = geo[tt];
                    const int32_t* Tn = &mesh.tets[4 * tt];
                    double L[3][3] = {};
                    for (int k = 0; k < 4; ++k)
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j) L[i][j] += U[3 * (size_t)Tn[k] + (size_t)i] * G.g[k][j];
                    double dd = 0.0;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) { const double d = 0.5 * (L[i][j] + L[j][i]); dd += 2.0 * d * d; }
                    tetGamma[tt] = std::sqrt(dd);
                }

                tp += dt;
                ++steps;
                const double tAbs = t0 + tp;

                // ---- tracking
                for (size_t i = 0; i < nn; ++i)
                {
                    if (R.fillTimeS[i] < 0.0f || K.ejectTimeS[i] >= 0.0f) continue;
                    if (Tk[i] <= Teject)
                    {
                        const double a = tPrevK[i];
                        const double f = (a > Tk[i]) ? std::clamp((a - Teject) / (a - Tk[i]), 0.0, 1.0) : 1.0;
                        K.ejectTimeS[i] = (float)(tAbs - dt + f * dt);
                        --left;
                    }
                }
                tPrevK = Tk;
                if (holding && sealByInflow && sealedAtInflow < 0.0)
                {
                    if (qIn < 1.0e-3 * Q) ++lowInflowSteps;
                    else lowInflowSteps = 0;
                    if (lowInflowSteps >= 3) sealedAtInflow = tAbs;
                }
                recordPackFrame(tAbs, pin, holding ? 1 : 2);

                bool allSealed;
                if (sealByInflow) allSealed = sealedAtInflow >= 0.0;
                else
                {
                    allSealed = true;
                    for (const GateCol& c : cols) allSealed = allSealed && c.sealedAt >= 0.0;
                }
                double dtNext = std::min(PK.dtMaxS, dt * 1.25);
                if (holding && (autoHold ? (allSealed || tp >= holdLimit - 1.0e-9) : tp >= holdLimit - 1.0e-9))
                {
                    holding = false;
                    K.holdTimeS = tp;
                    dtNext = std::max(1.0e-4, PK.dtInitialS);   // the nozzle pressure drops: resolve it
                }
                dt = dtNext;
                if (!holding && left == 0) { K.ejectReached = true; break; }
                if (tp >= PK.maxTimeS) break;
                const double ejFrac = 1.0 - (double)left / (double)total;
                if (S.progress && !S.progress(progFill + (1.0 - progFill) * std::min(0.999, 0.15 * std::min(1.0, tp / 5.0) + 0.85 * ejFrac), tAbs))
                {
                    R.cancelled = true;
                    R.message = "Cancelled while packing.";
                    K.message = "Cancelled while packing.";
                    break;
                }
            }
            K.steps = steps;
            K.endS = t0 + tp;
            if (holding) K.holdTimeS = tp;
            R.stats.steps += steps;

            // ---- pack results
            double gf = -1.0;
            if (sealByInflow) { K.gatesFrozen = sealedAtInflow >= 0.0; gf = sealedAtInflow; }
            else
            {
                K.gatesFrozen = true;
                for (const GateCol& c : cols) { K.gatesFrozen = K.gatesFrozen && c.sealedAt >= 0.0; gf = std::max(gf, c.sealedAt); }
            }
            K.gateFreezeS = K.gatesFrozen ? gf : -1.0;
            K.gateFreezePerRegion.assign(nReg, -1.0f);
            for (size_t r = 0; r < nReg; ++r)
            {
                const int k = regNet[r];
                if (k >= 0 && colOfNet[(size_t)k] >= 0) K.gateFreezePerRegion[r] = (float)cols[(size_t)colOfNet[(size_t)k]].sealedAt;
                else if (k >= 0 && sealByInflow) K.gateFreezePerRegion[r] = (float)sealedAtInflow;
            }
            K.ejectS = -1.0;
            if (K.ejectReached)
                for (float e : K.ejectTimeS) K.ejectS = std::max(K.ejectS, (double)e);
            double massEnd = 0.0, vSum = 0.0, wSum = 0.0;
            K.shrinkPct.assign(nn, 0.0f);
            K.minShrinkPct = 1e9;
            K.maxShrinkPct = -1e9;
            for (size_t i = 0; i < nn; ++i)
            {
                if (R.fillTimeS[i] < 0.0f) continue;
                massEnd += M[i];
                const double sh = 100.0 * (1.0 - M[i] / (rhoRoom * cvVol[i]));
                K.shrinkPct[i] = (float)sh;
                wSum += sh * cvVol[i];
                vSum += cvVol[i];
                K.minShrinkPct = std::min(K.minShrinkPct, sh);
                K.maxShrinkPct = std::max(K.maxShrinkPct, sh);
            }
            K.meanShrinkPct = vSum > 0.0 ? wSum / vSum : 0.0;
            if (K.minShrinkPct > K.maxShrinkPct) K.minShrinkPct = K.maxShrinkPct = 0.0;
            K.massG = massEnd * 1.0e-6;               // (kg/m^3 x mm^3) -> g
            K.packedMassG = (massEnd - mass0) * 1.0e-6;
            K.massBalanceErrPct = mass0 > 0.0 ? 100.0 * std::fabs((massEnd - mass0) - inMass) / mass0 : 0.0;
            if (K.message.empty())
                K.message = K.ejectReached ? "Packed and cooled to the ejection temperature."
                                           : "Stopped before every node reached the ejection temperature.";
            if (!K.ejectReached)
                R.warnings.push_back("Cooling stopped " + std::to_string((int)std::lround(PK.maxTimeS)) +
                                     " s after the fill before all the melt was below the ejection temperature.");
        }

        R.filledVolumeMm3 = filledVol;
        R.endTimeS = tMax;
        R.stats.meanIterations = R.stats.solves ? itersSum / R.stats.solves : 0.0;
        R.stats.backflowPct = posFlux > 0.0 ? 100.0 * negFlux / posFlux : 0.0;
        R.stats.totalSeconds = std::chrono::duration<double>(clock::now() - tStart).count();
        R.ok = true;
        R.message = R.stalled ? "Short shot: the melt froze off before filling."
                  : R.incomplete ? "The fill didn't reach the whole mesh." : "Filled.";
        return R;
    }
}
