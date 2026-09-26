// ===========================================================================
// CoupledFill.cpp — feed network + part midplanes filled as one CVFEM system,
// optionally with the gap-wise thermal model. See CoupledFill.h.
// ===========================================================================

#include "CoupledFill.h"
#include "GapThermal.h"

// Eigen lands at <...>/include/eigen3/Eigen under vcpkg's MSBuild integration;
// resolve both layouts (same guard as FlowSolver.cpp).
#if __has_include(<Eigen/Sparse>)
  #include <Eigen/Sparse>
#elif __has_include(<eigen3/Eigen/Sparse>)
  #include <eigen3/Eigen/Sparse>
#else
  #error "Eigen not found. Ensure vcpkg installed eigen3 (see vcpkg.json)."
#endif

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace Flow
{
    namespace
    {
        using dvec3 = glm::dvec3;
        constexpr double kTonN = 9806.65;        // newtons per tonne-force
        constexpr double kZeroC = 273.15;
        constexpr double kSafetyCeilingMPa = 400.0;

        struct El1                               // 1D feed segment
        {
            int a = -1, b = -1;
            double G = 0.0, L = 0.0;             // conductance shape factor (mm^4), segment length (mm)
            FeedSection sec;
            int edge = -1;                       // network edge it belongs to
            GapGeom geom = GapGeom::Cylinder;    // thermal: round (radial layers) or slit (slab)
            double size = 0.0;                   // radius / half-gap (mm)
            double widthFactor = 1.0;            // slab: width x the rectangle's shape factor
            double eta = 0.0;                    // isothermal viscosity
            double coef = 0.0, coefSolve = 0.0;  // conductance / L [mm^3/(Pa s)]; value in the last solve
            bool   seen = false;                 // has carried flow (eta is live)
        };

        struct El2                               // 2D midplane triangle
        {
            int    n[3] = { -1, -1, -1 };
            double K[3][3] = { {0,0,0},{0,0,0},{0,0,0} };   // cotangent stiffness (dimensionless)
            dvec3  g[3];                         // grad of each linear shape function (1/mm)
            double h = 0.0;                      // gap (mm)
            double vol = 0.0;                    // area * h (mm^3)
            double eta = 0.0;                    // isothermal viscosity
            double S = 0.0, Ssolve = 0.0;        // gap conductance [mm^3/(Pa s)]; value in the last solve
            bool   seen = false;
            int    seenStep = -1;
        };

        void SectionGeom(const FeedSection& s, GapGeom& geom, double& size, double& widthFactor)
        {
            if (s.shape == SectionShape::Rect)
            {
                const double h = std::min(s.widthMm, s.heightMm);
                geom = GapGeom::Slab;
                size = 0.5 * h;
                widthFactor = (h > 0.0) ? s.conductanceMm4 / (h * h * h / 12.0) : 0.0;
            }
            else
            {
                geom = GapGeom::Cylinder;
                size = 0.5 * s.diameterMm;
                widthFactor = 1.0;
            }
        }
    } // namespace

    CoupledFillResult SolveCoupledFill(const FeedNetwork& net,
                                       const std::vector<MidplaneMesh>& parts,
                                       const CoupledFillParams& P)
    {
        CoupledFillResult R;
        const int NN = (int)net.nodes.size();
        const bool thermal = P.thermal.enabled;
        R.thermal = thermal;
        R.netFillTimeS.assign((size_t)NN, -1.0f);
        R.netPressureMPa.assign((size_t)NN, -1.0f);
        R.feedEdges.assign(net.edges.size(), FeedEdgeFillResult{});
        R.parts.resize(parts.size());
        for (size_t k = 0; k < parts.size(); ++k)
        {
            R.parts[k].objectIndex = parts[k].objectIndex;
            R.parts[k].label = parts[k].label;
        }
        if (!thermal && !P.viscosity) { R.message = "No viscosity model supplied."; return R; }
        if (net.inletNode < 0 || net.inletNode >= NN) { R.message = "The feed network has no sprue inlet."; return R; }
        const double fillTime = std::max(1.0e-6, P.fillTimeS);

        // ---- Melt reachability from the inlet --------------------------------
        std::vector<std::vector<int>> nadj((size_t)NN);
        for (int i = 0; i < (int)net.edges.size(); ++i)
            if (net.edges[(size_t)i].carriesMelt())
            {
                nadj[(size_t)net.edges[(size_t)i].a].push_back(i);
                nadj[(size_t)net.edges[(size_t)i].b].push_back(i);
            }
        std::vector<char> reach((size_t)NN, 0);
        {
            std::queue<int> q; q.push(net.inletNode); reach[(size_t)net.inletNode] = 1;
            while (!q.empty())
            {
                const int u = q.front(); q.pop();
                for (int ei : nadj[(size_t)u])
                {
                    const FeedEdge& e = net.edges[(size_t)ei];
                    const int v = (e.a == u) ? e.b : e.a;
                    if (!reach[(size_t)v]) { reach[(size_t)v] = 1; q.push(v); }
                }
            }
        }

        // ---- Cavity entries -> nearest node of their part's midplane --------
        std::unordered_map<int, int> partOfObject;
        for (size_t k = 0; k < parts.size(); ++k)
            if (!parts[k].empty()) partOfObject[parts[k].objectIndex] = (int)k;

        struct Entry { int netNode; int part; int mid; };
        std::vector<Entry> entries;
        for (const FeedEdge& e : net.edges)
        {
            if (e.kind != FeedEdgeKind::CavityIn) continue;
            const bool aPart = net.nodes[(size_t)e.a].kind == FeedNodeKind::Part;
            const bool bPart = net.nodes[(size_t)e.b].kind == FeedNodeKind::Part;
            if (aPart == bPart) continue;
            const int pn = aPart ? e.a : e.b, en = aPart ? e.b : e.a;
            if (!reach[(size_t)en])
            {
                R.warnings.push_back(net.nodes[(size_t)en].label + " is not reached by the feed system.");
                continue;
            }
            auto it = partOfObject.find(net.nodes[(size_t)pn].objectIndex);
            if (it == partOfObject.end())
            {
                R.warnings.push_back(net.nodes[(size_t)pn].label + " has no midplane mesh, so " +
                                     net.nodes[(size_t)en].label + " is a dead end.");
                continue;
            }
            const MidplaneMesh& m = parts[(size_t)it->second];
            const int mid = NearestMidplaneNode(m, net.nodes[(size_t)en].pos);
            if (mid < 0) continue;
            const glm::vec3 d = m.nodes[(size_t)mid] - net.nodes[(size_t)en].pos;
            const float plan = std::sqrt(d.x * d.x + d.z * d.z);
            entries.push_back({ en, it->second, mid });
            R.parts[(size_t)it->second].entryNodes.push_back(mid);
            R.parts[(size_t)it->second].entryPlanDistMm.push_back(plan);
        }
        if (entries.empty()) { R.message = "No gate (or direct-injection sprue) reaches a part midplane."; return R; }

        // ---- Degrees of freedom -------------------------------------------------
        // Fed parts first (their whole midplane), then the reachable feed nodes;
        // an entry node takes its midplane node's DOF (the merge). Interior
        // points of the segmented feed edges are appended below.
        int ndof = 0;
        std::vector<int> partBase(parts.size(), -1);
        for (const Entry& en : entries)
            if (partBase[(size_t)en.part] < 0)
            {
                partBase[(size_t)en.part] = ndof;
                ndof += (int)parts[(size_t)en.part].nodes.size();
                R.parts[(size_t)en.part].fed = true;
            }
        std::vector<int> netDof((size_t)NN, -1);
        for (const Entry& en : entries)
            netDof[(size_t)en.netNode] = partBase[(size_t)en.part] + en.mid;
        for (int n = 0; n < NN; ++n)
            if (reach[(size_t)n] && netDof[(size_t)n] < 0 && net.nodes[(size_t)n].kind != FeedNodeKind::Part)
                netDof[(size_t)n] = ndof++;
        for (size_t k = 0; k < parts.size(); ++k)
            if (!parts[k].empty() && partBase[k] < 0)
                R.warnings.push_back(parts[k].label + " is not fed (no gate reaches it).");

        // ---- Feed segments ----------------------------------------------------------
        const int firstInterior = ndof;
        std::vector<int> interiorEdge;                      // (dof - firstInterior) -> network edge
        std::vector<El1> e1;
        const double segMm = std::max(0.5, P.feedSegmentMm);
        for (int ei = 0; ei < (int)net.edges.size(); ++ei)
        {
            const FeedEdge& fe = net.edges[(size_t)ei];
            if (!fe.carriesMelt()) continue;
            const int da = netDof[(size_t)fe.a], db = netDof[(size_t)fe.b];
            if (da < 0 || db < 0 || da == db) continue;
            if (fe.section.conductanceMm4 <= 0.0) continue;
            R.feedEdges[(size_t)ei].modelled = true;
            const double L = std::max(1.0e-3, (double)fe.lengthMm);
            const int nseg = std::max(2, (int)std::ceil(L / segMm - 1.0e-9));
            GapGeom geom; double size, wf;
            SectionGeom(fe.section, geom, size, wf);
            int prev = da;
            for (int s = 0; s < nseg; ++s)
            {
                int next = db;
                if (s < nseg - 1) { next = ndof++; interiorEdge.push_back(ei); }
                El1 el;
                el.a = prev; el.b = next;
                el.G = fe.section.conductanceMm4;
                el.L = L / nseg;
                el.sec = fe.section;
                el.edge = ei;
                el.geom = geom; el.size = std::max(0.01, size); el.widthFactor = wf;
                e1.push_back(el);
                prev = next;
            }
        }
        R.dofs = ndof;

        // ---- Control volumes, thermal geometry, midplane elements -----------------
        std::vector<double> V((size_t)ndof, 0.0), planA((size_t)ndof, 0.0);
        std::vector<GapGeom> dofGeom((size_t)ndof, GapGeom::Slab);
        std::vector<double> dofSize((size_t)ndof, 1.0), bestVol((size_t)ndof, -1.0);
        std::vector<char> isMid((size_t)ndof, 0);
        for (size_t k = 0; k < parts.size(); ++k)
        {
            if (partBase[k] < 0) continue;
            const MidplaneMesh& m = parts[k];
            for (size_t i = 0; i < m.nodes.size(); ++i)
            {
                const size_t d = (size_t)(partBase[k] + (int)i);
                isMid[d] = 1;
                dofGeom[d] = GapGeom::Slab;
                dofSize[d] = std::max(0.025, 0.5 * (double)(i < m.thicknessMm.size() ? m.thicknessMm[i] : 0.0f));
            }
        }
        double feedVol = 0.0, cavVol = 0.0, projA = 0.0;
        for (const El1& el : e1)
        {
            const double vol = (double)el.sec.areaMm2 * el.L;
            V[(size_t)el.a] += 0.5 * vol; V[(size_t)el.b] += 0.5 * vol;
            feedVol += vol;
            for (int d : { el.a, el.b })
                if (!isMid[(size_t)d] && 0.5 * vol > bestVol[(size_t)d])
                {
                    bestVol[(size_t)d] = 0.5 * vol;
                    dofGeom[(size_t)d] = el.geom;
                    dofSize[(size_t)d] = el.size;
                }
        }
        std::vector<El2> e2;
        for (size_t k = 0; k < parts.size(); ++k)
        {
            if (partBase[k] < 0) continue;
            const MidplaneMesh& m = parts[k];
            for (const glm::ivec3& T : m.tris)
            {
                const dvec3 p0(m.nodes[(size_t)T.x]), p1(m.nodes[(size_t)T.y]), p2(m.nodes[(size_t)T.z]);
                const dvec3 cr = glm::cross(p1 - p0, p2 - p0);
                const double twoA = glm::length(cr);
                if (twoA <= 1.0e-12) continue;
                const double h = ((double)m.thicknessMm[(size_t)T.x] + m.thicknessMm[(size_t)T.y] +
                                  m.thicknessMm[(size_t)T.z]) / 3.0;
                if (h <= 0.0) continue;
                El2 el;
                el.n[0] = partBase[k] + T.x; el.n[1] = partBase[k] + T.y; el.n[2] = partBase[k] + T.z;
                el.h = h;
                const double cot0 = glm::dot(p1 - p0, p2 - p0) / twoA;
                const double cot1 = glm::dot(p0 - p1, p2 - p1) / twoA;
                const double cot2 = glm::dot(p0 - p2, p1 - p2) / twoA;
                const double L01 = -0.5 * cot2, L12 = -0.5 * cot0, L20 = -0.5 * cot1;
                el.K[0][1] = el.K[1][0] = L01;
                el.K[1][2] = el.K[2][1] = L12;
                el.K[2][0] = el.K[0][2] = L20;
                el.K[0][0] = -(L01 + L20);
                el.K[1][1] = -(L01 + L12);
                el.K[2][2] = -(L12 + L20);
                const dvec3 nh = cr / twoA;
                el.g[0] = glm::cross(nh, p2 - p1) / twoA;
                el.g[1] = glm::cross(nh, p0 - p2) / twoA;
                el.g[2] = glm::cross(nh, p1 - p0) / twoA;
                const double area = 0.5 * twoA;
                const double plan = 0.5 * std::fabs((p1.x - p0.x) * (p2.z - p0.z) - (p2.x - p0.x) * (p1.z - p0.z));
                el.vol = area * h;
                for (int j = 0; j < 3; ++j)
                {
                    V[(size_t)el.n[j]] += area * h / 3.0;
                    planA[(size_t)el.n[j]] += plan / 3.0;
                }
                cavVol += area * h;
                projA += plan;
                e2.push_back(el);
            }
        }
        R.feedVolumeMm3 = (float)feedVol;
        R.cavityVolumeMm3 = (float)cavVol;
        R.projectedAreaMm2 = (float)projA;
        const double Vtot = feedVol + cavVol;
        if (Vtot <= 0.0 || e2.empty()) { R.message = "Nothing to fill (zero cavity volume)."; return R; }
        const double Q = Vtot / fillTime;
        R.flowRateMm3s = (float)Q;

        // Neighbours (for the front and for spilling overflow), node -> triangles.
        std::vector<std::vector<int>> nb((size_t)ndof);
        auto link = [&](int a, int b) { nb[(size_t)a].push_back(b); nb[(size_t)b].push_back(a); };
        for (const El1& el : e1) link(el.a, el.b);
        for (const El2& el : e2) { link(el.n[0], el.n[1]); link(el.n[1], el.n[2]); link(el.n[2], el.n[0]); }
        for (auto& v : nb) { std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); }
        std::vector<int> n2eStart((size_t)ndof + 1, 0), n2e;
        for (const El2& el : e2) for (int j = 0; j < 3; ++j) ++n2eStart[(size_t)el.n[j] + 1];
        for (int i = 0; i < ndof; ++i) n2eStart[(size_t)i + 1] += n2eStart[(size_t)i];
        n2e.resize((size_t)n2eStart[(size_t)ndof]);
        {
            std::vector<int> fillp(n2eStart.begin(), n2eStart.end() - 1);
            for (int e = 0; e < (int)e2.size(); ++e)
                for (int j = 0; j < 3; ++j) n2e[(size_t)fillp[(size_t)e2[(size_t)e].n[j]]++] = e;
        }

        // ---- Thermal set-up ---------------------------------------------------------------
        const FillThermalParams& TP = P.thermal;
        const int nz = thermal ? std::max(2, TP.layers) : 0;
        LayerGrid lg;
        MeltViscosity mv;
        double Tm = 0.0, Tw = 0.0, Tnf = 0.0, rhoCp = 1.0, kMelt = 0.0;
        std::vector<double> phi1, phi2, frac1, frac2, Tl, heatAcc, fq, fqT, dofSizeM, outRate;
        std::vector<char> hasT;
        std::vector<double> frontT;
        if (thermal)
        {
            lg.build(nz);
            Tm = TP.meltTempC + kZeroC;
            Tnf = TP.noFlowTempC + kZeroC;
            rhoCp = TP.meltDensity * TP.meltSpecificHeat;
            kMelt = TP.meltConductivity;
            const double eMelt = std::sqrt(std::max(0.0, kMelt * rhoCp));
            Tw = ContactTemperature(Tm, eMelt, TP.mouldTempC + kZeroC, TP.mouldEffusivity);
            mv.init(TP.viscosity, Tnf, std::min(Tw, Tnf) - 5.0, Tm + 80.0);
            phi1.assign(e1.size() * (size_t)nz, 0.0); frac1.assign(e1.size() * (size_t)nz, 0.0);
            phi2.assign(e2.size() * (size_t)nz, 0.0); frac2.assign(e2.size() * (size_t)nz, 0.0);
            Tl.assign((size_t)ndof * (size_t)nz, Tm);
            heatAcc.assign((size_t)ndof * (size_t)nz, 0.0);
            outRate.assign((size_t)ndof * (size_t)nz, 0.0);
            fq.assign((size_t)ndof, 0.0); fqT.assign((size_t)ndof, 0.0);
            hasT.assign((size_t)ndof, 0);
            frontT.assign((size_t)ndof, std::numeric_limits<double>::quiet_NaN());
            dofSizeM.resize((size_t)ndof);
            for (int i = 0; i < ndof; ++i) dofSizeM[(size_t)i] = dofSize[(size_t)i] * 1.0e-3;
            R.meltTempC = (float)TP.meltTempC;
            R.wallTempC = (float)(Tw - kZeroC);
        }
        auto e1Integral = [&](size_t i) {
            const El1& el = e1[i];
            return el.widthFactor * FluidityIntegral(lg, el.geom, el.size, &phi1[i * (size_t)nz]) / el.L;
        };
        auto e2Integral = [&](size_t i) {
            return FluidityIntegral(lg, GapGeom::Slab, 0.5 * e2[i].h, &phi2[i * (size_t)nz]);
        };

        // Initial viscosity guesses: the full shot rate through each feed section,
        // a typical 1000 1/s in the cavity. Picard corrects them on the first step.
        if (!thermal)
        {
            for (El1& el : e1)
            {
                el.eta = std::max(1.0e-3, P.viscosity(std::max(1.0, ApparentWallShearRate(el.sec, Q))));
                el.coef = el.G / (el.eta * el.L);
            }
            for (El2& el : e2)
            {
                el.eta = std::max(1.0e-3, P.viscosity(1000.0));
                el.S = el.h * el.h * el.h / (12.0 * el.eta);
            }
        }
        else
        {
            for (size_t i = 0; i < e1.size(); ++i)
            {
                const double gw = std::max(1.0, ApparentWallShearRate(e1[i].sec, Q));
                for (int k = 0; k < nz; ++k) phi1[i * (size_t)nz + (size_t)k] = mv.phi(gw * lg.xc[(size_t)k], Tm);
                e1[i].coef = e1Integral(i);
            }
            for (size_t i = 0; i < e2.size(); ++i)
            {
                for (int k = 0; k < nz; ++k) phi2[i * (size_t)nz + (size_t)k] = mv.phi(1000.0 * lg.xc[(size_t)k], Tm);
                e2[i].S = e2Integral(i);
            }
        }

        // ---- Fill march -----------------------------------------------------------
        const int in = netDof[(size_t)net.inletNode];
        if (in < 0) { R.message = "The sprue inlet is not part of the melt path."; return R; }
        std::vector<double> c((size_t)ndof, 0.0), tf((size_t)ndof, -1.0), p((size_t)ndof, 0.0), inflow((size_t)ndof, 0.0);
        std::vector<char> full((size_t)ndof, 0);
        double t = V[(size_t)in] / Q;
        c[(size_t)in] = V[(size_t)in]; full[(size_t)in] = 1; tf[(size_t)in] = t;
        if (thermal) { hasT[(size_t)in] = 1; frontT[(size_t)in] = Tm; }
        double stored = V[(size_t)in], injected = V[(size_t)in];
        double peakP = 0.0;
        int nFull = 1;
        bool ceiling = false;
        double pLimit = (P.maxInjectionPressureMPa > 0.0) ? P.maxInjectionPressureMPa * 1.0e6 : 0.0;
        if (thermal && pLimit <= 0.0) { pLimit = kSafetyCeilingMPa * 1.0e6; ceiling = true; }
        const double vpFrac = std::min(1.0, std::max(0.0, P.vpSwitchFraction));
        // Progress: the fill's share of the bar (packing takes the rest).
        const double progFill = (thermal && P.pack.enabled) ? 0.4 : 1.0;
        std::vector<char> fixedT((size_t)ndof, 0);
        fixedT[(size_t)in] = 1;

        // V/P switchover snapshot.
        bool snapTaken = false;
        std::vector<double> pSnap;
        double snapT = 0.0, snapInlet = 0.0;

        // Excess volume from a full node goes to the nearest unfilled node(s):
        // its unfilled neighbours if it has any, else the first unfilled layer
        // reached through filled nodes (a node the front left behind). Returns
        // false only when nothing is left to fill.
        std::vector<int> stamp((size_t)ndof, 0);
        int stampId = 0;
        std::vector<int> over;
        auto give = [&](int from, double excess) -> bool
        {
            ++stampId;
            std::vector<int> layer{ from }, next, targets;
            stamp[(size_t)from] = stampId;
            while (!layer.empty())
            {
                next.clear(); targets.clear();
                for (int u : layer)
                    for (int v : nb[(size_t)u])
                    {
                        if (stamp[(size_t)v] == stampId) continue;
                        stamp[(size_t)v] = stampId;
                        (full[(size_t)v] ? next : targets).push_back(v);
                    }
                if (!targets.empty())
                {
                    const double share = excess / (double)targets.size();
                    for (int v : targets)
                    {
                        c[(size_t)v] += share;
                        if (c[(size_t)v] >= V[(size_t)v] * (1.0 - 1.0e-12)) over.push_back(v);
                    }
                    return true;
                }
                layer.swap(next);
            }
            return false;
        };

        // Wall shear rate diagnostics (both modes), from the last solve's field.
        std::vector<float> edgeShear(net.edges.size(), 0.0f), stepShear(net.edges.size(), 0.0f);
        double maxCavShear = 0.0, stepCavShear = 0.0;

        // Thermal scratch.
        struct Pair { int from, to; double q; const double* fr; };
        std::vector<Pair> pairs;
        std::vector<int> inStart, inFrom, order;
        std::vector<double> inQ;
        std::vector<const double*> inFrac;
        std::vector<double> Tavg((size_t)std::max(1, nz));
        std::vector<int> newlyFull, newEls;

        std::vector<int> cidx((size_t)ndof, -1);
        std::vector<Eigen::Triplet<double>> trip;
        std::vector<char> isFront((size_t)ndof, 0);
        std::vector<int> front;
        double lastQeff = Q, injectedBeforeLast = injected, tBeforeLast = t;
        double pinLastStep = -1.0;
        double pressureWork = 0.0;

        // Animation frames: the state at the start of a step (pressure from this
        // step's solve, temperatures before its thermal update, the filled set
        // so far), taken when the time passes the next frame mark.
        const int frameTarget = std::max(0, P.animationFrames);
        double frameDt = (frameTarget > 0) ? fillTime / frameTarget : 0.0;
        double nextFrameT = 0.0;
        auto recordFrame = [&](double pinNow)
        {
            FillFrame f;
            f.timeS = (float)t; f.filledFrac = (float)(stored / Vtot); f.inletMPa = (float)(pinNow / 1.0e6);
            R.frames.push_back(f);
            for (size_t k = 0; k < parts.size(); ++k)
            {
                if (partBase[k] < 0) continue;
                PartFillResult& pr = R.parts[k];
                const size_t nn = parts[k].nodes.size();
                std::vector<float> pf(nn, 0.0f), tb, fz;
                if (thermal) { tb.assign(nn, -1.0f); fz.assign(nn, -1.0f); }
                for (size_t i = 0; i < nn; ++i)
                {
                    const size_t d = (size_t)(partBase[k] + (int)i);
                    if (!full[d]) continue;
                    pf[i] = (float)(p[d] / 1.0e6);
                    if (thermal && hasT[d])
                    {
                        const double* Td = &Tl[d * (size_t)nz];
                        tb[i] = (float)(MeanTemperature(lg, dofGeom[d], Td) - kZeroC);
                        fz[i] = (float)(100.0 * FrozenFraction(lg, Td, Tw, Tnf));
                    }
                }
                pr.framePressureMPa.push_back(std::move(pf));
                if (thermal) { pr.frameBulkTempC.push_back(std::move(tb)); pr.frameFrozenPct.push_back(std::move(fz)); }
            }
            while (nextFrameT <= t) nextFrameT += frameDt;
            // A long (pressure-limited) fill: keep every other frame, double the spacing.
            if ((int)R.frames.size() > 3 * frameTarget)
            {
                auto thin = [](auto& v) { size_t w = 0; for (size_t r = 0; r < v.size(); r += 2) v[w++] = std::move(v[r]); v.resize(w); };
                thin(R.frames);
                for (PartFillResult& pr : R.parts) { thin(pr.framePressureMPa); thin(pr.frameBulkTempC); thin(pr.frameFrozenPct); }
                frameDt *= 2.0;
                nextFrameT = 0.0;
                while (nextFrameT <= t) nextFrameT += frameDt;
            }
        };

        // Mean layer temperature of an element over its filled corners.
        auto elemTemps = [&](const int* nodes, int cnt) -> const double*
        {
            std::fill(Tavg.begin(), Tavg.end(), 0.0);
            int m = 0;
            for (int j = 0; j < cnt; ++j)
            {
                const int d = nodes[j];
                if (!full[(size_t)d] || !hasT[(size_t)d]) continue;
                const double* Td = &Tl[(size_t)d * (size_t)nz];
                for (int k = 0; k < nz; ++k) Tavg[(size_t)k] += Td[k];
                ++m;
            }
            if (m == 0) std::fill(Tavg.begin(), Tavg.end(), Tm);
            else for (int k = 0; k < nz; ++k) Tavg[(size_t)k] /= m;
            return Tavg.data();
        };
        // Lagged-shear-rate Picard update of a layer fluidity profile.
        auto updatePhi = [&](double* ph, const double* Tk, GapGeom geom, double size, double grad) -> void
        {
            const double floorPhi = mv.frozenPhi();
            for (int k = 0; k < nz; ++k)
            {
                if (mv.frozen(Tk[k])) { ph[k] = floorPhi; continue; }
                const double tau = LayerStress(geom, size, lg.xc[(size_t)k], grad);
                const double old = ph[k];
                const double tgt = mv.phi(tau * old, Tk[k]);
                ph[k] = (old <= 10.0 * floorPhi) ? tgt : std::sqrt(old * tgt);
            }
        };

        int step = 0;
        for (; step < P.maxSteps; ++step)
        {
            if (nFull >= ndof) { R.complete = true; break; }
            if (t > P.shortShotTimeFactor * fillTime)
            {
                R.shortShot = true;
                R.message = thermal
                    ? "Short shot: the frozen layer choked the flow — at the machine pressure limit the fill slowed past " +
                      std::to_string((int)P.shortShotTimeFactor) + "x the target time."
                    : "Short shot: at the machine pressure limit the fill slowed past " +
                      std::to_string((int)P.shortShotTimeFactor) + "x the target time (freeze-off likely).";
                break;
            }

            // Front: unfilled nodes next to a full one.
            front.clear();
            std::fill(isFront.begin(), isFront.end(), 0);
            for (int i = 0; i < ndof; ++i)
            {
                if (!full[(size_t)i]) continue;
                for (int j : nb[(size_t)i])
                    if (!full[(size_t)j] && !isFront[(size_t)j]) { isFront[(size_t)j] = 1; front.push_back(j); }
            }
            if (front.empty())
            {
                R.shortShot = true;
                R.message = "The melt front stopped: the remaining regions are not connected to the filled ones.";
                break;
            }

            int nf = 0;
            for (int i = 0; i < ndof; ++i) cidx[(size_t)i] = full[(size_t)i] ? nf++ : -1;

            // Elements joining the flow this step start from the viscosity already
            // established around their filled nodes (log-average of live elements
            // there), not the generic first guess — otherwise each new ring of
            // elements near a gate starts far too stiff and spikes the pressure.
            {
                newEls.clear();
                for (int e = 0; e < (int)e2.size(); ++e)
                {
                    const El2& el = e2[(size_t)e];
                    if (el.seen) continue;
                    if (!full[(size_t)el.n[0]] && !full[(size_t)el.n[1]] && !full[(size_t)el.n[2]]) continue;
                    newEls.push_back(e);
                }
                std::vector<double> sLk((size_t)std::max(1, nz));
                for (int e : newEls)
                {
                    El2& el = e2[(size_t)e];
                    double sL = 0.0; int cn = 0;
                    std::fill(sLk.begin(), sLk.end(), 0.0);
                    for (int j = 0; j < 3; ++j)
                    {
                        const int d = el.n[j];
                        for (int m = n2eStart[(size_t)d]; m < n2eStart[(size_t)d + 1]; ++m)
                        {
                            const El2& f = e2[(size_t)n2e[(size_t)m]];
                            if (!f.seen) continue;           // new ones are marked after this loop
                            ++cn;
                            if (!thermal) sL += std::log(f.eta);
                            else
                                for (int k = 0; k < nz; ++k)
                                    sLk[(size_t)k] += std::log(phi2[(size_t)n2e[(size_t)m] * (size_t)nz + (size_t)k]);
                        }
                    }
                    if (cn > 0)
                    {
                        if (!thermal)
                        {
                            el.eta = std::exp(sL / cn);
                            el.S = el.h * el.h * el.h / (12.0 * el.eta);
                        }
                        else
                        {
                            for (int k = 0; k < nz; ++k)
                                phi2[(size_t)e * (size_t)nz + (size_t)k] = std::exp(sLk[(size_t)k] / cn);
                            el.S = e2Integral((size_t)e);
                        }
                    }
                }
                for (int e : newEls) { e2[(size_t)e].seen = true; e2[(size_t)e].seenStep = step; }
                for (El1& el : e1)
                    if (!el.seen && (full[(size_t)el.a] || full[(size_t)el.b])) el.seen = true;
            }

            const int minPasses = std::max(1, P.maxPicard);
            const int passes = (step == 0) ? std::max(10, minPasses)
                                           : std::max(minPasses, P.maxPicardAdaptive);
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt;
            Eigen::SparseMatrix<double> Kff(nf, nf);
            bool analysed = false;
            double scale = 1.0;
            double pinPrev = -1.0;
            for (int pass = 0; pass < passes; ++pass)
            {
                // Assemble the filled x filled block over active elements.
                trip.clear();
                for (const El1& el : e1)
                {
                    const int ia = cidx[(size_t)el.a], ib = cidx[(size_t)el.b];
                    if (ia < 0 && ib < 0) continue;
                    const double k = el.coef;
                    if (ia >= 0) trip.emplace_back(ia, ia, k);
                    if (ib >= 0) trip.emplace_back(ib, ib, k);
                    if (ia >= 0 && ib >= 0) { trip.emplace_back(ia, ib, -k); trip.emplace_back(ib, ia, -k); }
                }
                for (const El2& el : e2)
                {
                    const int ci[3] = { cidx[(size_t)el.n[0]], cidx[(size_t)el.n[1]], cidx[(size_t)el.n[2]] };
                    if (ci[0] < 0 && ci[1] < 0 && ci[2] < 0) continue;
                    const double S = el.S;
                    for (int a = 0; a < 3; ++a)
                    {
                        if (ci[a] < 0) continue;
                        for (int b = 0; b < 3; ++b)
                            if (ci[b] >= 0) trip.emplace_back(ci[a], ci[b], S * el.K[a][b]);
                    }
                }
                Kff.setFromTriplets(trip.begin(), trip.end());
                if (!analysed) { ldlt.analyzePattern(Kff); analysed = true; }
                ldlt.factorize(Kff);
                if (ldlt.info() != Eigen::Success) { R.message = "Pressure solve failed (matrix not factorable)."; return R; }
                Eigen::VectorXd b = Eigen::VectorXd::Zero(nf);
                b[cidx[(size_t)in]] = Q;
                const Eigen::VectorXd x = ldlt.solve(b);
                if (ldlt.info() != Eigen::Success) { R.message = "Pressure solve failed."; return R; }
                ++R.solves;

                // Pressure control at the machine limit: the system is linear in Q
                // for fixed viscosities, so scaling the field scales the flow rate.
                scale = 1.0;
                if (pLimit > 0.0 && x[cidx[(size_t)in]] > pLimit) scale = pLimit / x[cidx[(size_t)in]];
                for (int i = 0; i < ndof; ++i) p[(size_t)i] = full[(size_t)i] ? scale * x[cidx[(size_t)i]] : 0.0;

                // Inflow into the front, consistent with this solve's conductances.
                for (int i : front) inflow[(size_t)i] = 0.0;
                for (El1& el : e1)
                {
                    el.coefSolve = el.coef;
                    const bool fa = full[(size_t)el.a], fb = full[(size_t)el.b];
                    if (!fa && !fb) continue;
                    const double q = el.coef * (p[(size_t)el.a] - p[(size_t)el.b]);
                    if (!fb) inflow[(size_t)el.b] += q;
                    if (!fa) inflow[(size_t)el.a] -= q;
                }
                for (El2& el : e2)
                {
                    el.Ssolve = el.S;
                    if (!full[(size_t)el.n[0]] && !full[(size_t)el.n[1]] && !full[(size_t)el.n[2]]) continue;
                    for (int a = 0; a < 3; ++a)
                    {
                        if (full[(size_t)el.n[a]]) continue;
                        double r = 0.0;
                        for (int b2 = 0; b2 < 3; ++b2) r += el.K[a][b2] * p[(size_t)el.n[b2]];
                        inflow[(size_t)el.n[a]] -= el.S * r;
                    }
                }

                // Viscosity / fluidity for the next solve (log-relaxed Picard on the
                // lagged shear rate).
                double maxRel = 0.0;
                if (!thermal)
                {
                    for (El1& el : e1)
                    {
                        if (!full[(size_t)el.a] && !full[(size_t)el.b]) continue;
                        const double q = el.coef * (p[(size_t)el.a] - p[(size_t)el.b]);
                        const double tgt = std::max(1.0e-3, P.viscosity(ApparentWallShearRate(el.sec, q)));
                        maxRel = std::max(maxRel, std::fabs(tgt - el.eta) / el.eta);
                        el.eta = std::exp(0.5 * std::log(el.eta) + 0.5 * std::log(tgt));
                        el.coef = el.G / (el.eta * el.L);
                    }
                    for (El2& el : e2)
                    {
                        if (!full[(size_t)el.n[0]] && !full[(size_t)el.n[1]] && !full[(size_t)el.n[2]]) continue;
                        const dvec3 gp = el.g[0] * p[(size_t)el.n[0]] + el.g[1] * p[(size_t)el.n[1]] + el.g[2] * p[(size_t)el.n[2]];
                        const double gd = el.h * glm::length(gp) / (2.0 * el.eta);
                        const double tgt = std::max(1.0e-3, P.viscosity(gd));
                        maxRel = std::max(maxRel, std::fabs(tgt - el.eta) / el.eta);
                        el.eta = std::exp(0.5 * std::log(el.eta) + 0.5 * std::log(tgt));
                        el.S = el.h * el.h * el.h / (12.0 * el.eta);
                    }
                }
                else
                {
                    // Before updating: layer flow shares, viscous heating and wall
                    // shear from the fluidities this solve used (so the heat put
                    // into the melt equals the flow work of this field). Each
                    // element's whole dissipation goes to its filled, non-inlet
                    // corners, per layer in the element's own geometry.
                    std::fill(heatAcc.begin(), heatAcc.end(), 0.0);
                    stepShear.assign(net.edges.size(), 0.0f);
                    stepCavShear = 0.0;
                    const bool heatOn = TP.viscousHeating;
                    for (size_t i = 0; i < e1.size(); ++i)
                    {
                        El1& el = e1[i];
                        if (!full[(size_t)el.a] && !full[(size_t)el.b]) continue;
                        const int nodes[2] = { el.a, el.b };
                        const double* Tk = elemTemps(nodes, 2);
                        const double grad = std::fabs(p[(size_t)el.a] - p[(size_t)el.b]) / el.L;
                        const double* ph = &phi1[i * (size_t)nz];
                        LayerFlowFractions(lg, el.geom, ph, &frac1[i * (size_t)nz]);
                        const bool ra = full[(size_t)el.a] && !fixedT[(size_t)el.a];
                        const bool rb = full[(size_t)el.b] && !fixedT[(size_t)el.b];
                        const int nr = (int)ra + (int)rb;
                        const double w = nr ? (double)el.sec.areaMm2 * el.L / nr : 0.0;
                        const int gi = (el.geom == GapGeom::Slab) ? 0 : 1;
                        double gmax = 0.0;
                        for (int k = 0; k < nz; ++k)
                        {
                            const double tau = LayerStress(el.geom, el.size, lg.xc[(size_t)k], grad);
                            gmax = std::max(gmax, tau * ph[k]);
                            if (!heatOn || !nr) continue;
                            const double Pk = w * lg.vf[gi][(size_t)k] * tau * tau * ph[k];
                            if (ra) heatAcc[(size_t)el.a * (size_t)nz + (size_t)k] += Pk;
                            if (rb) heatAcc[(size_t)el.b * (size_t)nz + (size_t)k] += Pk;
                        }
                        stepShear[(size_t)el.edge] = std::max(stepShear[(size_t)el.edge], (float)gmax);
                        updatePhi(&phi1[i * (size_t)nz], Tk, el.geom, el.size, grad);
                        const double nc = e1Integral(i);
                        maxRel = std::max(maxRel, std::fabs(nc - el.coef) / std::max(1.0e-300, el.coef));
                        el.coef = nc;
                    }
                    for (size_t i = 0; i < e2.size(); ++i)
                    {
                        El2& el = e2[i];
                        if (!full[(size_t)el.n[0]] && !full[(size_t)el.n[1]] && !full[(size_t)el.n[2]]) continue;
                        const double* Tk = elemTemps(el.n, 3);
                        const dvec3 gp = el.g[0] * p[(size_t)el.n[0]] + el.g[1] * p[(size_t)el.n[1]] + el.g[2] * p[(size_t)el.n[2]];
                        const double grad = glm::length(gp);
                        const double* ph = &phi2[i * (size_t)nz];
                        LayerFlowFractions(lg, GapGeom::Slab, ph, &frac2[i * (size_t)nz]);
                        bool rc[3]; int nr = 0;
                        for (int j = 0; j < 3; ++j) { rc[j] = full[(size_t)el.n[j]] && !fixedT[(size_t)el.n[j]]; nr += rc[j]; }
                        const double w = nr ? el.vol / nr : 0.0;
                        double gmax = 0.0;
                        for (int k = 0; k < nz; ++k)
                        {
                            const double tau = LayerStress(GapGeom::Slab, 0.5 * el.h, lg.xc[(size_t)k], grad);
                            gmax = std::max(gmax, tau * ph[k]);
                            if (!heatOn || !nr) continue;
                            const double Pk = w * lg.vf[0][(size_t)k] * tau * tau * ph[k];
                            for (int j = 0; j < 3; ++j)
                                if (rc[j]) heatAcc[(size_t)el.n[j] * (size_t)nz + (size_t)k] += Pk;
                        }
                        stepCavShear = std::max(stepCavShear, gmax);
                        updatePhi(&phi2[i * (size_t)nz], Tk, GapGeom::Slab, 0.5 * el.h, grad);
                        const double nS = e2Integral(i);
                        maxRel = std::max(maxRel, std::fabs(nS - el.S) / std::max(1.0e-300, el.S));
                        el.S = nS;
                    }
                }
                if (maxRel < P.picardTol) break;
                // Adaptive: compare the inlet pressure with the previous pass (or,
                // on the first pass, the previous step). Smooth phases move < 1%
                // per step and stop here; an abrupt change keeps iterating.
                const double pinNow = p[(size_t)in];
                const double ref = (pass == 0) ? pinLastStep : pinPrev;
                if (step > 0 && pass + 1 >= minPasses && ref > 0.0 &&
                    std::fabs(pinNow - ref) <= P.picardTolAdaptive * ref) break;
                pinPrev = pinNow;
            }
            const double Qeff = Q * scale;
            if (scale < 1.0) R.pressureLimited = true;
            pinLastStep = p[(size_t)in];

            // The discrete operator is not an M-matrix where triangles are obtuse,
            // so a front node can show a sliver of negative inflow. Only positive
            // inflows advance the front; rescale them to carry exactly Qeff so
            // the stored volume matches the injected volume.
            {
                double sumPos = 0.0;
                for (int i : front) if (inflow[(size_t)i] > 0.0) sumPos += inflow[(size_t)i];
                if (sumPos > 0.0)
                {
                    const double k = Qeff / sumPos;
                    for (int i : front) inflow[(size_t)i] = (inflow[(size_t)i] > 0.0) ? inflow[(size_t)i] * k : 0.0;
                }
            }

            const double pin = p[(size_t)in];
            // Injection pressure = the peak over the velocity-controlled phase (up
            // to V/P switchover). Past it the last few nodes funnel the whole flow
            // and the inlet spikes, which is exactly why machines switch over.
            if (!snapTaken) peakP = std::max(peakP, pin);
            R.historyTimeS.push_back((float)t);
            R.historyInletMPa.push_back((float)(pin / 1.0e6));
            R.historyFilledFrac.push_back((float)(stored / Vtot));
            if (P.animationFrames > 0 && t >= nextFrameT) recordFrame(pin);

            // V/P switchover: snapshot the field that drives the step crossing it.
            if (!snapTaken && stored >= vpFrac * Vtot)
            {
                snapTaken = true; pSnap = p; snapT = t; snapInlet = pin;
            }

            // Step: the time the fastest `frontFractionPerStep` of the front needs.
            std::vector<std::pair<double, int>> need;
            need.reserve(front.size());
            for (int i : front)
                if (inflow[(size_t)i] > 1.0e-18)
                    need.push_back({ std::max(0.0, V[(size_t)i] - c[(size_t)i]) / inflow[(size_t)i], i });
            if (need.empty())
            {
                R.shortShot = true;
                R.message = "The melt front stopped (no inflow at the front).";
                break;
            }
            const double frac = std::min(1.0, std::max(0.0, P.frontFractionPerStep));
            const size_t kq = (size_t)std::floor(frac * (double)(need.size() - 1));
            std::nth_element(need.begin(), need.begin() + (std::ptrdiff_t)kq, need.end());
            double dt = std::max(need[kq].first, 1.0e-12 * fillTime);
            // A stalled front (pressure-limited, freezing) would take one huge step;
            // stop at the short-shot time instead and leave the rest unfilled.
            const double tStop = P.shortShotTimeFactor * fillTime * (1.0 + 1.0e-9);
            if (t + dt > tStop) dt = std::max(tStop - t, 1.0e-12 * fillTime);

            // ---- Wall shear diagnostics + thermal update over dt ------------------
            if (!thermal)
            {
                for (const El1& el : e1)
                {
                    if (!full[(size_t)el.a] && !full[(size_t)el.b]) continue;
                    const double gd = ApparentWallShearRate(el.sec, el.coefSolve * (p[(size_t)el.a] - p[(size_t)el.b]));
                    edgeShear[(size_t)el.edge] = std::max(edgeShear[(size_t)el.edge], (float)gd);
                }
                for (const El2& el : e2)
                {
                    if (!full[(size_t)el.n[0]] && !full[(size_t)el.n[1]] && !full[(size_t)el.n[2]]) continue;
                    const dvec3 gp = el.g[0] * p[(size_t)el.n[0]] + el.g[1] * p[(size_t)el.n[1]] + el.g[2] * p[(size_t)el.n[2]];
                    const double etaSolve = el.h * el.h * el.h / (12.0 * el.Ssolve);
                    maxCavShear = std::max(maxCavShear, el.h * glm::length(gp) / (2.0 * etaSolve));
                }
            }
            else
            {
                pairs.clear();
                for (int i : front) { fq[(size_t)i] = 0.0; fqT[(size_t)i] = 0.0; }
                for (size_t ei = 0; ei < edgeShear.size(); ++ei) edgeShear[ei] = std::max(edgeShear[ei], stepShear[ei]);
                maxCavShear = std::max(maxCavShear, stepCavShear);
                for (size_t i = 0; i < e1.size(); ++i)
                {
                    const El1& el = e1[i];
                    const bool fa = full[(size_t)el.a], fb = full[(size_t)el.b];
                    if (!fa && !fb) continue;
                    const double* fr = &frac1[i * (size_t)nz];
                    const double q = el.coefSolve * (p[(size_t)el.a] - p[(size_t)el.b]);
                    if (q > 0.0 && fa) pairs.push_back({ el.a, el.b, q, fr });
                    else if (q < 0.0 && fb) pairs.push_back({ el.b, el.a, -q, fr });
                }
                for (size_t i = 0; i < e2.size(); ++i)
                {
                    const El2& el = e2[i];
                    const bool f3[3] = { (bool)full[(size_t)el.n[0]], (bool)full[(size_t)el.n[1]], (bool)full[(size_t)el.n[2]] };
                    if (!f3[0] && !f3[1] && !f3[2]) continue;
                    const double* fr = &frac2[i * (size_t)nz];
                    static const int pr[3][2] = { {0,1}, {1,2}, {2,0} };
                    for (const auto& ab : pr)
                    {
                        // Pairwise edge flux (exact decomposition of the element's
                        // nodal fluxes; an obtuse corner gives a negative coupling,
                        // whose flux then runs against the pressure difference).
                        const double cab = -el.Ssolve * el.K[ab[0]][ab[1]];
                        if (cab == 0.0) continue;
                        const int na = el.n[ab[0]], nbn = el.n[ab[1]];
                        const double q = cab * (p[(size_t)na] - p[(size_t)nbn]);
                        if (q > 0.0 && f3[ab[0]]) pairs.push_back({ na, nbn, q, fr });
                        else if (q < 0.0 && f3[ab[1]]) pairs.push_back({ nbn, na, -q, fr });
                    }
                }

                // Convection: incoming fluxes of each filled node (CSR), swept
                // from the highest pressure down.
                inStart.assign((size_t)ndof + 1, 0);
                for (const Pair& pp : pairs) if (full[(size_t)pp.to]) ++inStart[(size_t)pp.to + 1];
                for (int i = 0; i < ndof; ++i) inStart[(size_t)i + 1] += inStart[(size_t)i];
                const size_t nIn = (size_t)inStart[(size_t)ndof];
                inFrom.resize(nIn); inQ.resize(nIn); inFrac.resize(nIn);
                {
                    std::vector<int> fillp(inStart.begin(), inStart.end() - 1);
                    for (const Pair& pp : pairs)
                    {
                        if (!full[(size_t)pp.to]) continue;
                        const size_t m = (size_t)fillp[(size_t)pp.to]++;
                        inFrom[m] = pp.from; inQ[m] = pp.q; inFrac[m] = pp.fr;
                    }
                }
                std::fill(outRate.begin(), outRate.end(), 0.0);
                for (const Pair& pp : pairs)
                    for (int k = 0; k < nz; ++k) outRate[(size_t)pp.from * (size_t)nz + (size_t)k] += pp.q * pp.fr[k];
                order.clear();
                for (int i = 0; i < ndof; ++i) if (full[(size_t)i]) order.push_back(i);
                std::sort(order.begin(), order.end(), [&](int a, int b2) { return p[(size_t)a] > p[(size_t)b2]; });
                // Heating power per layer -> W/m^3 of each node's layers.
                if (TP.viscousHeating)
                    for (int i : order)
                    {
                        const int gi = (dofGeom[(size_t)i] == GapGeom::Slab) ? 0 : 1;
                        for (int k = 0; k < nz; ++k)
                            heatAcc[(size_t)i * (size_t)nz + (size_t)k] /= (V[(size_t)i] * lg.vf[gi][(size_t)k]);
                    }
                // Convection + conduction to the wall + viscous heating, one
                // implicit solve per volume, swept from the highest pressure down.
                ThermalSweep(lg, order, dt, V, dofGeom, dofSizeM, kMelt, rhoCp, Tw, inStart, inFrom, inQ, inFrac,
                             outRate, TP.viscousHeating ? heatAcc.data() : nullptr, &fixedT, Tl);

                // Melt arriving at the front: cup-mixing temperature of what flows in.
                for (const Pair& pp : pairs)
                {
                    if (full[(size_t)pp.to]) continue;
                    const double* Tf = &Tl[(size_t)pp.from * (size_t)nz];
                    double cup = 0.0;
                    for (int k = 0; k < nz; ++k) cup += pp.fr[k] * Tf[k];
                    fq[(size_t)pp.to] += pp.q;
                    fqT[(size_t)pp.to] += pp.q * cup;
                }
            }

            // Advance; overflow goes to the nearest unfilled node(s).
            injectedBeforeLast = injected; tBeforeLast = t; lastQeff = Qeff;
            over.clear();
            newlyFull.clear();
            for (const auto& ni : need)
            {
                const int i = ni.second;
                const double add = inflow[(size_t)i] * dt;
                c[(size_t)i] += add;
                stored += add;
                if (c[(size_t)i] >= V[(size_t)i] * (1.0 - 1.0e-12))
                {
                    tf[(size_t)i] = t + std::min(ni.first, dt);
                    over.push_back(i);
                }
            }
            injected += Qeff * dt;
            pressureWork += pin * Qeff * dt * 1.0e-9;      // Pa mm^3 -> J
            while (!over.empty())
            {
                const int i = over.back(); over.pop_back();
                if (full[(size_t)i]) continue;
                const double excess = c[(size_t)i] - V[(size_t)i];
                c[(size_t)i] = V[(size_t)i];
                full[(size_t)i] = 1; ++nFull;
                newlyFull.push_back(i);
                if (tf[(size_t)i] < 0.0) tf[(size_t)i] = t + dt;
                if (excess > 0.0 && !give(i, excess)) stored -= excess;   // cavity full: over-injection
            }
            t += dt;

            // New melt: a uniform profile at its arrival temperature (fountain flow
            // lays the core melt against the wall); overflow-filled nodes take the
            // bulk temperature of their filled neighbours.
            if (thermal && !newlyFull.empty())
            {
                std::vector<int> pending;
                for (int i : newlyFull)
                {
                    if (fq[(size_t)i] > 0.0)
                    {
                        const double Ta = fqT[(size_t)i] / fq[(size_t)i];
                        std::fill(Tl.begin() + (std::ptrdiff_t)((size_t)i * (size_t)nz),
                                  Tl.begin() + (std::ptrdiff_t)((size_t)(i + 1) * (size_t)nz), Ta);
                        frontT[(size_t)i] = Ta; hasT[(size_t)i] = 1;
                    }
                    else pending.push_back(i);
                }
                bool progress = true;
                while (!pending.empty() && progress)
                {
                    progress = false;
                    std::vector<int> still;
                    for (int i : pending)
                    {
                        double s = 0.0; int m = 0;
                        for (int j : nb[(size_t)i])
                            if (hasT[(size_t)j])
                            {
                                s += MeanTemperature(lg, dofGeom[(size_t)j], &Tl[(size_t)j * (size_t)nz]);
                                ++m;
                            }
                        if (m == 0) { still.push_back(i); continue; }
                        const double Ta = s / m;
                        std::fill(Tl.begin() + (std::ptrdiff_t)((size_t)i * (size_t)nz),
                                  Tl.begin() + (std::ptrdiff_t)((size_t)(i + 1) * (size_t)nz), Ta);
                        frontT[(size_t)i] = Ta; hasT[(size_t)i] = 1;
                        progress = true;
                    }
                    pending.swap(still);
                }
                for (int i : pending) { frontT[(size_t)i] = Tm; hasT[(size_t)i] = 1; }
            }

            if (P.progress && (step % 5) == 0 && !P.progress(progFill * std::min(1.0, stored / Vtot)))
            {
                R.cancelled = true;
                R.message = "Cancelled.";
                break;
            }
        }
        if (step >= P.maxSteps && !R.complete && !R.cancelled && !R.shortShot)
        {
            R.shortShot = true;
            R.message = "Stopped at the step limit before the cavity filled.";
        }
        R.steps = step;
        if (!snapTaken) { pSnap = p; snapT = t; snapInlet = p[(size_t)in]; }
        if (ceiling && R.pressureLimited)
            R.warnings.push_back("No machine pressure limit was set; the fill hit the " +
                                 std::to_string((int)kSafetyCeilingMPa) + " MPa safety ceiling.");

        // ---- Results ----------------------------------------------------------------
        double tEnd = 0.0;
        for (int i = 0; i < ndof; ++i) if (tf[(size_t)i] > tEnd) tEnd = tf[(size_t)i];
        // Injected volume up to the moment the last node filled (the final step can
        // overshoot the end of fill).
        const double injectedAtEnd = R.complete
            ? injectedBeforeLast + lastQeff * std::max(0.0, tEnd - tBeforeLast) : injected;
        R.massErrorPct = (injectedAtEnd > 0.0) ? (float)(100.0 * (stored - injectedAtEnd) / injectedAtEnd) : 0.0f;
        R.fillTimeS = (float)(R.complete ? tEnd : t);
        R.peakInletPressureMPa = (float)(peakP / 1.0e6);
        R.switchoverInletPressureMPa = (float)(snapInlet / 1.0e6);
        R.switchoverTimeS = (float)snapT;
        R.maxCavityShearRate = (float)maxCavShear;

        // Frozen share of each filled volume at the end of the fill.
        std::vector<double> frozen;
        if (thermal)
        {
            frozen.assign((size_t)ndof, -1.0);
            for (int i = 0; i < ndof; ++i)
                if (full[(size_t)i] && hasT[(size_t)i])
                    frozen[(size_t)i] = FrozenFraction(lg, &Tl[(size_t)i * (size_t)nz], Tw, Tnf);
            double gain = 0.0;
            for (int i = 0; i < ndof; ++i)
                if (full[(size_t)i] && hasT[(size_t)i])
                    gain += V[(size_t)i] * (MeanTemperature(lg, dofGeom[(size_t)i], &Tl[(size_t)i * (size_t)nz]) - Tm);
            R.heatGainJ = (float)(gain * rhoCp * 1.0e-9);            // mm^3 -> m^3
            R.pressureWorkJ = (float)pressureWork;
            R.netFrontTempC.assign((size_t)NN, 0.0f);
            R.netFrozenPct.assign((size_t)NN, -1.0f);
        }

        for (int n = 0; n < NN; ++n)
        {
            const int d = netDof[(size_t)n];
            if (d < 0) continue;
            R.netFillTimeS[(size_t)n] = (float)tf[(size_t)d];
            R.netPressureMPa[(size_t)n] = (float)(pSnap[(size_t)d] / 1.0e6);
            if (thermal && hasT[(size_t)d])
            {
                R.netFrontTempC[(size_t)n] = (float)(frontT[(size_t)d] - kZeroC);
                R.netFrozenPct[(size_t)n] = (float)(100.0 * frozen[(size_t)d]);
            }
        }
        for (size_t ei = 0; ei < net.edges.size(); ++ei)
            R.feedEdges[ei].maxWallShearRate = edgeShear[ei];
        if (thermal)
        {
            for (int d = firstInterior; d < ndof; ++d)
            {
                FeedEdgeFillResult& er = R.feedEdges[(size_t)interiorEdge[(size_t)(d - firstInterior)]];
                if (frozen[(size_t)d] >= 0.0) er.frozenPct = std::max(er.frozenPct, (float)(100.0 * frozen[(size_t)d]));
            }
            for (size_t ei = 0; ei < net.edges.size(); ++ei)
            {
                FeedEdgeFillResult& er = R.feedEdges[ei];
                if (!er.modelled) continue;
                const int da = netDof[(size_t)net.edges[ei].a], db = netDof[(size_t)net.edges[ei].b];
                const int far = (tf[(size_t)db] >= tf[(size_t)da]) ? db : da;
                if (hasT[(size_t)far]) er.frontTempC = (float)(frontT[(size_t)far] - kZeroC);
            }
        }

        double clampN = 0.0;
        float cavTmin = std::numeric_limits<float>::infinity(), cavTmax = -std::numeric_limits<float>::infinity(), cavFz = 0.0f;
        for (size_t k = 0; k < parts.size(); ++k)
        {
            PartFillResult& pr = R.parts[k];
            if (partBase[k] < 0) continue;
            const MidplaneMesh& m = parts[k];
            const int base = partBase[k];
            const size_t nn = m.nodes.size();
            pr.fillTimeS.assign(nn, -1.0f);
            pr.pressureMPa.assign(nn, 0.0f);
            if (thermal) { pr.frontTempC.assign(nn, 0.0f); pr.frozenPct.assign(nn, -1.0f); pr.bulkTempC.assign(nn, -1.0f); }
            double vIn = 0.0, vAll = 0.0, fzV = 0.0, fzW = 0.0;
            float t0 = std::numeric_limits<float>::infinity(), t1 = -1.0f, pMax = 0.0f;
            float tMin = std::numeric_limits<float>::infinity(), tMax = -std::numeric_limits<float>::infinity(), fzMax = 0.0f;
            for (size_t i = 0; i < nn; ++i)
            {
                const size_t d = (size_t)(base + (int)i);
                pr.fillTimeS[i] = full[d] ? (float)tf[d] : -1.0f;
                pr.pressureMPa[i] = (float)(pSnap[d] / 1.0e6);
                if (full[d]) { t0 = std::min(t0, (float)tf[d]); t1 = std::max(t1, (float)tf[d]); }
                pMax = std::max(pMax, pr.pressureMPa[i]);
                vIn += c[d]; vAll += V[d];
                clampN += pSnap[d] * planA[d] * 1.0e-6;     // Pa * mm^2 -> N
                if (thermal && full[d] && hasT[d])
                {
                    const float Tc = (float)(frontT[d] - kZeroC), fz = (float)(100.0 * frozen[d]);
                    pr.frontTempC[i] = Tc; pr.frozenPct[i] = fz;
                    pr.bulkTempC[i] = (float)(MeanTemperature(lg, dofGeom[d], &Tl[d * (size_t)nz]) - kZeroC);
                    tMin = std::min(tMin, Tc); tMax = std::max(tMax, Tc); fzMax = std::max(fzMax, fz);
                    fzV += V[d] * fz; fzW += V[d];
                }
            }
            pr.fillStartS = std::isfinite(t0) ? t0 : -1.0f;
            pr.fillEndS = t1;
            pr.filledPct = (vAll > 0.0) ? (float)(100.0 * vIn / vAll) : 0.0f;
            pr.maxPressureMPa = pMax;
            if (thermal && fzW > 0.0)
            {
                pr.minFrontTempC = tMin; pr.maxFrontTempC = tMax;
                pr.maxFrozenPct = fzMax; pr.meanFrozenPct = (float)(fzV / fzW);
                cavTmin = std::min(cavTmin, tMin); cavTmax = std::max(cavTmax, tMax); cavFz = std::max(cavFz, fzMax);
            }
        }
        R.clampForceTonne = (float)(clampN / kTonN);
        if (thermal && std::isfinite(cavTmin))
        {
            R.minFrontTempC = cavTmin; R.maxFrontTempC = cavTmax; R.maxFrozenPct = cavFz;
        }

        // ==== Packing, holding and cooling =========================================
        // Continues from the end-of-fill state (every volume full). Unknown per
        // volume: its pressure; conserved: its mass M. While a volume is full,
        // M = V * mean_k rho(T_k, p) over its layers (Tait), so cooling (T down,
        // rho up) at fixed mass drops the pressure and draws melt in. Each step:
        //   1. temperatures: convection by the last step's flows + conduction to
        //      the wall (one implicit sweep, no viscous heating — flows are slow);
        //   2. fluidities at the new temperatures (frozen layers carry nothing);
        //   3. pressure, implicit and linearised in p around the last step:
        //        c_i (p_i - p_i^n)/dt + sum rho_e G_e (p_i - p_j) = -(m_i(p^n, T) - M_i)/dt
        //      with c_i = V dm/dp, the inlet at the pack pressure while holding (0
        //      after), and volumes that come out below zero held at zero (active
        //      set: the melt there has pulled away from the wall);
        //   4. masses updated from the solved fluxes (exactly conservative).
        if (thermal && R.complete && P.pack.enabled && !R.cancelled)
        {
            const FillPackParams& PK = P.pack;
            PackResult& K = R.pack;
            K.ran = true;
            const TestMaterial::TaitPVT& pvt = TP.pvt;
            const double Teject = TP.ejectionTempC + kZeroC;
            const double rhoRoom = 1.0 / pvt.specificVolume(TP.roomTempC + kZeroC, 0.0);
            auto rhoOf = [&](double T, double pPa) { return 1.0 / pvt.specificVolume(T, std::max(0.0, pPa)); };
            // d rho / d p within the current (melt / solid) domain.
            auto drhoOf = [&](double T, double pPa)
            {
                pPa = std::max(0.0, pPa);
                const bool melt = T > pvt.b5 + pvt.b6 * pPa;
                const double b1 = melt ? pvt.b1m : pvt.b1s, b2 = melt ? pvt.b2m : pvt.b2s;
                const double b3 = melt ? pvt.b3m : pvt.b3s, b4 = melt ? pvt.b4m : pvt.b4s;
                const double dT = T - pvt.b5;
                const double v0 = b1 + b2 * dT, B = b3 * std::exp(-b4 * dT);
                const double v = v0 * (1.0 - pvt.C * std::log(1.0 + pPa / B));
                return v0 * pvt.C / ((B + pPa) * v * v);
            };
            const double pPack = ((PK.packPressureMPa > 0.0) ? PK.packPressureMPa
                                                            : PK.packFraction * R.peakInletPressureMPa) * 1.0e6;
            K.packPressureMPa = (float)(pPack / 1.0e6);
            const double t0 = R.fillTimeS;

            auto nodeMass = [&](int i, double pPa, double* dmdp)
            {
                const int gi = (dofGeom[(size_t)i] == GapGeom::Slab) ? 0 : 1;
                const double* Ti = &Tl[(size_t)i * (size_t)nz];
                double m = 0.0, d = 0.0;
                for (int k = 0; k < nz; ++k)
                {
                    m += lg.vf[gi][(size_t)k] * rhoOf(Ti[k], pPa);
                    if (dmdp) d += lg.vf[gi][(size_t)k] * drhoOf(Ti[k], pPa);
                }
                if (dmdp) *dmdp = d * V[(size_t)i];
                return m * V[(size_t)i];
            };
            auto isCavity = [&](int i) { return isMid[(size_t)i] != 0; };

            std::vector<double> pk((size_t)ndof), M((size_t)ndof), mT((size_t)ndof), cN((size_t)ndof), rhoN((size_t)ndof);
            for (int i = 0; i < ndof; ++i) { pk[(size_t)i] = std::max(0.0, p[(size_t)i]); M[(size_t)i] = nodeMass(i, pk[(size_t)i], nullptr); }
            double cavMass0 = 0.0;
            for (int i = 0; i < ndof; ++i) if (isCavity(i)) cavMass0 += M[(size_t)i];
            double sysMass0 = 0.0;
            for (int i = 0; i < ndof; ++i) if (i != in) sysMass0 += M[(size_t)i];

            // Seals: each gate's interior sections (fully frozen anywhere = sealed);
            // with no gates, the cavity entry volumes themselves.
            struct Seal { int edge; std::vector<int> dofs; double frozenAt = -1.0; };
            std::vector<Seal> seals;
            for (size_t ei = 0; ei < net.edges.size(); ++ei)
            {
                const FeedEdge& e = net.edges[ei];
                if (!R.feedEdges[ei].modelled || (e.kind != FeedEdgeKind::Gate && e.kind != FeedEdgeKind::SubRunner)) continue;
                Seal s; s.edge = (int)ei;
                for (int d = firstInterior; d < ndof; ++d)
                    if (interiorEdge[(size_t)(d - firstInterior)] == (int)ei) s.dofs.push_back(d);
                if (!s.dofs.empty()) seals.push_back(std::move(s));
            }
            if (seals.empty())
                for (const Entry& en : entries)
                {
                    Seal s; s.edge = -1;
                    s.dofs.push_back(partBase[(size_t)en.part] + en.mid);
                    seals.push_back(std::move(s));
                }

            std::vector<double> ejectT((size_t)ndof, -1.0), tmaxPrev((size_t)ndof, 0.0);
            for (int i = 0; i < ndof; ++i)
            {
                const double* Ti = &Tl[(size_t)i * (size_t)nz];
                tmaxPrev[(size_t)i] = *std::max_element(Ti, Ti + nz);
            }
            int cavityLeft = 0;
            for (int i = 0; i < ndof; ++i) if (isCavity(i)) ++cavityLeft;
            const int cavityTotal = std::max(1, cavityLeft);

            // Pack frames: the state at the end of each step, thinned to PK.frames.
            const size_t fillFrames = R.frames.size();
            auto recordPackFrame = [&](double tAbs, double pinPa, int phase)
            {
                FillFrame f; f.timeS = (float)tAbs; f.filledFrac = 1.0f; f.inletMPa = (float)(pinPa / 1.0e6); f.phase = phase;
                R.frames.push_back(f);
                for (size_t k = 0; k < parts.size(); ++k)
                {
                    if (partBase[k] < 0) continue;
                    PartFillResult& pr = R.parts[k];
                    const size_t nn = parts[k].nodes.size();
                    std::vector<float> pf(nn), tb(nn), fz(nn);
                    for (size_t i = 0; i < nn; ++i)
                    {
                        const size_t d = (size_t)(partBase[k] + (int)i);
                        const double* Td = &Tl[d * (size_t)nz];
                        pf[i] = (float)(pk[d] / 1.0e6);
                        tb[i] = (float)(MeanTemperature(lg, dofGeom[d], Td) - kZeroC);
                        fz[i] = (float)(100.0 * FrozenFraction(lg, Td, Tw, Tnf));
                    }
                    pr.framePressureMPa.push_back(std::move(pf));
                    pr.frameBulkTempC.push_back(std::move(tb));
                    pr.frameFrozenPct.push_back(std::move(fz));
                }
                const size_t nPack = R.frames.size() - fillFrames;
                if (PK.frames > 0 && (int)nPack > 2 * PK.frames)
                {
                    // Keep every other pack frame (the fill's frames stay as they are).
                    auto thinTail = [&](auto& v)
                    {
                        size_t w = fillFrames;
                        for (size_t r = fillFrames; r < v.size(); r += 2) v[w++] = std::move(v[r]);
                        if (v.size() > fillFrames && (v.size() - fillFrames) % 2 == 0) v[w++] = std::move(v.back());   // keep the latest
                        v.resize(w);
                    };
                    thinTail(R.frames);
                    for (PartFillResult& pr : R.parts)
                        if (!pr.framePressureMPa.empty()) { thinTail(pr.framePressureMPa); thinTail(pr.frameBulkTempC); thinTail(pr.frameFrozenPct); }
                }
            };

            std::vector<int> pidx((size_t)ndof, -1);
            std::vector<char> clamp0((size_t)ndof, 0);
            std::vector<double> xsol;
            double tp = 0.0, dt = std::max(1.0e-4, PK.dtInitialS);
            bool holding = true;
            const bool autoHold = PK.holdTimeS <= 0.0;
            const double holdLimit = autoHold ? std::max(0.0, PK.holdMaxS) : PK.holdTimeS;
            double inletMass = 0.0;
            int steps = 0;
            while (true)
            {
                if (holding && !autoHold && tp + dt > holdLimit - 1.0e-9) dt = std::max(1.0e-4, holdLimit - tp);
                const double pin = holding ? pPack : 0.0;

                // ---- 1. temperatures (the last step's flows, no viscous heating) ----
                pairs.clear();
                for (size_t i = 0; i < e1.size(); ++i)
                {
                    const El1& el = e1[i];
                    double* fr = &frac1[i * (size_t)nz];
                    LayerFlowFractions(lg, el.geom, &phi1[i * (size_t)nz], fr);
                    const double q = el.coef * (pk[(size_t)el.a] - pk[(size_t)el.b]);
                    if (q > 0.0) pairs.push_back({ el.a, el.b, q, fr });
                    else if (q < 0.0) pairs.push_back({ el.b, el.a, -q, fr });
                }
                for (size_t i = 0; i < e2.size(); ++i)
                {
                    const El2& el = e2[i];
                    double* fr = &frac2[i * (size_t)nz];
                    LayerFlowFractions(lg, GapGeom::Slab, &phi2[i * (size_t)nz], fr);
                    static const int prs[3][2] = { {0,1}, {1,2}, {2,0} };
                    for (const auto& ab : prs)
                    {
                        const double cab = -el.S * el.K[ab[0]][ab[1]];
                        if (cab == 0.0) continue;
                        const int na = el.n[ab[0]], nbn = el.n[ab[1]];
                        const double q = cab * (pk[(size_t)na] - pk[(size_t)nbn]);
                        if (q > 0.0) pairs.push_back({ na, nbn, q, fr });
                        else if (q < 0.0) pairs.push_back({ nbn, na, -q, fr });
                    }
                }
                inStart.assign((size_t)ndof + 1, 0);
                for (const Pair& pp : pairs) ++inStart[(size_t)pp.to + 1];
                for (int i = 0; i < ndof; ++i) inStart[(size_t)i + 1] += inStart[(size_t)i];
                inFrom.resize(pairs.size()); inQ.resize(pairs.size()); inFrac.resize(pairs.size());
                {
                    std::vector<int> fillp(inStart.begin(), inStart.end() - 1);
                    for (const Pair& pp : pairs)
                    {
                        const size_t m = (size_t)fillp[(size_t)pp.to]++;
                        inFrom[m] = pp.from; inQ[m] = pp.q; inFrac[m] = pp.fr;
                    }
                }
                std::fill(outRate.begin(), outRate.end(), 0.0);
                for (const Pair& pp : pairs)
                    for (int k = 0; k < nz; ++k) outRate[(size_t)pp.from * (size_t)nz + (size_t)k] += pp.q * pp.fr[k];
                order.resize((size_t)ndof);
                std::iota(order.begin(), order.end(), 0);
                std::sort(order.begin(), order.end(), [&](int a, int b2) { return pk[(size_t)a] > pk[(size_t)b2]; });
                ThermalSweep(lg, order, dt, V, dofGeom, dofSizeM, kMelt, rhoCp, Tw, inStart, inFrom, inQ, inFrac,
                             outRate, nullptr, &fixedT, Tl);

                // ---- 2. fluidities at the new temperatures ----
                for (size_t i = 0; i < e1.size(); ++i)
                {
                    El1& el = e1[i];
                    const int nodes[2] = { el.a, el.b };
                    updatePhi(&phi1[i * (size_t)nz], elemTemps(nodes, 2), el.geom, el.size,
                              std::fabs(pk[(size_t)el.a] - pk[(size_t)el.b]) / el.L);
                    el.coef = e1Integral(i);
                }
                for (size_t i = 0; i < e2.size(); ++i)
                {
                    El2& el = e2[i];
                    const dvec3 gp = el.g[0] * pk[(size_t)el.n[0]] + el.g[1] * pk[(size_t)el.n[1]] + el.g[2] * pk[(size_t)el.n[2]];
                    updatePhi(&phi2[i * (size_t)nz], elemTemps(el.n, 3), GapGeom::Slab, 0.5 * el.h, glm::length(gp));
                    el.S = e2Integral(i);
                }

                // ---- 3. pressure (implicit, active set p >= 0) ----
                for (int i = 0; i < ndof; ++i)
                {
                    double d = 0.0;
                    mT[(size_t)i] = nodeMass(i, pk[(size_t)i], &d);
                    cN[(size_t)i] = d;
                    rhoN[(size_t)i] = mT[(size_t)i] / V[(size_t)i];
                }
                std::fill(clamp0.begin(), clamp0.end(), 0);
                std::vector<double> pNew((size_t)ndof, 0.0), inflowN((size_t)ndof, 0.0);
                // Mass conductance of every coupled pair, for the flux sums.
                auto forEachPair = [&](auto&& fn)
                {
                    for (const El1& el : e1)
                        fn(el.a, el.b, 0.5 * (rhoN[(size_t)el.a] + rhoN[(size_t)el.b]) * el.coef);
                    for (const El2& el : e2)
                    {
                        const double rbar = (rhoN[(size_t)el.n[0]] + rhoN[(size_t)el.n[1]] + rhoN[(size_t)el.n[2]]) / 3.0;
                        for (int a = 0; a < 3; ++a)
                            for (int b2 = a + 1; b2 < 3; ++b2)
                            {
                                const double g = -rbar * el.S * el.K[a][b2];
                                if (g != 0.0) fn(el.n[a], el.n[b2], g);
                            }
                    }
                };
                // Active set: a volume is held at p = 0 only while it would hold
                // less melt than fits at p = 0 (it has pulled away from the wall).
                // Solved p < 0 -> hold it; held but receiving more than fits -> release.
                for (int pass = 0; pass < 12; ++pass)
                {
                    int nu = 0;
                    for (int i = 0; i < ndof; ++i) pidx[(size_t)i] = (i == in || clamp0[(size_t)i]) ? -1 : nu++;
                    auto fixedP = [&](int i) { return (i == in) ? pin : 0.0; };
                    trip.clear();
                    Eigen::VectorXd bvec = Eigen::VectorXd::Zero(nu);
                    for (int i = 0; i < ndof; ++i)
                    {
                        const int r = pidx[(size_t)i];
                        if (r < 0) continue;
                        trip.emplace_back(r, r, cN[(size_t)i] / dt);
                        bvec[r] += cN[(size_t)i] / dt * pk[(size_t)i] - (mT[(size_t)i] - M[(size_t)i]) / dt;
                    }
                    auto addPair = [&](int a, int b, double g)          // mass conductance g between a and b
                    {
                        const int ra = pidx[(size_t)a], rb = pidx[(size_t)b];
                        if (ra >= 0) { trip.emplace_back(ra, ra, g); if (rb >= 0) trip.emplace_back(ra, rb, -g); else bvec[ra] += g * fixedP(b); }
                        if (rb >= 0) { trip.emplace_back(rb, rb, g); if (ra >= 0) trip.emplace_back(rb, ra, -g); else bvec[rb] += g * fixedP(a); }
                    };
                    forEachPair(addPair);
                    Eigen::SparseMatrix<double> A(nu, nu);
                    A.setFromTriplets(trip.begin(), trip.end());
                    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
                    solver.compute(A);
                    if (solver.info() != Eigen::Success) { K.message = "Packing solve failed (matrix not factorable)."; break; }
                    const Eigen::VectorXd x = solver.solve(bvec);
                    ++K.solves;
                    bool changed = false;
                    for (int i = 0; i < ndof; ++i)
                    {
                        const int r = pidx[(size_t)i];
                        pNew[(size_t)i] = (r >= 0) ? x[r] : fixedP(i);
                        if (r >= 0 && x[r] < 0.0) { clamp0[(size_t)i] = 1; changed = true; }
                    }
                    std::fill(inflowN.begin(), inflowN.end(), 0.0);
                    forEachPair([&](int a, int b, double g)
                    {
                        const double q = g * (pNew[(size_t)a] - pNew[(size_t)b]);
                        inflowN[(size_t)a] -= q; inflowN[(size_t)b] += q;
                    });
                    for (int i = 0; i < ndof; ++i)
                    {
                        if (!clamp0[(size_t)i] || pidx[(size_t)i] >= 0) continue;     // only volumes held in this pass
                        const double capacity = mT[(size_t)i] - cN[(size_t)i] * pk[(size_t)i];   // mass that fits at p = 0
                        if (M[(size_t)i] + dt * inflowN[(size_t)i] > capacity * (1.0 + 1.0e-9))
                        { clamp0[(size_t)i] = 0; changed = true; }
                    }
                    if (!changed) break;
                }
                if (!K.message.empty()) break;
                for (double& v : pNew) v = std::max(0.0, v);

                // ---- 4. masses from the solved fluxes ----
                auto flow = [&](int a, int b, double g)
                {
                    const double q = g * (pNew[(size_t)a] - pNew[(size_t)b]) * dt;   // mass a -> b
                    M[(size_t)a] -= q; M[(size_t)b] += q;
                    if (a == in) inletMass += q;
                    if (b == in) inletMass -= q;
                };
                for (const El1& el : e1)
                    flow(el.a, el.b, 0.5 * (rhoN[(size_t)el.a] + rhoN[(size_t)el.b]) * el.coef);
                for (const El2& el : e2)
                {
                    const double rbar = (rhoN[(size_t)el.n[0]] + rhoN[(size_t)el.n[1]] + rhoN[(size_t)el.n[2]]) / 3.0;
                    for (int a = 0; a < 3; ++a)
                        for (int b2 = a + 1; b2 < 3; ++b2)
                        {
                            const double g = -rbar * el.S * el.K[a][b2];
                            if (g != 0.0) flow(el.n[a], el.n[b2], g);
                        }
                }
                pk.swap(pNew);
                M[(size_t)in] = nodeMass(in, pk[(size_t)in], nullptr);           // the machine's reservoir
                tp += dt;
                ++steps;
                const double tAbs = t0 + tp;

                // ---- tracking ----
                for (Seal& s : seals)
                    if (s.frozenAt < 0.0)
                        for (int d : s.dofs)
                            if (FrozenFraction(lg, &Tl[(size_t)d * (size_t)nz], Tw, Tnf) >= 0.999) { s.frozenAt = tAbs; break; }
                bool allSealed = !seals.empty();
                for (const Seal& s : seals) allSealed = allSealed && s.frozenAt >= 0.0;
                double cavMax = 0.0;
                for (int i = 0; i < ndof; ++i)
                {
                    if (!isCavity(i)) continue;
                    cavMax = std::max(cavMax, pk[(size_t)i]);
                    if (ejectT[(size_t)i] >= 0.0) continue;
                    const double* Ti = &Tl[(size_t)i * (size_t)nz];
                    const double tmax = *std::max_element(Ti, Ti + nz);
                    if (tmax <= Teject)
                    {
                        // Interpolate the crossing inside the step.
                        const double t0s = tAbs - dt, a = tmaxPrev[(size_t)i];
                        const double f = (a > tmax) ? std::clamp((a - Teject) / (a - tmax), 0.0, 1.0) : 1.0;
                        ejectT[(size_t)i] = t0s + f * dt;
                        --cavityLeft;
                    }
                    tmaxPrev[(size_t)i] = tmax;
                }
                K.historyTimeS.push_back((float)tAbs);
                K.historyInletMPa.push_back((float)(pin / 1.0e6));
                K.historyCavityMaxMPa.push_back((float)(cavMax / 1.0e6));
                R.historyTimeS.push_back((float)tAbs);
                R.historyInletMPa.push_back((float)(pin / 1.0e6));
                R.historyFilledFrac.push_back(1.0f);
                if (P.animationFrames > 0) recordPackFrame(tAbs, pin, holding ? 1 : 2);

                // ---- hold end, stop, next step size ----
                double dtNext = std::min(PK.dtMaxS, dt * 1.25);
                if (holding && (autoHold ? (allSealed || tp >= holdLimit - 1.0e-9) : tp >= holdLimit - 1.0e-9))
                {
                    holding = false;
                    K.holdTimeS = (float)tp;
                    dtNext = std::max(1.0e-4, PK.dtInitialS);                 // the inlet drops: resolve it
                }
                dt = dtNext;
                if (!holding && cavityLeft <= 0) { K.ejectReached = true; break; }
                if (tp >= PK.maxTimeS) break;
                const double ejFrac = 1.0 - (double)cavityLeft / cavityTotal;
                if (P.progress && (steps % 3) == 0 &&
                    !P.progress(progFill + (1.0 - progFill) * std::min(0.999, 0.15 * std::min(1.0, tp / 5.0) + 0.85 * ejFrac)))
                { R.cancelled = true; K.message = "Cancelled while packing."; break; }
            }
            K.steps = steps;
            K.endS = (float)(t0 + tp);
            if (holding) K.holdTimeS = (float)tp;

            // ---- results ----
            double gf = -1.0;
            K.gatesFrozen = !seals.empty();
            for (const Seal& s : seals)
            {
                if (s.frozenAt < 0.0) K.gatesFrozen = false;
                gf = std::max(gf, s.frozenAt);
                if (s.edge >= 0) R.feedEdges[(size_t)s.edge].freezeS = (float)s.frozenAt;
            }
            K.gateFreezeS = K.gatesFrozen ? (float)gf : -1.0f;
            double worstState = 0.0, sysMass1 = 0.0, cavMass1 = 0.0;
            for (int i = 0; i < ndof; ++i)
            {
                if (i == in) continue;
                sysMass1 += M[(size_t)i];
                if (isCavity(i)) cavMass1 += M[(size_t)i];
                if (pk[(size_t)i] > 0.0)
                {
                    const double m = nodeMass(i, pk[(size_t)i], nullptr);
                    worstState = std::max(worstState, std::fabs(m - M[(size_t)i]) / m);
                }
            }
            K.maxStateErrPct = (float)(100.0 * worstState);
            K.massBalanceErrPct = (float)(100.0 * ((sysMass1 - sysMass0) - inletMass) / sysMass0);
            constexpr double kToGrams = 1.0e-6;                                 // (kg/m^3) * mm^3 -> g
            K.partsMassG = (float)(cavMass1 * kToGrams);
            K.packedMassG = (float)((cavMass1 - cavMass0) * kToGrams);
            double ejectAll = 0.0;
            bool allEject = true;
            for (size_t k = 0; k < parts.size(); ++k)
            {
                if (partBase[k] < 0) continue;
                PartFillResult& pr = R.parts[k];
                const size_t nn = parts[k].nodes.size();
                pr.shrinkPct.assign(nn, 0.0f);
                pr.ejectTimeS.assign(nn, -1.0f);
                double sv = 0.0, sw = 0.0, smin = 1e30, smax = -1e30, pe = 0.0, mass = 0.0;
                bool partEject = true;
                for (size_t i = 0; i < nn; ++i)
                {
                    const size_t d = (size_t)(partBase[k] + (int)i);
                    const double s = 100.0 * (1.0 - M[d] / (rhoRoom * V[d]));
                    pr.shrinkPct[i] = (float)s;
                    pr.ejectTimeS[i] = (float)ejectT[d];
                    sv += V[d] * s; sw += V[d];
                    smin = std::min(smin, s); smax = std::max(smax, s);
                    mass += M[d];
                    if (ejectT[d] < 0.0) partEject = false; else pe = std::max(pe, ejectT[d]);
                }
                pr.meanShrinkPct = (float)(sv / sw);
                pr.minShrinkPct = (float)smin; pr.maxShrinkPct = (float)smax;
                pr.ejectS = partEject ? (float)pe : -1.0f;
                pr.massG = (float)(mass * kToGrams);
                if (!partEject) allEject = false; else ejectAll = std::max(ejectAll, pe);
            }
            K.ejectS = allEject ? (float)ejectAll : -1.0f;
            if (K.message.empty())
            {
                char buf[256];
                std::snprintf(buf, sizeof buf, "Packed at %.1f MPa for %.2f s; %s.", K.packPressureMPa, K.holdTimeS,
                              K.ejectReached ? "every part reached the ejection temperature" : "stopped before every part could be ejected");
                K.message = buf;
            }
        }

        R.ok = true;
        if (R.message.empty())
            R.message = R.complete ? (R.pressureLimited ? "Fill complete (pressure-limited: slower than the target)."
                                                        : "Fill complete.")
                                   : "Fill incomplete.";
        return R;
    }

} // namespace Flow
