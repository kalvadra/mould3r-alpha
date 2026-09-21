#include "DesignChecks.h"

#include <opencascade/TopoDS_Shape.hxx>
#include <opencascade/TopoDS_Face.hxx>
#include <opencascade/TopoDS.hxx>
#include <opencascade/TopExp.hxx>
#include <opencascade/TopExp_Explorer.hxx>
#include <opencascade/TopAbs_Orientation.hxx>
#include <opencascade/TopTools_IndexedMapOfShape.hxx>
#include <opencascade/TopLoc_Location.hxx>
#include <opencascade/BRep_Tool.hxx>
#include <opencascade/BRepMesh_IncrementalMesh.hxx>
#include <opencascade/Poly_Triangulation.hxx>
#include <opencascade/Geom_Surface.hxx>
#include <opencascade/GeomLProp_SLProps.hxx>
#include <opencascade/IntCurvesFace_ShapeIntersector.hxx>
#include <opencascade/BRepClass3d_SolidClassifier.hxx>
#include <opencascade/TopAbs_State.hxx>
#include <opencascade/Bnd_Box.hxx>
#include <opencascade/BRepBndLib.hxx>
#include <opencascade/Poly_PolygonOnTriangulation.hxx>
#include <opencascade/Poly_Triangulation.hxx>
#include <opencascade/TColStd_Array1OfInteger.hxx>
#include <opencascade/BRepAlgoAPI_Common.hxx>
#include <opencascade/BRepBuilderAPI_Transform.hxx>
#include <opencascade/BRepGProp.hxx>
#include <opencascade/GProp_GProps.hxx>
#include <opencascade/BRep_Builder.hxx>
#include <opencascade/TopoDS_Compound.hxx>
#include <opencascade/Standard_Failure.hxx>
#include <opencascade/gp_Pnt.hxx>
#include <opencascade/gp_Pnt2d.hxx>
#include <opencascade/gp_Dir.hxx>
#include <opencascade/gp_Lin.hxx>
#include <opencascade/gp_Trsf.hxx>
#include <opencascade/gp_Vec.hxx>
#include <opencascade/Precision.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <map>
#include <set>
#include <array>
#include <vector>
#include <cstdlib>
#include <functional>

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    inline float Deg(float rad) { return rad * (180.0f / kPi); }

    // ---- Mesh accelerator (uniform grid) for the mesh draft path ---------
    // Ray any-hit (trapped-area) + nearest-triangle-within-radius (per-cavity
    // objectId). The algorithm was validated against brute force before use.
    inline float Comp(const glm::vec3& v, int a)
    { return (a == 0) ? v.x : (a == 1) ? v.y : v.z; }

    // Moller-Trumbore; reports a hit with t > tmin.
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

    // Squared distance from a point to a triangle.
    inline float DistPointTri2(const glm::vec3& p, const glm::vec3& a,
                               const glm::vec3& b, const glm::vec3& c)
    {
        const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
        const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) { const glm::vec3 r = p - a; return glm::dot(r, r); }
        const glm::vec3 bp = p - b;
        const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) { const glm::vec3 r = p - b; return glm::dot(r, r); }
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        { const float t = d1 / (d1 - d3); const glm::vec3 r = p - (a + ab * t); return glm::dot(r, r); }
        const glm::vec3 cp = p - c;
        const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) { const glm::vec3 r = p - c; return glm::dot(r, r); }
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        { const float t = d2 / (d2 - d6); const glm::vec3 r = p - (a + ac * t); return glm::dot(r, r); }
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        { const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); const glm::vec3 r = p - (b + (c - b) * t); return glm::dot(r, r); }
        const float den = 1.0f / (va + vb + vc);
        const float vv = vb * den, ww = vc * den;
        const glm::vec3 r = p - (a + ab * vv + ac * ww);
        return glm::dot(r, r);
    }

    // Closest point on a triangle to p (Ericson's regions).
    inline glm::vec3 ClosestPtOnTri(const glm::vec3& p, const glm::vec3& a,
                                    const glm::vec3& b, const glm::vec3& c)
    {
        const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
        const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) return a;
        const glm::vec3 bp = p - b;
        const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) return b;
        const float vc = d1*d4 - d3*d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) { const float t = d1/(d1-d3); return a + ab*t; }
        const glm::vec3 cp = p - c;
        const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) return c;
        const float vb = d5*d2 - d1*d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) { const float t = d2/(d2-d6); return a + ac*t; }
        const float va = d3*d6 - d5*d4;
        if (va <= 0.0f && (d4-d3) >= 0.0f && (d5-d6) >= 0.0f)
        { const float t = (d4-d3)/((d4-d3)+(d5-d6)); return b + (c-b)*t; }
        const float den = 1.0f/(va+vb+vc), v = vb*den, w = vc*den;
        return a + ab*v + ac*w;
    }

    // Uniform grid over a triangle soup (positions xyz + index buffer). Stores
    // pointers to the caller's arrays, which must outlive the grid.
    struct TriGrid
    {
        const std::vector<float>* V = nullptr;
        const std::vector<unsigned int>* I = nullptr;
        glm::vec3 bmin{0.0f}, bmax{0.0f};
        int nx = 1, ny = 1, nz = 1;
        float cx = 1.0f, cy = 1.0f, cz = 1.0f;
        std::vector<std::vector<int>> cells;

        glm::vec3 Vert(unsigned int i) const
        { return glm::vec3((*V)[i*3], (*V)[i*3+1], (*V)[i*3+2]); }
        int Idx(int x, int y, int z) const { return (z*ny + y)*nx + x; }
        void Cell(const glm::vec3& p, int& x, int& y, int& z) const
        {
            x = std::min(nx-1, std::max(0, (int)((p.x - bmin.x) / cx)));
            y = std::min(ny-1, std::max(0, (int)((p.y - bmin.y) / cy)));
            z = std::min(nz-1, std::max(0, (int)((p.z - bmin.z) / cz)));
        }
        bool Empty() const { return !V || !I || I->size() < 3; }

        void Build(const std::vector<float>& verts, const std::vector<unsigned int>& inds)
        {
            V = &verts; I = &inds;
            if (Empty()) { nx = ny = nz = 1; cells.assign(1, {}); return; }
            bmin = glm::vec3(1e30f); bmax = glm::vec3(-1e30f);
            for (size_t i = 0; i + 2 < verts.size(); i += 3)
            {
                bmin.x = std::min(bmin.x, verts[i]);   bmax.x = std::max(bmax.x, verts[i]);
                bmin.y = std::min(bmin.y, verts[i+1]); bmax.y = std::max(bmax.y, verts[i+1]);
                bmin.z = std::min(bmin.z, verts[i+2]); bmax.z = std::max(bmax.z, verts[i+2]);
            }
            const glm::vec3 d0 = bmax - bmin;
            const float diag = std::sqrt(glm::dot(d0, d0));
            const float pad = diag * 1.0e-4f + 1.0e-4f;
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
                const glm::vec3 a = Vert(inds[t*3]), b = Vert(inds[t*3+1]), c = Vert(inds[t*3+2]);
                const glm::vec3 lo(std::min({a.x,b.x,c.x}), std::min({a.y,b.y,c.y}), std::min({a.z,b.z,c.z}));
                const glm::vec3 hi(std::max({a.x,b.x,c.x}), std::max({a.y,b.y,c.y}), std::max({a.z,b.z,c.z}));
                int x0,y0,z0,x1,y1,z1; Cell(lo,x0,y0,z0); Cell(hi,x1,y1,z1);
                for (int z=z0; z<=z1; ++z) for (int y=y0; y<=y1; ++y) for (int x=x0; x<=x1; ++x)
                    cells[Idx(x,y,z)].push_back((int)t);
            }
        }

        // Amanatides-Woo DDA any-hit.
        bool RayAnyHit(const glm::vec3& o, const glm::vec3& dir, float tmin) const
        {
            if (Empty()) return false;
            float t0 = tmin, t1 = 1e30f;
            for (int a = 0; a < 3; ++a)
            {
                const float od = Comp(o,a), dd = Comp(dir,a), lo = Comp(bmin,a), hi = Comp(bmax,a);
                if (std::fabs(dd) < 1.0e-20f) { if (od < lo || od > hi) return false; }
                else { float ta = (lo-od)/dd, tb = (hi-od)/dd; if (ta>tb) std::swap(ta,tb);
                       t0 = std::max(t0,ta); t1 = std::min(t1,tb); if (t0>t1) return false; }
            }
            const glm::vec3 start = o + dir * (t0 + 1.0e-6f);
            int x,y,z; Cell(start,x,y,z);
            const int sx = dir.x > 0 ? 1 : -1, sy = dir.y > 0 ? 1 : -1, sz = dir.z > 0 ? 1 : -1;
            auto edgeT = [&](int cc, int s, int a, float od, float dd)->float {
                const float cell = (a==0?cx:a==1?cy:cz);
                const float edge = Comp(bmin,a) + (cc + (s>0?1:0)) * cell;
                return std::fabs(dd) < 1.0e-20f ? 1e30f : (edge - od) / dd;
            };
            float tMaxX = edgeT(x,sx,0,o.x,dir.x), tMaxY = edgeT(y,sy,1,o.y,dir.y), tMaxZ = edgeT(z,sz,2,o.z,dir.z);
            const float tDx = std::fabs(dir.x) < 1.0e-20f ? 1e30f : std::fabs(cx/dir.x);
            const float tDy = std::fabs(dir.y) < 1.0e-20f ? 1e30f : std::fabs(cy/dir.y);
            const float tDz = std::fabs(dir.z) < 1.0e-20f ? 1e30f : std::fabs(cz/dir.z);
            const int maxg = (nx+ny+nz)*3 + 10;
            for (int guard = 0; guard < maxg; ++guard)
            {
                for (int tri : cells[Idx(x,y,z)])
                {
                    const glm::vec3 a = Vert((*I)[tri*3]), b = Vert((*I)[tri*3+1]), c = Vert((*I)[tri*3+2]);
                    float tt; if (RayTri(o,dir,a,b,c,tmin,tt)) return true;
                }
                if (tMaxX < tMaxY && tMaxX < tMaxZ) { x += sx; if (x<0||x>=nx) break; tMaxX += tDx; }
                else if (tMaxY < tMaxZ)             { y += sy; if (y<0||y>=ny) break; tMaxY += tDy; }
                else                                { z += sz; if (z<0||z>=nz) break; tMaxZ += tDz; }
            }
            return false;
        }

        // Amanatides-Woo DDA nearest-hit: the smallest t > tmin at which the ray
        // strikes any triangle. Marches cells front-to-back and returns as soon
        // as the best hit so far lies within the current cell (no closer hit can
        // follow). Used to pick the mould half a facet's outward normal enters.
        bool RayNearestHit(const glm::vec3& o, const glm::vec3& dir,
                           float tmin, float& tHit) const
        {
            tHit = 1e30f;
            if (Empty()) return false;
            float t0 = tmin, t1 = 1e30f;
            for (int a = 0; a < 3; ++a)
            {
                const float od = Comp(o,a), dd = Comp(dir,a), lo = Comp(bmin,a), hi = Comp(bmax,a);
                if (std::fabs(dd) < 1.0e-20f) { if (od < lo || od > hi) return false; }
                else { float ta = (lo-od)/dd, tb = (hi-od)/dd; if (ta>tb) std::swap(ta,tb);
                       t0 = std::max(t0,ta); t1 = std::min(t1,tb); if (t0>t1) return false; }
            }
            const glm::vec3 start = o + dir * (t0 + 1.0e-6f);
            int x,y,z; Cell(start,x,y,z);
            const int sx = dir.x > 0 ? 1 : -1, sy = dir.y > 0 ? 1 : -1, sz = dir.z > 0 ? 1 : -1;
            auto edgeT = [&](int cc, int s, int a, float od, float dd)->float {
                const float cell = (a==0?cx:a==1?cy:cz);
                const float edge = Comp(bmin,a) + (cc + (s>0?1:0)) * cell;
                return std::fabs(dd) < 1.0e-20f ? 1e30f : (edge - od) / dd;
            };
            float tMaxX = edgeT(x,sx,0,o.x,dir.x), tMaxY = edgeT(y,sy,1,o.y,dir.y), tMaxZ = edgeT(z,sz,2,o.z,dir.z);
            const float tDx = std::fabs(dir.x) < 1.0e-20f ? 1e30f : std::fabs(cx/dir.x);
            const float tDy = std::fabs(dir.y) < 1.0e-20f ? 1e30f : std::fabs(cy/dir.y);
            const float tDz = std::fabs(dir.z) < 1.0e-20f ? 1e30f : std::fabs(cz/dir.z);
            const int maxg = (nx+ny+nz)*3 + 10;
            float best = 1e30f;
            for (int guard = 0; guard < maxg; ++guard)
            {
                const float tExit = std::min(tMaxX, std::min(tMaxY, tMaxZ));
                for (int tri : cells[Idx(x,y,z)])
                {
                    const glm::vec3 a = Vert((*I)[tri*3]), b = Vert((*I)[tri*3+1]), c = Vert((*I)[tri*3+2]);
                    float tt; if (RayTri(o,dir,a,b,c,tmin,tt) && tt < best) best = tt;
                }
                if (best <= tExit) { tHit = best; return true; }
                if (tMaxX < tMaxY && tMaxX < tMaxZ) { x += sx; if (x<0||x>=nx) break; tMaxX += tDx; }
                else if (tMaxY < tMaxZ)             { y += sy; if (y<0||y>=ny) break; tMaxY += tDy; }
                else                                { z += sz; if (z<0||z>=nz) break; tMaxZ += tDz; }
            }
            if (best < 1e30f) { tHit = best; return true; }
            return false;
        }

        // Nearest triangle within radius r; returns tri index (or -1) + dist^2.
        int Nearest(const glm::vec3& p, float r, float& outD2) const
        {
            if (Empty()) { outD2 = r*r; return -1; }
            int x0,y0,z0; Cell(p,x0,y0,z0);
            const float minCell = std::min(cx, std::min(cy, cz));
            const int rc = std::max(1, (int)std::ceil(r / minCell));
            int best = -1; float bd2 = r*r;
            for (int z = std::max(0,z0-rc); z <= std::min(nz-1,z0+rc); ++z)
            for (int y = std::max(0,y0-rc); y <= std::min(ny-1,y0+rc); ++y)
            for (int x = std::max(0,x0-rc); x <= std::min(nx-1,x0+rc); ++x)
                for (int tri : cells[Idx(x,y,z)])
                {
                    const glm::vec3 a = Vert((*I)[tri*3]), b = Vert((*I)[tri*3+1]), c = Vert((*I)[tri*3+2]);
                    const float d2 = DistPointTri2(p, a, b, c);
                    if (d2 < bd2) { bd2 = d2; best = tri; }
                }
            outD2 = bd2; return best;
        }

        // Closest point on the whole mesh (expanding ring search; brute-force
        // fallback). Query points during remeshing sit near the surface.
        glm::vec3 ClosestPoint(const glm::vec3& p) const
        {
            if (Empty()) return p;
            int x0, y0, z0; Cell(p, x0, y0, z0);
            const int maxR = std::max(nx, std::max(ny, nz));
            float bd2 = 1e30f; glm::vec3 best = p; bool found = false;
            for (int rc = 1; rc <= maxR; rc = (rc < 2 ? 2 : rc*2))
            {
                for (int z = std::max(0,z0-rc); z <= std::min(nz-1,z0+rc); ++z)
                for (int y = std::max(0,y0-rc); y <= std::min(ny-1,y0+rc); ++y)
                for (int x = std::max(0,x0-rc); x <= std::min(nx-1,x0+rc); ++x)
                    for (int tri : cells[Idx(x,y,z)])
                    {
                        const glm::vec3 a = Vert((*I)[tri*3]), b = Vert((*I)[tri*3+1]), c = Vert((*I)[tri*3+2]);
                        const glm::vec3 q = ClosestPtOnTri(p, a, b, c);
                        const glm::vec3 d = p - q; const float d2 = glm::dot(d, d);
                        if (d2 < bd2) { bd2 = d2; best = q; found = true; }
                    }
                if (found && rc >= 2) break;
            }
            if (!found)
                for (size_t t = 0; t < I->size()/3; ++t)
                {
                    const glm::vec3 a = Vert((*I)[t*3]), b = Vert((*I)[t*3+1]), c = Vert((*I)[t*3+2]);
                    const glm::vec3 q = ClosestPtOnTri(p, a, b, c);
                    const glm::vec3 d = p - q; const float d2 = glm::dot(d, d);
                    if (d2 < bd2) { bd2 = d2; best = q; }
                }
            return best;
        }

        // Nearest triangle index to p (expanding ring search + brute fallback).
        int NearestTriangle(const glm::vec3& p) const
        {
            if (Empty()) return -1;
            int x0, y0, z0; Cell(p, x0, y0, z0);
            const int maxR = std::max(nx, std::max(ny, nz));
            float bd2 = 1e30f; int best = -1; bool found = false;
            for (int rc = 1; rc <= maxR; rc = (rc < 2 ? 2 : rc*2))
            {
                for (int z = std::max(0,z0-rc); z <= std::min(nz-1,z0+rc); ++z)
                for (int y = std::max(0,y0-rc); y <= std::min(ny-1,y0+rc); ++y)
                for (int x = std::max(0,x0-rc); x <= std::min(nx-1,x0+rc); ++x)
                    for (int tri : cells[Idx(x,y,z)])
                    {
                        const glm::vec3 a = Vert((*I)[tri*3]), b = Vert((*I)[tri*3+1]), c = Vert((*I)[tri*3+2]);
                        const float d2 = DistPointTri2(p, a, b, c);
                        if (d2 < bd2) { bd2 = d2; best = tri; found = true; }
                    }
                if (found && rc >= 2) break;
            }
            if (!found)
                for (size_t t = 0; t < I->size()/3; ++t)
                {
                    const glm::vec3 a = Vert((*I)[t*3]), b = Vert((*I)[t*3+1]), c = Vert((*I)[t*3+2]);
                    const float d2 = DistPointTri2(p, a, b, c);
                    if (d2 < bd2) { bd2 = d2; best = (int)t; }
                }
            return best;
        }
    };
}



namespace DesignChecks
{

    // ======================================================================
    // Parting-plane mesh split (step 1 of the face-by-face draft method).
    // Original vertices pass through untouched; a triangle straddling the plane
    // is clipped into its + and - parts, and the new edge vertices (snapped
    // exactly onto the plane, normals interpolated) are welded so the parting
    // line is one shared ring of vertices.
    // ======================================================================
    void SplitMeshByPlane(
        const std::vector<float>& posNorm,
        const std::vector<unsigned int>& indices,
        const glm::vec3& planeNormal,
        float planeOffset,
        std::vector<float>& outPosNorm,
        std::vector<unsigned int>& outIndices,
        float onPlaneEps)
    {
        outPosNorm.clear(); outIndices.clear();
        const size_t stride = 6;
        if (posNorm.size() < stride || indices.size() < 3)
        { outPosNorm = posNorm; outIndices = indices; return; }
        const size_t vcount = posNorm.size() / stride;

        glm::vec3 n = planeNormal;
        {
            const float L = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
            if (L > 1.0e-12f) n /= L;
            else { outPosNorm = posNorm; outIndices = indices; return; }
        }

        outPosNorm = posNorm;                 // keep all original verts; append new
        outIndices.reserve(indices.size());

        auto P = [&](unsigned int i) {
            return glm::vec3(posNorm[i*stride+0], posNorm[i*stride+1], posNorm[i*stride+2]);
        };
        auto N = [&](unsigned int i) {
            return glm::vec3(posNorm[i*stride+3], posNorm[i*stride+4], posNorm[i*stride+5]);
        };
        auto dist = [&](const glm::vec3& p) { return glm::dot(p, n) - planeOffset; };

        // Weld only the newly created plane vertices (shared parting ring).
        const float q = 1.0e-4f;
        std::map<std::array<long,3>, unsigned int> planeVerts;
        auto addPlaneVert = [&](glm::vec3 p, glm::vec3 nn)->unsigned int
        {
            p -= n * (glm::dot(p, n) - planeOffset);   // snap exactly onto plane
            const std::array<long,3> k{ (long)std::llround(p.x/q),
                (long)std::llround(p.y/q), (long)std::llround(p.z/q) };
            auto it = planeVerts.find(k);
            if (it != planeVerts.end()) return it->second;
            const float L = std::sqrt(nn.x*nn.x + nn.y*nn.y + nn.z*nn.z);
            if (L > 1.0e-12f) nn /= L; else nn = n;
            const unsigned int idx = (unsigned int)(outPosNorm.size() / stride);
            outPosNorm.push_back(p.x);  outPosNorm.push_back(p.y);  outPosNorm.push_back(p.z);
            outPosNorm.push_back(nn.x); outPosNorm.push_back(nn.y); outPosNorm.push_back(nn.z);
            planeVerts.emplace(k, idx);
            return idx;
        };

        // A clipped-polygon vertex: an existing vertex (orig >= 0) or a new
        // plane-intersection vertex (orig < 0, carrying interpolated attributes).
        struct CV { int orig; glm::vec3 p; glm::vec3 nrm; };
        auto emitPoly = [&](const std::vector<CV>& poly)
        {
            auto resolve = [&](const CV& v)->unsigned int {
                return (v.orig >= 0) ? (unsigned int)v.orig : addPlaneVert(v.p, v.nrm);
            };
            for (size_t i = 1; i + 1 < poly.size(); ++i)   // fan triangulation
            {
                const unsigned int ia = resolve(poly[0]);
                const unsigned int ib = resolve(poly[i]);
                const unsigned int ic = resolve(poly[i+1]);
                if (ia == ib || ib == ic || ia == ic) continue;   // degenerate
                outIndices.push_back(ia); outIndices.push_back(ib); outIndices.push_back(ic);
            }
        };

        for (size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            const unsigned int a = indices[t], b = indices[t+1], c = indices[t+2];
            if (a >= vcount || b >= vcount || c >= vcount) continue;

            const glm::vec3 pa = P(a), pb = P(b), pc = P(c);
            const float dd[3] = { dist(pa), dist(pb), dist(pc) };
            const bool anyPos = (dd[0] > onPlaneEps) || (dd[1] > onPlaneEps) || (dd[2] > onPlaneEps);
            const bool anyNeg = (dd[0] < -onPlaneEps) || (dd[1] < -onPlaneEps) || (dd[2] < -onPlaneEps);

            if (!(anyPos && anyNeg))   // wholly on one side (or just touching)
            {
                outIndices.push_back(a); outIndices.push_back(b); outIndices.push_back(c);
                continue;
            }

            const CV tri[3] = { { (int)a, pa, N(a) },
                                { (int)b, pb, N(b) },
                                { (int)c, pc, N(c) } };
            auto clip = [&](bool keepPos)->std::vector<CV>
            {
                std::vector<CV> out;
                for (int i = 0; i < 3; ++i)
                {
                    const float dcur = dd[i], dnxt = dd[(i+1)%3];
                    const bool curIn = keepPos ? (dcur >= -onPlaneEps) : (dcur <= onPlaneEps);
                    if (curIn) out.push_back(tri[i]);
                    const bool strictCross =
                        (dcur > onPlaneEps && dnxt < -onPlaneEps) ||
                        (dcur < -onPlaneEps && dnxt > onPlaneEps);
                    if (strictCross)
                    {
                        const float u = dcur / (dcur - dnxt);
                        const CV& cur = tri[i]; const CV& nxt = tri[(i+1)%3];
                        CV I; I.orig = -1;
                        I.p   = cur.p + (nxt.p - cur.p) * u;
                        I.nrm = cur.nrm + (nxt.nrm - cur.nrm) * u;
                        out.push_back(I);
                    }
                }
                return out;
            };
            emitPoly(clip(true));
            emitPoly(clip(false));
        }
    }

    // ======================================================================
    // Face-by-face draft check with ray-assigned mould-half ownership.
    // Runs on the shot display mesh directly (no remesh). See the header.
    // ======================================================================
    std::vector<DraftSample> BuildFaceDraftSamples(
        const std::vector<float>& posNorm,
        const std::vector<unsigned int>& indices,
        const std::vector<std::vector<float>>& halfVerts,
        const std::vector<std::vector<unsigned int>>& halfIndices,
        const std::vector<int>& triFaceId,
        const FaceDraftParams& params,
        int* outFallbackCount)
    {
        std::vector<DraftSample> out;
        if (outFallbackCount) *outFallbackCount = 0;
        const size_t stride = 6;
        if (posNorm.size() < stride || indices.size() < 3) return out;
        const size_t vcount = posNorm.size() / stride;

        glm::vec3 draw = params.drawAxis;
        {
            const float L = std::sqrt(draw.x*draw.x + draw.y*draw.y + draw.z*draw.z);
            if (L > 1.0e-12f) draw /= L; else draw = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        auto Pos = [&](unsigned int i) {
            return glm::vec3(posNorm[i*stride+0], posNorm[i*stride+1], posNorm[i*stride+2]);
        };
        auto Nrm = [&](unsigned int i) {
            return glm::vec3(posNorm[i*stride+3], posNorm[i*stride+4], posNorm[i*stride+5]);
        };

        // One grid per half + each half's pull side (0 = +draw, 1 = -draw),
        // inferred from the half's centroid relative to the parting plane.
        const size_t H = std::min(halfVerts.size(), halfIndices.size());
        std::vector<TriGrid> grids(H);
        std::vector<int>     halfSide(H, 0);
        for (size_t h = 0; h < H; ++h)
        {
            grids[h].Build(halfVerts[h], halfIndices[h]);
            const size_t n = halfVerts[h].size() / 3;
            glm::dvec3 c(0.0);
            for (size_t i = 0; i < n; ++i)
                c += glm::dvec3(halfVerts[h][i*3], halfVerts[h][i*3+1], halfVerts[h][i*3+2]);
            if (n > 0) c /= (double)n;
            const double s = c.x*draw.x + c.y*draw.y + c.z*draw.z - (double)params.partingOffset;
            halfSide[h] = (s >= 0.0) ? 0 : 1;
        }

        out.reserve(indices.size() / 3);
        for (size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            const unsigned int a = indices[t], b = indices[t+1], c = indices[t+2];
            if (a >= vcount || b >= vcount || c >= vcount) continue;

            const glm::vec3 p0 = Pos(a), p1 = Pos(b), p2 = Pos(c);
            const glm::vec3 cr = glm::cross(p1 - p0, p2 - p0);
            const float len = std::sqrt(cr.x*cr.x + cr.y*cr.y + cr.z*cr.z);
            if (len <= 1.0e-12f) continue;                  // degenerate triangle

            glm::vec3 nrm = cr / len;                        // geometric facet normal
            const glm::vec3 vn = Nrm(a) + Nrm(b) + Nrm(c);   // orient outward
            if ((nrm.x*vn.x + nrm.y*vn.y + nrm.z*vn.z) < 0.0f) nrm = -nrm;

            const float area = 0.5f * len;
            const glm::vec3 ctr = (p0 + p1 + p2) / 3.0f;

            // Ownership: cast along +normal; the nearest half hit owns the facet.
            const glm::vec3 o = ctr + nrm * (float)params.rayEpsilon;
            int   owner = -1;
            float bestT = 1e30f;
            for (size_t h = 0; h < H; ++h)
            {
                float tt;
                if (grids[h].RayNearestHit(o, nrm, 0.0f, tt) && tt < bestT)
                { bestT = tt; owner = (int)h; }
            }

            int side;
            if (owner >= 0)
                side = halfSide[owner];
            else
            {
                // Fallback: parting-plane side of the centroid (normal tie-break).
                float sd = ctr.x*draw.x + ctr.y*draw.y + ctr.z*draw.z - params.partingOffset;
                if (std::fabs(sd) < 1.0e-9f) sd = nrm.x*draw.x + nrm.y*draw.y + nrm.z*draw.z;
                side = (sd >= 0.0f) ? 0 : 1;
                if (outFallbackCount) ++(*outFallbackCount);
            }

            const glm::vec3 pull = (side == 0) ? draw : -draw;
            float dp = nrm.x*pull.x + nrm.y*pull.y + nrm.z*pull.z;
            if (dp < -1.0f) dp = -1.0f; else if (dp > 1.0f) dp = 1.0f;

            const int triIdx = (int)(t / 3);

            DraftSample smp;
            smp.signedDraftDeg = Deg(std::asin(dp));
            smp.area     = area;
            smp.faceId   = (triIdx < (int)triFaceId.size()) ? triFaceId[triIdx]
                                                            : triIdx + 1;
            smp.half     = side;
            smp.objectId = -1;      // per-cavity split not used by this check
            smp.trapped  = false;   // trapped/undercut intentionally omitted
            out.push_back(smp);
        }

        return out;
    }

    FaceDraftStats ClassifyFaceDraft(
        const std::vector<DraftSample>& samples, const FaceDraftParams& params)
    {
        FaceDraftStats r;
        r.totalFaces = (int)samples.size();

        float failDeg = params.failDraftDeg;
        float warnDeg = params.warnDraftDeg;
        if (warnDeg < failDeg) warnDeg = failDeg;   // keep the bands ordered
        const float bdEps = params.backdraftEpsDeg; // near-vertical isn't back-draft

        float minD = 90.0f;
        double totalA = 0.0, failA = 0.0, warnA = 0.0;
        for (const DraftSample& s : samples)
        {
            if (s.signedDraftDeg < minD) minD = s.signedDraftDeg;
            totalA += (double)s.area;
            if (s.signedDraftDeg < -bdEps)        ++r.backdraftCount;
            if (s.signedDraftDeg < failDeg)       { ++r.failCount; failA += (double)s.area; }
            else if (s.signedDraftDeg < warnDeg)  { ++r.warnCount; warnA += (double)s.area; }
            else                                  ++r.passCount;
        }
        if (!samples.empty()) r.minDraftDeg = minD;
        r.failAreaMm2  = (float)failA;
        r.warnAreaMm2  = (float)warnA;
        r.totalAreaMm2 = (float)totalA;

        // Significance gate: a band counts toward the verdict only when its total
        // area reaches the threshold, so isolated mesh-artifact facets can't fail
        // an otherwise-good part. Value is a % of surface area, or absolute mm^2.
        float sigAbs = params.significanceByPercent
            ? (float)(totalA * (double)params.significanceValue / 100.0)
            : params.significanceValue;
        if (sigAbs < 0.0f) sigAbs = 0.0f;
        r.significanceMm2 = sigAbs;

        const bool failSignificant = (failA > 0.0) && ((float)failA >= sigAbs);
        const bool warnSignificant = (warnA > 0.0) && ((float)warnA >= sigAbs);
        r.failSuppressed = (r.failCount > 0) && !failSignificant;
        r.warnSuppressed = (r.warnCount > 0) && !warnSignificant;

        if      (failSignificant) r.overall = Severity::Fail;
        else if (warnSignificant) r.overall = Severity::Warning;
        else                      r.overall = Severity::Pass;

        return r;
    }


}  // namespace DesignChecks
