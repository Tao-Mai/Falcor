#include "RemoteRenderer.h"
#include "Core/AssetResolver.h"
#include "Core/Window.h"
#include "GlobalState.h"
#include "RenderGraph/RenderGraphImportExport.h"
#include "RenderGraph/RenderPassStandardFlags.h"
#include "Scene/Material/StandardMaterial.h"
#include "Utils/Color/ColorHelpers.slang"
#include "Utils/Settings/Settings.h"
#include "Utils/Threading.h"
#include "Utils/Scripting/ScriptBindings.h"
#include "Utils/Scripting/Scripting.h"
#include "Utils/Scripting/ScriptWriter.h"
#include "Utils/Timing/TimeReport.h"
#include <args.hxx>
#include <nlohmann/json.hpp>
#include <pybind11/functional.h>
#include <pybind11/stl/filesystem.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <fstream>
#include <thread>
#include <utility>

namespace Falcor::Remote
{
namespace
{
constexpr char kDefaultScene[] = "media/test_scenes/cornell_box.pyscene";
volatile std::sig_atomic_t gStop = 0;
void stopSignal(int) { gStop = 1; }

void validateSize(uint32_t width, uint32_t height)
{
    FALCOR_CHECK(width >= 128 && height >= 128 && width <= 4096 && height <= 4096,
                 "Render dimensions must be between 128 and 4096.");
}

uint4 previewRect(uint2 size)
{
    const auto fit = max(uint2(1), size * kPreviewSide / std::max(size.x, size.y));
    const auto origin = (uint2(kPreviewSide) - fit) / 2u;
    return uint4(origin.x, origin.y, origin.x + fit.x, origin.y + fit.y);
}

bool isMovementKey(Input::Key key)
{
    switch (key)
    {
    case Input::Key::W:
    case Input::Key::A:
    case Input::Key::S:
    case Input::Key::D:
    case Input::Key::Q:
    case Input::Key::E:
        return true;
    default:
        return false;
    }
}
} // namespace

RemoteRenderer::RemoteRenderer(ref<Device> device, uint16_t port, uint32_t fps)
    : mpDevice(std::move(device)), mStream(mpDevice, port, fps)
{
    mpPreview = Fbo::create2D(mpDevice, kPreviewSide, kPreviewSide, ResourceFormat::BGRA8UnormSrgb, ResourceFormat::Unknown);
    mpGraphTarget = mpPreview;
}

RemoteRenderer::~RemoteRenderer()
{
    mpDevice->wait();
    mSceneCallback = {};
    mKeyCallback = {};
    setActivePythonRenderGraphDevice(nullptr);
    Scripting::shutdown();
}

void RemoteRenderer::initializeScripting()
{
    using namespace pybind11::literals;
    ScriptBindings::registerBinding([](pybind11::module& module) {
        pybind11::class_<RemoteRenderer> renderer(module, "RemoteRenderer");
        renderer.def("addGraph", &RemoteRenderer::addGraph);
        renderer.def("removeGraph", &RemoteRenderer::removeGraph);
        renderer.def("getGraph", &RemoteRenderer::getGraph);
        renderer.def("setActiveGraph", &RemoteRenderer::setActiveGraph);
        renderer.def_property_readonly("activeGraph", &RemoteRenderer::activeGraph);
        renderer.def_property_readonly("scene", &RemoteRenderer::scene);
        renderer.def_property_readonly("clock", &RemoteRenderer::clock, pybind11::return_value_policy::reference);
        renderer.def_property_readonly("profiler", [](RemoteRenderer& app) { return app.mpDevice->getProfiler(); },
                                       pybind11::return_value_policy::reference);
        renderer.def("loadScene", &RemoteRenderer::loadScene, "path"_a, "buildFlags"_a = SceneBuilder::Flags::Default);
        renderer.def("unloadScene", &RemoteRenderer::unloadScene);
        renderer.def("script", &RemoteRenderer::loadScript);
        renderer.def("saveConfig", &RemoteRenderer::saveConfig);
        renderer.def("resizeFrameBuffer", &RemoteRenderer::resizeFrameBuffer);
        renderer.def_property("sceneUpdateCallback", [](RemoteRenderer& app) { return app.mSceneCallback; },
                              [](RemoteRenderer& app, SceneCallback callback) { app.mSceneCallback = std::move(callback); });
        renderer.def_property("keyCallback", [](RemoteRenderer& app) { return app.mKeyCallback; },
                              [](RemoteRenderer& app, KeyCallback callback) { app.mKeyCallback = std::move(callback); });
        renderer.def("exit", [](RemoteRenderer& app) { app.mStop = true; });
    });
    setActivePythonRenderGraphDevice(mpDevice);
    Scripting::start();
    auto app = pybind11::cast(this, pybind11::return_value_policy::reference);
    Scripting::getDefaultContext().setObject("renderer", app);
    // Existing Falcor graph and scene scripts use this global variable.
    Scripting::getDefaultContext().setObject("m", app);
}

void RemoteRenderer::loadScript(const std::filesystem::path& path)
{
    auto& resolver = AssetResolver::getDefaultResolver();
    const AssetResolver oldResolver = resolver;
    resolver.addSearchPath(std::filesystem::absolute(path).parent_path(), SearchPathPriority::First);
    try
    {
        Scripting::runScriptFromFile(path);
    }
    catch (...)
    {
        resolver = oldResolver;
        throw;
    }
    resolver = oldResolver;
    rememberPath(mRecentScripts, path);
}

void RemoteRenderer::loadScene(const std::filesystem::path& path, SceneBuilder::Flags flags)
{
    TimeReport report;
    auto scene = SceneBuilder(mpDevice, path, Settings::getGlobalSettings(), flags).getScene();
    std::error_code pathError;
    if (std::filesystem::equivalent(scene->getPath(), getProjectDirectory() / kDefaultScene, pathError))
    {
        // Frame the built-in Cornell box edge to edge in the square preview.
        scene->getCamera()->setFocalLength(42.f);
        for (const auto& material : scene->getMaterials())
        {
            const auto standard = dynamic_ref_cast<StandardMaterial>(material);
            if (!standard)
                continue;
            const auto& name = standard->getName();
            if (name == "Light")
                standard->setEmissiveColor(float3(luminance(standard->getEmissiveColor())));
            else if (name != "Left Wall" && name != "Right Wall")
                standard->setBaseColor3(float3(luminance(standard->getBaseColor3())));
        }
    }
    setScene(std::move(scene));
    report.measure("Loading scene (total)");
    report.printToLog();
    rememberPath(mRecentScenes, path);
}

void RemoteRenderer::unloadScene()
{
    setScene(nullptr);
}

void RemoteRenderer::setScene(ref<Scene> scene)
{
    releaseInput();
    mpScene = std::move(scene);
    if (mpScene)
        mpScene->setCameraAspectRatio(float(mpGraphTarget->getWidth()) / mpGraphTarget->getHeight());
    for (auto& entry : mGraphs)
    {
        entry.graph->setScene(mpScene);
        entry.sceneUpdates = IScene::UpdateFlags::None;
    }
    mFrameValid = false;
}

RemoteRenderer::Graph* RemoteRenderer::currentGraph()
{
    return mActiveGraph < mGraphs.size() ? &mGraphs[mActiveGraph] : nullptr;
}

ref<RenderGraph> RemoteRenderer::activeGraph() const
{
    return mActiveGraph < mGraphs.size() ? mGraphs[mActiveGraph].graph : nullptr;
}

ref<RenderGraph> RemoteRenderer::getGraph(const std::string& name) const
{
    const auto it = std::find_if(mGraphs.begin(), mGraphs.end(), [&](const Graph& entry) { return entry.graph->getName() == name; });
    return it == mGraphs.end() ? nullptr : it->graph;
}

RemoteRenderer::Graph& RemoteRenderer::graphByName(const std::string& name)
{
    const auto it = std::find_if(mGraphs.begin(), mGraphs.end(), [&](const Graph& entry) { return entry.graph->getName() == name; });
    FALCOR_CHECK(it != mGraphs.end(), "Graph '{}' is not loaded.", name);
    return *it;
}

void RemoteRenderer::addGraph(const ref<RenderGraph>& graph)
{
    FALCOR_CHECK(graph, "An empty render graph cannot be added.");
    if (graph->getName().empty())
        graph->setName("RenderGraph");
    graph->onResize(mpGraphTarget.get());
    graph->setScene(mpScene);
    Graph entry{graph, graph->getOutputCount() ? graph->getOutputName(0) : std::string{}};
    if (getGraph(graph->getName()))
        graphByName(graph->getName()) = std::move(entry);
    else
        mGraphs.push_back(std::move(entry));
    mFrameValid = false;
    logInfo("Loaded render graph '{}'.", graph->getName());
}

void RemoteRenderer::removeGraph(const std::string& name)
{
    const size_t index = size_t(&graphByName(name) - mGraphs.data());
    mGraphs.erase(mGraphs.begin() + index);
    if (mActiveGraph > index)
        --mActiveGraph;
    if (mActiveGraph && mActiveGraph == mGraphs.size())
        --mActiveGraph;
    mFrameValid = false;
}

void RemoteRenderer::setActiveGraph(const ref<RenderGraph>& graph)
{
    FALCOR_CHECK(graph, "Select a loaded render graph.");
    if (getGraph(graph->getName()) != graph)
        addGraph(graph);
    mActiveGraph = size_t(&graphByName(graph->getName()) - mGraphs.data());
    mFrameValid = false;
}

void RemoteRenderer::selectOutput(Graph& entry, const std::string& name)
{
    if (entry.output == name)
        return;
    const auto available = entry.graph->getAvailableOutputs();
    FALCOR_CHECK(std::find(available.begin(), available.end(), name) != available.end(), "Unknown graph output '{}'.", name);
    if (!entry.extraOutput.empty())
        entry.graph->unmarkOutput(std::exchange(entry.extraOutput, {}));
    if (!entry.graph->isGraphOutput(name))
    {
        entry.graph->markOutput(name);
        entry.extraOutput = name;
    }
    entry.output = name;
    mFrameValid = false;
}

void RemoteRenderer::resizeFrameBuffer(uint32_t width, uint32_t height)
{
    validateSize(width, height);
    if (width == mpGraphTarget->getWidth() && height == mpGraphTarget->getHeight())
        return;
    mpGraphTarget = width == kPreviewSide && height == kPreviewSide
        ? mpPreview
        : Fbo::create2D(mpDevice, width, height, ResourceFormat::BGRA8UnormSrgb, ResourceFormat::Unknown);
    for (auto& entry : mGraphs)
        entry.graph->onResize(mpGraphTarget.get());
    if (mpScene)
        mpScene->setCameraAspectRatio(float(width) / height);
    mFrameValid = false;
}

void RemoteRenderer::run()
{
    const auto onInput = [this](const InputEvent& input) { this->onInput(input); };
    const auto onCommand = [this](const nlohmann::json& request) { return command(request); };
    bool wasConnected = false;
    while (!gStop && !mStop && !mClock.shouldExit())
    {
        mStream.poll(onInput, onCommand);
        const bool connected = mStream.connected();
        if (connected && !wasConnected)
        {
            if (!mClock.isPaused())
                mClock.play();
            mFpsStart = {};
            mRenderFps = 0;
        }
        wasConnected = connected;
        if (mStream.ready() && !mStop)
            renderFrame();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void RemoteRenderer::renderFrame()
{
    const auto started = std::chrono::steady_clock::now();
    auto& context = *mpDevice->getRenderContext();
    mClock.tick();
    bool rendered = false;
    if (!mFrameValid || (!mRenderPaused && mRenderError.empty()))
    {
        try
        {
            drawScene(context);
            mRenderError.clear();
            ++mFrames;
            rendered = true;
        }
        catch (const std::exception& error)
        {
            mRenderError = error.what();
            logError("{}", mRenderError);
            context.clearRtv(mpPreview->getRenderTargetView(0).get(), float4(0.f, 0.f, 0.f, 1.f));
        }
        mFrameValid = true;
    }
    mSubmitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    mStream.present(context, *mpPreview->getColorTexture(0));
    if (rendered && mCapture)
        captureFrameRange();
    const auto now = std::chrono::steady_clock::now();
    if (mFpsStart == std::chrono::steady_clock::time_point{})
    {
        mFpsStart = now;
        mSampledFrame = mFrames;
    }
    const double elapsed = std::chrono::duration<double>(now - mFpsStart).count();
    if (elapsed >= 1.0)
    {
        mRenderFps = (mFrames - mSampledFrame) / elapsed;
        mSampledFrame = mFrames;
        mFpsStart = now;
    }
#if FALCOR_ENABLE_PROFILER
    mpDevice->getProfiler()->endFrame(&context);
#endif
    mpDevice->endFrame();
}

void RemoteRenderer::drawScene(RenderContext& context)
{
    if (mpScene && mSceneCallback)
    {
        const auto scene = mpScene;
        mSceneCallback(scene, mClock.getTime());
    }
    if (mpScene)
    {
        const auto flags = mpScene->update(&context, mClock.getTime());
        for (auto& graph : mGraphs)
            graph.sceneUpdates |= flags;
    }

    ref<Texture> output;
    if (auto* entry = currentGraph())
    {
        auto& graph = *entry->graph;
        std::string compileLog;
        FALCOR_CHECK(graph.compile(&context, compileLog), "Graph compilation failed:\n{}", compileLog);
        graph.onSceneUpdates(&context, std::exchange(entry->sceneUpdates, IScene::UpdateFlags::None));
        graph.getPassesDictionary()[kRenderPassRefreshFlags] = RenderPassRefreshFlags::None;
        graph.execute(&context);
        if (mOverlay)
            graph.renderOverlayUI(&context);
        if (!entry->output.empty())
        {
            if (auto resource = graph.getOutput(entry->output))
                output = resource->asTexture();
        }
    }

    const uint2 sourceSize = output ? uint2(output->getWidth(), output->getHeight()) : uint2(0);
    const auto dest = mpPreview->getRenderTargetView(0);
    if (!output || sourceSize.x != sourceSize.y)
        context.clearRtv(dest.get(), float4(0.f, 0.f, 0.f, 1.f));
    if (output)
        context.blit(output->getSRV(), dest, RenderContext::kMaxRect, previewRect(sourceSize));
}

void RemoteRenderer::keyEvent(const KeyboardEvent& event)
{
    if (mLookNavigation && isMovementKey(event.key))
    {
        if (mpLookScene)
            mpLookScene->onKeyEvent(event);
        return;
    }
    if (auto graph = activeGraph())
        graph->onKeyEvent(event);
    if (mpScene && mpScene->onKeyEvent(event))
        return;
    if (mKeyCallback && event.type != KeyboardEvent::Type::KeyRepeated)
        mKeyCallback(event.type == KeyboardEvent::Type::KeyPressed, uint32_t(event.key));
}

void RemoteRenderer::onInput(const InputEvent& input)
{
    if (input.type == InputType::EnableLookNavigation)
    {
        mLookNavigation = true;
        return;
    }
    if (input.type == InputType::ReleaseAll)
    {
        releaseInput();
        if (input.code == 1)
            mLookNavigation = false;
        return;
    }
    if (input.type == InputType::Key)
    {
        KeyboardEvent event{};
        if (input.code <= GLFW_KEY_LAST && input.action <= GLFW_REPEAT &&
            Window::translateGlfwKeyEvent(int(input.code), int(input.action), int(input.mods & 7), event) && event.key != Input::Key::Unknown)
        {
            mKeys.set(size_t(event.key), input.action != GLFW_RELEASE);
            keyEvent(event);
        }
        return;
    }
    if (!std::isfinite(input.x) || !std::isfinite(input.y))
        return;
    MouseEvent event{};
    if (input.type == InputType::Look && input.action <= GLFW_REPEAT)
    {
        if (input.action == GLFW_PRESS && !mpLookScene)
            mpLookScene = mpScene;
        if (!mpLookScene)
            return;
        event.button = Input::MouseButton::Left;
        event.pos = input.action == GLFW_REPEAT ? clamp(float2(input.x, input.y), float2(-4096.f), float2(4096.f)) : float2(0.5f);
        event.screenPos = float2(kPreviewSide) * event.pos;
        event.type = input.action == GLFW_REPEAT ? MouseEvent::Type::Move :
                     input.action == GLFW_PRESS ? MouseEvent::Type::ButtonDown : MouseEvent::Type::ButtonUp;
        mpLookScene->onMouseEvent(event);
        if (input.action == GLFW_RELEASE)
            mpLookScene.reset();
        return;
    }

    if (input.type == InputType::Scroll)
    {
        event.type = MouseEvent::Type::Wheel;
        event.wheelDelta = clamp(float2(input.x, input.y), float2(-20.f), float2(20.f));
    }
    else
    {
        mPointer = clamp(float2(input.x, input.y), float2(0.f), float2(1.f));
        if (input.type == InputType::Move)
            event.type = MouseEvent::Type::Move;
        else if (input.type == InputType::Button && input.code <= GLFW_MOUSE_BUTTON_MIDDLE && input.action <= GLFW_PRESS)
        {
            constexpr Input::MouseButton buttons[] = {Input::MouseButton::Left, Input::MouseButton::Right, Input::MouseButton::Middle};
            event.button = buttons[input.code];
            event.type = input.action == GLFW_PRESS ? MouseEvent::Type::ButtonDown : MouseEvent::Type::ButtonUp;
            event.mods = Input::ModifierFlags(input.mods & 7);
            mButtons.set(size_t(event.button), input.action == GLFW_PRESS);
        }
        else
            return;
    }
    const uint2 size(mpGraphTarget->getWidth(), mpGraphTarget->getHeight());
    const auto rect = previewRect(size);
    const auto pixel = mPointer * float2(kPreviewSide) - float2(rect.x, rect.y);
    event.pos = clamp(pixel / float2(rect.z - rect.x, rect.w - rect.y), float2(0.f), float2(1.f));
    event.screenPos = event.pos * float2(size);
    if (auto graph = activeGraph())
        graph->onMouseEvent(event);
    if (mpScene && !mLookNavigation)
        mpScene->onMouseEvent(event);
}

void RemoteRenderer::releaseInput()
{
    for (size_t i = 0; i < mKeys.size(); ++i)
        if (mKeys[i])
            keyEvent({KeyboardEvent::Type::KeyReleased, Input::Key(i), Input::ModifierFlags::None});
    if (mpLookScene)
        onInput({InputType::Look, 0, GLFW_RELEASE});
    for (size_t i = 0; i < mButtons.size(); ++i)
    {
        if (mButtons[i])
        {
            const uint32_t button = i == size_t(Input::MouseButton::Left) ? GLFW_MOUSE_BUTTON_LEFT :
                i == size_t(Input::MouseButton::Right) ? GLFW_MOUSE_BUTTON_RIGHT : GLFW_MOUSE_BUTTON_MIDDLE;
            onInput({InputType::Button, button, GLFW_RELEASE, 0, mPointer.x, mPointer.y});
        }
    }
    mKeys.reset();
    mButtons.reset();
}
} // namespace Falcor::Remote

int main(int argc, char** argv)
{
    using namespace Falcor;
    using namespace Falcor::Remote;
    args::ArgumentParser parser("Falcor remote render server");
    args::HelpFlag help(parser, "help", "Show this message", {'h', "help"});
    args::ValueFlag<uint32_t> port(parser, "port", "SSH tunnel target (default 9000)", {"remote-port"}, 9000);
    args::ValueFlag<uint32_t> fps(parser, "fps", "Maximum preview frames per second (default 60)", {"fps"}, 60);
    args::ValueFlag<uint32_t> gpu(parser, "gpu", "GPU adapter index", {"gpu"});
    args::ValueFlag<uint32_t> width(parser, "width", "Render graph width", {"width"}, kPreviewSide);
    args::ValueFlag<uint32_t> height(parser, "height", "Render graph height", {"height"}, kPreviewSide);
    args::ValueFlag<std::string> script(parser, "path", "Falcor graph script", {'s', "script"}, "scripts/MinimalPathTracer.py");
    args::ValueFlag<std::string> scene(parser, "path", "Scene to load", {"scene"}, kDefaultScene);
    try
    {
        parser.ParseCLI(argc, argv);
    }
    catch (const args::Help&)
    {
        std::cout << parser;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n' << parser;
        return 1;
    }
    if (!args::get(port) || args::get(port) > 65535 || !args::get(fps) || args::get(fps) > 240)
    {
        std::cerr << "Use a port from 1 to 65535 and FPS from 1 to 240.\n";
        return 1;
    }

    std::signal(SIGINT, stopSignal);
    std::signal(SIGTERM, stopSignal);
    Threading::start();
    const int result = catchAndReportAllExceptions([&]() {
        auto& resolver = AssetResolver::getDefaultResolver();
        resolver.addSearchPath(getProjectDirectory() / "media");
        for (const auto& path : Settings::getGlobalSettings().getSearchDirectories("media"))
            resolver.addSearchPath(path);
        Device::Desc desc;
        desc.type = Device::Type::Vulkan;
        if (gpu)
            desc.gpu = args::get(gpu);
        auto device = make_ref<Device>(desc);
        PluginManager::instance().loadAllPlugins();
        RemoteRenderer renderer(device, uint16_t(args::get(port)), args::get(fps));
        renderer.initializeScripting();
        renderer.resizeFrameBuffer(args::get(width), args::get(height));
        if (!args::get(script).empty())
            renderer.loadScript(args::get(script));
        if (!args::get(scene).empty())
            renderer.loadScene(args::get(scene));
        renderer.run();
        return 0;
    });
    Threading::shutdown();
    return result;
}
