#pragma once
#include "../3rdparty/imgui/imgui.h"
#include "../utils/utilities.h"

namespace ImGuiGui {
    // Display the ImGui interface
    void RenderGui();
    
    // Individual tab rendering functions
    void RenderGameValuesTab();
    void RenderAutoActionTab();
    void RenderHelpTab();
    void RenderCharacterTab();
    void RenderDebugInputTab();  // Add debug input tab
    
    // Overall GUI state
    struct ImGuiGuiState {
        bool visible;
        int currentTab;
        int requestedTab; 
        DisplayData localData;
    };
    
    // Global GUI state
    extern ImGuiGuiState guiState;
    
    // Initialize the GUI
    void Initialize();
    
    // Apply settings from ImGui interface to game
    void ApplyImGuiSettings();
    void RefreshLocalData();
    
    // Helper function to check if character settings should be shown
    bool ShouldShowCharacterSettings();
}