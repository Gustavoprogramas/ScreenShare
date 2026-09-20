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
#include <chrono>
#include <memory>

#include "network/video_receiver.h"
#include "network/input_sender.h"
#include "decode/hw_decoder.h"
#include "render/sdl_renderer.h"
#include "../shared/protocol.h"

static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_connected{ false };

static DecodeQueue g_decode_queue;
static RenderQueue g_render_queue;

static std::unique_ptr<VideoReceiver> g_video_recv;
static std::unique_ptr<HWDecoder> g_decoder;
static std::unique_ptr<InputSender> g_input_sender;

static std::string g_host_ip = "127.0.0.1";
static int g_host_width = 1920;
static int g_host_height = 1080;
static char input_ip_buf[128] = "127.0.0.1";

void Disconnect() {
    if (!g_connected) return;
    g_connected = false;

    if (g_input_sender) g_input_sender->Disconnect();
    if (g_decoder) g_decoder->Stop();
    if (g_video_recv) g_video_recv->Stop();

    g_input_sender.reset();
    g_decoder.reset();
    g_video_recv.reset();

    SDL_SetRelativeMouseMode(SDL_FALSE);
}

void ConnectToHost() {
    if (g_connected) return;
    g_host_ip = input_ip_buf;

    g_video_recv = std::make_unique<VideoReceiver>();
    if (!g_video_recv->Init(g_host_ip, VIDEO_PORT, &g_decode_queue, 0)) {
        g_video_recv.reset();
        return;
    }

    g_decoder = std::make_unique<HWDecoder>();
    if (!g_decoder->Init(g_host_width, g_host_height, &g_decode_queue, &g_render_queue, 1)) {
        g_decoder.reset(); g_video_recv.reset();
        return;
    }

    g_input_sender = std::make_unique<InputSender>();
    g_input_sender->Connect(g_host_ip, INPUT_PORT); // Non-fatal if fails immediately

    g_connected = true;
}

int main(int argc, char* argv[]) {
    printf("=== Remote Desktop CLIENT ===\n");

    SDLRenderer renderer;
    if (!renderer.Init(1280, 720, "Nexus Share - Client")) {
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForSDLRenderer(renderer.GetWindow(), renderer.GetRenderer());
    ImGui_ImplSDLRenderer2_Init(renderer.GetRenderer());

    uint64_t title_update_tick = SDL_GetTicks64();
    double last_latency_ms = 0.0;
    uint32_t frames_per_sec = 0;
    uint64_t fps_tick = SDL_GetTicks64();

    while (g_running) {
        SDL_Event ev;
        while (renderer.PollEvents(ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);

            if (ev.type == SDL_QUIT) {
                g_running = false;
                break;
            }
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE && ev.window.windowID == SDL_GetWindowID(renderer.GetWindow())) {
                g_running = false;
                break;
            }

            // Forward input events to Host if connected
            if (g_connected && g_input_sender && g_input_sender->IsConnected()) {
                // When connected, always forward mouse and keyboard to the host.
                // ImGui's WantCapture flags would block keyboard/mouse forwarding
                // because the overlay window exists. We only check WantCapture
                // when NOT connected (i.e., when the connection menu is open).
                int win_w, win_h;
                SDL_GetWindowSize(renderer.GetWindow(), &win_w, &win_h);
                g_input_sender->ProcessEvent(ev, win_w, win_h, g_host_width, g_host_height);
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        SDL_RenderClear(renderer.GetRenderer());

        if (g_connected) {
            auto frame_opt = g_render_queue.pop();
            if (frame_opt) {
                static uint32_t dbg_render = 0;
                dbg_render++;
                if (dbg_render % 60 == 1) printf("[DEBUG-Render] Drawing frame #%u %dx%d\n", dbg_render, frame_opt->width, frame_opt->height);
                renderer.DrawTexture(*frame_opt);
                frames_per_sec++;

                uint64_t now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                last_latency_ms = static_cast<double>(frame_opt->decode_ts_us - frame_opt->capture_ts_us) / 1000.0;
            }

            // Small overlay for Disconnect button — NoInputs prevents it from stealing keyboard/mouse focus
            ImGui::SetNextWindowPos(ImVec2(10, 10));
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGui::Begin("##Overlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
            if (ImGui::Button("Disconnect", ImVec2(100, 30))) {
                Disconnect();
            }
            ImGui::End();

        } else {
            // Connection Menu
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Client Connection", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
            ImGui::Text("Nexus Share - Client Mode");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Target Host IP (Radmin VPN):");
            ImGui::InputText("##ip", input_ip_buf, sizeof(input_ip_buf));
            ImGui::Spacing();
            if (ImGui::Button("Connect", ImVec2(-1, 40))) {
                ConnectToHost();
            }
            ImGui::End();
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer.GetRenderer());
        SDL_RenderPresent(renderer.GetRenderer());

        uint64_t now_ms = SDL_GetTicks64();
        if (now_ms - fps_tick >= 1000) {
            renderer.UpdateStats(static_cast<double>(frames_per_sec), last_latency_ms);
            frames_per_sec = 0;
            fps_tick = now_ms;
        }

        SDL_Delay(1);
    }

    Disconnect();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    return 0;
}
