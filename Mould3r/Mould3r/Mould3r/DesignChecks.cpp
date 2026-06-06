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

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    inline float Deg(float rad) { return rad * (180.0f / kPi); }

    // Largest |n.draw| we still treat as "no definite pull side" (vertical
    // wall): such a face is parallel to the pull and cannot be trapped, so it
    // is judged on draft only, not accessibility. sin(0.01 deg) ~ 1.7e-4.
    constexpr double kVerticalEps = 2.0e-4;
}

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

        // Analytic ray/solid intersector, loaded once.
        IntCurvesFace_ShapeIntersector intersector;
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
                // trapped. Cast along the side the surface faces.
                if (!faceUndercut && std::fabs(d) > kVerticalEps)
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

}  // namespace DesignChecks
