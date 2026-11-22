#pragma once

#include "RemoteStream.h"
#include "RenderGraph/RenderGraph.h"
#include "Scene/SceneBuilder.h"
#include "Utils/Timing/Clock.h"
#include <bitset>
#include <filesystem>
#include <optional>

namespace Falcor::Remote
{
inline constexpr uint32_t kPreviewSide = 1080;

class RemoteRenderer
{
public:
    using SceneCallback = std::function<void(const ref<Scene>&, double)>;
    using KeyCallback = std::function<bool(bool, uint32_t)>;

    RemoteRenderer(ref<Device> device, uint16_t port, uint32_t fps);
    ~RemoteRenderer();
    void run();

    void loadScript(const std::filesystem::path& path);
    void saveConfig(const std::filesystem::path& path) const;
    void loadScene(const std::filesystem::path& path, SceneBuilder::Flags flags = SceneBuilder::Flags::Default);
    void unloadScene();
    void addGraph(const ref<RenderGraph>& graph);
    void removeGraph(const std::string& name);
    void setActiveGraph(const ref<RenderGraph>& graph);
    ref<RenderGraph> getGraph(const std::string& name) const;
    ref<RenderGraph> activeGraph() const;
    const ref<Scene>& scene() const { return mpScene; }
    Clock& clock() { return mClock; }
    void resizeFrameBuffer(uint32_t width, uint32_t height);
    void initializeScripting();

private:
    struct Graph
    {
        ref<RenderGraph> graph;
        std::string output;
        std::string extraOutput;
        IScene::UpdateFlags sceneUpdates = IScene::UpdateFlags::None;
    };
    struct Capture
    {
        std::filesystem::path file;
        std::string output;
        uint32_t count;
        uint32_t written = 0;
    };

    void setScene(ref<Scene> scene);
    Graph& graphByName(const std::string& name);
    Graph* currentGraph();
    void selectOutput(Graph& graph, const std::string& name);
    void renderFrame();
    void drawScene(RenderContext& context);
    void onInput(const InputEvent& input);
    void keyEvent(const KeyboardEvent& event);
    void releaseInput();
    nlohmann::json command(const nlohmann::json& request);
    nlohmann::json state() const;
    nlohmann::json stats() const;
    void applyGraph(const nlohmann::json& description, const std::string& selectedOutput);
    void updateScene(const nlohmann::json& changes);
    void captureOutput(const std::filesystem::path& path, const std::string& output);
    void captureFrameRange();
    void rememberPath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path);

    ref<Device> mpDevice;
    ref<Fbo> mpPreview;
    ref<Fbo> mpGraphTarget;
    RemoteStream mStream;
    std::vector<Graph> mGraphs;
    size_t mActiveGraph = 0;
    ref<Scene> mpScene;
    ref<Scene> mpLookScene;
    Clock mClock;
    SceneCallback mSceneCallback;
    KeyCallback mKeyCallback;
    std::bitset<size_t(Input::Key::Count)> mKeys;
    std::bitset<size_t(Input::MouseButton::Count)> mButtons;
    float2 mPointer{0.f};
    bool mLookNavigation = false;
    bool mRenderPaused = false;
    bool mOverlay = false;
    bool mFrameValid = false;
    bool mStop = false;
    uint64_t mFrames = 0;
    uint64_t mSampledFrame = 0;
    std::chrono::steady_clock::time_point mFpsStart{};
    double mRenderFps = 0;
    double mSubmitMs = 0;
    std::string mRenderError;
    std::optional<Capture> mCapture;
    uint32_t mCapturedFrames = 0;
    std::string mCaptureError;
    std::vector<std::filesystem::path> mRecentScripts;
    std::vector<std::filesystem::path> mRecentScenes;
};
} // namespace Falcor::Remote
