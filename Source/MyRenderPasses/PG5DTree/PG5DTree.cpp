/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "PG5DTree.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"
#include <algorithm>

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, PG5DTree>();
}

namespace
{
const char kShaderFile[] = "MyRenderPasses/PG5DTree/PG5DTree.rt.slang";
const char kUpdateShaderFile[] = "MyRenderPasses/PG5DTree/UpdateRadianceCache.cs.slang";
const uint32_t kMaxGridResolution = 32;
const uint32_t kMaxDirectionBins = 64;

/* 缓存分支还记录首段直接命中的发光项及其拆分标志。 */
const uint32_t kMaxPayloadSizeBytes = 104u;
const uint32_t kMaxRecursionDepth = 2u;

const char kInputViewDir[] = "viewW";

const ChannelList kInputChannels = {
    // clang-format off
    { "vbuffer",        "gVBuffer",     "Visibility buffer in packed format" },
    { kInputViewDir,    "gViewW",       "World-space view direction (xyz float format)", true /* optional */ },
    // clang-format on
};

const ChannelList kOutputChannels = {
    // clang-format off
    { "color",          "gOutputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float },
    // clang-format on
};

const char kMaxBounces[] = "maxBounces";
const char kComputeDirect[] = "computeDirect";
const char kUseImportanceSampling[] = "useImportanceSampling";
const char kEnableGuiding[] = "enableGuiding";
const char kEnableControlVariate[] = "enableControlVariate";
const char kGuideMix[] = "guideMix";
const char kCvStrength[] = "cvStrength";
const char kMinRoughness[] = "minRoughness";
const char kGridResolution[] = "gridResolution";
const char kDirectionResolution[] = "directionResolution";
const char kWarmupFrames[] = "warmupFrames";
const char kUpdateInterval[] = "updateInterval";
const char kMinCellSamples[] = "minCellSamples";
const char kSeed[] = "seed";

using SceneUpdate = IScene::UpdateFlags;
const auto kCacheResetFlags =
    SceneUpdate::GeometryMoved | SceneUpdate::MeshesChanged | SceneUpdate::DisplacementChanged | SceneUpdate::CurvesMoved |
    SceneUpdate::SDFGeometryChanged | SceneUpdate::CustomPrimitivesMoved | SceneUpdate::MaterialsChanged |
    SceneUpdate::EmissiveMaterialsChanged | SceneUpdate::LightsMoved | SceneUpdate::LightIntensityChanged |
    SceneUpdate::LightPropertiesChanged | SceneUpdate::LightCollectionChanged | SceneUpdate::LightCountChanged |
    SceneUpdate::EnvMapChanged | SceneUpdate::EnvMapPropertiesChanged | SceneUpdate::RenderSettingsChanged;
} // namespace

PG5DTree::PG5DTree(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    setProperties(props);

    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

void PG5DTree::setProperties(const Properties& props)
{
    Options options = mOptions;
    for (const auto& [key, value] : props)
    {
        if (key == kMaxBounces)
            options.maxBounces = value;
        else if (key == kComputeDirect)
            options.computeDirect = value;
        else if (key == kUseImportanceSampling)
            options.useImportanceSampling = value;
        else if (key == kEnableGuiding)
            options.enableGuiding = value;
        else if (key == kEnableControlVariate)
            options.enableControlVariate = value;
        else if (key == kGuideMix)
            options.guideMix = value;
        else if (key == kCvStrength)
            options.cvStrength = value;
        else if (key == kMinRoughness)
            options.minRoughness = value;
        else if (key == kGridResolution)
            options.gridResolution = value;
        else if (key == kDirectionResolution)
            options.directionResolution = value;
        else if (key == kWarmupFrames)
            options.warmupFrames = value;
        else if (key == kUpdateInterval)
            options.updateInterval = value;
        else if (key == kMinCellSamples)
            options.minCellSamples = value;
        else if (key == kSeed)
            options.seed = value;
        else
            logWarning("Unknown property '{}' in PG5DTree properties.", key);
    }

    FALCOR_CHECK(options.guideMix >= 0.f && options.guideMix <= 0.9f, "PG5DTree guideMix must be in [0, 0.9].");
    FALCOR_CHECK(options.cvStrength >= 0.f && options.cvStrength <= 1.f, "PG5DTree cvStrength must be in [0, 1].");
    FALCOR_CHECK(options.minRoughness >= 0.f && options.minRoughness <= 1.f, "PG5DTree minRoughness must be in [0, 1].");
    FALCOR_CHECK(options.gridResolution > 0 && options.gridResolution <= kMaxGridResolution, "PG5DTree gridResolution must be in [1, 32].");
    FALCOR_CHECK(
        options.directionResolution.x > 0 && options.directionResolution.y > 0 &&
            options.directionResolution.x <= kMaxDirectionBins / options.directionResolution.y,
        "PG5DTree directionResolution must contain two positive dimensions with at most 64 bins."
    );
    FALCOR_CHECK(options.updateInterval > 0 && options.minCellSamples > 0, "PG5DTree updateInterval and minCellSamples must be positive.");

    mOptions = options;
    if (props.has(kSeed))
        mFrameCount = 0;
    mOptionsChanged = true;
}

Properties PG5DTree::getProperties() const
{
    Properties props;
    props[kMaxBounces] = mOptions.maxBounces;
    props[kComputeDirect] = mOptions.computeDirect;
    props[kUseImportanceSampling] = mOptions.useImportanceSampling;
    props[kEnableGuiding] = mOptions.enableGuiding;
    props[kEnableControlVariate] = mOptions.enableControlVariate;
    props[kGuideMix] = mOptions.guideMix;
    props[kCvStrength] = mOptions.cvStrength;
    props[kMinRoughness] = mOptions.minRoughness;
    props[kGridResolution] = mOptions.gridResolution;
    props[kDirectionResolution] = mOptions.directionResolution;
    props[kWarmupFrames] = mOptions.warmupFrames;
    props[kUpdateInterval] = mOptions.updateInterval;
    props[kMinCellSamples] = mOptions.minCellSamples;
    props[kSeed] = mOptions.seed;
    return props;
}

RenderPassReflection PG5DTree::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);

    return reflector;
}

void PG5DTree::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
        mTrainingFrames = 0;
    }

    // If we have no scene, just clear the outputs and return.
    if (!mpScene)
    {
        for (auto it : kOutputChannels)
        {
            Texture* pDst = renderData.getTexture(it.name).get();
            if (pDst)
                pRenderContext->clearTexture(pDst);
        }
        return;
    }

    if (is_set(mpScene->getUpdates(), IScene::UpdateFlags::RecompileNeeded) ||
        is_set(mpScene->getUpdates(), IScene::UpdateFlags::GeometryChanged))
    {
        FALCOR_THROW("This render pass does not support scene changes that require shader recompilation.");
    }

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    if ((mpScene->getUpdates() & kCacheResetFlags) != SceneUpdate::None)
        mTrainingFrames = 0;

    const bool useCache = mOptions.maxBounces > 0 && mpScene->getSceneBounds().valid() &&
        ((mOptions.enableGuiding && mOptions.guideMix > 0.f) || (mOptions.enableControlVariate && mOptions.cvStrength > 0.f));
    if (mUseCacheVariant != useCache)
    {
        mTracer.pVars = nullptr;
        mUseCacheVariant = useCache;
        mTrainingFrames = 0;
    }
    mTracer.pProgram->addDefine("USE_RADIANCE_CACHE", useCache ? "1" : "0");

    // Configure depth-of-field.
    const bool useDOF = mpScene->getCamera()->getApertureRadius() > 0.f;
    if (useDOF && renderData[kInputViewDir] == nullptr)
    {
        logWarning("Depth-of-field requires the '{}' input. Expect incorrect shading.", kInputViewDir);
    }

    // Specialize program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracer.pProgram->addDefine("MAX_BOUNCES", std::to_string(mOptions.maxBounces));
    mTracer.pProgram->addDefine("COMPUTE_DIRECT", mOptions.computeDirect ? "1" : "0");
    mTracer.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", mOptions.useImportanceSampling ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // TODO: This should be moved to a more general mechanism using Slang.
    mTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necessary defines set at this point.
    if (!mTracer.pVars)
        prepareVars();
    FALCOR_ASSERT(mTracer.pVars);

    // Set constants.
    auto var = mTracer.pVars->getRootVar();
    var["CB"]["gFrameCount"] = mFrameCount + mOptions.seed;
    var["CB"]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;
    if (useCache)
        bindRadianceCache(pRenderContext, var);

    // Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData.getTexture(desc.name);
        }
    };
    for (auto channel : kInputChannels)
        bind(channel);
    for (auto channel : kOutputChannels)
        bind(channel);

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));
    if (useCache)
        updateRadianceCache(pRenderContext);

    mFrameCount++;
}

void PG5DTree::bindRadianceCache(RenderContext* pRenderContext, const ShaderVar& var)
{
    const uint cells = mOptions.gridResolution * mOptions.gridResolution * mOptions.gridResolution;
    const uint bins = cells * mOptions.directionResolution.x * mOptions.directionResolution.y;
    if (!mpCacheBins || mpCacheBins->getElementCount() != bins || mpCacheCells->getElementCount() != cells)
    {
        mpTrainingBins = mpDevice->createBuffer(size_t(bins) * 4 * sizeof(uint32_t));
        mpTrainingCounts = mpDevice->createTypedBuffer<uint32_t>(cells);
        mpCacheBins = mpDevice->createStructuredBuffer(var["gCacheBins"], bins);
        mpCacheCells = mpDevice->createStructuredBuffer(var["gCacheCells"], cells);
        mTrainingFrames = 0;
    }
    if (mTrainingFrames == 0)
    {
        pRenderContext->clearUAV(mpTrainingBins->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mpTrainingCounts->getUAV().get(), uint4(0));
    }

    const auto& bounds = mpScene->getSceneBounds();
    const float3 extent = bounds.extent();
    const float3 invExtent(
        extent.x > 0.f ? 1.f / extent.x : 0.f, extent.y > 0.f ? 1.f / extent.y : 0.f, extent.z > 0.f ? 1.f / extent.z : 0.f
    );
    const auto constants = var["GuideCB"];
    constants["gGridMin"] = bounds.minPoint;
    constants["gGridInvExtent"] = invExtent;
    constants["gGridResolution"] = mOptions.gridResolution;
    constants["gDirectionResolution"] = mOptions.directionResolution;
    constants["gGuideMix"] = mOptions.enableGuiding ? mOptions.guideMix : 0.f;
    constants["gCvStrength"] = mOptions.enableControlVariate ? mOptions.cvStrength : 0.f;
    constants["gMinRoughness"] = mOptions.minRoughness;
    constants["gCacheReady"] = uint(mTrainingFrames >= std::max(1u, mOptions.warmupFrames));
    var["gTrainingBins"] = mpTrainingBins;
    var["gTrainingCounts"] = mpTrainingCounts;
    var["gCacheBins"] = mpCacheBins;
    var["gCacheCells"] = mpCacheCells;
}

void PG5DTree::updateRadianceCache(RenderContext* pRenderContext)
{
    const uint firstUpdate = std::max(1u, mOptions.warmupFrames);
    if (++mTrainingFrames < firstUpdate || (mTrainingFrames - firstUpdate) % mOptions.updateInterval != 0)
        return;

    FALCOR_PROFILE(pRenderContext, "UpdateRadianceCache");
    if (!mpUpdatePass)
        mpUpdatePass = ComputePass::create(mpDevice, kUpdateShaderFile);

    const uint cells = mpCacheCells->getElementCount();
    const auto var = mpUpdatePass->getRootVar();
    var["UpdateCB"]["gCellCount"] = cells;
    var["UpdateCB"]["gBinCount"] = mOptions.directionResolution.x * mOptions.directionResolution.y;
    var["UpdateCB"]["gMinCellSamples"] = mOptions.minCellSamples;
    var["gTrainingBins"] = mpTrainingBins;
    var["gTrainingCounts"] = mpTrainingCounts;
    var["gCacheBins"] = mpCacheBins;
    var["gCacheCells"] = mpCacheCells;
    mpUpdatePass->execute(pRenderContext, cells, 1);
}

void PG5DTree::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.var("Max bounces", mOptions.maxBounces, 0u, 1u << 16);
    widget.tooltip("Maximum path length for indirect illumination.\n0 = direct only\n1 = one indirect bounce etc.", true);

    dirty |= widget.checkbox("Evaluate direct illumination", mOptions.computeDirect);
    widget.tooltip("Compute direct illumination.\nIf disabled only indirect is computed (when max bounces > 0).", true);

    dirty |= widget.checkbox("Use importance sampling", mOptions.useImportanceSampling);
    widget.tooltip("Use importance sampling for materials", true);

    dirty |= widget.checkbox("Enable guiding", mOptions.enableGuiding);
    dirty |= widget.checkbox("Enable control variate", mOptions.enableControlVariate);
    dirty |= widget.var("Guide mixture probability", mOptions.guideMix, 0.f, 0.9f, 0.01f);
    dirty |= widget.var("Control variate strength", mOptions.cvStrength, 0.f, 1.f, 0.01f);
    dirty |= widget.var("Minimum roughness", mOptions.minRoughness, 0.f, 1.f, 0.01f);
    dirty |= widget.var("Grid resolution", mOptions.gridResolution, 1u, kMaxGridResolution);
    dirty |= widget.var("Azimuth bins", mOptions.directionResolution.x, 1u, kMaxDirectionBins / mOptions.directionResolution.y);
    dirty |= widget.var("Cosine bins", mOptions.directionResolution.y, 1u, kMaxDirectionBins / mOptions.directionResolution.x);
    dirty |= widget.var("Warmup frames", mOptions.warmupFrames, 0u, 65535u);
    dirty |= widget.var("Cache update interval", mOptions.updateInterval, 1u, 65535u);
    dirty |= widget.var("Minimum cell samples", mOptions.minCellSamples, 1u, 1u << 24);
    if (widget.var("Seed", mOptions.seed))
    {
        mFrameCount = 0;
        dirty = true;
    }

    // If rendering options that modify the output have changed, set flag to indicate that.
    // In execute() we will pass the flag to other passes for reset of temporal data etc.
    if (dirty)
    {
        mOptionsChanged = true;
    }
}

void PG5DTree::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Clear data for previous scene.
    // After changing scene, the raytracing program should to be recreated.
    mTracer.pProgram = nullptr;
    mTracer.pBindingTable = nullptr;
    mTracer.pVars = nullptr;
    mFrameCount = 0;
    mTrainingFrames = 0;

    // Set new scene.
    mpScene = pScene;

    if (mpScene)
    {
        if (pScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("PG5DTree: This render pass does not support custom primitives.");
        }

        // Create ray tracing program.
        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderFile);
        desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

        mTracer.pBindingTable = RtBindingTable::create(2, 2, mpScene->getGeometryCount());
        auto& sbt = mTracer.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("scatterMiss"));
        sbt->setMiss(1, desc.addMiss("shadowMiss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(
                0,
                mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh),
                desc.addHitGroup("scatterTriangleMeshClosestHit", "scatterTriangleMeshAnyHit")
            );
            sbt->setHitGroup(
                1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowTriangleMeshAnyHit")
            );
        }

        if (mpScene->hasGeometryType(Scene::GeometryType::DisplacedTriangleMesh))
        {
            sbt->setHitGroup(
                0,
                mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
                desc.addHitGroup("scatterDisplacedTriangleMeshClosestHit", "", "displacedTriangleMeshIntersection")
            );
            sbt->setHitGroup(
                1,
                mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
                desc.addHitGroup("", "", "displacedTriangleMeshIntersection")
            );
        }

        if (mpScene->hasGeometryType(Scene::GeometryType::Curve))
        {
            sbt->setHitGroup(
                0, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("scatterCurveClosestHit", "", "curveIntersection")
            );
            sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("", "", "curveIntersection"));
        }

        if (mpScene->hasGeometryType(Scene::GeometryType::SDFGrid))
        {
            sbt->setHitGroup(
                0,
                mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid),
                desc.addHitGroup("scatterSdfGridClosestHit", "", "sdfGridIntersection")
            );
            sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid), desc.addHitGroup("", "", "sdfGridIntersection"));
        }

        mTracer.pProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
    }
}

void PG5DTree::prepareVars()
{
    FALCOR_ASSERT(mpScene);
    FALCOR_ASSERT(mTracer.pProgram);

    // Configure program.
    mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());

    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    mTracer.pVars = RtProgramVars::create(mpDevice, mTracer.pProgram, mTracer.pBindingTable);

    // Bind utility classes into shared data.
    auto var = mTracer.pVars->getRootVar();
    mpSampleGenerator->bindShaderData(var);
}
