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
        // Sub-tab indices for tab bars within top-level tabs
        int mainMenuSubTab;   // 0=Opponent, 1=Values, 2=Options
        int autoActionSubTab; // 0=Triggers, 1=Macros
        int helpSubTab;       // 0..N-1 across Help tabs
        // One-shot programmatic selection requests for sub-tabs
        int requestedMainMenuSubTab;   // -1 or 0..2
        int requestedAutoActionSubTab; // -1 or 0..1
        int requestedHelpSubTab;       // -1 or 0..5
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
    
    // Programmatic navigation helpers
    // Cycle the top-level tab bar (order: Main, Auto Action, Settings, Character, Help)
    void RequestTopTabCycle(int direction);
    // Cycle the active sub-tab group based on the current top-level tab
    void RequestActiveSubTabCycle(int direction);
    // Request an absolute top-level tab by logical index in the above order
    void RequestTopTabAbsolute(int logicalIndex);
}