#pragma once
// ===========================================================================
// MeshGrid — a small, header-only uniform-grid ray accelerator over a triangle
// soup (flat xyz positions + a uint index buffer). Purpose-built for the flow
// mesh's opposite-wall pairing: cast a ray and return the nearest hit whose
// triangle passes a caller predicate (used to accept only the true opposing
// wall, rejecting perpendicular ribs / same-wall grazes).
//
// NOTE: DesignChecks.cpp carries an older inline `TriGrid` with a similar DDA;
// the two should be consolidated onto this one in a later cleanup. This copy is
// standalone so the flow work doesn't disturb the shipped design-check code.
// ===========================================================================

#include <glm/glm.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>

namespace Flow
{
    // Moller-Trumbore ray/triangle, hit with t > tmin (orientation-independent).
    inline bool RayTri(const glm::vec3& o, const glm::vec3& d,
                       const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                       float tmin, float& tout)
    {
        const float EPS = 1.0e-8f;
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 p = glm::cross(d, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < EPS) return false;
        const float inv = 1.0f / det;
        const glm::vec3 tv = o - a;
        const float u = glm::dot(tv, p) * inv;
        if (u < -1.0e-6f || u > 1.0f + 1.0e-6f) return false;
        const glm::vec3 q = glm::cross(tv, e1);
        const float v = glm::dot(d, q) * inv;
        if (v < -1.0e-6f || u + v > 1.0f + 1.0e-6f) return false;
        const float t = glm::dot(e2, q) * inv;
        if (t <= tmin) return false;
        tout = t; return true;
    }

    struct MeshGrid
    {
        const std::vector<float>*        V = nullptr;   // xyz per vertex
        const std::vector<unsigned int>* I = nullptr;   // 3 indices / triangle
        glm::vec3 bmin{ 0.0f }, bmax{ 0.0f };
        int nx = 1, ny = 1, nz = 1;
        float cx = 1.0f, cy = 1.0f, cz = 1.0f;
        std::vector<std::vector<int>> cells;

        glm::vec3 vert(unsigned int i) const
        { return glm::vec3((*V)[i*3], (*V)[i*3+1], (*V)[i*3+2]); }
        int idx(int x, int y, int z) const { return (z*ny + y)*nx + x; }
        bool empty() const { return !V || !I || I->size() < 3; }

        static float comp(const glm::vec3& v, int a)
        { return (a == 0) ? v.x : (a == 1) ? v.y : v.z; }

        void cell(const glm::vec3& p, int& x, int& y, int& z) const
        {
            x = std::min(nx-1, std::max(0, (int)((p.x - bmin.x) / cx)));
            y = std::min(ny-1, std::max(0, (int)((p.y - bmin.y) / cy)));
            z = std::min(nz-1, std::max(0, (int)((p.z - bmin.z) / cz)));
        }

        void build(const std::vector<float>& verts, const std::vector<unsigned int>& inds)
        {
            V = &verts; I = &inds;
            if (empty()) { nx = ny = nz = 1; cells.assign(1, {}); return; }
            bmin = glm::vec3(1e30f); bmax = glm::vec3(-1e30f);
            for (size_t i = 0; i + 2 < verts.size(); i += 3)
            {
                bmin.x = std::min(bmin.x, verts[i]);   bmax.x = std::max(bmax.x, verts[i]);
                bmin.y = std::min(bmin.y, verts[i+1]); bmax.y = std::max(bmax.y, verts[i+1]);
                bmin.z = std::min(bmin.z, verts[i+2]); bmax.z = std::max(bmax.z, verts[i+2]);
            }
            const glm::vec3 d0 = bmax - bmin;
            const float pad = std::sqrt(glm::dot(d0, d0)) * 1.0e-4f + 1.0e-4f;
            bmin -= glm::vec3(pad); bmax += glm::vec3(pad);
            const size_t ntri = inds.size() / 3;
            const int res = (int)std::cbrt((double)std::max<size_t>(1, ntri));
            nx = std::max(1, res); ny = nx; nz = nx;
            const glm::vec3 d = bmax - bmin;
            cx = d.x / nx; cy = d.y / ny; cz = d.z / nz;
            if (cx <= 0.0f) cx = 1.0f; if (cy <= 0.0f) cy = 1.0f; if (cz <= 0.0f) cz = 1.0f;
            cells.assign((size_t)nx*ny*nz, {});
            for (size_t t = 0; t < ntri; ++t)
            {
                const glm::vec3 a = vert(inds[t*3]), b = vert(inds[t*3+1]), c = vert(inds[t*3+2]);
                const glm::vec3 lo(std::min({a.x,b.x,c.x}), std::min({a.y,b.y,c.y}), std::min({a.z,b.z,c.z}));
                const glm::vec3 hi(std::max({a.x,b.x,c.x}), std::max({a.y,b.y,c.y}), std::max({a.z,b.z,c.z}));
                int x0,y0,z0,x1,y1,z1; cell(lo,x0,y0,z0); cell(hi,x1,y1,z1);
                for (int z=z0; z<=z1; ++z) for (int y=y0; y<=y1; ++y) for (int x=x0; x<=x1; ++x)
                    cells[idx(x,y,z)].push_back((int)t);
            }
        }

        // Nearest hit at t > tmin whose triangle satisfies `accept(tri)` (pass an
        // empty function to accept any). Amanatides-Woo DDA, front-to-back, with
        // the standard "stop once the best hit lies within the current cell span"
        // exit. Returns the triangle index (or -1) and sets tHit.
        int nearestHit(const glm::vec3& o, const glm::vec3& dir, float tmin,
                       const std::function<bool(int)>& accept, float& tHit) const
        {
            tHit = 1e30f;
            if (empty()) return -1;
            float t0 = tmin, t1 = 1e30f;
            for (int a = 0; a < 3; ++a)
            {
                const float od = comp(o,a), dd = comp(dir,a), lo = comp(bmin,a), hi = comp(bmax,a);
                if (std::fabs(dd) < 1.0e-20f) { if (od < lo || od > hi) return -1; }
                else { float ta = (lo-od)/dd, tb = (hi-od)/dd; if (ta>tb) std::swap(ta,tb);
                       t0 = std::max(t0,ta); t1 = std::min(t1,tb); if (t0>t1) return -1; }
            }
            const glm::vec3 start = o + dir * (t0 + 1.0e-6f);
            int x,y,z; cell(start,x,y,z);
            const int sx = dir.x > 0 ? 1 : -1, sy = dir.y > 0 ? 1 : -1, sz = dir.z > 0 ? 1 : -1;
            auto edgeT = [&](int cc, int s, int a, float od, float dd)->float {
                const float cs = (a==0?cx:a==1?cy:cz);
                const float edge = comp(bmin,a) + (cc + (s>0?1:0)) * cs;
                return std::fabs(dd) < 1.0e-20f ? 1e30f : (edge - od) / dd;
            };
            float tMaxX = edgeT(x,sx,0,o.x,dir.x), tMaxY = edgeT(y,sy,1,o.y,dir.y), tMaxZ = edgeT(z,sz,2,o.z,dir.z);
            const float tDx = std::fabs(dir.x) < 1.0e-20f ? 1e30f : std::fabs(cx/dir.x);
            const float tDy = std::fabs(dir.y) < 1.0e-20f ? 1e30f : std::fabs(cy/dir.y);
            const float tDz = std::fabs(dir.z) < 1.0e-20f ? 1e30f : std::fabs(cz/dir.z);
            const int maxg = (nx+ny+nz)*3 + 10;
            int   best = -1;
            float bestT = 1e30f;
            for (int guard = 0; guard < maxg; ++guard)
            {
                const float tExit = std::min(tMaxX, std::min(tMaxY, tMaxZ));
                for (int tri : cells[idx(x,y,z)])
                {
                    const glm::vec3 a = vert((*I)[tri*3]), b = vert((*I)[tri*3+1]), c = vert((*I)[tri*3+2]);
                    float tt;
                    if (RayTri(o,dir,a,b,c,tmin,tt) && tt < bestT)
                    {
                        if (!accept || accept(tri)) { bestT = tt; best = tri; }
                    }
                }
                if (best >= 0 && bestT <= tExit) { tHit = bestT; return best; }
                if (tMaxX < tMaxY && tMaxX < tMaxZ) { x += sx; if (x<0||x>=nx) break; tMaxX += tDx; }
                else if (tMaxY < tMaxZ)             { y += sy; if (y<0||y>=ny) break; tMaxY += tDy; }
                else                                { z += sz; if (z<0||z>=nz) break; tMaxZ += tDz; }
            }
            if (best >= 0) { tHit = bestT; return best; }
            return -1;
        }
    };

} // namespace Flow
