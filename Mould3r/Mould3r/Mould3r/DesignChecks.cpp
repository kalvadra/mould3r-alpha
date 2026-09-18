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

    // Largest |n.draw| we still treat as "no definite pull side" (vertical
    // wall): such a face is parallel to the pull and cannot be trapped, so it
    // is judged on draft only, not accessibility. sin(0.01 deg) ~ 1.7e-4.
    constexpr double kVerticalEps = 2.0e-4;

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

// ===== Isotropic remesher (Botsch-Kobbelt), file-local ====================
namespace   // reopened; uses TriGrid from the anonymous namespace above
{
namespace rmsh
{
    struct RV3 { double x, y, z; };
    inline RV3 operator+(RV3 a, RV3 b){ return {a.x+b.x,a.y+b.y,a.z+b.z}; }
    inline RV3 operator-(RV3 a, RV3 b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
    inline RV3 operator*(RV3 a, double s){ return {a.x*s,a.y*s,a.z*s}; }
    inline double rdot(RV3 a, RV3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
    inline RV3 rcross(RV3 a, RV3 b){ return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
    inline double rlen(RV3 a){ return std::sqrt(rdot(a,a)); }
    inline RV3 rnorm(RV3 a){ double l=rlen(a); return l>1e-20?a*(1.0/l):RV3{0,0,0}; }

    struct Mesh {
        std::vector<RV3> V; std::vector<char> vAlive, vFeature;
        std::vector<std::array<int,3>> F; std::vector<char> fAlive;
    };
    struct Ref {
        const TriGrid* grid=nullptr;
        RV3 closest(RV3 p) const {
            if(!grid) return p;
            glm::vec3 q=grid->ClosestPoint(glm::vec3((float)p.x,(float)p.y,(float)p.z));
            return {(double)q.x,(double)q.y,(double)q.z};
        }
    };

    typedef std::pair<int,int> Edge;
    inline Edge ek(int a,int b){ return a<b?Edge(a,b):Edge(b,a); }
    inline double triArea(const Mesh&m,int f){auto&t=m.F[f];return 0.5*rlen(rcross(m.V[t[1]]-m.V[t[0]],m.V[t[2]]-m.V[t[0]]));}
    inline RV3 triNormal(const Mesh&m,int f){auto&t=m.F[f];return rnorm(rcross(m.V[t[1]]-m.V[t[0]],m.V[t[2]]-m.V[t[0]]));}

    inline std::map<Edge,std::vector<int>> edgeFaces(const Mesh&m){
        std::map<Edge,std::vector<int>> em;
        for(int f=0;f<(int)m.F.size();++f){ if(!m.fAlive[f])continue; auto&t=m.F[f];
            for(int e=0;e<3;++e) em[ek(t[e],t[(e+1)%3])].push_back(f); }
        return em;
    }
    inline std::vector<std::vector<int>> vertFaces(const Mesh&m){
        std::vector<std::vector<int>> vf(m.V.size());
        for(int f=0;f<(int)m.F.size();++f){ if(!m.fAlive[f])continue; for(int k=0;k<3;++k) vf[m.F[f][k]].push_back(f); }
        return vf;
    }
    inline void markFeatures(Mesh&m,double angleDeg){
        auto em=edgeFaces(m);
        std::fill(m.vFeature.begin(),m.vFeature.end(),(char)0);
        double cthr=std::cos(angleDeg*3.14159265358979/180.0);
        for(auto&kv:em){
            if(kv.second.size()==1){ m.vFeature[kv.first.first]=1; m.vFeature[kv.first.second]=1; continue; }
            if(kv.second.size()==2){ RV3 n0=triNormal(m,kv.second[0]),n1=triNormal(m,kv.second[1]);
                if(rdot(n0,n1)<cthr){ m.vFeature[kv.first.first]=1; m.vFeature[kv.first.second]=1; } }
        }
    }
    inline int addVertex(Mesh&m,RV3 p,char feat){m.V.push_back(p);m.vAlive.push_back(1);m.vFeature.push_back(feat);return (int)m.V.size()-1;}
    inline int addFace(Mesh&m,int a,int b,int c){m.F.push_back({a,b,c});m.fAlive.push_back(1);return (int)m.F.size()-1;}
    inline void compact(Mesh&m){
        std::vector<int> remap(m.V.size(),-1); Mesh o;
        for(int v=0;v<(int)m.V.size();++v) if(m.vAlive[v]){remap[v]=(int)o.V.size();o.V.push_back(m.V[v]);o.vAlive.push_back(1);o.vFeature.push_back(m.vFeature[v]);}
        for(int f=0;f<(int)m.F.size();++f) if(m.fAlive[f]){auto&t=m.F[f]; if(remap[t[0]]<0||remap[t[1]]<0||remap[t[2]]<0)continue; o.F.push_back({remap[t[0]],remap[t[1]],remap[t[2]]});o.fAlive.push_back(1);}
        m=o;
    }
    inline void splitLong(Mesh&m,const Ref&R,double high){
      for(int sweep=0;sweep<8;++sweep){
        auto em=edgeFaces(m);
        std::vector<Edge> longE;
        for(auto&kv:em) if(kv.second.size()==2){ RV3 a=m.V[kv.first.first],b=m.V[kv.first.second]; if(rlen(b-a)>high) longE.push_back(kv.first); }
        if(longE.empty()) break;
        std::vector<char> faceTouched(m.F.size(),0);
        for(auto&e:longE){
            auto it=em.find(e); if(it==em.end()||it->second.size()!=2) continue;
            if(faceTouched[it->second[0]]||faceTouched[it->second[1]]) continue;
            int a=e.first,b=e.second; if(!m.vAlive[a]||!m.vAlive[b]) continue;
            RV3 mid=(m.V[a]+m.V[b])*0.5; char feat=(m.vFeature[a]&&m.vFeature[b])?1:0;
            if(!feat) mid=R.closest(mid);
            int mI=addVertex(m,mid,feat);
            for(int f:it->second){ if(!m.fAlive[f])continue; auto t=m.F[f];
                for(int k=0;k<3;++k){ int u=t[k],v=t[(k+1)%3],w=t[(k+2)%3];
                    if((u==a&&v==b)||(u==b&&v==a)){ m.fAlive[f]=0; addFace(m,u,mI,w); addFace(m,mI,v,w); break; } } }
            faceTouched[it->second[0]]=1; faceTouched[it->second[1]]=1; em.erase(it);
        }
        compact(m);
      }
    }
    inline bool wouldFlip(const Mesh&m,const std::vector<std::vector<int>>&vf,int keep,int rem,RV3 np){
        for(int f:vf[rem]){ if(!m.fAlive[f])continue; auto t=m.F[f];
            int cnt=0; for(int k=0;k<3;++k) if(t[k]==keep) cnt++; if(cnt>0) continue;
            RV3 p[3]; for(int k=0;k<3;++k) p[k]=(t[k]==rem)?np:m.V[t[k]];
            RV3 nOld=triNormal(m,f); RV3 nNew=rnorm(rcross(p[1]-p[0],p[2]-p[0]));
            if(rdot(nOld,nNew)<0.2) return true;
            if(rlen(rcross(p[1]-p[0],p[2]-p[0]))<1e-14) return true; }
        return false;
    }
    inline void collapseShort(Mesh&m,const Ref&R,double low,double high){
        auto em=edgeFaces(m); auto vf=vertFaces(m);
        std::vector<Edge> shortE;
        for(auto&kv:em) if(kv.second.size()==2){ RV3 a=m.V[kv.first.first],b=m.V[kv.first.second]; if(rlen(b-a)<low) shortE.push_back(kv.first); }
        std::vector<char> touched(m.V.size(),0);
        for(auto&e:shortE){
            int a=e.first,b=e.second;
            if(!m.vAlive[a]||!m.vAlive[b]||touched[a]||touched[b]) continue;
            if(m.vFeature[a]!=m.vFeature[b]) continue;
            auto it=em.find(e); if(it==em.end()||it->second.size()!=2) continue;
            std::set<int> na,nb;
            for(int f:vf[a]){ if(!m.fAlive[f])continue; for(int k=0;k<3;++k) if(m.F[f][k]!=a) na.insert(m.F[f][k]); }
            for(int f:vf[b]){ if(!m.fAlive[f])continue; for(int k=0;k<3;++k) if(m.F[f][k]!=b) nb.insert(m.F[f][k]); }
            std::vector<int> inter; for(int x:na) if(nb.count(x)) inter.push_back(x);
            std::set<int> opp; for(int f:it->second){auto t=m.F[f]; for(int k=0;k<3;++k) if(t[k]!=a&&t[k]!=b) opp.insert(t[k]);}
            if(inter.size()!=opp.size()) continue;
            bool ok=true; for(int x:inter) if(!opp.count(x)) ok=false; if(!ok) continue;
            RV3 np=m.vFeature[a]?m.V[a]:(m.vFeature[b]?m.V[b]:(m.V[a]+m.V[b])*0.5);
            if(!(m.vFeature[a]||m.vFeature[b])) np=R.closest(np);
            bool tooLong=false; for(int x:na) if(x!=b&&rlen(m.V[x]-np)>high) tooLong=true; for(int x:nb) if(x!=a&&rlen(m.V[x]-np)>high) tooLong=true;
            if(tooLong) continue;
            if(wouldFlip(m,vf,a,b,np)||wouldFlip(m,vf,b,a,np)) continue;
            m.V[a]=np; for(int f:it->second) m.fAlive[f]=0;
            for(int f:vf[b]){ if(!m.fAlive[f])continue; for(int k=0;k<3;++k) if(m.F[f][k]==b) m.F[f][k]=a;
                auto&t=m.F[f]; if(t[0]==t[1]||t[1]==t[2]||t[0]==t[2]) m.fAlive[f]=0; }
            m.vAlive[b]=0; touched[a]=1; touched[b]=1; for(int x:inter) touched[x]=1;
        }
        compact(m);
    }
    inline void flipValence(Mesh&m){
        auto em=edgeFaces(m);
        std::vector<int> val(m.V.size(),0);
        for(auto&kv:em){ val[kv.first.first]++; val[kv.first.second]++; }
        for(auto&kv:em){ if(kv.second.size()!=2) continue; int a=kv.first.first,b=kv.first.second;
            if(m.vFeature[a]&&m.vFeature[b]) continue;
            int f0=kv.second[0],f1=kv.second[1]; if(!m.fAlive[f0]||!m.fAlive[f1])continue;
            int c=-1,d=-1; for(int k=0;k<3;++k){ if(m.F[f0][k]!=a&&m.F[f0][k]!=b)c=m.F[f0][k]; if(m.F[f1][k]!=a&&m.F[f1][k]!=b)d=m.F[f1][k]; }
            if(c<0||d<0||c==d) continue; if(em.count(ek(c,d))) continue;
            int tgt=6;
            int devOld=std::abs(val[a]-tgt)+std::abs(val[b]-tgt)+std::abs(val[c]-tgt)+std::abs(val[d]-tgt);
            int devNew=std::abs(val[a]-1-tgt)+std::abs(val[b]-1-tgt)+std::abs(val[c]+1-tgt)+std::abs(val[d]+1-tgt);
            if(devNew>=devOld) continue;
            RV3 nc=rnorm(rcross(m.V[d]-m.V[c],m.V[a]-m.V[c]));
            RV3 nd=rnorm(rcross(m.V[b]-m.V[c],m.V[d]-m.V[c]));
            RV3 o0=triNormal(m,f0);
            if(rdot(nc,o0)<0.2||rdot(nd,o0)<0.2) continue;
            m.F[f0]={c,d,a}; m.F[f1]={d,c,b}; val[a]--; val[b]--; val[c]++; val[d]++; em[ek(c,d)]={f0,f1};
        }
        compact(m);
    }
    inline void relax(Mesh&m,const Ref&R){
        auto vf=vertFaces(m);
        std::vector<RV3> np(m.V.size());
        for(int v=0;v<(int)m.V.size();++v){ np[v]=m.V[v];
            if(!m.vAlive[v]||m.vFeature[v]) continue;
            std::set<int> nb; for(int f:vf[v]){ if(!m.fAlive[f])continue; for(int k=0;k<3;++k) if(m.F[f][k]!=v) nb.insert(m.F[f][k]); }
            if(nb.empty()) continue;
            RV3 c{0,0,0}; for(int x:nb) c=c+m.V[x]; c=c*(1.0/nb.size());
            RV3 vn{0,0,0}; for(int f:vf[v]){ if(!m.fAlive[f])continue; vn=vn+triNormal(m,f)*triArea(m,f); } vn=rnorm(vn);
            RV3 d=c-m.V[v]; d=d-vn*rdot(d,vn);
            np[v]=R.closest(m.V[v]+d);
        }
        m.V=np;
    }
    inline void killDegenerate(Mesh&m,double L){
        for(int f=0;f<(int)m.F.size();++f){ if(!m.fAlive[f])continue; auto&t=m.F[f];
            if(t[0]==t[1]||t[1]==t[2]||t[0]==t[2]){m.fAlive[f]=0;continue;}
            if(triArea(m,f)<1e-10*L*L) m.fAlive[f]=0; }
        compact(m);
    }
    inline int boundaryEdges(const Mesh&m){ auto em=edgeFaces(m); int b=0; for(auto&kv:em) if(kv.second.size()==1) b++; return b; }
    inline void weld(Mesh&m,double eps){
        auto key=[&](RV3 p){ return std::array<long,3>{ (long)std::llround(p.x/eps),(long)std::llround(p.y/eps),(long)std::llround(p.z/eps) }; };
        std::map<std::array<long,3>,std::vector<int>> grid;
        Mesh o; std::vector<int> remap(m.V.size(),-1);
        for(int v=0;v<(int)m.V.size();++v){ if(!m.vAlive[v])continue;
            RV3 p=m.V[v]; auto k=key(p); int found=-1;
            for(long dz=-1;dz<=1&&found<0;++dz)for(long dy=-1;dy<=1&&found<0;++dy)for(long dx=-1;dx<=1&&found<0;++dx){
                std::array<long,3> nk{k[0]+dx,k[1]+dy,k[2]+dz}; auto it=grid.find(nk); if(it==grid.end())continue;
                for(int ov:it->second){ if(rlen(o.V[ov]-p)<=eps){found=ov;break;} } }
            if(found>=0) remap[v]=found;
            else{ int id=(int)o.V.size(); o.V.push_back(p);o.vAlive.push_back(1);o.vFeature.push_back(0); grid[k].push_back(id); remap[v]=id; } }
        for(int f=0;f<(int)m.F.size();++f){ if(!m.fAlive[f])continue; auto&t=m.F[f];
            int a=remap[t[0]],b=remap[t[1]],c=remap[t[2]]; if(a<0||b<0||c<0||a==b||b==c||a==c)continue;
            o.F.push_back({a,b,c});o.fAlive.push_back(1); }
        m=o;
    }
    inline void fillHoles(Mesh&m){
        std::set<std::pair<int,int>> faceEdges;
        for(int f=0;f<(int)m.F.size();++f){ if(!m.fAlive[f])continue; auto&t=m.F[f];
            for(int k=0;k<3;++k) faceEdges.insert({t[k],t[(k+1)%3]}); }
        std::multimap<int,int> holeEdges;
        for(auto&e:faceEdges) if(!faceEdges.count({e.second,e.first})) holeEdges.insert({e.second,e.first});
        while(!holeEdges.empty()){
            int start=holeEdges.begin()->first;
            std::vector<int> loop; int cur=start; bool closed=false; std::set<int> seen;
            for(int guard=0; guard<200000; ++guard){
                auto it=holeEdges.find(cur); if(it==holeEdges.end()) break;
                int nxt=it->second; holeEdges.erase(it); loop.push_back(cur);
                if(nxt==start){ closed=true; break; }
                if(seen.count(nxt)) break; seen.insert(nxt); cur=nxt; }
            if(closed && loop.size()>=3)
                for(size_t i=1;i+1<loop.size();++i) addFace(m,loop[0],loop[(int)i],loop[(int)i+1]);
        }
    }

    inline void run(Mesh&m,const Ref&R,double targetArea,int iters,double featDeg,
                    const std::function<bool(float)>& onProgress){
        double L=std::sqrt(4.0*targetArea/std::sqrt(3.0));
        double high=4.0/3.0*L, low=4.0/5.0*L;
        weld(m, L*1e-3);              // merge coincident seam verts (unwelded tess)
        markFeatures(m,featDeg);
        for(int i=0;i<iters;++i){
            if(onProgress && !onProgress((float)i/(float)iters*0.9f)) return;  // cancelled
            splitLong(m,R,high); killDegenerate(m,L);
            collapseShort(m,R,low,high); killDegenerate(m,L);
            flipValence(m); relax(m,R); markFeatures(m,featDeg);
        }
        int prevB=-1;                 // patch dropped faces so the grid is closed
        for(int pass=0; pass<6; ++pass){
            killDegenerate(m,L); fillHoles(m);
            int b=boundaryEdges(m); if(b==0||b==prevB) break; prevB=b;
        }
        if(onProgress) onProgress(0.95f);
    }
} // namespace rmsh
} // anonymous


namespace DesignChecks
{
    DemoldabilityResult CheckDemoldability(
        const TopoDS_Shape& shot, const Params& params,
        std::vector<UndercutRay>* debugRays)
    {
        DemoldabilityResult result;
        if (debugRays) debugRays->clear();
        if (shot.IsNull()) return result;

        // Canonical, stable face indexing shared with the display tessellation.
        TopTools_IndexedMapOfShape faceMap;
        TopExp::MapShapes(shot, TopAbs_FACE, faceMap);
        if (faceMap.Extent() == 0) return result;

        // Sampling tessellation (coarse — accessibility itself is analytic).
        BRepMesh_IncrementalMesh mesher(shot, params.sampleDeflection, false, 0.5, true);

        // Analytic ray/solid intersector, loaded once (only when accessibility
        // is being tested — skipped for draft-only runs).
        IntCurvesFace_ShapeIntersector intersector;
        if (params.checkUndercuts)
            intersector.Load(shot, 1.0e-6);

        const gp_Dir drawDir(params.drawAxis.x, params.drawAxis.y, params.drawAxis.z);
        const double eps = (double)params.rayEpsilon;

        glm::vec3 undercutLoc(0.0f), failLoc(0.0f), warnLoc(0.0f);
        bool haveUndercutLoc = false, haveFailLoc = false, haveWarnLoc = false;

        for (int fi = 1; fi <= faceMap.Extent(); ++fi)
        {
            const TopoDS_Face face = TopoDS::Face(faceMap(fi));

            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull() || !tri->HasUVNodes()) continue;

            Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
            if (surf.IsNull()) continue;

            const gp_Trsf tr = loc.Transformation();
            const bool reversed = (face.Orientation() == TopAbs_REVERSED);

            float faceMinDraft = 90.0f;
            bool  faceUndercut = false;
            glm::vec3 faceRepPoint(0.0f);
            bool  haveRep = false;
            bool  faceHasSample = false;

            for (int t = 1; t <= tri->NbTriangles(); ++t)
            {
                int n1, n2, n3;
                tri->Triangle(t).Get(n1, n2, n3);

                // 3D centroid (on the trimmed face) for the ray origin.
                gp_Pnt p1 = tri->Node(n1); p1.Transform(tr);
                gp_Pnt p2 = tri->Node(n2); p2.Transform(tr);
                gp_Pnt p3 = tri->Node(n3); p3.Transform(tr);
                const gp_Pnt c(
                    (p1.X() + p2.X() + p3.X()) / 3.0,
                    (p1.Y() + p2.Y() + p3.Y()) / 3.0,
                    (p1.Z() + p2.Z() + p3.Z()) / 3.0);

                // Analytic normal at the centroid UV.
                const gp_Pnt2d uv1 = tri->UVNode(n1);
                const gp_Pnt2d uv2 = tri->UVNode(n2);
                const gp_Pnt2d uv3 = tri->UVNode(n3);
                const double u = (uv1.X() + uv2.X() + uv3.X()) / 3.0;
                const double v = (uv1.Y() + uv2.Y() + uv3.Y()) / 3.0;

                GeomLProp_SLProps props(surf, u, v, 1, 1.0e-7);
                if (!props.IsNormalDefined()) continue;

                gp_Dir nrm = props.Normal();
                if (reversed) nrm.Reverse();

                faceHasSample = true;

                const double d = nrm.Dot(drawDir);             // [-1, 1]
                const float draft = Deg((float)std::asin(std::min(std::fabs(d), 1.0)));
                faceMinDraft = std::min(faceMinDraft, draft);

                if (!haveRep)
                {
                    faceRepPoint = glm::vec3((float)c.X(), (float)c.Y(), (float)c.Z());
                    haveRep = true;
                }

                // Accessibility: only facets with a definite pull side can be
                // trapped. Cast along the side the surface faces. Skipped on
                // draft-only runs (params.checkUndercuts == false).
                if (params.checkUndercuts && !faceUndercut && std::fabs(d) > kVerticalEps)
                {
                    gp_Dir pull = drawDir;
                    if (d < 0.0) pull.Reverse();   // faces -draw => pull -draw

                    const gp_Lin ray(c, pull);
                    intersector.Perform(ray, eps, Precision::Infinite());
                    if (intersector.IsDone() && intersector.NbPnt() > 0)
                    {
                        faceUndercut = true;        // blocked => undercut

                        if (debugRays)
                        {
                            // Nearest hit along the ray (smallest parameter).
                            int best = 1;
                            double bestW = intersector.WParameter(1);
                            for (int k = 2; k <= intersector.NbPnt(); ++k)
                            {
                                const double w = intersector.WParameter(k);
                                if (w < bestW) { bestW = w; best = k; }
                            }
                            const gp_Pnt hp = intersector.Pnt(best);
                            UndercutRay rec;
                            rec.origin = glm::vec3((float)c.X(), (float)c.Y(), (float)c.Z());
                            rec.dir = glm::vec3((float)pull.X(), (float)pull.Y(), (float)pull.Z());
                            rec.hit = glm::vec3((float)hp.X(), (float)hp.Y(), (float)hp.Z());
                            debugRays->push_back(rec);
                        }
                    }
                }
            }

            if (!faceHasSample) continue;
            result.totalFaces++;
            result.minDraftDeg = std::min(result.minDraftDeg, faceMinDraft);

            if (faceUndercut)
            {
                result.undercutFaces.push_back(fi);
                if (!haveUndercutLoc) { undercutLoc = faceRepPoint; haveUndercutLoc = true; }
            }
            else if (faceMinDraft < params.failDraftDeg)
            {
                result.failDraftFaces.push_back(fi);
                if (!haveFailLoc) { failLoc = faceRepPoint; haveFailLoc = true; }
            }
            else if (faceMinDraft < params.warnDraftDeg)
            {
                result.warnDraftFaces.push_back(fi);
                if (!haveWarnLoc) { warnLoc = faceRepPoint; haveWarnLoc = true; }
            }
        }

        result.undercutCount  = (int)result.undercutFaces.size();
        result.failDraftCount = (int)result.failDraftFaces.size();
        result.warnDraftCount = (int)result.warnDraftFaces.size();

        if (result.totalFaces == 0)
        {
            result.minDraftDeg = 0.0f;
            return result;
        }

        auto fmt = [](int n, const char* b) {
            return std::to_string(n) + std::string(b);
        };

        if (result.undercutCount > 0)
        {
            Issue is;
            is.severity = Severity::Fail;
            is.description = fmt(result.undercutCount,
                " face(s) are undercut — blocked along the pull axis. "
                "These trap the body in the mould and need a side action.");
            is.location = undercutLoc;
            result.issues.push_back(std::move(is));
        }
        if (result.failDraftCount > 0)
        {
            Issue is;
            is.severity = Severity::Fail;
            is.description = fmt(result.failDraftCount,
                " face(s) have draft below the fail threshold.");
            is.location = failLoc;
            result.issues.push_back(std::move(is));
        }
        if (result.warnDraftCount > 0)
        {
            Issue is;
            is.severity = Severity::Warning;
            is.description = fmt(result.warnDraftCount,
                " face(s) have draft below the warn threshold.");
            is.location = warnLoc;
            result.issues.push_back(std::move(is));
        }

        result.overall = Severity::Pass;
        for (const Issue& is : result.issues)
        {
            if (is.severity == Severity::Fail) { result.overall = Severity::Fail; break; }
            if (is.severity == Severity::Warning) result.overall = Severity::Warning;
        }

        return result;
    }

    DraftSignResult ClassifyDraftSign(
        const TopoDS_Shape& shot, const Params& params)
    {
        DraftSignResult result;
        if (shot.IsNull()) return result;

        TopTools_IndexedMapOfShape faceMap;
        TopExp::MapShapes(shot, TopAbs_FACE, faceMap);
        if (faceMap.Extent() == 0) return result;

        BRepMesh_IncrementalMesh mesher(shot, params.sampleDeflection, false, 0.5, true);

        const gp_Dir drawDir(params.drawAxis.x, params.drawAxis.y, params.drawAxis.z);

        for (int fi = 1; fi <= faceMap.Extent(); ++fi)
        {
            const TopoDS_Face face = TopoDS::Face(faceMap(fi));

            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull() || !tri->HasUVNodes()) continue;

            Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
            if (surf.IsNull()) continue;

            const bool reversed = (face.Orientation() == TopAbs_REVERSED);

            bool hasUp = false, hasDown = false, hasVert = false, hasSample = false;

            for (int t = 1; t <= tri->NbTriangles(); ++t)
            {
                int n1, n2, n3;
                tri->Triangle(t).Get(n1, n2, n3);

                const gp_Pnt2d uv1 = tri->UVNode(n1);
                const gp_Pnt2d uv2 = tri->UVNode(n2);
                const gp_Pnt2d uv3 = tri->UVNode(n3);
                const double u = (uv1.X() + uv2.X() + uv3.X()) / 3.0;
                const double v = (uv1.Y() + uv2.Y() + uv3.Y()) / 3.0;

                GeomLProp_SLProps props(surf, u, v, 1, 1.0e-7);
                if (!props.IsNormalDefined()) continue;

                gp_Dir nrm = props.Normal();
                if (reversed) nrm.Reverse();

                hasSample = true;
                const double d = nrm.Dot(drawDir);
                if (d > kVerticalEps)       hasUp = true;
                else if (d < -kVerticalEps) hasDown = true;
                else                        hasVert = true;
            }

            if (!hasSample) continue;
            result.totalFaces++;

            if (hasUp && hasDown)   result.mixedFaces.push_back(fi);
            else if (hasUp)         result.upFaces.push_back(fi);
            else if (hasDown)       result.downFaces.push_back(fi);
            else                    result.verticalFaces.push_back(fi);
        }

        return result;
    }

    SeparationResult CheckSeparation(
        const TopoDS_Shape& shot,
        const std::vector<TopoDS_Shape>& halves,
        const SeparationParams& params,
        TopoDS_Shape* outOverlap)
    {
        SeparationResult result;
        result.perHalfVolume.assign(halves.size(), 0.0);
        result.perHalfStatus.assign(halves.size(), 0);
        if (outOverlap) *outOverlap = TopoDS_Shape();
        if (shot.IsNull()) return result;

        const gp_Vec axis(params.drawAxis.x, params.drawAxis.y, params.drawAxis.z);

        TopoDS_Compound overlap;
        BRep_Builder builder;
        builder.MakeCompound(overlap);
        bool anyOverlap = false;

        for (size_t i = 0; i < halves.size(); ++i)
        {
            const TopoDS_Shape& half = halves[i];
            if (half.IsNull()) { result.perHalfStatus[i] = 2; result.halvesFailedToEval++; continue; }

            result.halvesTested++;

            // Lift along the side of the draw axis this half sits on.
            GProp_GProps gp;
            BRepGProp::VolumeProperties(half, gp);
            const gp_Pnt com = gp.CentreOfMass();
            const double side =
                com.X() * axis.X() + com.Y() * axis.Y() + com.Z() * axis.Z();
            const double sgn = (side >= 0.0) ? 1.0 : -1.0;

            gp_Trsf tr;
            tr.SetTranslation(gp_Vec(axis).Multiplied(sgn * (double)params.liftMm));

            try
            {
                BRepBuilderAPI_Transform mover(half, tr, true);
                if (!mover.IsDone()) { result.perHalfStatus[i] = 2; result.halvesFailedToEval++; continue; }
                const TopoDS_Shape lifted = mover.Shape();

                BRepAlgoAPI_Common common(lifted, shot);
                common.Build();
                if (!common.IsDone()) { result.perHalfStatus[i] = 2; result.halvesFailedToEval++; continue; }

                const TopoDS_Shape ov = common.Shape();
                double vol = 0.0;
                if (!ov.IsNull())
                {
                    GProp_GProps ovProps;
                    BRepGProp::VolumeProperties(ov, ovProps);
                    vol = std::fabs(ovProps.Mass());
                }

                result.perHalfVolume[i] = vol;
                if (vol > params.volumeThreshold)
                {
                    result.perHalfStatus[i] = 1;
                    result.halvesCollided++;
                    result.totalOverlapVolume += vol;
                    if (!ov.IsNull()) { builder.Add(overlap, ov); anyOverlap = true; }
                }
                else
                {
                    result.perHalfStatus[i] = 0;
                }
            }
            catch (const Standard_Failure&)
            {
                result.perHalfStatus[i] = 2;
                result.halvesFailedToEval++;
            }
        }

        if (result.halvesCollided > 0)      result.overall = Severity::Fail;
        else if (result.halvesFailedToEval > 0) result.overall = Severity::Warning;
        else                                 result.overall = Severity::Pass;

        if (outOverlap && anyOverlap) *outOverlap = overlap;
        return result;
    }


    // ======================================================================
    // Stage 0a - area-weighted draft scoring (BREP).
    // ======================================================================
    std::vector<DraftSample> BuildDraftSamplesBREP(
        const DraftSampleInputBREP& in, const DraftSampleParams& params)
    {
        std::vector<DraftSample> out;
        if (!in.shot || in.shot->IsNull()) return out;
        const TopoDS_Shape& shot = *in.shot;

        TopTools_IndexedMapOfShape faceMap;
        TopExp::MapShapes(shot, TopAbs_FACE, faceMap);
        if (faceMap.Extent() == 0) return out;

        BRepMesh_IncrementalMesh mesher(shot, params.sampleDeflection, false, 0.5, true);

        const gp_Dir drawDir(params.drawAxis.x, params.drawAxis.y, params.drawAxis.z);
        const gp_Vec drawVec(drawDir);
        const double off = (double)params.classifyOffset;

        IntCurvesFace_ShapeIntersector intersector;
        if (params.checkTrapped) intersector.Load(shot, 1.0e-6);

        // Per-half solid classifiers + the direction each half physically opens
        // (drawAxis pointing away from the parting plane on that half's side).
        std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> halfCls;
        std::vector<gp_Vec> halfPull;
        if (in.halves)
        {
            for (const TopoDS_Shape& h : *in.halves)
            {
                if (h.IsNull()) continue;
                halfCls.emplace_back(std::make_unique<BRepClass3d_SolidClassifier>(h));
                GProp_GProps gp; BRepGProp::VolumeProperties(h, gp);
                const gp_Pnt com = gp.CentreOfMass();
                const double side = com.X()*drawVec.X() + com.Y()*drawVec.Y()
                                  + com.Z()*drawVec.Z();
                gp_Vec pull = drawVec;
                if (side < 0.0) pull.Reverse();
                halfPull.push_back(pull);
            }
        }

        // Per-object solid classifiers (BREP-scene imports are sewn solids). A
        // shot face whose just-inside point lands in object k is that part's
        // surface; a feed face matches none and keeps objectId -1.
        std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> objCls;
        if (in.objectShapes)
            for (const TopoDS_Shape& o : *in.objectShapes)
                objCls.emplace_back(o.IsNull() ? nullptr
                    : std::make_unique<BRepClass3d_SolidClassifier>(o));

        for (int fi = 1; fi <= faceMap.Extent(); ++fi)
        {
            const TopoDS_Face face = TopoDS::Face(faceMap(fi));

            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull() || !tri->HasUVNodes()) continue;

            Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
            if (surf.IsNull()) continue;

            const gp_Trsf trsf = loc.Transformation();
            const bool reversed = (face.Orientation() == TopAbs_REVERSED);

            // objectId is a whole-face property (the fuse never makes a face
            // that is part on one patch and feed on another). Classify once,
            // from the first usable sample of the face.
            int  faceObjectId  = -1;
            bool faceClassified = false;

            // Half assignment is a whole-face property EXCEPT on faces that
            // cross the parting plane. Resolving it per face (not per sample)
            // is the difference between fast and unusable: a solid point-
            // classify per triangle against each big half solid stalls the
            // whole generation. A non-straddling face belongs wholly to the
            // half on its side of the plane (a cheap sign test); only the few
            // straddling faces pay for the per-sample classifier below.
            bool   faceStraddles = false;
            gp_Vec faceHalfPull  = drawVec;   // used only when !faceStraddles
            {
                Bnd_Box bb;
                BRepBndLib::Add(face, bb);
                if (!bb.IsVoid())
                {
                    double xr[2], yr[2], zr[2];
                    bb.Get(xr[0], yr[0], zr[0], xr[1], yr[1], zr[1]);
                    double lo = 1.0e300, hi = -1.0e300;
                    for (int a = 0; a < 2; ++a)
                    for (int b = 0; b < 2; ++b)
                    for (int d = 0; d < 2; ++d)
                    {
                        const double proj = xr[a]*drawVec.X()
                                          + yr[b]*drawVec.Y()
                                          + zr[d]*drawVec.Z();
                        lo = std::min(lo, proj);
                        hi = std::max(hi, proj);
                    }
                    const double tol = 1.0e-2;
                    faceStraddles = (lo < -tol && hi > tol);
                    if (!faceStraddles && (lo + hi) < 0.0) faceHalfPull.Reverse();
                }
            }

            for (int t = 1; t <= tri->NbTriangles(); ++t)
            {
                int n1, n2, n3;
                tri->Triangle(t).Get(n1, n2, n3);

                gp_Pnt p1 = tri->Node(n1); p1.Transform(trsf);
                gp_Pnt p2 = tri->Node(n2); p2.Transform(trsf);
                gp_Pnt p3 = tri->Node(n3); p3.Transform(trsf);
                const gp_Pnt c(
                    (p1.X() + p2.X() + p3.X()) / 3.0,
                    (p1.Y() + p2.Y() + p3.Y()) / 3.0,
                    (p1.Z() + p2.Z() + p3.Z()) / 3.0);

                const gp_Pnt2d uv1 = tri->UVNode(n1);
                const gp_Pnt2d uv2 = tri->UVNode(n2);
                const gp_Pnt2d uv3 = tri->UVNode(n3);
                const double u = (uv1.X() + uv2.X() + uv3.X()) / 3.0;
                const double v = (uv1.Y() + uv2.Y() + uv3.Y()) / 3.0;

                GeomLProp_SLProps props(surf, u, v, 1, 1.0e-7);
                if (!props.IsNormalDefined()) continue;

                gp_Dir nrm = props.Normal();
                if (reversed) nrm.Reverse();
                const gp_Vec nVec(nrm);

                // Triangle area (mm^2) - the integration weight.
                const gp_Vec e1(p1, p2), e2(p1, p3);
                const double area = 0.5 * e1.Crossed(e2).Magnitude();
                if (area <= 0.0) continue;

                // objectId: classify a point just inside the shot, once/face.
                if (!faceClassified)
                {
                    faceClassified = true;
                    const gp_Pnt inside(c.X() - nrm.X()*off,
                                        c.Y() - nrm.Y()*off,
                                        c.Z() - nrm.Z()*off);
                    for (size_t k = 0; k < objCls.size(); ++k)
                    {
                        if (!objCls[k]) continue;
                        objCls[k]->Perform(inside, 1.0e-7);
                        const TopAbs_State st = objCls[k]->State();
                        if (st == TopAbs_IN || st == TopAbs_ON)
                        {
                            faceObjectId = (int)k;
                            break;
                        }
                    }
                }

                // Pull direction = the way this facet's half opens. Cheap for
                // the common (non-straddling) face; the classifier is only
                // touched on faces that actually cross the parting plane.
                gp_Vec pull;
                if (!faceStraddles)
                {
                    pull = faceHalfPull;
                }
                else
                {
                    bool havePull = false;
                    const gp_Pnt outside(c.X() + nrm.X()*off,
                                         c.Y() + nrm.Y()*off,
                                         c.Z() + nrm.Z()*off);
                    for (size_t h = 0; h < halfCls.size(); ++h)
                    {
                        halfCls[h]->Perform(outside, 1.0e-7);
                        const TopAbs_State st = halfCls[h]->State();
                        if (st == TopAbs_IN || st == TopAbs_ON)
                        {
                            pull = halfPull[h];
                            havePull = true;
                            break;
                        }
                    }
                    if (!havePull)
                    {
                        // On the plane / owned by neither: the side the
                        // centroid sits on, then the normal's own side.
                        double s = c.X()*drawVec.X() + c.Y()*drawVec.Y()
                                 + c.Z()*drawVec.Z();
                        if (std::fabs(s) < 1.0e-9) s = nVec.Dot(drawVec);
                        pull = drawVec;
                        if (s < 0.0) pull.Reverse();
                    }
                }

                const double dp = std::max(-1.0, std::min(1.0, nVec.Dot(pull)));

                DraftSample smp;
                smp.signedDraftDeg = Deg((float)std::asin(dp));
                smp.area = (float)area;
                smp.objectId = faceObjectId;
                smp.faceId = fi;
                smp.half = (pull.Dot(drawVec) > 0.0) ? 0 : 1;

                if (params.checkTrapped &&
                    std::fabs(dp) > std::sin(params.undercutEpsilonDeg * 0.01745329252))
                {
                    const gp_Dir pullDir(pull);
                    const gp_Lin ray(c, pullDir);
                    intersector.Perform(ray, (double)params.rayEpsilon, Precision::Infinite());
                    smp.trapped = (intersector.IsDone() && intersector.NbPnt() > 0);
                }

                out.push_back(smp);
            }
        }

        return out;
    }

    // ======================================================================
    // Mesh draft sampling. Draft Index needs only per-triangle arithmetic;
    // Trapped-Area (ray any-hit) and per-cavity objectId (nearest part
    // triangle) use the uniform-grid accelerator above.
    // ======================================================================
    std::vector<DraftSample> BuildDraftSamplesMesh(
        const std::vector<float>& posNorm,
        const std::vector<unsigned int>& indices,
        const std::vector<float>& objVerts,
        const std::vector<unsigned int>& objIndices,
        const std::vector<int>& objTriId,
        const DraftSampleParams& params)
    {
        std::vector<DraftSample> out;
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

        // Object grid for per-cavity objectId (nearest part triangle).
        TriGrid objGrid;
        const bool haveObjects = !objVerts.empty() && objIndices.size() >= 3
                              && objTriId.size() == objIndices.size() / 3;
        if (haveObjects) objGrid.Build(objVerts, objIndices);

        // Shot grid for the trapped-area ray (positions extracted from posNorm).
        std::vector<float> shotPos;
        TriGrid shotGrid;
        if (params.checkTrapped)
        {
            shotPos.resize(vcount * 3);
            for (size_t i = 0; i < vcount; ++i)
            {
                shotPos[i*3+0] = posNorm[i*stride+0];
                shotPos[i*3+1] = posNorm[i*stride+1];
                shotPos[i*3+2] = posNorm[i*stride+2];
            }
            shotGrid.Build(shotPos, indices);
        }

        const float objTol = params.meshObjectTol;

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

            float side = ctr.x*draw.x + ctr.y*draw.y + ctr.z*draw.z;
            if (std::fabs(side) < 1.0e-9f)
                side = nrm.x*draw.x + nrm.y*draw.y + nrm.z*draw.z;  // tie-break
            const glm::vec3 pull = (side < 0.0f) ? -draw : draw;

            float dp = nrm.x*pull.x + nrm.y*pull.y + nrm.z*pull.z;
            if (dp < -1.0f) dp = -1.0f; else if (dp > 1.0f) dp = 1.0f;

            DraftSample smp;
            smp.signedDraftDeg = Deg(std::asin(dp));
            smp.area   = area;
            smp.faceId = (int)(t / 3) + 1;   // triangle index (mesh heatmap)
            smp.half   = (glm::dot(pull, draw) > 0.0f) ? 0 : 1;

            // objectId: nearest part triangle within tolerance, else feed (-1).
            smp.objectId = -1;
            if (haveObjects)
            {
                float d2 = 0.0f;
                const int tri = objGrid.Nearest(ctr, objTol, d2);
                if (tri >= 0 && tri < (int)objTriId.size())
                    smp.objectId = objTriId[tri];
            }

            // trapped: cast along pull from just outside the facet.
            smp.trapped = false;
            if (params.checkTrapped &&
                std::fabs(dp) > std::sin(params.undercutEpsilonDeg * 0.01745329252f))
            {
                const glm::vec3 o = ctr + pull * (float)params.rayEpsilon;
                smp.trapped = shotGrid.RayAnyHit(o, pull, 0.0f);
            }

            out.push_back(smp);
        }

        return out;
    }
    bool IsotropicRemesh(
        const std::vector<float>& inVerts, const std::vector<unsigned int>& inIndices,
        float targetAreaMm2, std::vector<float>& outPosNorm,
        std::vector<unsigned int>& outIndices, int iterations, float featureDeg,
        const std::function<bool(float)>& onProgress)
    {
        outPosNorm.clear(); outIndices.clear();
        if (inVerts.size() < 9 || inIndices.size() < 3 || targetAreaMm2 <= 0.0f) return false;

        TriGrid ref; ref.Build(inVerts, inIndices);   // reprojection reference

        rmsh::Mesh m;
        const size_t vc = inVerts.size() / 3;
        m.V.reserve(vc); m.vAlive.assign(vc, 1); m.vFeature.assign(vc, 0);
        for (size_t i = 0; i < vc; ++i)
            m.V.push_back({ (double)inVerts[i*3], (double)inVerts[i*3+1], (double)inVerts[i*3+2] });
        const size_t tc = inIndices.size() / 3;
        m.F.reserve(tc); m.fAlive.assign(tc, 1);
        for (size_t t = 0; t < tc; ++t)
            m.F.push_back({ (int)inIndices[t*3], (int)inIndices[t*3+1], (int)inIndices[t*3+2] });

        rmsh::Ref R; R.grid = &ref;
        rmsh::run(m, R, (double)targetAreaMm2, iterations, (double)featureDeg, onProgress);

        // Emit posNorm. Each vertex's normal is the outward facet normal of the
        // nearest INPUT triangle (the input winding is outward), so downstream
        // facet-normal orientation is correct regardless of remesh winding.
        auto inPos = [&](unsigned i) {
            return glm::vec3(inVerts[i*3], inVerts[i*3+1], inVerts[i*3+2]);
        };
        outPosNorm.reserve(m.V.size() * 6);
        for (const auto& pd : m.V)
        {
            const glm::vec3 pos((float)pd.x, (float)pd.y, (float)pd.z);
            glm::vec3 nrm(0.0f, 1.0f, 0.0f);
            const int tri = ref.NearestTriangle(pos);
            if (tri >= 0)
            {
                const glm::vec3 a = inPos(inIndices[tri*3]), b = inPos(inIndices[tri*3+1]), c = inPos(inIndices[tri*3+2]);
                const glm::vec3 fn = glm::cross(b - a, c - a);
                const float l = std::sqrt(fn.x*fn.x + fn.y*fn.y + fn.z*fn.z);
                if (l > 1.0e-12f) nrm = fn / l;
            }
            outPosNorm.push_back(pos.x); outPosNorm.push_back(pos.y); outPosNorm.push_back(pos.z);
            outPosNorm.push_back(nrm.x); outPosNorm.push_back(nrm.y); outPosNorm.push_back(nrm.z);
        }
        for (const auto& f : m.F) { outIndices.push_back((unsigned)f[0]); outIndices.push_back((unsigned)f[1]); outIndices.push_back((unsigned)f[2]); }
        return !outIndices.empty();
    }

    DraftScoreResult ScoreDraft(
        const std::vector<DraftSample>& samples, const DraftScoreParams& params)
    {
        DraftScoreResult r;

        double totalA = 0.0, scoredA = 0.0, signedA = 0.0;
        double belowFailA = 0.0, belowWarnA = 0.0, trappedA = 0.0;
        int n = 0;

        // Per-cavity needs per-object provenance. When none is present (a mesh
        // scene does not tag objectId yet), score the whole shot instead of
        // filtering everything out.
        bool haveObjects = false;
        for (const DraftSample& s : samples)
            if (s.objectId >= 0) { haveObjects = true; break; }
        const bool perCav = params.perCavity && haveObjects;

        for (const DraftSample& s : samples)
        {
            totalA += (double)s.area;
            if (perCav && s.objectId < 0) continue;  // feed excluded
            scoredA += (double)s.area;
            signedA += (double)s.signedDraftDeg * (double)s.area;
            if (s.signedDraftDeg < params.failDraftDeg) belowFailA += (double)s.area;
            if (s.signedDraftDeg < params.warnDraftDeg) belowWarnA += (double)s.area;
            if (s.trapped) trappedA += (double)s.area;
            ++n;
        }

        r.totalAreaMm2  = (float)totalA;
        r.scoredAreaMm2 = (float)scoredA;
        r.sampleCount   = n;
        if (scoredA <= 0.0) return r;   // valid stays false

        r.valid = true;
        r.draftIndexDeg         = (float)(signedA / scoredA);
        r.trappedAreaFraction   = (float)(trappedA / scoredA);
        r.areaBelowFailFraction = (float)(belowFailA / scoredA);
        r.areaBelowWarnFraction = (float)(belowWarnA / scoredA);

        // Draft verdict: by the aggregate index only (decision 2 - the
        // area-below fractions are reported for context, not as a trigger).
        if (r.draftIndexDeg < params.failDraftDeg)      r.overall = Severity::Fail;
        else if (r.draftIndexDeg < params.warnDraftDeg) r.overall = Severity::Warning;
        else                                            r.overall = Severity::Pass;

        // Trapped area is its own axis (decision 4): any real blocked area is a
        // fail, since an undercut cannot release by straight pull.
        r.trappedSeverity = (r.trappedAreaFraction > params.trappedNoise)
            ? Severity::Fail : Severity::Pass;

        return r;
    }

}  // namespace DesignChecks
