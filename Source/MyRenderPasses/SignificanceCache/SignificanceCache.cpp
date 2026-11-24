#include "SignificanceCache.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

#include <algorithm>
#include <cmath>

namespace Falcor
{
extern "C" FALCOR_API_EXPORT void registerPlugin(PluginRegistry& registry)
{
    registry.registerClass<RenderPass, SignificanceCache>();
}

namespace
{
constexpr char kCameraShader[] = "MyRenderPasses/SignificanceCache/SignificanceCache.cs.slang";
constexpr char kUpdateShader[] = "MyRenderPasses/SignificanceCache/UpdateCache.cs.slang";
constexpr uint32_t kLobesPerCell = 8;
constexpr uint32_t kSamplesPerCell = 32;

const ChannelList kInputs = {
    {"vbuffer", "gVBuffer", "Packed visibility buffer"},
    {"viewW", "gViewW", "World-space view direction", true},
};
const ChannelList kOutputs = {
    {"color", "gOutputColor", "Linear HDR radiance", false, ResourceFormat::RGBA32Float},
    {"guide", "gOutputGuide", "Primary hit: lobe count, mean exponent, mixture probability, training eligibility", false,
     ResourceFormat::RGBA32Float},
};

using Update = IScene::UpdateFlags;
const auto kCacheUpdates = Update::GeometryMoved | Update::GeometryChanged | Update::MeshesChanged | Update::CurvesMoved |
    Update::CustomPrimitivesMoved | Update::DisplacementChanged | Update::SDFGeometryChanged | Update::SDFGridConfigChanged |
    Update::LightCollectionChanged | Update::MaterialsChanged | Update::EmissiveMaterialsChanged | Update::RenderSettingsChanged |
    Update::LightsMoved | Update::LightIntensityChanged | Update::LightPropertiesChanged | Update::LightCountChanged |
    Update::EnvMapChanged | Update::EnvMapPropertiesChanged | Update::RecompileNeeded;
} // namespace

SignificanceCache::SignificanceCache(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    FALCOR_CHECK(mpDevice->isShaderModelSupported(ShaderModel::SM6_5), "SignificanceCache requires Shader Model 6.5.");
    setProperties(props);
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
}

void SignificanceCache::setProperties(const Properties& props)
{
    auto options = mOptions;
    for (const auto& [key, value] : props)
    {
        if (key == "enableGuiding") options.enableGuiding = value;
        else if (key == "searchNeighborCells") options.searchNeighborCells = value;
        else if (key == "maxBounces") options.maxBounces = value;
        else if (key == "guidedBounces") options.guidedBounces = value;
        else if (key == "queryLobes") options.queryLobes = value;
        else if (key == "gridResolution") options.gridResolution = value;
        else if (key == "warmupFrames") options.warmupFrames = value;
        else if (key == "updateInterval") options.updateInterval = value;
        else if (key == "seed") options.seed = value;
        else if (key == "guideMix") options.guideMix = value;
        else if (key == "minRoughness") options.minRoughness = value;
        else if (key == "maxExponent") options.maxExponent = value;
        else logWarning("Unknown property '{}' in SignificanceCache properties.", key);
    }
    applyOptions(options);
}

void SignificanceCache::applyOptions(const Options& options)
{
    FALCOR_CHECK(options.maxBounces <= 16 && options.guidedBounces <= 16, "Bounce limits must be in [0, 16].");
    FALCOR_CHECK(options.queryLobes >= 1 && options.queryLobes <= kLobesPerCell, "queryLobes must be in [1, 8].");
    FALCOR_CHECK(options.gridResolution >= 1 && options.gridResolution <= 32, "gridResolution must be in [1, 32].");
    FALCOR_CHECK(options.updateInterval > 0, "updateInterval must be positive.");
    FALCOR_CHECK(options.guideMix >= 0.f && options.guideMix <= 0.9f, "guideMix must be in [0, 0.9].");
    FALCOR_CHECK(options.minRoughness >= 0.f && options.minRoughness <= 1.f, "minRoughness must be in [0, 1].");
    FALCOR_CHECK(options.maxExponent >= 1.f && options.maxExponent <= 128.f, "maxExponent must be in [1, 128].");

    if (options.maxBounces != mOptions.maxBounces || options.queryLobes != mOptions.queryLobes)
        mCameraPasses = {};
    mOptions = options;
    mFrameCount = 0;
    mCacheDirty = true;
    mOptionsChanged = true;
}

Properties SignificanceCache::getProperties() const
{
    Properties props;
    props["enableGuiding"] = mOptions.enableGuiding;
    props["searchNeighborCells"] = mOptions.searchNeighborCells;
    props["maxBounces"] = mOptions.maxBounces;
    props["guidedBounces"] = mOptions.guidedBounces;
    props["queryLobes"] = mOptions.queryLobes;
    props["gridResolution"] = mOptions.gridResolution;
    props["warmupFrames"] = mOptions.warmupFrames;
    props["updateInterval"] = mOptions.updateInterval;
    props["seed"] = mOptions.seed;
    props["guideMix"] = mOptions.guideMix;
    props["minRoughness"] = mOptions.minRoughness;
    props["maxExponent"] = mOptions.maxExponent;
    return props;
}

RenderPassReflection SignificanceCache::reflect(const CompileData& compileData)
{
    RenderPassReflection reflection;
    addRenderPassInputs(reflection, kInputs);
    addRenderPassOutputs(reflection, kOutputs);
    return reflection;
}

void SignificanceCache::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    mCameraPasses = {};
    mFrameCount = 0;
    mCacheDirty = true;
    mOptionsChanged = true;
    if (mpScene && mpScene->hasGeometryType(Scene::GeometryType::Custom))
        logWarning("SignificanceCache does not support custom primitives.");
}

void SignificanceCache::onSceneUpdates(RenderContext* pRenderContext, IScene::UpdateFlags updates)
{
    if ((updates & kCacheUpdates) != Update::None)
        mCacheDirty = true;
    if ((updates & Update::RecompileNeeded) != Update::None)
        mCameraPasses = {};
}

ref<ComputePass> SignificanceCache::createCameraPass(bool guiding)
{
    ProgramDesc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kCameraShader).csEntry("main");
    desc.addTypeConformances(mpScene->getTypeConformances());
    desc.setShaderModel(ShaderModel::SM6_5);
    DefineList defines = mpScene->getSceneDefines();
    defines.add(mpSampleGenerator->getDefines());
    defines.add("SC_GUIDING", guiding ? "1" : "0");
    defines.add("SC_PATH_CAPACITY", std::to_string(mOptions.maxBounces + 2));
    defines.add("SC_QUERY_LOBES", std::to_string(mOptions.queryLobes));
    auto pass = ComputePass::create(mpDevice, desc, defines);
    mpSampleGenerator->bindShaderData(pass->getRootVar());
    return pass;
}

void SignificanceCache::prepareCache(RenderContext* pRenderContext, const ShaderVar& var)
{
    const uint32_t cells = mOptions.gridResolution * mOptions.gridResolution * mOptions.gridResolution;
    if (!mpRecords || mpRecords->getElementCount() != cells * kLobesPerCell)
    {
        mpRecords = mpDevice->createStructuredBuffer(var["gRecords"], cells * kLobesPerCell);
        mpSamples = mpDevice->createStructuredBuffer(var["gSamples"], cells * kSamplesPerCell);
        mpSampleCounts = mpDevice->createStructuredBuffer(var["gSampleCounts"], cells);
        mCacheDirty = true;
    }
    if (mCacheDirty)
    {
        FALCOR_PROFILE(pRenderContext, "ResetCache");
        pRenderContext->clearUAV(mpRecords->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mpSampleCounts->getUAV().get(), uint4(0));
        mTrainingFrames = 0;
        mCacheDirty = false;
    }

    const auto bounds = mpScene->getSceneBounds();
    const float3 extent = bounds.extent();
    const float radius = length(extent) / float(mOptions.gridResolution);
    const auto cb = var["GuideCB"];
    const auto grid = cb["gGrid"];
    grid["origin"] = bounds.minPoint;
    grid["invExtent"] = float3(extent.x > 0.f ? 1.f / extent.x : 0.f, extent.y > 0.f ? 1.f / extent.y : 0.f,
                               extent.z > 0.f ? 1.f / extent.z : 0.f);
    grid["resolution"] = mOptions.gridResolution;
    grid["radiusSquared"] = radius * radius;
    cb["gCacheReady"] = uint32_t(mTrainingFrames >= std::max(1u, mOptions.warmupFrames));
    cb["gSearchNeighbors"] = uint32_t(mOptions.searchNeighborCells);
    cb["gGuidedBounces"] = mOptions.guidedBounces;
    cb["gGuideMix"] = mOptions.guideMix;
    cb["gMinRoughness"] = mOptions.minRoughness;
    var["gRecords"] = mpRecords;
    var["gSamples"] = mpSamples;
    var["gSampleCounts"] = mpSampleCounts;
}

void SignificanceCache::updateCache(RenderContext* pRenderContext)
{
    const uint32_t first = std::max(1u, mOptions.warmupFrames);
    if (++mTrainingFrames < first || (mTrainingFrames - first) % mOptions.updateInterval != 0)
        return;

    FALCOR_PROFILE(pRenderContext, "UpdateCache");
    if (!mpUpdatePass)
        mpUpdatePass = ComputePass::create(mpDevice, kUpdateShader);
    const auto var = mpUpdatePass->getRootVar();
    const auto extent = mpScene->getSceneBounds().extent();
    const float radius = length(extent) / float(mOptions.gridResolution);
    var["UpdateCB"]["gCellCount"] = mpSampleCounts->getElementCount();
    var["UpdateCB"]["gMaxExponent"] = mOptions.maxExponent;
    var["UpdateCB"]["gRadiusSquared"] = radius * radius;
    var["gRecords"] = mpRecords;
    var["gSamples"] = mpSamples;
    var["gSampleCounts"] = mpSampleCounts;
    mpUpdatePass->execute(pRenderContext, mpSampleCounts->getElementCount(), 1);
}

void SignificanceCache::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        const auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[kRenderPassRefreshFlags] = flags | RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }
    if (!mpScene)
    {
        for (const auto& channel : kOutputs)
            if (auto output = renderData.getTexture(channel.name))
                pRenderContext->clearTexture(output.get(), float4(0.f));
        return;
    }
    if (mpScene->getRenderSettings().useEmissiveLights)
        mpScene->getLightCollection(pRenderContext);

    const auto bounds = mpScene->getSceneBounds();
    const bool guiding = mOptions.enableGuiding && mOptions.guideMix > 0.f && mOptions.guidedBounces > 0 &&
        mOptions.maxBounces > 0 && bounds.valid() && length(bounds.extent()) > 0.f;
    auto& pass = mCameraPasses[guiding];
    if (!pass)
        pass = createCameraPass(guiding);
    const auto var = pass->getRootVar();
    const auto cb = var["CB"];
    if (guiding)
        prepareCache(pRenderContext, var);

    const auto view = renderData.getTexture("viewW");
    cb["gHasView"] = uint32_t(bool(view));
    cb["gFrameIndex"] = mOptions.seed + mFrameCount;
    cb["gPRNGDimension"] = dict.getValue(kRenderPassPRNGDimension, 0u);
    cb["gMaxBounces"] = mOptions.maxBounces;
    cb["gUseAnalyticLights"] = uint32_t(mpScene->useAnalyticLights());
    cb["gUseEmissiveLights"] = uint32_t(mpScene->useEmissiveLights());
    cb["gUseEnvLight"] = uint32_t(mpScene->useEnvLight());
    cb["gUseEnvBackground"] = uint32_t(mpScene->useEnvBackground());
    var["gVBuffer"] = renderData.getTexture("vbuffer");
    var["gViewW"] = view;
    var["gOutputColor"] = renderData.getTexture("color");
    var["gOutputGuide"] = renderData.getTexture("guide");
    mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"]);
    const uint2 dim = renderData.getDefaultTextureDims();
    pass->execute(pRenderContext, dim.x, dim.y);
    if (guiding)
        updateCache(pRenderContext);
    ++mFrameCount;
}

void SignificanceCache::renderUI(Gui::Widgets& widget)
{
    auto options = mOptions;
    bool changed = widget.checkbox("Enable significance cache", options.enableGuiding);
    changed |= widget.var("Max indirect bounces", options.maxBounces, 0u, 16u);
    changed |= widget.var("Guided bounces", options.guidedBounces, 0u, 16u);
    changed |= widget.var("Queried lobes", options.queryLobes, 1u, kLobesPerCell);
    changed |= widget.var("Guide probability", options.guideMix, 0.f, 0.9f, 0.01f);
    changed |= widget.var("Grid resolution", options.gridResolution, 1u, 32u);
    changed |= widget.checkbox("Search neighboring cells", options.searchNeighborCells);
    changed |= widget.var("Warmup frames", options.warmupFrames, 0u, 65535u);
    changed |= widget.var("Update interval", options.updateInterval, 1u, 65535u);
    changed |= widget.var("Minimum roughness", options.minRoughness, 0.f, 1.f, 0.01f);
    changed |= widget.var("Maximum exponent", options.maxExponent, 1.f, 128.f, 1.f);
    changed |= widget.var("Seed", options.seed);
    if (widget.button("Reset cache"))
        changed = true;
    if (changed)
        applyOptions(options);
}
} // namespace Falcor
