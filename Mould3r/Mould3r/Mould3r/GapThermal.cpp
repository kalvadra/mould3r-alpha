// ===========================================================================
// GapThermal.cpp — see GapThermal.h.
// ===========================================================================

#include "GapThermal.h"

#include <algorithm>
#include <cmath>

namespace Flow
{
    void LayerGrid::build(int layers, double grading)
    {
        n = std::max(2, layers);
        const double a = std::clamp(grading, 0.0, 0.9);
        xe.resize((size_t)n + 1);
        xc.resize((size_t)n);
        for (int i = 0; i <= n; ++i)
        {
            const double s = (double)i / (double)n;
            xe[(size_t)i] = s + a * s * (1.0 - s);        // slope 1+a at the centre, 1-a at the wall
        }
        xe[0] = 0.0; xe[(size_t)n] = 1.0;
        for (int g = 0; g < 2; ++g) { vf[g].resize((size_t)n); mom[g].resize((size_t)n); }
        for (int k = 0; k < n; ++k)
        {
            const double lo = xe[(size_t)k], hi = xe[(size_t)k + 1];
            xc[(size_t)k] = 0.5 * (lo + hi);
            vf[0][(size_t)k]  = hi - lo;
            vf[1][(size_t)k]  = hi * hi - lo * lo;
            mom[0][(size_t)k] = (hi * hi * hi - lo * lo * lo) / 3.0;
            mom[1][(size_t)k] = (hi * hi * hi * hi - lo * lo * lo * lo) / 4.0;
        }
    }

    // ---- Viscosity -------------------------------------------------------------

    void MeltViscosity::init(const TestMaterial::CrossWLF& cw, double noFlowK, double tMinK, double tMaxK)
    {
        m_cw = cw;
        m_noFlowK = noFlowK;
        m_t0 = std::min(tMinK, noFlowK) - 1.0;
        const double t1 = std::max(tMaxK, m_t0 + 10.0) + 1.0;
        m_dT = 0.1;
        const int nT = (int)std::ceil((t1 - m_t0) / m_dT) + 2;
        m_lnEta0.resize((size_t)nT);
        for (int i = 0; i < nT; ++i)
            m_lnEta0[(size_t)i] = std::log(std::max(1.0e-6, cw.eta0(m_t0 + m_dT * i)));
        m_oneMinusN = 1.0 - cw.n;
        m_lnTau = std::log(std::max(1.0e-9, cw.tauStar));
        // Frozen layers keep a millionth of the no-flow fluidity: effectively no
        // flow, but the pressure system stays positive definite.
        m_phiFloor = 1.0e-6 * std::exp(-lnEta0(noFlowK));
    }

    double MeltViscosity::lnEta0(double TK) const
    {
        const double u = (TK - m_t0) / m_dT;
        const int last = (int)m_lnEta0.size() - 1;
        if (u <= 0.0) return m_lnEta0[0];
        if (u >= (double)last)          // hotter than tabulated: extend the last slope
            return m_lnEta0[(size_t)last] + (u - last) * (m_lnEta0[(size_t)last] - m_lnEta0[(size_t)last - 1]);
        const int i = (int)u;
        const double f = u - i;
        return m_lnEta0[(size_t)i] + f * (m_lnEta0[(size_t)i + 1] - m_lnEta0[(size_t)i]);
    }

    double MeltViscosity::phi(double gammaDot, double TK) const
    {
        if (TK < m_noFlowK) return m_phiFloor;
        const double le0 = lnEta0(TK);
        double r = 1.0;                                   // eta0 / eta
        if (gammaDot > 0.0)
        {
            const double x = m_oneMinusN * (le0 + std::log(gammaDot) - m_lnTau);
            r += std::exp(std::min(x, 600.0));
        }
        return r * std::exp(-le0);
    }

    // ---- Profiles ----------------------------------------------------------------

    double FluidityIntegral(const LayerGrid& g, GapGeom geom, double sizeMm, const double* phi)
    {
        const int gi = (geom == GapGeom::Slab) ? 0 : 1;
        double s = 0.0;
        for (int k = 0; k < g.n; ++k) s += phi[k] * g.mom[gi][(size_t)k];
        if (geom == GapGeom::Slab) return 2.0 * sizeMm * sizeMm * sizeMm * s;
        const double R2 = sizeMm * sizeMm;
        return 0.5 * 3.14159265358979323846 * R2 * R2 * s;
    }

    void LayerFlowFractions(const LayerGrid& g, GapGeom geom, const double* phi, double* frac)
    {
        // u(x) ~ int_x^1 x' phi dx' (both geometries); integrate each layer's u
        // over its area (dz for a slab, r dr for a pipe).
        double U1 = 0.0, sum = 0.0;                       // u at the upper edge of the layer
        for (int k = g.n - 1; k >= 0; --k)
        {
            const double lo = g.xe[(size_t)k], hi = g.xe[(size_t)k + 1];
            const double U0 = U1 + phi[k] * 0.5 * (hi * hi - lo * lo);
            const double w = (geom == GapGeom::Slab) ? (hi - lo) : 0.5 * (hi * hi - lo * lo);
            frac[k] = 0.5 * (U0 + U1) * w;
            sum += frac[k];
            U1 = U0;
        }
        const int gi = (geom == GapGeom::Slab) ? 0 : 1;
        if (sum > 0.0) { for (int k = 0; k < g.n; ++k) frac[k] /= sum; }
        else
        {
            double vs = 0.0;
            for (int k = 0; k < g.n; ++k) vs += g.vf[gi][(size_t)k];
            for (int k = 0; k < g.n; ++k) frac[k] = g.vf[gi][(size_t)k] / vs;
        }
    }

    // ---- Conduction ----------------------------------------------------------------

    void ConductStep(const LayerGrid& g, GapGeom geom, double sizeM, double k, double rhoCp,
                     double dt, double Tw, const double* heat, double* T,
                     const double* inRate, const double* inT, const double* outRate)
    {
        const int n = g.n;
        const bool cyl = (geom == GapGeom::Cylinder);
        // Per unit area (slab) or per unit length and radian (cylinder).
        double A[64], B[64], C[64], D[64];
        std::vector<double> big;
        double *a = A, *b = B, *c = C, *d = D;
        if (n > 64) { big.resize((size_t)n * 4); a = big.data(); b = a + n; c = b + n; d = c + n; }

        // Whole-volume rates [1/s] -> this per-unit column: x rho c * (column volume).
        const double colVol = cyl ? 0.5 * sizeM * sizeM : sizeM;
        const bool adv = (inRate && outRate);
        const double advK = rhoCp * colVol;

        auto faceArea = [&](double x) { return cyl ? x * sizeM : 1.0; };
        double F = 0.0;                                  // cross-layer flux entering layer i from below (centre side)
        for (int i = 0; i < n; ++i)
        {
            const double lo = g.xe[(size_t)i], hi = g.xe[(size_t)i + 1];
            const double vol = cyl ? 0.5 * (hi * hi - lo * lo) * sizeM * sizeM : (hi - lo) * sizeM;
            const double cap = rhoCp * vol / dt;
            double lower = 0.0, upper = 0.0, wall = 0.0;
            if (i > 0)     lower = k * faceArea(lo) / ((g.xc[(size_t)i] - g.xc[(size_t)i - 1]) * sizeM);
            if (i < n - 1) upper = k * faceArea(hi) / ((g.xc[(size_t)i + 1] - g.xc[(size_t)i]) * sizeM);
            else           wall  = k * faceArea(1.0) / ((1.0 - g.xc[(size_t)i]) * sizeM);
            a[i] = -lower;
            c[i] = -upper;
            b[i] = cap + lower + upper + wall;
            d[i] = cap * T[i] + wall * Tw + (heat ? heat[i] * vol : 0.0);
            if (adv)
            {
                const double fin = advK * inRate[i], fout = advK * outRate[i];
                b[i] += fout;
                d[i] += fin * (inRate[i] > 0.0 ? inT[i] : 0.0);
                // Flux from below (F > 0: layer i-1 feeds i at T[i-1]); flux out the
                // top face Fu = F + in - out (> 0: i feeds i+1 at T[i]).
                if (i > 0)
                {
                    if (F > 0.0) a[i] -= F;               // gains F * T[i-1]
                    else         b[i] += -F;              // loses |F| * T[i] downwards
                }
                const double Fu = F + fin - fout;
                if (i < n - 1)
                {
                    if (Fu > 0.0) b[i] += Fu;             // loses Fu * T[i] upwards
                    else          c[i] -= -Fu;            // gains |Fu| * T[i+1]
                }
                F = Fu;
            }
        }
        // Thomas algorithm.
        for (int i = 1; i < n; ++i)
        {
            const double m = a[i] / b[i - 1];
            b[i] -= m * c[i - 1];
            d[i] -= m * d[i - 1];
        }
        T[n - 1] = d[n - 1] / b[n - 1];
        for (int i = n - 2; i >= 0; --i) T[i] = (d[i] - c[i] * T[i + 1]) / b[i];
    }

    double FrozenFraction(const LayerGrid& g, const double* T, double Tw, double noFlowK)
    {
        if (Tw >= noFlowK) return 0.0;
        double xPrev = 1.0, TPrev = Tw;
        for (int k = g.n - 1; k >= 0; --k)
        {
            if (T[k] >= noFlowK)
            {
                const double f = (noFlowK - TPrev) / std::max(1.0e-12, T[k] - TPrev);
                return std::clamp(1.0 - (xPrev + f * (g.xc[(size_t)k] - xPrev)), 0.0, 1.0);
            }
            xPrev = g.xc[(size_t)k]; TPrev = T[k];
        }
        return 1.0;
    }

    double MeanTemperature(const LayerGrid& g, GapGeom geom, const double* T)
    {
        const int gi = (geom == GapGeom::Slab) ? 0 : 1;
        double s = 0.0;
        for (int k = 0; k < g.n; ++k) s += g.vf[gi][(size_t)k] * T[k];
        return s;                                        // fractions sum to 1
    }

    double ContactTemperature(double Tm, double eMelt, double Tmould, double eMould)
    {
        if (eMould <= 0.0 || eMelt <= 0.0) return Tmould;
        return (eMelt * Tm + eMould * Tmould) / (eMelt + eMould);
    }

    // ---- Convection -------------------------------------------------------------------

    void ThermalSweep(const LayerGrid& g, const std::vector<int>& order, double dt,
                      const std::vector<double>& vol, const std::vector<GapGeom>& geom,
                      const std::vector<double>& sizeM, double k, double rhoCp, double Tw,
                      const std::vector<int>& inStart, const std::vector<int>& inFrom,
                      const std::vector<double>& inQ, const std::vector<const double*>& inFrac,
                      const std::vector<double>& outRate,
                      const double* heat, const std::vector<char>* fixed, std::vector<double>& T)
    {
        const int n = g.n;
        std::vector<double> inR((size_t)n), inT((size_t)n), outR((size_t)n);
        for (int i : order)
        {
            if (fixed && (*fixed)[(size_t)i]) continue;
            const int m0 = inStart[(size_t)i], m1 = inStart[(size_t)i + 1];
            const double V = vol[(size_t)i];
            if (V <= 0.0) continue;
            bool any = false;
            double sIn = 0.0, sOut = 0.0;
            for (int kk = 0; kk < n; ++kk)
            {
                double w = 0.0, wT = 0.0;
                for (int m = m0; m < m1; ++m)
                {
                    const double q = inQ[(size_t)m] * inFrac[(size_t)m][kk];
                    w += q;
                    wT += q * T[(size_t)inFrom[(size_t)m] * (size_t)n + (size_t)kk];
                }
                const double o = outRate[(size_t)i * (size_t)n + (size_t)kk];
                inR[(size_t)kk] = w / V;
                inT[(size_t)kk] = (w > 0.0) ? wT / w : 0.0;
                outR[(size_t)kk] = o / V;
                sIn += w; sOut += o;
                any |= (w > 0.0 || o > 0.0);
            }
            // Keep the volume's melt balanced (in = out): discretisation leaves
            // small mismatches (obtuse-corner couplings, dead ends); an unbalanced
            // conservative update would drain or pump energy in proportion to
            // the absolute temperature. The outflow is scaled to the inflow
            // (a dead end passes its inflow straight through, layer by layer).
            if (any)
            {
                if (sOut > 0.0) { const double sc = sIn / sOut; for (int kk = 0; kk < n; ++kk) outR[(size_t)kk] *= sc; }
                else            { for (int kk = 0; kk < n; ++kk) outR[(size_t)kk] = inR[(size_t)kk]; }
            }
            ConductStep(g, geom[(size_t)i], sizeM[(size_t)i], k, rhoCp, dt, Tw,
                        heat ? heat + (size_t)i * (size_t)n : nullptr, &T[(size_t)i * (size_t)n],
                        any ? inR.data() : nullptr, any ? inT.data() : nullptr, any ? outR.data() : nullptr);
        }
    }

} // namespace Flow
