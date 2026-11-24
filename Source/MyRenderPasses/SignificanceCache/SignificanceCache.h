#pragma once

#include "Falcor.h"
#include "Core/Pass/ComputePass.h"
#include "RenderGraph/RenderPass.h"
#include "Utils/Sampling/SampleGenerator.h"

#include <array>

namespace Falcor
{
class SignificanceCache : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(SignificanceCache, "SignificanceCache", "Path guiding with adaptive oriented cosine lobes.");

    static ref<SignificanceCache> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<SignificanceCache>(pDevice, props);
    }

    SignificanceCache(ref<Device> pDevice, const Properties& props);

    Properties getProperties() const override;
    void setProperties(const Properties& props) override;
    RenderPassReflection reflect(const CompileData& compileData) override;
    void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    void onSceneUpdates(RenderContext* pRenderContext, IScene::UpdateFlags updates) override;
    void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    void renderUI(Gui::Widgets& widget) override;

private:
    struct Options
    {
        bool enableGuiding = true;
        bool searchNeighborCells = false;
        uint32_t maxBounces = 6;
        uint32_t guidedBounces = 6;
        uint32_t queryLobes = 4;
        uint32_t gridResolution = 16;
        uint32_t warmupFrames = 4;
        uint32_t updateInterval = 4;
        uint32_t seed = 0;
        float guideMix = 0.5f;
        float minRoughness = 0.05f;
        float maxExponent = 64.f;
    };

    void applyOptions(const Options& options);
    ref<ComputePass> createCameraPass(bool guiding);
    void prepareCache(RenderContext* pRenderContext, const ShaderVar& var);
    void updateCache(RenderContext* pRenderContext);

    Options mOptions;
    ref<Scene> mpScene;
    ref<SampleGenerator> mpSampleGenerator;
    std::array<ref<ComputePass>, 2> mCameraPasses;
    ref<ComputePass> mpUpdatePass;
    ref<Buffer> mpRecords;
    ref<Buffer> mpSamples;
    ref<Buffer> mpSampleCounts;
    uint32_t mFrameCount = 0;
    uint32_t mTrainingFrames = 0;
    bool mCacheDirty = true;
    bool mOptionsChanged = true;
};
} // namespace Falcor
