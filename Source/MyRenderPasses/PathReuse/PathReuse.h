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
 * 两阶段路径追踪。第一阶段逐像素生成路径样本，第二阶段在像素块内复用路径后缀。
 */
class PathReuse final : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(PathReuse, "PathReuse", "Path tracer with tile-local path reuse.");

    enum class Grouping : uint32_t
    {
        Tiles,
        NRooks,
    };

    FALCOR_ENUM_INFO(Grouping, {{Grouping::Tiles, "Tiles"}, {Grouping::NRooks, "N-rooks"}});

    static ref<PathReuse> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<PathReuse>(pDevice, props);
    }

    PathReuse(ref<Device> pDevice, const Properties& props);

    Properties getProperties() const override;
    RenderPassReflection reflect(const CompileData& compileData) override;
    void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    void renderUI(Gui::Widgets& widget) override;
    void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;

private:
    void parseProperties(const Properties& props);
    void resetPrograms();
    void preparePrograms(const RenderData& renderData);
    void tracePaths(RenderContext* pRenderContext, const RenderData& renderData, uint2 frameDim);
    void reusePaths(RenderContext* pRenderContext, const ref<Texture>& pOutput, uint2 frameDim);

    ref<Scene> mpScene;
    sigs::Connection mSceneUpdateConnection;
    ref<SampleGenerator> mpSampleGenerator;

    // Configuration

    /// Max number of indirect bounces (0 = none).
    uint mMaxBounces = 3;
    /// Compute direct illumination (otherwise indirect only).
    bool mComputeDirect = true;
    /// Use importance sampling for materials.
    bool mUseImportanceSampling = true;
    Grouping mGrouping = Grouping::Tiles;

    uint2 mReflectedOutputDim = {};
    uint32_t mFrameIndex = 0;
    bool mOptionsChanged = false;
    bool mSceneProgramsDirty = false;

    ref<Program> mpTraceProgram;
    ref<RtProgramVars> mpTraceVars;
    ref<ComputePass> mpReusePass;
    ref<Buffer> mpPathSamples;
};

FALCOR_ENUM_REGISTER(PathReuse::Grouping);
