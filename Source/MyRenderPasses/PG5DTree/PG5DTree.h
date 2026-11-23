/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
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
#pragma once
#include "Falcor.h"
#include "Core/Pass/ComputePass.h"
#include "RenderGraph/RenderPass.h"
#include "Utils/Sampling/SampleGenerator.h"

using namespace Falcor;

/**
 * 使用体素方向直方图进行路径引导和控制变量估计的路径追踪器。
 */
class PG5DTree : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(PG5DTree, "PG5DTree", "Path tracing with voxel-based guiding and control variates.");

    static ref<PG5DTree> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<PG5DTree>(pDevice, props);
    }

    PG5DTree(ref<Device> pDevice, const Properties& props);

    virtual void setProperties(const Properties& props) override;
    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

private:
    void prepareVars();
    void bindRadianceCache(RenderContext* pRenderContext, const ShaderVar& var);
    void updateRadianceCache(RenderContext* pRenderContext);

    // Internal state

    /// Current scene.
    ref<Scene> mpScene;
    /// GPU sample generator.
    ref<SampleGenerator> mpSampleGenerator;

    struct Options
    {
        uint maxBounces = 6;
        bool computeDirect = true;
        bool useImportanceSampling = true;
        bool enableGuiding = false;
        bool enableControlVariate = false;
        float guideMix = 0.5f;
        float cvStrength = 0.5f;
        float minRoughness = 0.25f;
        uint gridResolution = 16;
        uint2 directionResolution = uint2(8, 4);
        uint warmupFrames = 8;
        uint updateInterval = 4;
        uint minCellSamples = 32;
        uint seed = 0;
    } mOptions;

    // Runtime data

    /// Frame count since scene was loaded.
    uint mFrameCount = 0;
    bool mOptionsChanged = false;
    bool mUseCacheVariant = false;
    uint64_t mTrainingFrames = 0;
    ref<Buffer> mpTrainingBins;
    ref<Buffer> mpTrainingCounts;
    ref<Buffer> mpCacheBins;
    ref<Buffer> mpCacheCells;
    ref<ComputePass> mpUpdatePass;

    // Ray tracing program.
    struct
    {
        ref<Program> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;
    } mTracer;
};
