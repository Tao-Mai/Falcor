#include "RemoteRenderer.h"
#include "Core/Program/ProgramManager.h"
#include "RenderGraph/RenderGraphImportExport.h"
#include "Utils/Scripting/ScriptWriter.h"
#include "Utils/Scripting/Scripting.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace Falcor::Remote
{
using nlohmann::json;

namespace
{
json vectorJson(const float3& value)
{
    return {value.x, value.y, value.z};
}

float3 readVector(const json& value)
{
    FALCOR_CHECK(value.is_array() && value.size() == 3, "Provide three coordinates.");
    const float3 result(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
    FALCOR_CHECK(std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z), "Coordinates must be finite.");
    return result;
}

json directoryInfo(const std::filesystem::path& requested, const std::string& purpose)
{
    auto path = std::filesystem::absolute(requested.empty() ? "." : requested).lexically_normal();
    FALCOR_CHECK(std::filesystem::is_directory(path), "'{}' is not a directory on the render server.", path.string());
    json folders = json::array(), files = json::array();
    for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied))
    {
        const auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '.')
            continue;
        if (entry.is_directory())
            folders.push_back(name);
        else if (purpose != "script" || entry.path().extension() == ".py")
            files.push_back(name);
    }
    std::sort(folders.begin(), folders.end());
    std::sort(files.begin(), files.end());
    return {{"path", path.string()}, {"parent", path.parent_path().string()}, {"folders", std::move(folders)}, {"files", std::move(files)}};
}
} // namespace

void RemoteRenderer::rememberPath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path)
{
    auto full = std::filesystem::absolute(path).lexically_normal();
    paths.erase(std::remove(paths.begin(), paths.end(), full), paths.end());
    paths.insert(paths.begin(), std::move(full));
    if (paths.size() > 12)
        paths.resize(12);
}

void RemoteRenderer::saveConfig(const std::filesystem::path& path) const
{
    std::ofstream file(path);
    FALCOR_CHECK(file, "Could not open '{}' for writing.", path.string());
    file << "# Falcor render configuration\n";
    for (const auto& entry : mGraphs)
    {
        file << RenderGraphExporter::getIR(entry.graph);
        file << "m.addGraph(" << RenderGraphExporter::getFuncName(entry.graph->getName()) << "())\n\n";
    }
    if (auto graph = activeGraph())
    {
        const auto name = ScriptWriter::getArgString(graph->getName());
        file << "m.setActiveGraph(m.getGraph(" << name << "))\n";
    }
    if (mpScene)
    {
        file << ScriptWriter::makeMemberFunc("m", "loadScene", ScriptWriter::getPathString(mpScene->getPath()));
        file << mpScene->getScript("m.scene") << "\n";
    }
    file << ScriptWriter::makeMemberFunc("m", "resizeFrameBuffer", mpGraphTarget->getWidth(), mpGraphTarget->getHeight());
    file << mClock.getScript("m.clock");
    file.close();
    FALCOR_CHECK(file, "Could not save '{}'.", path.string());
}

json RemoteRenderer::stats() const
{
    json events = json::array();
    const auto profiler = mpDevice->getProfiler();
    if (profiler->isEnabled())
    {
        for (const auto* event : profiler->getEvents())
            events.push_back({{"name", event->getName()}, {"cpu_ms", event->getCpuTime()}, {"gpu_ms", event->getGpuTime()}});
    }
    const auto& streamError = mStream.encodingError();
    const auto& error = mRenderError.empty() ? (streamError.empty() ? mCaptureError : streamError) : mRenderError;
    return {
        {"fps", mStream.framesPerSecond()}, {"render_fps", mRenderFps}, {"limit", mStream.frameLimit()}, {"frame", mFrames},
        {"encoding", mStream.encoding()},
        {"submit_ms", mSubmitMs}, {"gpu_wait_ms", mStream.gpuWaitMs()}, {"encode_ms", mStream.encodeMs()},
        {"time", mClock.getTime()}, {"clock_frame", mClock.getFrame()}, {"events", std::move(events)},
        {"error", error},
        {"capturing", bool(mCapture)}, {"captured", mCapturedFrames}
    };
}

json RemoteRenderer::state() const
{
    json graphNames = json::array();
    for (const auto& entry : mGraphs)
        graphNames.push_back(entry.graph->getName());

    json graph = nullptr;
    if (mActiveGraph < mGraphs.size())
    {
        const auto& entry = mGraphs[mActiveGraph];
        graph = RenderGraphExporter::describe(*entry.graph);
        graph["preview"] = entry.output;
        graph["available"] = entry.graph->getAvailableOutputs();
        if (!entry.extraOutput.empty())
        {
            auto& outputs = graph["outputs"];
            outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [&](const json& output) {
                return output.at("name") == entry.extraOutput;
            }), outputs.end());
        }
    }

    json scene = nullptr;
    if (mpScene)
    {
        const auto camera = mpScene->getCamera();
        json cameras = json::array();
        for (const auto& item : mpScene->getCameras())
            cameras.push_back(item->getName());
        const auto& settings = mpScene->getRenderSettings();
        scene = {
            {"path", mpScene->getPath().string()}, {"camera_speed", mpScene->getCameraSpeed()},
            {"controller", uint32_t(mpScene->getCameraControllerType())}, {"animated", mpScene->isAnimated()},
            {"camera", camera->getName()}, {"cameras", std::move(cameras)}, {"position", vectorJson(camera->getPosition())},
            {"viewpoint", mpScene->getCurrentViewpoint()}, {"viewpoint_count", mpScene->getViewpointCount()},
            {"target", vectorJson(camera->getTarget())}, {"up", vectorJson(camera->getUpVector())},
            {"focal_length", camera->getFocalLength()}, {"aperture", camera->getApertureRadius()},
            {"near", camera->getNearPlane()}, {"far", camera->getFarPlane()},
            {"env_light", settings.useEnvLight}, {"analytic_lights", settings.useAnalyticLights},
            {"emissive_lights", settings.useEmissiveLights}, {"grid_volumes", settings.useGridVolumes},
            {"albedo_multiplier", settings.diffuseAlbedoMultiplier}
        };
    }

    return {
        {"graphs", std::move(graphNames)}, {"graph", std::move(graph)}, {"scene", std::move(scene)}, {"stats", stats()},
        {"render_size", {mpGraphTarget->getWidth(), mpGraphTarget->getHeight()}},
        {"clock", {{"paused", mClock.isPaused()}, {"scale", mClock.getTimeScale()}, {"fps", mClock.getFramerate()}}},
        {"render_paused", mRenderPaused}, {"overlay", mOverlay}, {"profile_active", mpDevice->getProfiler()->isEnabled()},
        {"profile_recording", mpDevice->getProfiler()->isCapturing()},
        {"recent_scripts", mRecentScripts}, {"recent_scenes", mRecentScenes}
    };
}

void RemoteRenderer::applyGraph(const json& description, const std::string& selectedOutput)
{
    const auto name = description.at("name").get<std::string>();
    FALCOR_CHECK(!name.empty(), "Name the graph before applying it.");
    auto graph = RenderGraph::create(mpDevice, name);
    graph->onResize(mpGraphTarget.get());
    graph->setScene(mpScene);
    for (const auto& pass : description.at("passes"))
    {
        const auto passName = pass.at("name").get<std::string>();
        const auto type = pass.at("type").get<std::string>();
        FALCOR_CHECK(!passName.empty() && passName.find('.') == std::string::npos, "Pass names must be nonempty and cannot contain dots.");
        FALCOR_CHECK(graph->createPass(passName, type, Properties(pass.at("properties"))), "Could not add pass '{}'.", passName);
    }
    for (const auto& edge : description.at("edges"))
    {
        const auto src = edge.at(0).get<std::string>();
        const auto dst = edge.at(1).get<std::string>();
        FALCOR_CHECK(graph->addEdge(src, dst) != RenderGraph::kInvalidIndex, "Could not connect '{}' to '{}'.", src, dst);
    }
    for (const auto& output : description.at("outputs"))
    {
        const auto outputName = output.at("name").get<std::string>();
        for (uint32_t mask : output.value("masks", json::array({uint32_t(TextureChannelFlags::RGB)})))
        {
            FALCOR_CHECK(mask && (mask & ~15u) == 0, "Invalid channel mask for '{}'.", outputName);
            graph->markOutput(outputName, TextureChannelFlags(mask));
        }
        FALCOR_CHECK(graph->isGraphOutput(outputName), "'{}' is not an output of this graph.", outputName);
    }
    FALCOR_CHECK(graph->getOutputCount(), "Mark at least one output before applying the graph.");
    std::string log;
    FALCOR_CHECK(graph->compile(mpDevice->getRenderContext(), log), "Graph compilation failed:\n{}", log);
    if (!selectedOutput.empty() && graph->isGraphOutput(selectedOutput))
    {
        setActiveGraph(graph);
        selectOutput(graphByName(name), selectedOutput);
    }
    else
        setActiveGraph(graph);
}

void RemoteRenderer::updateScene(const json& changes)
{
    FALCOR_CHECK(mpScene, "Load a scene first.");
    if (changes.contains("camera"))
    {
        const auto name = changes.at("camera").get<std::string>();
        const auto& cameras = mpScene->getCameras();
        const auto found = std::find_if(cameras.begin(), cameras.end(), [&](const ref<Camera>& camera) { return camera->getName() == name; });
        FALCOR_CHECK(found != cameras.end(), "Unknown camera '{}'.", name);
        mpScene->selectCamera(uint32_t(found - cameras.begin()));
    }
    if (changes.contains("controller"))
    {
        const auto controller = changes.at("controller").get<uint32_t>();
        FALCOR_CHECK(controller <= uint32_t(Scene::CameraControllerType::SixDOF), "Unknown camera controller.");
        mpScene->setCameraController(Scene::CameraControllerType(controller));
    }
    if (changes.contains("animated"))
        mpScene->setIsAnimated(changes.at("animated").get<bool>());
    if (changes.contains("camera_speed"))
    {
        const float speed = changes.at("camera_speed").get<float>();
        FALCOR_CHECK(std::isfinite(speed) && speed >= 0.f, "Camera speed must be nonnegative.");
        mpScene->setCameraSpeed(speed);
    }

    auto camera = mpScene->getCamera();
    if (changes.contains("position") || changes.contains("target") || changes.contains("up"))
    {
        const auto position = changes.contains("position") ? readVector(changes.at("position")) : camera->getPosition();
        const auto target = changes.contains("target") ? readVector(changes.at("target")) : camera->getTarget();
        const auto up = changes.contains("up") ? readVector(changes.at("up")) : camera->getUpVector();
        FALCOR_CHECK(length(cross(target - position, up)) > 1e-6f, "Camera direction and up vector must be independent.");
        camera->setPosition(position);
        camera->setTarget(target);
        camera->setUpVector(up);
    }
    const auto focal = changes.value("focal_length", camera->getFocalLength());
    const auto aperture = changes.value("aperture", camera->getApertureRadius());
    const auto nearPlane = changes.value("near", camera->getNearPlane());
    const auto farPlane = changes.value("far", camera->getFarPlane());
    FALCOR_CHECK(std::isfinite(focal) && focal > 0.f && std::isfinite(aperture) && aperture >= 0.f, "Invalid camera lens parameters.");
    FALCOR_CHECK(std::isfinite(nearPlane) && nearPlane > 0.f && std::isfinite(farPlane) && farPlane > nearPlane, "Invalid camera depth range.");
    if (changes.contains("focal_length"))
        camera->setFocalLength(focal);
    if (changes.contains("aperture"))
        camera->setApertureRadius(aperture);
    if (changes.contains("near") || changes.contains("far"))
        camera->setDepthRange(nearPlane, farPlane);

    auto& lighting = mpScene->getRenderSettings();
    lighting.useEnvLight = changes.value("env_light", lighting.useEnvLight);
    lighting.useAnalyticLights = changes.value("analytic_lights", lighting.useAnalyticLights);
    lighting.useEmissiveLights = changes.value("emissive_lights", lighting.useEmissiveLights);
    lighting.useGridVolumes = changes.value("grid_volumes", lighting.useGridVolumes);
    lighting.diffuseAlbedoMultiplier = changes.value("albedo_multiplier", lighting.diffuseAlbedoMultiplier);
    mFrameValid = false;
}

void RemoteRenderer::captureOutput(const std::filesystem::path& path, const std::string& output)
{
    FALCOR_CHECK(mFrameValid, "Wait for a rendered frame.");
    auto texture = mpPreview->getColorTexture(0);
    if (!output.empty())
    {
        auto graph = activeGraph();
        FALCOR_CHECK(graph && graph->isGraphOutput(output), "Select a marked graph output to capture.");
        const auto resource = graph->getOutput(output);
        texture = resource ? resource->asTexture() : nullptr;
        FALCOR_CHECK(texture, "'{}' is not a rendered texture.", output);
    }
    const auto format = Bitmap::getFormatFromFileExtension(getExtensionFromPath(path));
    texture->captureToFile(0, 0, path, format, Bitmap::ExportFlags::None, false);
}

void RemoteRenderer::captureFrameRange()
{
    try
    {
        auto& capture = *mCapture;
        const auto name = fmt::format("{}-{:06d}{}", capture.file.stem().string(), capture.written, capture.file.extension().string());
        const auto path = capture.file.parent_path() / name;
        FALCOR_CHECK(!std::filesystem::exists(path), "Capture file '{}' already exists.", path.string());
        captureOutput(path, capture.output);
        mCapturedFrames = ++capture.written;
        if (capture.written == capture.count)
            mCapture.reset();
    }
    catch (const std::exception& error)
    {
        mCapture.reset();
        mCaptureError = error.what();
        logError("Could not capture the frame: {}", error.what());
    }
}

json RemoteRenderer::command(const json& request)
{
    const auto op = request.at("op").get<std::string>();
    if (op == "state")
        return state();
    if (op == "stats")
        return stats();
    if (op == "browse")
        return directoryInfo(request.value("path", "."), request.value("purpose", ""));
    if (op == "catalog")
    {
        json catalog = json::array();
        for (const auto& [type, info] : PluginManager::instance().getInfos<RenderPass>())
            catalog.push_back({{"type", type}, {"description", info.desc}});
        return catalog;
    }
    if (op == "describe_pass")
    {
        auto graph = RenderGraph::create(mpDevice);
        graph->onResize(mpGraphTarget.get());
        const auto type = request.at("type").get<std::string>();
        const auto name = request.at("name").get<std::string>();
        FALCOR_CHECK(graph->createPass(name, type, Properties{}), "Pass '{}' is unavailable.", type);
        return RenderGraphExporter::describe(*graph).at("passes").at(0);
    }
    if (op == "console")
    {
        const auto result = Scripting::runScript(request.at("code").get<std::string>(), Scripting::getDefaultContext(), true);
        mFrameValid = false;
        return {{"output", result.out + result.err}, {"state", state()}};
    }
    if (op == "load_script")
        loadScript(request.at("path").get<std::string>());
    else if (op == "load_scene")
        loadScene(request.at("path").get<std::string>());
    else if (op == "unload_scene")
        unloadScene();
    else if (op == "save_config")
    {
        const std::filesystem::path path = request.at("path").get<std::string>();
        saveConfig(path);
        rememberPath(mRecentScripts, path);
    }
    else if (op == "activate_graph")
        setActiveGraph(getGraph(request.at("name").get<std::string>()));
    else if (op == "remove_graph")
        removeGraph(request.at("name").get<std::string>());
    else if (op == "apply_graph")
        applyGraph(request.at("graph"), request.value("preview", ""));
    else if (op == "preview_output")
    {
        auto* entry = currentGraph();
        FALCOR_CHECK(entry, "Load a graph first.");
        selectOutput(*entry, request.at("name").get<std::string>());
    }
    else if (op == "pass_properties")
    {
        auto& entry = graphByName(request.at("graph").get<std::string>());
        const auto& name = request.at("name").get_ref<const std::string&>();
        const auto previous = entry.graph->getPass(name)->getProperties();
        auto properties = previous.toJson();
        properties.update(request.at("properties"));
        entry.graph->updatePass(name, Properties(std::move(properties)));
        std::string log;
        if (!entry.graph->compile(mpDevice->getRenderContext(), log))
        {
            entry.graph->updatePass(name, previous);
            FALCOR_THROW("The updated pass failed to compile:\n{}", log);
        }
        mFrameValid = false;
    }
    else if (op == "scene")
        updateScene(request.at("changes"));
    else if (op == "environment")
    {
        FALCOR_CHECK(mpScene, "Load a scene first.");
        const auto path = request.value("path", "");
        if (path.empty())
            mpScene->setEnvMap(nullptr);
        else
            FALCOR_CHECK(mpScene->loadEnvMap(path), "Could not load environment map '{}'.", path);
        mFrameValid = false;
    }
    else if (op == "viewpoint")
    {
        FALCOR_CHECK(mpScene, "Load a scene first.");
        const auto action = request.at("action").get<std::string>();
        if (action == "add")
            mpScene->addViewpoint();
        else if (action == "remove")
            mpScene->removeViewpoint();
        else
            mpScene->selectViewpoint(request.at("index").get<uint32_t>());
        mFrameValid = false;
    }
    else if (op == "render_size")
        resizeFrameBuffer(request.at("width").get<uint32_t>(), request.at("height").get<uint32_t>());
    else if (op == "render_pause")
        mRenderPaused = request.at("paused").get<bool>();
    else if (op == "overlay")
    {
        mOverlay = request.at("enabled").get<bool>();
        mFrameValid = false;
    }
    else if (op == "clock")
    {
        const auto& values = request.at("changes");
        if (values.contains("fps"))
            mClock.setFramerate(values.at("fps").get<uint32_t>());
        if (values.contains("time"))
        {
            const double value = values.at("time").get<double>();
            FALCOR_CHECK(value >= 0.0 && std::isfinite(value), "Time must be nonnegative.");
            mClock.setTime(value);
            mFrameValid = false;
        }
        if (values.contains("scale"))
        {
            const double scale = values.at("scale").get<double>();
            FALCOR_CHECK(std::isfinite(scale) && scale >= 0.0, "Time scale must be nonnegative.");
            mClock.setTimeScale(scale);
        }
        if (values.contains("paused"))
        {
            if (values.at("paused").get<bool>())
                mClock.pause();
            else
                mClock.play();
        }
        if (values.contains("step"))
        {
            mClock.pause();
            mClock.step(values.at("step").get<int>());
            mFrameValid = false;
        }
    }
    else if (op == "reload_shaders")
    {
        if (mpDevice->getProgramManager()->reloadAllPrograms())
            for (auto& entry : mGraphs)
                entry.graph->onHotReload(HotReloadFlags::Program);
        mFrameValid = false;
    }
    else if (op == "capture")
        captureOutput(request.at("path").get<std::string>(), request.value("output", ""));
    else if (op == "capture_start")
    {
        FALCOR_CHECK(!mCapture, "A frame capture is already active.");
        const std::filesystem::path path = request.at("path").get<std::string>();
        const auto count = request.at("count").get<uint32_t>();
        FALCOR_CHECK(path.has_filename() && path.has_extension() && count > 0, "Choose an output filename and a positive frame count.");
        mCapturedFrames = 0;
        mCaptureError.clear();
        mCapture = Capture{path, request.value("output", ""), count};
    }
    else if (op == "capture_stop")
        mCapture.reset();
    else if (op == "profile")
        mpDevice->getProfiler()->setEnabled(request.at("enabled").get<bool>());
    else if (op == "profile_start")
    {
        auto profiler = mpDevice->getProfiler();
        FALCOR_CHECK(!profiler->isCapturing(), "A profiling capture is already active.");
        profiler->setEnabled(true);
        profiler->startCapture();
    }
    else if (op == "profile_save")
    {
        auto profiler = mpDevice->getProfiler();
        FALCOR_CHECK(profiler->isCapturing(), "Start a profiling capture first.");
        profiler->endCapture()->writeToFile(request.at("path").get<std::string>());
    }
    else
        FALCOR_THROW("Unknown remote command '{}'.", op);
    return state();
}
} // namespace Falcor::Remote
