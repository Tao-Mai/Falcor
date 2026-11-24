#pragma once

#include "Falcor.h"
#include "Core/Pass/ComputePass.h"
#include "RenderGraph/RenderPass.h"
#include "Rendering/Lights/EnvMapSampler.h"
#include "Utils/Sampling/SampleGenerator.h"

#include <array>

namespace Falcor
{
class PhotonMapPathTracer : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(PhotonMapPathTracer, "PhotonMapPathTracer", "Path tracing guided by a map of indirect photons.");

    static ref<PhotonMapPathTracer> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<PhotonMapPathTracer>(pDevice, props);
    }

    PhotonMapPathTracer(ref<Device> pDevice, const Properties& props);

    Properties getProperties() const override;
    void setProperties(const Properties& props) override;
    RenderPassReflection reflect(const CompileData& compileData) override;
    void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    void onSceneUpdates(RenderContext* pRenderContext, IScene::UpdateFlags sceneUpdates) override;
    void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    void renderUI(Gui::Widgets& widget) override;

private:
    struct Options
    {
        bool enableGuiding = true;
        uint32_t maxBounces = 6;
        uint32_t guidedBounces = 1;
        uint32_t photonPaths = 131072;
        uint32_t photonBounces = 4;
        float gatherRadius = 0.04f;
        uint32_t minPhotons = 4;
        float guideMix = 0.5f;
        uint32_t seed = 0;
        uint32_t photonSeed = 0;
    };

    void applyOptions(const Options& options);
    ref<ComputePass> createPass(const char* shaderFile, const DefineList& extraDefines = {});
    void buildPhotonMap(RenderContext* pRenderContext, float radius);
    void bindPhotonGrid(const ShaderVar& var) const;

    Options mOptions;
    ref<Scene> mpScene;
    ref<SampleGenerator> mpSampleGenerator;
    std::unique_ptr<EnvMapSampler> mpEnvMapSampler;
    std::array<ref<ComputePass>, 2> mCameraPasses;
    ref<ComputePass> mpTracePhotons;
    ref<Buffer> mpCellCounts;
    ref<Buffer> mpPhotons;
    float3 mGridOrigin = float3(0.f);
    uint3 mGridDimensions = uint3(0);
    float mGridRadius = 0.f;
    uint32_t mFrameCount = 0;
    bool mMapDirty = true;
    bool mOptionsChanged = true;
};
} // namespace Falcor
