#include "PathReuse.h"
#include "PathReuseParams.slangh"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, PathReuse>();
}

namespace
{
const char kTracePathsShader[] = "MyRenderPasses/PathReuse/TracePaths.rt.slang";
const char kReusePathsShader[] = "MyRenderPasses/PathReuse/ReusePaths.cs.slang";

static_assert(kPathReuseGroupSize == 16u);
/* 散射载荷最多为一个 uint4，所有射线均从 raygen 发出。 */
constexpr uint32_t kMaxPayloadSizeBytes = 16u;
constexpr uint32_t kMaxRecursionDepth = 1u;

const char kInputVBuffer[] = "vbuffer";
const char kInputViewDir[] = "viewW";
const char kOutputColor[] = "color";

const ChannelList kInputChannels = {
    {kInputVBuffer, "gVBuffer", "Visibility buffer in packed format"},
    {kInputViewDir, "gViewW", "World-space view direction (xyz float format)", true},
};

const ChannelList kOutputChannels = {
    {kOutputColor, "gOutputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float},
};

const char kMaxBounces[] = "maxBounces";
const char kComputeDirect[] = "computeDirect";
const char kUseImportanceSampling[] = "useImportanceSampling";
const char kGrouping[] = "grouping";
} // namespace

PathReuse::PathReuse(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    FALCOR_CHECK(mpDevice->isShaderModelSupported(ShaderModel::SM6_5), "PathReuse requires Shader Model 6.5 support.");
    FALCOR_CHECK(
        mpDevice->isFeatureSupported(Device::SupportedFeatures::RaytracingTier1_1), "PathReuse requires Raytracing Tier 1.1 support."
    );

    parseProperties(props);
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

void PathReuse::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kMaxBounces)
            mMaxBounces = value;
        else if (key == kComputeDirect)
            mComputeDirect = value;
        else if (key == kUseImportanceSampling)
            mUseImportanceSampling = value;
        else if (key == kGrouping)
            mGrouping = value;
        else
            logWarning("Unknown property '{}' in PathReuse properties.", key);
    }
}

Properties PathReuse::getProperties() const
{
    Properties props;
    props[kMaxBounces] = mMaxBounces;
    props[kComputeDirect] = mComputeDirect;
    props[kUseImportanceSampling] = mUseImportanceSampling;
    props[kGrouping] = mGrouping;
    return props;
}

RenderPassReflection PathReuse::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);

    if (const auto* pVBuffer = compileData.connectedResources.getField(kInputVBuffer))
        mReflectedOutputDim = uint2(pVBuffer->getWidth(), pVBuffer->getHeight());
    addRenderPassOutputs(reflector, kOutputChannels, ResourceBindFlags::UnorderedAccess, mReflectedOutputDim);
    return reflector;
}

void PathReuse::compile(RenderContext*, const CompileData& compileData)
{
    // 第一次反射没有连接信息。尺寸变化时让 Falcor 带上连接信息重新反射。
    if (const auto* pVBuffer = compileData.connectedResources.getField(kInputVBuffer))
    {
        const uint2 inputDim(pVBuffer->getWidth(), pVBuffer->getHeight());
        FALCOR_CHECK(all(mReflectedOutputDim == inputDim), "PathReuse output sizing requires connected V-buffer reflection.");
    }
}

void PathReuse::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto& dictionary = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dictionary.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dictionary[kRenderPassRefreshFlags] = flags | RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }

    const auto pOutput = renderData.getTexture(kOutputColor);
    if (!pOutput)
        return;
    if (!mpScene)
    {
        pRenderContext->clearTexture(pOutput.get());
        return;
    }

    const uint2 frameDim(pOutput->getWidth(), pOutput->getHeight());
    const auto pVBuffer = renderData.getTexture(kInputVBuffer);
    FALCOR_CHECK(
        pVBuffer && all(uint2(pVBuffer->getWidth(), pVBuffer->getHeight()) == frameDim),
        "PathReuse requires the V-buffer and output to have matching dimensions."
    );
    const auto pViewW = renderData.getTexture(kInputViewDir);
    if (pViewW)
    {
        FALCOR_CHECK(
            all(uint2(pViewW->getWidth(), pViewW->getHeight()) == frameDim),
            "PathReuse requires the viewW input and V-buffer to have matching dimensions."
        );
    }
    else if (mpScene->getCamera()->getApertureRadius() > 0.f)
    {
        logWarning("Depth-of-field requires the '{}' input. Expect incorrect shading.", kInputViewDir);
    }

    if (mpScene->getRenderSettings().useEmissiveLights)
        mpScene->getLightCollection(pRenderContext);

    if (mSceneProgramsDirty)
        resetPrograms();
    preparePrograms(renderData);

    const uint32_t pixelCount = frameDim.x * frameDim.y;
    if (!mpPathSamples || mpPathSamples->getElementCount() != pixelCount)
    {
        mpPathSamples = mpDevice->createStructuredBuffer(
            mpTraceVars->getRootVar()["gPathSamples"], pixelCount, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
    }

    tracePaths(pRenderContext, renderData, frameDim);
    reusePaths(pRenderContext, pOutput, frameDim);
    ++mFrameIndex;
}

void PathReuse::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.var("Max bounces", mMaxBounces, 0u, 1u << 16);
    widget.tooltip("Maximum path length for indirect illumination.\n0 = direct only\n1 = one indirect bounce etc.", true);

    dirty |= widget.checkbox("Evaluate direct illumination", mComputeDirect);
    widget.tooltip("Compute direct illumination.\nIf disabled only indirect is computed (when max bounces > 0).", true);

    dirty |= widget.checkbox("Use importance sampling", mUseImportanceSampling);
    widget.tooltip("Use importance sampling for materials", true);

    dirty |= widget.dropdown("Grouping", mGrouping);
    widget.tooltip("Tiles groups 4x4 adjacent pixels. N-rooks takes one pixel per row and column in each 16x16 tile.", true);

    mOptionsChanged |= dirty;
}

void PathReuse::setScene(RenderContext*, const ref<Scene>& pScene)
{
    mSceneUpdateConnection = {};
    mpScene = pScene;
    mFrameIndex = 0;
    resetPrograms();

    if (!mpScene)
        return;

    if (mpScene->hasGeometryType(Scene::GeometryType::Custom))
        logWarning("PathReuse: This render pass does not support custom primitives.");

    mSceneUpdateConnection = mpScene->getUpdateFlagsSignal().connect([this](IScene::UpdateFlags flags)
    {
        const auto rebuildFlags =
            IScene::UpdateFlags::RecompileNeeded | IScene::UpdateFlags::GeometryChanged | IScene::UpdateFlags::SDFGridConfigChanged;
        if (is_set(flags, rebuildFlags))
            mSceneProgramsDirty = true;
    });
}

void PathReuse::resetPrograms()
{
    mpTraceVars = nullptr;
    mpTraceProgram = nullptr;
    mpReusePass = nullptr;
    mpPathSamples = nullptr;
    mSceneProgramsDirty = false;
}

void PathReuse::preparePrograms(const RenderData& renderData)
{
    const std::string importanceSampling = mUseImportanceSampling ? "1" : "0";
    DefineList traceDefines = getValidResourceDefines(kInputChannels, renderData);
    traceDefines.add("MAX_BOUNCES", std::to_string(mMaxBounces));
    traceDefines.add("COMPUTE_DIRECT", mComputeDirect ? "1" : "0");
    traceDefines.add("USE_IMPORTANCE_SAMPLING", importanceSampling);
    traceDefines.add("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    traceDefines.add("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    traceDefines.add("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    traceDefines.add("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");

    if (mpTraceProgram)
    {
        mpTraceProgram->addDefines(traceDefines);
        mpReusePass->addDefine("USE_IMPORTANCE_SAMPLING", importanceSampling);
        return;
    }

    DefineList sceneDefines = mpScene->getSceneDefines();
    sceneDefines.add(mpSampleGenerator->getDefines());
    const auto shaderModules = mpScene->getShaderModules();
    const auto typeConformances = mpScene->getTypeConformances();

    ProgramDesc desc;
    desc.addShaderModules(shaderModules);
    desc.addShaderLibrary(kTracePathsShader);
    desc.addTypeConformances(typeConformances);
    desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
    desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

    auto sbt = RtBindingTable::create(kPathReuseRayTypeCount, kPathReuseRayTypeCount, mpScene->getGeometryCount());
    sbt->setRayGen(desc.addRayGen("rayGen"));
    sbt->setMiss(kPathReuseScatterRay, desc.addMiss("scatterMiss"));
    sbt->setMiss(kPathReuseShadowRay, desc.addMiss("shadowMiss"));

    if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
    {
        sbt->setHitGroup(
            kPathReuseScatterRay,
            mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh),
            desc.addHitGroup("scatterTriangleMeshClosestHit", "scatterTriangleMeshAnyHit")
        );
        sbt->setHitGroup(
            kPathReuseShadowRay, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowTriangleMeshAnyHit")
        );
    }

    if (mpScene->hasGeometryType(Scene::GeometryType::DisplacedTriangleMesh))
    {
        sbt->setHitGroup(
            kPathReuseScatterRay,
            mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
            desc.addHitGroup("scatterDisplacedTriangleMeshClosestHit", "", "displacedTriangleMeshIntersection")
        );
        sbt->setHitGroup(
            kPathReuseShadowRay,
            mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
            desc.addHitGroup("", "", "displacedTriangleMeshIntersection")
        );
    }

    if (mpScene->hasGeometryType(Scene::GeometryType::Curve))
    {
        sbt->setHitGroup(
            kPathReuseScatterRay, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("scatterCurveClosestHit", "", "curveIntersection")
        );
        sbt->setHitGroup(kPathReuseShadowRay, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("", "", "curveIntersection"));
    }

    if (mpScene->hasGeometryType(Scene::GeometryType::SDFGrid))
    {
        sbt->setHitGroup(
            kPathReuseScatterRay,
            mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid),
            desc.addHitGroup("scatterSdfGridClosestHit", "", "sdfGridIntersection")
        );
        sbt->setHitGroup(kPathReuseShadowRay, mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid), desc.addHitGroup("", "", "sdfGridIntersection"));
    }

    DefineList traceProgramDefines = sceneDefines;
    traceProgramDefines.add(traceDefines);
    auto pTraceProgram = Program::create(mpDevice, desc, traceProgramDefines);
    auto pTraceVars = RtProgramVars::create(mpDevice, pTraceProgram, sbt);
    mpSampleGenerator->bindShaderData(pTraceVars->getRootVar());

    ProgramDesc reuseDesc;
    reuseDesc.addShaderModules(shaderModules);
    reuseDesc.addShaderLibrary(kReusePathsShader).csEntry("main");
    reuseDesc.addTypeConformances(typeConformances);
    sceneDefines.add("USE_IMPORTANCE_SAMPLING", importanceSampling);
    auto pReusePass = ComputePass::create(mpDevice, reuseDesc, sceneDefines);
    mpSampleGenerator->bindShaderData(pReusePass->getRootVar());

    mpTraceProgram = std::move(pTraceProgram);
    mpTraceVars = std::move(pTraceVars);
    mpReusePass = std::move(pReusePass);
}

void PathReuse::tracePaths(RenderContext* pRenderContext, const RenderData& renderData, uint2 frameDim)
{
    const auto var = mpTraceVars->getRootVar();
    const TracePathsParams params{mFrameIndex, renderData.getDictionary().getValue(kRenderPassPRNGDimension, 0u)};
    var["gTraceParams"].setBlob(params);
    for (const auto& channel : kInputChannels)
        var[channel.texname] = renderData.getTexture(channel.name);
    var["gPathSamples"] = mpPathSamples;

    mpScene->raytrace(pRenderContext, mpTraceProgram.get(), mpTraceVars, uint3(frameDim, 1));
}

void PathReuse::reusePaths(RenderContext* pRenderContext, const ref<Texture>& pOutput, uint2 frameDim)
{
    const bool useNRooks = mGrouping == Grouping::NRooks;
    const uint32_t tileSide = useNRooks ? kPathReuseGroupSize : kPathReuseTileSide;
    const uint32_t groupsPerTile = useNRooks ? kPathReuseGroupSize : 1u;
    const uint2 tileOffset = useNRooks ? uint2(mFrameIndex % tileSide) : uint2(0u);

    const auto var = mpReusePass->getRootVar();
    mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"]);
    var["gReuseParams"].setBlob(ReusePathsParams{mFrameIndex, uint32_t(useNRooks), tileOffset});
    var["gPathSamples"] = mpPathSamples;
    var["gOutputColor"] = pOutput;

    const uint2 tileCount = div_round_up(frameDim + tileOffset, uint2(tileSide));
    mpReusePass->execute(pRenderContext, uint3(tileCount, groupsPerTile) * mpReusePass->getThreadGroupSize());
}
