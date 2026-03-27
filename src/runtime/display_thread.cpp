#include "display_thread.hpp"
#include "engine_types.hpp"

#include <cinttypes>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

static void render_order_book(const BookSnapshot& s) {
    ImGui::Begin("Order Book");

    // Header metrics
    ImGui::Text("Spread: %.6f", to_display(s.spread));
    ImGui::SameLine();
    ImGui::Text("  Imbalance: %.4f", s.imbalance);
    ImGui::SameLine();
    ImGui::Text("  VWMID: %.6f", s.vwmid);

    ImGui::Text("EMA: %.6f", to_display(s.ema));
    ImGui::SameLine();
    ImGui::Text("  Tick Rate: %.1f/s", s.tick_rate);
    ImGui::SameLine();

    if (s.in_burst) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::Text("  BURST");
        ImGui::PopStyleColor();
    } else {
        ImGui::Text("  --");
    }

    ImGui::Text("Seq: %" PRIu64 "  BidQty: %" PRIu64 "  AskQty: %" PRIu64,
                s.event_sequence, s.total_bid_qty, s.total_ask_qty);

    ImGui::Separator();

    // Depth table
    if (ImGui::BeginTable("book", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {

        ImGui::TableSetupColumn("BID PRICE");
        ImGui::TableSetupColumn("QTY");
        ImGui::TableSetupColumn("ORD");
        ImGui::TableSetupColumn("ORD");
        ImGui::TableSetupColumn("QTY");
        ImGui::TableSetupColumn("ASK PRICE");
        ImGui::TableHeadersRow();

        uint32_t rows = SNAPSHOT_DEPTH;

        for (uint32_t i = 0; i < rows; ++i) {
            ImGui::TableNextRow();

            // Bid side (green)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.9f, 0.2f, 1.0f));

            ImGui::TableSetColumnIndex(0);
            if (i < s.bid_level_count)
                ImGui::Text("%.6f", to_display(s.bids[i].price));

            ImGui::TableSetColumnIndex(1);
            if (i < s.bid_level_count)
                ImGui::Text("%" PRId64, s.bids[i].total_qty);

            ImGui::TableSetColumnIndex(2);
            if (i < s.bid_level_count)
                ImGui::Text("%u", s.bids[i].order_count);

            ImGui::PopStyleColor();

            // Ask side (red)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));

            ImGui::TableSetColumnIndex(3);
            if (i < s.ask_level_count)
                ImGui::Text("%u", s.asks[i].order_count);

            ImGui::TableSetColumnIndex(4);
            if (i < s.ask_level_count)
                ImGui::Text("%" PRId64, s.asks[i].total_qty);

            ImGui::TableSetColumnIndex(5);
            if (i < s.ask_level_count)
                ImGui::Text("%.6f", to_display(s.asks[i].price));

            ImGui::PopStyleColor();
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void DisplayThread::run(const BookSnapshot& snapshot, volatile sig_atomic_t& running) {
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
    });

    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(900, 500, "Order Book", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync ~60fps

#ifdef __APPLE__
    const char* glsl_version = "#version 150";
#else
    const char* glsl_version = "#version 330";
#endif

    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    ImGui::StyleColorsDark();

    BookSnapshot local{};

    while (!glfwWindowShouldClose(window) && running) {
        glfwPollEvents();

        snapshot_read(snapshot, local);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        render_order_book(local);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}
