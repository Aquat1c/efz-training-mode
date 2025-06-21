#pragma once
#include "../3rdparty/imgui/imgui.h"
#include "utilities.h"

namespace ImGuiGui {
    // Display the ImGui interface
    void RenderGui();
    
    // Individual tab rendering functions
    void RenderGameValuesTab();
    void RenderMovementOptionsTab();
    void RenderAutoActionTab();
    
    // Overall GUI state
    struct ImGuiGuiState {
        bool visible;
        int currentTab;
        DisplayData localData; // Local copy of display data
    };
    
    // Global GUI state
    extern ImGuiGuiState guiState;
    
    // Initialize the GUI
    void Initialize();
    
    // Apply settings from ImGui interface to game
    void ApplyImGuiSettings();
}