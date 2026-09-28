// ===========================================================================
// Warpage.cpp — see Warpage.h.
// ===========================================================================

#include "Warpage.h"

#if __has_include(<Eigen/Sparse>)
  #include <Eigen/Sparse>
  #include <Eigen/Dense>
#elif __has_include(<eigen3/Eigen/Sparse>)
  #include <eigen3/Eigen/Sparse>
  #include <eigen3/Eigen/Dense>
#else
  #error "Eigen not found. Ensure vcpkg installed eigen3 (see vcpkg.json)."
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace Flow
{
    namespace
    {
        using dvec3 = glm::dvec3;

        // Signed dihedral angle across edge (e0, e1) between triangle (e0, e1, a)
        // and triangle (e1, e0, b); zero when flat.
        double Dihedral(const dvec3& e0, const dvec3& e1, const dvec3& a, const dvec3& b)
        {
            const dvec3 e = e1 - e0;
            const dvec3 n1 = glm::cross(e, a - e0);
            const dvec3 n2 = glm::cross(b - e0, e);
            const double el = glm::length(e);
            if (el <= 0.0) return 0.0;
            return std::atan2(glm::dot(glm::cross(n1, n2), e / el), glm::dot(n1, n2));
        }

        // Best fit q ~ c R p + t (Umeyama; c = 1 for a rigid fit), weights w.
        void FitSimilarity(const std::vector<dvec3>& p, const std::vector<dvec3>& q, const std::vector<double>& w,
                           bool withScale, Eigen::Matrix3d& R, double& c, dvec3& t)
        {
            double W = 0.0; dvec3 pm(0.0), qm(0.0);
            for (size_t i = 0; i < p.size(); ++i) { W += w[i]; pm += w[i] * p[i]; qm += w[i] * q[i]; }
            pm /= W; qm /= W;
            Eigen::Matrix3d S = Eigen::Matrix3d::Zero();
            double varP = 0.0;
            for (size_t i = 0; i < p.size(); ++i)
            {
                const dvec3 a = p[i] - pm, b = q[i] - qm;
                const Eigen::Vector3d av(a.x, a.y, a.z), bv(b.x, b.y, b.z);
                S += w[i] * bv * av.transpose();
                varP += w[i] * glm::dot(a, a);
            }
            Eigen::JacobiSVD<Eigen::Matrix3d> svd(S, Eigen::ComputeFullU | Eigen::ComputeFullV);
            Eigen::Matrix3d D = Eigen::Matrix3d::Identity();
            if ((svd.matrixU() * svd.matrixV().transpose()).determinant() < 0.0) D(2, 2) = -1.0;
            R = svd.matrixU() * D * svd.matrixV().transpose();
            c = withScale ? (svd.singularValues().asDiagonal() * D).trace() / varP : 1.0;
            const Eigen::Vector3d pmv(pm.x, pm.y, pm.z), qmv(qm.x, qm.y, qm.z);
            const Eigen::Vector3d tv = qmv - c * R * pmv;
            t = dvec3(tv.x(), tv.y(), tv.z());
        }
    } // namespace

    ShellSystem AssembleShell(const MidplaneMesh& m, const std::vector<double>& shrink,
                              const std::vector<glm::dvec3>& triFlowDir, const WarpParams& P)
    {
        ShellSystem S;
        const size_t nn = m.nodes.size();
        S.n = (int)nn;
        S.load.assign(3 * nn, 0.0);
        const double E = P.elasticModulusMPa, nu = P.poissonRatio;
        const double Dm = E / (1.0 - nu * nu);
        auto add = [&](int r, int c, double v) { S.row.push_back(r); S.col.push_back(c); S.val.push_back(v); };
        const bool aniso = std::fabs(P.flowShrinkRatio - 1.0) > 1e-9 && triFlowDir.size() == m.tris.size();

        // ---- Membrane: constant-strain triangles in each facet's plane ----------
        std::vector<double> triArea(m.tris.size(), 0.0), triH(m.tris.size(), 0.0);
        for (size_t t = 0; t < m.tris.size(); ++t)
        {
            const int v[3] = { m.tris[t].x, m.tris[t].y, m.tris[t].z };
            const dvec3 p0(m.nodes[(size_t)v[0]]), p1(m.nodes[(size_t)v[1]]), p2(m.nodes[(size_t)v[2]]);
            const dvec3 cr = glm::cross(p1 - p0, p2 - p0);
            const double twoA = glm::length(cr);
            if (twoA <= 1e-12) continue;
            const double A = 0.5 * twoA;
            const double h = ((double)m.thicknessMm[(size_t)v[0]] + m.thicknessMm[(size_t)v[1]] + m.thicknessMm[(size_t)v[2]]) / 3.0;
            triArea[t] = A; triH[t] = h;
            const dvec3 e1 = glm::normalize(p1 - p0), nrm = cr / twoA, e2 = glm::cross(nrm, e1);
            const double x[3] = { 0.0, glm::dot(p1 - p0, e1), glm::dot(p2 - p0, e1) };
            const double y[3] = { 0.0, glm::dot(p1 - p0, e2), glm::dot(p2 - p0, e2) };
            // B (3x6) for local (u, v) per node.
            double B[3][6] = {};
            for (int i = 0; i < 3; ++i)
            {
                const int j = (i + 1) % 3, k = (i + 2) % 3;
                const double bi = y[j] - y[k], ci = x[k] - x[j];
                B[0][2 * i] = bi / twoA;
                B[1][2 * i + 1] = ci / twoA;
                B[2][2 * i] = ci / twoA;
                B[2][2 * i + 1] = bi / twoA;
            }
            const double Dmat[3][3] = { { Dm, Dm * nu, 0.0 }, { Dm * nu, Dm, 0.0 }, { 0.0, 0.0, Dm * 0.5 * (1.0 - nu) } };
            double DB[3][6] = {};
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 6; ++c) for (int q = 0; q < 3; ++q) DB[r][c] += Dmat[r][q] * B[q][c];
            double Kl[6][6] = {};
            for (int r = 0; r < 6; ++r) for (int c = 0; c < 6; ++c)
            {
                double s = 0.0;
                for (int q = 0; q < 3; ++q) s += B[q][r] * DB[q][c];
                Kl[r][c] = h * A * s;
            }
            // Free shrinkage strain (contraction): isotropic, or split along / across the flow.
            const double sIso = (shrink[(size_t)v[0]] + shrink[(size_t)v[1]] + shrink[(size_t)v[2]]) / 3.0;
            double eps0[3] = { -sIso, -sIso, 0.0 };
            if (aniso && glm::length(triFlowDir[t]) > 0.5)
            {
                const double r = P.flowShrinkRatio;
                const double sPerp = 2.0 * sIso / (1.0 + r), sPar = r * sPerp;
                double d1 = glm::dot(triFlowDir[t], e1), d2 = glm::dot(triFlowDir[t], e2);
                const double dl = std::sqrt(d1 * d1 + d2 * d2);
                if (dl > 1e-9) { d1 /= dl; d2 /= dl; }
                eps0[0] = -(sPerp + (sPar - sPerp) * d1 * d1);
                eps0[1] = -(sPerp + (sPar - sPerp) * d2 * d2);
                eps0[2] = -2.0 * (sPar - sPerp) * d1 * d2;
            }
            double fl[6] = {};
            for (int r = 0; r < 6; ++r)
            {
                double s = 0.0;
                for (int q = 0; q < 3; ++q) { double sq = 0.0; for (int z = 0; z < 3; ++z) sq += Dmat[q][z] * eps0[z]; s += B[q][r] * sq; }
                fl[r] = h * A * s;
            }
            // Local (u, v) -> global xyz: rows e1, e2 per node.
            const dvec3 ax[2] = { e1, e2 };
            for (int a = 0; a < 3; ++a)
                for (int la = 0; la < 2; ++la)
                {
                    const int ra = 2 * a + la;
                    for (int ca = 0; ca < 3; ++ca) S.load[3 * (size_t)v[a] + (size_t)ca] += ax[la][ca] * fl[ra];
                    for (int b = 0; b < 3; ++b)
                        for (int lb = 0; lb < 2; ++lb)
                        {
                            const double k = Kl[ra][2 * b + lb];
                            if (k == 0.0) continue;
                            for (int ca = 0; ca < 3; ++ca)
                                for (int cb = 0; cb < 3; ++cb)
                                {
                                    const double v2 = ax[la][ca] * k * ax[lb][cb];
                                    if (v2 != 0.0) add(3 * v[a] + ca, 3 * v[b] + cb, v2);
                                }
                        }
                }
        }

        // ---- Bending: hinges across interior edges ----------------------------------
        std::unordered_map<uint64_t, std::pair<int, int>> edgeTris;
        edgeTris.reserve(m.tris.size() * 2);
        for (size_t t = 0; t < m.tris.size(); ++t)
        {
            const int v[3] = { m.tris[t].x, m.tris[t].y, m.tris[t].z };
            for (int j = 0; j < 3; ++j)
            {
                const uint32_t a = (uint32_t)std::min(v[j], v[(j + 1) % 3]), b = (uint32_t)std::max(v[j], v[(j + 1) % 3]);
                const uint64_t key = ((uint64_t)a << 32) | b;
                auto it = edgeTris.find(key);
                if (it == edgeTris.end()) edgeTris.emplace(key, std::make_pair((int)t, -1));
                else it->second.second = (int)t;
            }
        }
        for (const auto& kv : edgeTris)
        {
            const int t1 = kv.second.first, t2 = kv.second.second;
            if (t2 < 0 || triArea[(size_t)t1] <= 0.0 || triArea[(size_t)t2] <= 0.0) continue;
            const int e0 = (int)(kv.first >> 32), e1i = (int)(kv.first & 0xFFFFFFFFu);
            auto opposite = [&](int t) { const glm::ivec3& T = m.tris[(size_t)t]; return (T.x != e0 && T.x != e1i) ? T.x : ((T.y != e0 && T.y != e1i) ? T.y : T.z); };
            const int a = opposite(t1), b = opposite(t2);
            const int ids[4] = { e0, e1i, a, b };
            dvec3 X[4];
            for (int i = 0; i < 4; ++i) X[i] = dvec3(m.nodes[(size_t)ids[i]]);
            const double el = glm::length(X[1] - X[0]);
            const double h = 0.5 * (triH[(size_t)t1] + triH[(size_t)t2]);
            const double Db = P.elasticModulusMPa * h * h * h / (12.0 * (1.0 - nu * nu));
            const double k = P.bendingCoefficient * Db * el * el / (triArea[(size_t)t1] + triArea[(size_t)t2]);
            // d(theta)/dX by central differences (the angle is smooth and the
            // gradient is needed once, at the as-moulded shape).
            double g[12];
            const double dh = 1e-5 * el;
            for (int i = 0; i < 4; ++i)
                for (int c = 0; c < 3; ++c)
                {
                    dvec3 Y[4] = { X[0], X[1], X[2], X[3] };
                    Y[i][c] += dh; const double tp = Dihedral(Y[0], Y[1], Y[2], Y[3]);
                    Y[i][c] -= 2.0 * dh; const double tm = Dihedral(Y[0], Y[1], Y[2], Y[3]);
                    g[3 * i + c] = (tp - tm) / (2.0 * dh);
                }
            for (int i = 0; i < 12; ++i)
                for (int j = 0; j < 12; ++j)
                {
                    const double v2 = k * g[i] * g[j];
                    if (v2 != 0.0) add(3 * ids[i / 3] + i % 3, 3 * ids[j / 3] + j % 3, v2);
                }
        }
        return S;
    }

    WarpResult SolveWarpage(const std::vector<MidplaneMesh>& parts, const CoupledFillResult& fill, const WarpParams& P)
    {
        WarpResult R;
        R.parts.resize(parts.size());
        if (!fill.pack.ran) { R.message = "Warpage needs a packed fill (shrinkage)."; return R; }
        for (size_t k = 0; k < parts.size() && k < fill.parts.size(); ++k)
        {
            PartWarp& W = R.parts[k];
            W.part = (int)k;
            const MidplaneMesh& m = parts[k];
            const PartFillResult& pr = fill.parts[k];
            const size_t nn = m.nodes.size();
            if (m.empty() || !pr.fed || pr.shrinkPct.size() != nn) { W.message = "No shrinkage for this part."; continue; }

            std::vector<double> shrink(nn);
            for (size_t i = 0; i < nn; ++i) shrink[i] = pr.shrinkPct[i] / 300.0;   // volumetric % -> linear strain
            std::vector<dvec3> flowDir;
            if (std::fabs(P.flowShrinkRatio - 1.0) > 1e-9 && pr.fillTimeS.size() == nn)
            {
                flowDir.assign(m.tris.size(), dvec3(0.0));
                for (size_t t = 0; t < m.tris.size(); ++t)
                {
                    const glm::ivec3& T = m.tris[t];
                    const float f0 = pr.fillTimeS[(size_t)T.x], f1 = pr.fillTimeS[(size_t)T.y], f2 = pr.fillTimeS[(size_t)T.z];
                    if (f0 < 0 || f1 < 0 || f2 < 0) continue;
                    const dvec3 p0(m.nodes[(size_t)T.x]), p1(m.nodes[(size_t)T.y]), p2(m.nodes[(size_t)T.z]);
                    const dvec3 cr = glm::cross(p1 - p0, p2 - p0);
                    const double twoA = glm::length(cr);
                    if (twoA <= 1e-12) continue;
                    const dvec3 nh = cr / twoA;
                    const dvec3 g = (glm::cross(nh, p2 - p1) * (double)f0 + glm::cross(nh, p0 - p2) * (double)f1 +
                                     glm::cross(nh, p1 - p0) * (double)f2) / twoA;
                    const double gl = glm::length(g);
                    if (gl > 1e-12) flowDir[t] = g / gl;
                }
            }

            const ShellSystem S = AssembleShell(m, shrink, flowDir, P);
            const int N = 3 * S.n;
            std::vector<Eigen::Triplet<double>> trip;
            trip.reserve(S.val.size() + (size_t)N);
            double diagSum = 0.0; int diagCnt = 0;
            for (size_t i = 0; i < S.val.size(); ++i)
            {
                trip.emplace_back(S.row[i], S.col[i], S.val[i]);
                if (S.row[i] == S.col[i]) { diagSum += S.val[i]; ++diagCnt; }
            }
            // Free body: a tiny regularisation holds the rigid motions (removed below).
            const double reg = 1e-9 * (diagCnt ? diagSum / diagCnt : 1.0);
            for (int i = 0; i < N; ++i) trip.emplace_back(i, i, reg);
            Eigen::SparseMatrix<double> K(N, N);
            K.setFromTriplets(trip.begin(), trip.end());
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(K);
            if (ldlt.info() != Eigen::Success) { W.message = "Shell solve failed (matrix not factorable)."; continue; }
            Eigen::VectorXd f(N);
            for (int i = 0; i < N; ++i) f[i] = S.load[(size_t)i];
            const Eigen::VectorXd u = ldlt.solve(f);
            if (ldlt.info() != Eigen::Success) { W.message = "Shell solve failed."; continue; }

            // Node weights (lumped area) for the fits.
            std::vector<double> w(nn, 0.0);
            for (const glm::ivec3& T : m.tris)
            {
                const double A = 0.5 * glm::length(glm::cross(m.nodes[(size_t)T.y] - m.nodes[(size_t)T.x], m.nodes[(size_t)T.z] - m.nodes[(size_t)T.x]));
                w[(size_t)T.x] += A / 3.0; w[(size_t)T.y] += A / 3.0; w[(size_t)T.z] += A / 3.0;
            }
            for (double& x : w) x = std::max(x, 1e-12);
            std::vector<dvec3> p(nn), q(nn);
            for (size_t i = 0; i < nn; ++i)
            {
                p[i] = dvec3(m.nodes[i]);
                q[i] = p[i] + dvec3(u[3 * (Eigen::Index)i], u[3 * (Eigen::Index)i + 1], u[3 * (Eigen::Index)i + 2]);
            }
            // Deflection: rigid motion removed (bring the shrunk part back onto the as-moulded one).
            Eigen::Matrix3d Rr; double c1; dvec3 t1;
            FitSimilarity(p, q, w, false, Rr, c1, t1);
            // Warp: uniform shrink removed too.
            Eigen::Matrix3d Rs; double cs; dvec3 ts;
            FitSimilarity(p, q, w, true, Rs, cs, ts);
            W.uniformShrinkPct = (float)(100.0 * (1.0 - cs));
            W.disp.resize(nn); W.dispMm.resize(nn); W.warp.resize(nn); W.warpMm.resize(nn); W.warpPullMm.resize(nn);
            dvec3 lo0(1e30), hi0(-1e30), lo1(1e30), hi1(-1e30);
            double ymin = 1e30, ymax = -1e30;
            for (size_t i = 0; i < nn; ++i)
            {
                const Eigen::Vector3d qi(q[i].x, q[i].y, q[i].z);
                const Eigen::Vector3d back = Rr.transpose() * (qi - Eigen::Vector3d(t1.x, t1.y, t1.z));
                const dvec3 d = dvec3(back.x(), back.y(), back.z()) - p[i];
                const Eigen::Vector3d backS = Rs.transpose() * (qi - Eigen::Vector3d(ts.x, ts.y, ts.z)) / cs;
                const dvec3 wv = dvec3(backS.x(), backS.y(), backS.z()) - p[i];
                W.disp[i] = glm::vec3(d); W.dispMm[i] = (float)glm::length(d);
                W.warp[i] = glm::vec3(wv); W.warpMm[i] = (float)glm::length(wv); W.warpPullMm[i] = (float)wv.y;
                W.maxDispMm = std::max(W.maxDispMm, W.dispMm[i]);
                if (W.warpMm[i] > W.maxWarpMm) { W.maxWarpMm = W.warpMm[i]; W.maxWarpPos = m.nodes[i]; }
                ymin = std::min(ymin, wv.y); ymax = std::max(ymax, wv.y);
                lo0 = glm::min(lo0, p[i]); hi0 = glm::max(hi0, p[i]);
                lo1 = glm::min(lo1, p[i] + d); hi1 = glm::max(hi1, p[i] + d);
            }
            W.flatnessMm = (float)(ymax - ymin);
            W.sizeBefore = glm::vec3(hi0 - lo0);
            W.sizeAfter = glm::vec3(hi1 - lo1);
            W.ok = true;
        }
        R.ok = true;
        return R;
    }

} // namespace Flow
