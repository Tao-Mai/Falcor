#include "PhotonMapPathTracer.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

#include <algorithm>
#include <cmath>

namespace Falcor
{
extern "C" FALCOR_API_EXPORT void registerPlugin(PluginRegistry& registry)
{
    registry.registerClass<RenderPass, PhotonMapPathTracer>();
}

namespace
{
constexpr char kCameraShader[] = "MyRenderPasses/PhotonMapPathTracer/PhotonMapPathTracer.cs.slang";
constexpr char kPhotonShader[] = "MyRenderPasses/PhotonMapPathTracer/TracePhotons.cs.slang";
constexpr uint32_t kCellCapacity = 32;
constexpr uint32_t kMaxBounces = 16;
constexpr uint32_t kMaxPhotonPaths = 1 << 20;
constexpr uint32_t kMaxPhotonBounces = 8;
constexpr float kMinGatherRadius = 1.f / 64.f;
constexpr float kMaxGatherRadius = 0.25f;
constexpr uint32_t kMaxGridDimension = uint32_t(0.5f / kMinGatherRadius);
constexpr float kMaxGuideMix = 0.9f;

const ChannelList kInputs = {
    {"vbuffer", "gVBuffer", "Packed visibility buffer"},
    {"viewW", "gViewW", "World-space view direction", true},
};
const ChannelList kOutputs = {
    {"color", "gOutputColor", "Linear HDR radiance", false, ResourceFormat::RGBA32Float},
};

using Update = IScene::UpdateFlags;
const auto kPhotonMapUpdates = Update::GeometryMoved | Update::GeometryChanged | Update::MeshesChanged | Update::CurvesMoved |
    Update::CustomPrimitivesMoved | Update::DisplacementChanged | Update::SDFGeometryChanged | Update::SDFGridConfigChanged |
    Update::LightCollectionChanged | Update::MaterialsChanged | Update::EmissiveMaterialsChanged | Update::RenderSettingsChanged |
    Update::LightsMoved | Update::LightIntensityChanged | Update::LightPropertiesChanged | Update::LightCountChanged |
    Update::EnvMapChanged | Update::EnvMapPropertiesChanged | Update::RecompileNeeded;
} // namespace

PhotonMapPathTracer::PhotonMapPathTracer(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    FALCOR_CHECK(mpDevice->isShaderModelSupported(ShaderModel::SM6_5), "PhotonMapPathTracer requires Shader Model 6.5.");
    setProperties(props);
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
}

Properties PhotonMapPathTracer::getProperties() const
{
    Properties props;
    props["enableGuiding"] = mOptions.enableGuiding;
    props["maxBounces"] = mOptions.maxBounces;
    props["guidedBounces"] = mOptions.guidedBounces;
    props["photonPaths"] = mOptions.photonPaths;
    props["photonBounces"] = mOptions.photonBounces;
    props["gatherRadius"] = mOptions.gatherRadius;
    props["minPhotons"] = mOptions.minPhotons;
    props["guideMix"] = mOptions.guideMix;
    props["seed"] = mOptions.seed;
    props["photonSeed"] = mOptions.photonSeed;
    return props;
}

void PhotonMapPathTracer::setProperties(const Properties& props)
{
    auto options = mOptions;
    for (const auto& [key, value] : props)
    {
        if (key == "enableGuiding")
            options.enableGuiding = value;
        else if (key == "maxBounces")
            options.maxBounces = value;
        else if (key == "guidedBounces")
            options.guidedBounces = value;
        else if (key == "photonPaths")
            options.photonPaths = value;
        else if (key == "photonBounces")
            options.photonBounces = value;
        else if (key == "gatherRadius")
            options.gatherRadius = value;
        else if (key == "minPhotons")
            options.minPhotons = value;
        else if (key == "guideMix")
            options.guideMix = value;
        else if (key == "seed")
            options.seed = value;
        else if (key == "photonSeed")
            options.photonSeed = value;
        else
            logWarning("Unknown property '{}' in PhotonMapPathTracer properties.", key);
    }
    applyOptions(options);
}

void PhotonMapPathTracer::applyOptions(const Options& options)
{
    FALCOR_CHECK(options.maxBounces <= kMaxBounces && options.guidedBounces <= kMaxBounces, "Camera bounce limits must be in [0, 16].");
    FALCOR_CHECK(options.photonPaths > 0 && options.photonPaths <= kMaxPhotonPaths, "photonPaths must be in [1, 1048576].");
    FALCOR_CHECK(options.photonBounces > 0 && options.photonBounces <= kMaxPhotonBounces, "photonBounces must be in [1, 8].");
    FALCOR_CHECK(options.gatherRadius >= kMinGatherRadius && options.gatherRadius <= kMaxGatherRadius, "gatherRadius must be in [1/64, 0.25].");
    FALCOR_CHECK(options.minPhotons > 0 && options.minPhotons <= kCellCapacity, "minPhotons must be in [1, 32].");
    FALCOR_CHECK(options.guideMix >= 0.f && options.guideMix <= kMaxGuideMix, "guideMix must be in [0, 0.9].");

    mMapDirty |= options.photonPaths != mOptions.photonPaths || options.photonBounces != mOptions.photonBounces ||
        options.gatherRadius != mOptions.gatherRadius || options.photonSeed != mOptions.photonSeed;
    mOptions = options;
    mFrameCount = 0;
    mOptionsChanged = true;
}

RenderPassReflection PhotonMapPathTracer::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputs);
    addRenderPassOutputs(reflector, kOutputs);
    return reflector;
}

void PhotonMapPathTracer::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    mCameraPasses = {};
    mpTracePhotons = nullptr;
    mpEnvMapSampler = nullptr;
    mpCellCounts = nullptr;
    mpPhotons = nullptr;
    mFrameCount = 0;
    mMapDirty = true;
    mOptionsChanged = true;
    if (mpScene && mpScene->hasGeometryType(Scene::GeometryType::Custom))
        logWarning("PhotonMapPathTracer uses scene ray queries and does not support custom primitives.");
}

void PhotonMapPathTracer::onSceneUpdates(RenderContext* pRenderContext, IScene::UpdateFlags sceneUpdates)
{
    if ((sceneUpdates & kPhotonMapUpdates) != Update::None)
        mMapDirty = true;
    if ((sceneUpdates & Update::RecompileNeeded) != Update::None)
    {
        mCameraPasses = {};
        mpTracePhotons = nullptr;
    }
}

ref<ComputePass> PhotonMapPathTracer::createPass(const char* shaderFile, const DefineList& extraDefines)
{
    ProgramDesc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(shaderFile).csEntry("main");
    desc.addTypeConformances(mpScene->getTypeConformances());
    desc.setShaderModel(ShaderModel::SM6_5);
    DefineList defines = mpScene->getSceneDefines();
    defines.add(mpSampleGenerator->getDefines());
    defines.add("PHOTON_CELL_CAPACITY", std::to_string(kCellCapacity));
    defines.add(extraDefines);
    auto pPass = ComputePass::create(mpDevice, desc, defines);
    mpSampleGenerator->bindShaderData(pPass->getRootVar());
    return pPass;
}

void PhotonMapPathTracer::bindPhotonGrid(const ShaderVar& var) const
{
    const auto grid = var["CB"]["gGrid"];
    grid["origin"] = mGridOrigin;
    grid["dimensions"] = mGridDimensions;
    grid["radius"] = mGridRadius;
    var["gCellCounts"] = mpCellCounts;
    var["gPhotons"] = mpPhotons;
}

void PhotonMapPathTracer::buildPhotonMap(RenderContext* pRenderContext, float radius)
{
    FALCOR_PROFILE(pRenderContext, "BuildPhotonMap");
    if (!mpTracePhotons)
        mpTracePhotons = createPass(kPhotonShader);

    const auto& bounds = mpScene->getSceneBounds();
    mGridOrigin = bounds.minPoint;
    mGridRadius = radius;
    const float3 cells = bounds.extent() / (2.f * radius);
    for (uint32_t axis = 0; axis < 3; ++axis)
        mGridDimensions[axis] = std::clamp(uint32_t(std::ceil(cells[axis])), 1u, kMaxGridDimension);
    const uint32_t cellCount = mGridDimensions.x * mGridDimensions.y * mGridDimensions.z;

    auto var = mpTracePhotons->getRootVar();
    if (!mpCellCounts || mpCellCounts->getElementCount() != cellCount)
    {
        auto pCellCounts = mpDevice->createStructuredBuffer(var["gCellCounts"], cellCount);
        auto pPhotons = mpDevice->createStructuredBuffer(var["gPhotons"], cellCount * kCellCapacity);
        mpCellCounts = std::move(pCellCounts);
        mpPhotons = std::move(pPhotons);
    }
    pRenderContext->clearUAV(mpCellCounts->getUAV().get(), uint4(0));
    pRenderContext->uavBarrier(mpCellCounts.get());

    bindPhotonGrid(var);
    const auto constants = var["CB"];
    constants["gPhotonPaths"] = mOptions.photonPaths;
    constants["gPhotonBounces"] = mOptions.photonBounces;
    constants["gPhotonSeed"] = mOptions.photonSeed;
    constants["gSceneCenter"] = bounds.center();
    constants["gSceneRadius"] = bounds.radius() * 1.001f;
    constants["gUseEmissiveLights"] = uint32_t(mpScene->useEmissiveLights());
    constants["gUseAnalyticLights"] = uint32_t(mpScene->useAnalyticLights());
    constants["gUseEnvLight"] = uint32_t(mpScene->useEnvLight());
    if (mpScene->useEnvLight())
    {
        if (!mpEnvMapSampler || mpEnvMapSampler->getEnvMap() != mpScene->getEnvMap())
            mpEnvMapSampler = std::make_unique<EnvMapSampler>(mpDevice, mpScene->getEnvMap());
        mpEnvMapSampler->bindShaderData(var["gEnvMapSampler"]);
    }
    mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"]);
    mpTracePhotons->execute(pRenderContext, mOptions.photonPaths, 1);
    mMapDirty = false;
}

void PhotonMapPathTracer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[kRenderPassRefreshFlags] = flags | RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }

    const auto pOutput = renderData.getTexture("color");
    if (!mpScene)
    {
        if (pOutput)
            pRenderContext->clearTexture(pOutput.get());
        return;
    }

    if (mpScene->getRenderSettings().useEmissiveLights)
        mpScene->getLightCollection(pRenderContext);

    const auto& bounds = mpScene->getSceneBounds();
    const auto extent = bounds.extent();
    const float radius = std::max({extent.x, extent.y, extent.z}) * mOptions.gatherRadius;
    const bool useEmissiveLights = mpScene->useEmissiveLights();
    const bool hasLightSources = useEmissiveLights || mpScene->useAnalyticLights() || mpScene->useEnvLight();
    const bool guiding = mOptions.enableGuiding && mOptions.guideMix > 0.f && mOptions.guidedBounces > 0 &&
        mOptions.maxBounces > 0 && hasLightSources && bounds.valid() && std::isfinite(radius) && radius > 0.f;

    if (guiding && mMapDirty)
        buildPhotonMap(pRenderContext, radius);

    auto& pCameraPass = mCameraPasses[guiding];
    if (!pCameraPass)
        pCameraPass = createPass(kCameraShader, {{"PHOTON_GUIDING", guiding ? "1" : "0"}});
    auto var = pCameraPass->getRootVar();
    const auto constants = var["CB"];

    if (guiding)
    {
        bindPhotonGrid(var);
        constants["gMinPhotons"] = mOptions.minPhotons;
        constants["gGuideMix"] = mOptions.guideMix;
        constants["gGuidedBounces"] = mOptions.guidedBounces;
    }

    const auto pView = renderData.getTexture("viewW");
    constants["gHasView"] = uint32_t(bool(pView));
    var["gVBuffer"] = renderData.getTexture("vbuffer");
    var["gViewW"] = pView;
    var["gOutputColor"] = pOutput;
    constants["gFrameIndex"] = mOptions.seed + mFrameCount;
    constants["gPRNGDimension"] = dict.getValue(kRenderPassPRNGDimension, 0u);
    constants["gMaxBounces"] = mOptions.maxBounces;
    constants["gUseAnalyticLights"] = uint32_t(mpScene->useAnalyticLights());
    constants["gUseEmissiveLights"] = uint32_t(useEmissiveLights);
    constants["gUseEnvLight"] = uint32_t(mpScene->useEnvLight());
    constants["gUseEnvBackground"] = uint32_t(mpScene->useEnvBackground());
    mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"]);

    const uint2 dim = renderData.getDefaultTextureDims();
    pCameraPass->execute(pRenderContext, dim.x, dim.y);
    ++mFrameCount;
}

void PhotonMapPathTracer::renderUI(Gui::Widgets& widget)
{
    auto options = mOptions;
    bool changed = widget.var("Max indirect bounces", options.maxBounces, 0u, kMaxBounces);
    changed |= widget.checkbox("Photon guiding", options.enableGuiding);
    if (options.enableGuiding)
    {
        changed |= widget.var("Guided bounces", options.guidedBounces, 0u, kMaxBounces);
        changed |= widget.var("Guide probability", options.guideMix, 0.f, kMaxGuideMix, 0.01f);
        changed |= widget.var("Photon paths", options.photonPaths, 1u, kMaxPhotonPaths, 1024u);
        changed |= widget.var("Photon bounces", options.photonBounces, 1u, kMaxPhotonBounces);
        changed |= widget.var("Relative gather radius", options.gatherRadius, kMinGatherRadius, kMaxGatherRadius, 0.001f);
        widget.tooltip("Fraction of the longest scene-bounds axis.");
        changed |= widget.var("Minimum gathered photons", options.minPhotons, 1u, kCellCapacity);
    }
    if (changed)
        applyOptions(options);
}
} // namespace Falcor
