#pragma once
#include "../3rdparty/imgui/imgui.h"
#include "utilities.h"

namespace ImGuiGui {
    // Display the ImGui interface
    void RenderGui();
    
    // Individual tab rendering functions
    void RenderGameValuesTab();
    void RenderAutoActionTab();
    void RenderHelpTab();
    
    // Overall GUI state
    struct ImGuiGuiState {
        bool visible;
        int currentTab;
        int requestedTab; // Add this for programmatic tab switching
        DisplayData localData; // Local copy of display data
    };
    
    // Global GUI state
    extern ImGuiGuiState guiState;
    
    // Initialize the GUI
    void Initialize();
    
    // Apply settings from ImGui interface to game
    void ApplyImGuiSettings();
    void RefreshLocalData();
}