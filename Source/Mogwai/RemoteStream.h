#pragma once

#include "Falcor.h"
#include "RemoteTransport.h"
#include <bitset>
#include <chrono>
#include <memory>
#include <vector>

namespace Mogwai
{
class Renderer;

class RemoteStream
{
public:
    RemoteStream(Renderer& renderer, const std::string& bindAddress, uint16_t port);
    ~RemoteStream();

    void pollInput(const Falcor::Fbo& target);
    void sendFrame(Falcor::RenderContext& context, const Falcor::Texture& texture);

private:
    void handleInput(const Falcor::Remote::InputEvent& input);
    void releaseInput();
    void flushFrame();
    void disconnect();

    struct Encoder;
    Renderer& mRenderer;
    Falcor::Remote::Socket mListener;
    Falcor::Remote::Socket mClient;
    std::unique_ptr<Encoder> mpEncoder;
    std::vector<uint8_t> mFrame;
    size_t mFrameOffset = 0;
    Falcor::Remote::InputPacket mInput{};
    size_t mInputOffset = 0;
    std::bitset<size_t(Falcor::Input::Key::Count)> mKeys;
    std::bitset<size_t(Falcor::Input::MouseButton::Count)> mButtons;
    Falcor::float2 mSize{1.f, 1.f};
    Falcor::float2 mPointer{0.f, 0.f};
    bool mRequestIdr = true;
    std::chrono::steady_clock::time_point mNextFrame{};
};
} // namespace Mogwai
