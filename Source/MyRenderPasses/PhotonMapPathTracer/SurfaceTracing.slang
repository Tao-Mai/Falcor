#include "Scene/SceneDefines.slangh"

__exported import Scene.RaytracingInline;
__exported import Rendering.Materials.IMaterialInstance;

ShadingData loadSurface(const HitInfo hit, const Ray ray)
{
    VertexData vertex = {};
    uint materialID = {};
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangle = hit.getTriangleHit();
        vertex = gScene.getVertexData(triangle);
        materialID = gScene.getMaterialID(triangle.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit triangle = hit.getDisplacedTriangleHit();
        vertex = gScene.getVertexData(triangle, -ray.dir);
        materialID = gScene.getMaterialID(triangle.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        const CurveHit curve = hit.getCurveHit();
        vertex = gScene.getVertexDataFromCurve(curve);
        materialID = gScene.getMaterialID(curve.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_SDF_GRID)
    if (hit.getType() == HitType::SDFGrid)
    {
        const SDFGridHit grid = hit.getSDFGridHit();
        vertex = gScene.getVertexDataFromSDFGrid(grid, ray.origin, ray.dir);
        materialID = gScene.getMaterialID(grid.instanceID);
    }
#endif
    return gScene.materials.prepareShadingData(vertex, materialID, -ray.dir);
}

float3 scatterOrigin(const HitInfo hit, const ShadingData sd, float3 direction)
{
    if (hit.getType() == HitType::Curve)
        return sd.posW - sd.curveRadius * sd.frame.N;
    return sd.computeRayOrigin(dot(sd.getOrientedFaceNormal(), direction) >= 0.f);
}

