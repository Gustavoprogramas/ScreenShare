#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <SDL2/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>

#include "capture/dxgi_capture.h"
#include "encode/hw_encoder.h"
#include "network/video_sender.h"
#include "network/input_receiver.h"
#include "input/input_injector.h"
#include "../shared/protocol.h"
#include "../shared/ring_buffer.h"

// ============================================================
//  Globals / shared state
// ============================================================
static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_hosting{ false };

static std::mutex                 g_input_mutex;
static std::condition_variable    g_input_cv;
static std::queue<InputPacket>    g_input_queue;

struct CaptureTask {
    CapturedFrame frame;
};
using CaptureQueue = RingBuffer<CaptureTask, 8>;
static CaptureQueue   g_capture_queue;
static VideoSendQueue g_send_queue;

// Pointers to active subsystems (so threads can access them)
static std::unique_ptr<DXGICapture> g_capture;
static std::unique_ptr<HWEncoder> g_encoder;
static std::unique_ptr<VideoSender> g_sender;
static std::unique_ptr<InputInjector> g_injector;
static std::unique_ptr<InputReceiver> g_input_recv;

static std::thread t_capture;
static std::thread t_encode;
static std::thread t_inject;

static std::string g_client_ip = "127.0.0.1";
static char input_ip_buf[128] = "127.0.0.1";

// ============================================================
//  Threads
// ============================================================
static void CaptureThreadFn() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << 0);
    while (g_hosting.load()) {
        CapturedFrame frame;
        if (!g_capture->AcquireNextFrame(frame, 16)) continue;
        CaptureTask task{ std::move(frame) };
        while (!g_capture_queue.push(std::move(task))) {
            g_capture_queue.pop();
            g_capture_queue.push(CaptureTask{ frame });
        }
    }
}

static void EncodeThreadFn() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << 1);
    while (g_hosting.load()) {
        auto item = g_capture_queue.pop();
        if (!item) {
            Sleep(1);
            continue;
        }
        CapturedFrame& f = item->frame;
        g_encoder->EncodeFrame(f.texture.Get(), f.subresource, f.timestamp_us);
    }
    g_encoder->Flush();
}

static void InputInjectThreadFn() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (g_hosting.load()) {
        std::unique_lock<std::mutex> lock(g_input_mutex);
        g_input_cv.wait(lock, [] {
            return !g_input_queue.empty() || !g_hosting.load();
        });
        while (!g_input_queue.empty()) {
            InputPacket pkt = g_input_queue.front();
            g_input_queue.pop();
            lock.unlock();
            g_injector->Inject(pkt);
            lock.lock();
        }
    }
}

// ============================================================
//  Start / Stop Hosting
// ============================================================
void StartHosting() {
    if (g_hosting.load()) return;
    g_client_ip = input_ip_buf;

    g_capture = std::make_unique<DXGICapture>();
    if (!g_capture->Init(0, 0)) {
        g_capture.reset();
        return;
    }

    HWEncoder::Config enc_cfg;
    enc_cfg.width = g_capture->GetOutputWidth();
    enc_cfg.height = g_capture->GetOutputHeight();
    enc_cfg.fps = 60;
    enc_cfg.bitrate_bps = 8'000'000;
    enc_cfg.nvenc_preset = "p1";
    enc_cfg.nvenc_tune = "ull";
    enc_cfg.nvenc_rc = "cbr";

    g_encoder = std::make_unique<HWEncoder>();
    if (!g_encoder->Init(g_capture->GetDevice(), g_capture->GetContext(), enc_cfg, [](EncodedPacket&& ep) {
        static uint32_t s_frame_id = 1;
        OutgoingPacket op;
        op.data = std::move(ep.data);
        op.timestamp_us = ep.pts_us;
        op.frame_id = s_frame_id++;
        op.is_keyframe = ep.is_keyframe;
        if (!g_send_queue.push(std::move(op))) {
            g_send_queue.pop();
            g_send_queue.push(std::move(op));
        }
    })) {
        g_encoder.reset();
        g_capture.reset();
        return;
    }

    g_sender = std::make_unique<VideoSender>();
    if (!g_sender->Init(VIDEO_PORT, &g_send_queue, 2)) {
        g_sender.reset(); g_encoder.reset(); g_capture.reset(); return;
    }

    g_injector = std::make_unique<InputInjector>();
    g_injector->Init(enc_cfg.width, enc_cfg.height);

    g_input_recv = std::make_unique<InputReceiver>();
    if (!g_input_recv->Init(INPUT_PORT, [](const InputPacket& pkt) {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_input_queue.push(pkt);
        g_input_cv.notify_one();
    }, 3)) {
        g_input_recv.reset(); g_injector.reset(); g_sender.reset(); g_encoder.reset(); g_capture.reset(); return;
    }

    g_hosting.store(true);
    t_capture = std::thread(CaptureThreadFn);
    t_encode = std::thread(EncodeThreadFn);
    t_inject = std::thread(InputInjectThreadFn);
}

void StopHosting() {
    if (!g_hosting.load()) return;
    g_hosting.store(false);
    g_input_cv.notify_all();

    if (t_capture.joinable()) t_capture.join();
    if (t_encode.joinable()) t_encode.join();
    if (t_inject.joinable()) t_inject.join();

    if (g_sender) g_sender->Stop();
    if (g_input_recv) g_input_recv->Stop();

    g_sender.reset();
    g_input_recv.reset();
    g_injector.reset();
    g_encoder.reset();
    g_capture.reset();
}

// ============================================================
//  MAIN
// ============================================================
int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return -1;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("Nexus Share - Host", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 400, 300, window_flags);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    while (g_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                g_running = false;
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) {
                g_running = false;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Host Control", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ImGui::Text("Nexus Share - Host Mode");
        ImGui::Separator();
        ImGui::Spacing();

        if (g_hosting) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "STATUS: HOSTING");
            ImGui::Text("Streaming to: %s", g_client_ip.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Stop Hosting", ImVec2(-1, 40))) {
                StopHosting();
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "STATUS: IDLE");
            ImGui::Spacing();
            ImGui::Text("Target Client IP (Radmin VPN):");
            ImGui::InputText("##ip", input_ip_buf, sizeof(input_ip_buf));
            ImGui::Spacing();
            if (ImGui::Button("Start Hosting", ImVec2(-1, 40))) {
                StartHosting();
            }
        }

        ImGui::End();

        ImGui::Render();
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    StopHosting();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
