cmake_minimum_required(VERSION 3.10)
project(efz_training_mode)

option(EFZ_XP_COMPAT "Enable Windows XP-oriented compile settings (requires v141_xp-compatible toolset)" OFF)
option(EFZ_ENABLE_INPUT_LOGS "Enable verbose input/menu navigation trace logging" OFF)
option(EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE "Enable verbose auto-action wake/subframe trace logging" OFF)
option(EFZ_ENABLE_EXPERIMENTAL_TRIAL_TUTORIAL
       "Enable the unfinished trial/tutorial modes and their title-screen Practice patch"
       OFF)
set(EFZ_MINHOOK_SOURCE_DIR "" CACHE PATH "Optional path to a MinHook source tree for source builds")

set(EFZ_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(EFZ_USE_SOURCE_MINHOOK OFF)
set(EFZ_USE_SOURCE_DETOURS OFF)

# Set C++ standard to C++17 for std::filesystem support
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force 32-bit build
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}" CACHE STRING "" FORCE)

    # Proper way to force 32-bit build
    set(CMAKE_GENERATOR_PLATFORM Win32)
    # Use appropriate architecture flags for 32-bit
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MACHINE:X86")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MACHINE:X86")
    
    # Static linking of runtime for both C and C++ targets.
    foreach(flag_var
            CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        if(${flag_var} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        endif()
    endforeach()
    
    # Additional flags for static linking
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
    
    # Disable annoying secure CRT warnings
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    
    # Disable ImGui's built-in XInput gamepad polling in the Win32 backend.
    # We handle ALL gamepad input ourselves via PreNewFrameInputs() with a
    # per-direction gate.  Without this, ImGui_ImplWin32_UpdateGamepads()
    # calls XInputGetState() every frame and feeds raw ungated dpad/stick
    # events, causing double-navigation.
    add_definitions(-DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    
    # Uncomment to enable verbose frame advantage debug logging:
    # add_definitions(-DENABLE_FRAME_ADV_DEBUG)

    if(EFZ_XP_COMPAT)
        add_definitions(-DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -DEFZ_XP_COMPAT=1)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:threadSafeInit-")
        set(EFZ_USE_SOURCE_MINHOOK ON)
        if(NOT CMAKE_GENERATOR_TOOLSET MATCHES "v141_xp")
            message(WARNING "EFZ_XP_COMPAT is enabled but generator toolset is not v141_xp; the binary may still import Vista+ APIs.")
        endif()
    endif()
endif()

# First, check if the main ImGui file exists
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui/imgui.cpp")
    message(FATAL_ERROR "ImGui files not found. Please check that 3rdparty/imgui/imgui.cpp exists.")
endif()

# Define ImGui directory with absolute paths
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui")

# List ImGui sources explicitly
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx9.cpp"
)

# Define source files using file globs for subfolders
file(GLOB ROOT_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "src/core/*.cpp")
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "src/game/*.cpp")
file(GLOB GAME_PRACTICE_MENU_SOURCES CONFIGURE_DEPENDS "src/game/practice_menu/*.cpp")
file(GLOB GAME_MISSION_SOURCES CONFIGURE_DEPENDS "src/game/mission/*.cpp")
file(GLOB GUI_SOURCES CONFIGURE_DEPENDS "src/gui/*.cpp")
file(GLOB GUI_CUSTOM_MENU_SOURCES CONFIGURE_DEPENDS "src/gui/custom_menu/*.cpp")
file(GLOB INPUT_SOURCES CONFIGURE_DEPENDS "src/input/*.cpp")
file(GLOB UTILS_SOURCES CONFIGURE_DEPENDS "src/utils/*.cpp")
set(RESOURCE_SOURCES "src/resources/efz_training_resources.rc")

# Remove duplicate legacy files
list(REMOVE_ITEM ROOT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frame_advantage.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frame_monitor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frame_analysis.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/input_handler.cpp"
)

# Create combined SOURCES variable
set(SOURCES
    ${ROOT_SOURCES}
    ${CORE_SOURCES}
    ${GAME_SOURCES}
    ${GAME_PRACTICE_MENU_SOURCES}
    ${GAME_MISSION_SOURCES}
    ${GUI_SOURCES}
    ${GUI_CUSTOM_MENU_SOURCES}
    ${INPUT_SOURCES}
    ${UTILS_SOURCES}
    ${IMGUI_SOURCES}
    ${RESOURCE_SOURCES}
)

set(EFZ_MINHOOK_SOURCE_CANDIDATES
    "${EFZ_MINHOOK_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/minhook"
    "${CMAKE_CURRENT_SOURCE_DIR}/../InGameControlsRebind/third_party/minhook"
    "${CMAKE_CURRENT_SOURCE_DIR}/../efz_bgtool/3rdparty/minhook"
)

if(EFZ_USE_SOURCE_MINHOOK)
    set(EFZ_RESOLVED_MINHOOK_SOURCE_DIR "")
    foreach(candidate IN LISTS EFZ_MINHOOK_SOURCE_CANDIDATES)
        if(candidate AND EXISTS "${candidate}/CMakeLists.txt")
            set(EFZ_RESOLVED_MINHOOK_SOURCE_DIR "${candidate}")
            break()
        endif()
    endforeach()

    if(NOT EFZ_RESOLVED_MINHOOK_SOURCE_DIR)
        message(FATAL_ERROR "EFZ_XP_COMPAT requires MinHook source. Set EFZ_MINHOOK_SOURCE_DIR or provide a source tree at ../InGameControlsRebind/third_party/minhook.")
    endif()

    message(STATUS "[XP] Using MinHook source build from ${EFZ_RESOLVED_MINHOOK_SOURCE_DIR}")

    if(NOT TARGET minhook)
        add_subdirectory("${EFZ_RESOLVED_MINHOOK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/third_party/minhook" EXCLUDE_FROM_ALL)
    endif()

    if(MSVC)
        set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    endif()
endif()

if(EFZ_USE_SOURCE_DETOURS)
    message(STATUS "[XP] Using in-tree Detours source build")

    add_library(efz_detours STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/detours.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/modules.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/disasm.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/image.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/creatwth.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/disolx86.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/disolx64.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/disolia64.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/disolarm.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src/disolarm64.cpp"
    )

    target_include_directories(efz_detours PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/include"
    )
    target_include_directories(efz_detours PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/src"
    )
    target_compile_definitions(efz_detours PRIVATE WIN32_LEAN_AND_MEAN)

    if(MSVC)
        set_property(TARGET efz_detours PROPERTY MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    endif()
endif()

# Create a shared library (DLL)
add_library(efz_training_mode SHARED ${SOURCES})

if(EFZ_ENABLE_INPUT_LOGS)
    target_compile_definitions(efz_training_mode PRIVATE EFZ_ENABLE_INPUT_LOGS=1)
endif()

if(EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE)
    target_compile_definitions(efz_training_mode PRIVATE EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE=1)
endif()

if(EFZ_ENABLE_EXPERIMENTAL_TRIAL_TUTORIAL)
    target_compile_definitions(efz_training_mode PRIVATE EFZ_ENABLE_EXPERIMENTAL_TRIAL_TUTORIAL=1)
endif()

# Update include directories to include subdirectories
target_include_directories(efz_training_mode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/core
    ${CMAKE_CURRENT_SOURCE_DIR}/include/game
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui
    ${CMAKE_CURRENT_SOURCE_DIR}/include/input
    ${CMAKE_CURRENT_SOURCE_DIR}/include/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/minhook/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include
)

set(EFZ_THIRD_PARTY_LINKS
    user32
    kernel32
    winmm
    ws2_32
    iphlpapi
    comctl32
    gdiplus
    shlwapi
    d3d9
    dbghelp
    legacy_stdio_definitions
)

if(EFZ_USE_SOURCE_DETOURS)
    list(APPEND EFZ_THIRD_PARTY_LINKS efz_detours)
else()
    if(EFZ_XP_COMPAT)
        message(STATUS "[XP] Using prebuilt Detours library to preserve the known-good runtime hook path")
    endif()
    list(APPEND EFZ_THIRD_PARTY_LINKS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/lib.X86/detours.lib")
endif()

if(EFZ_USE_SOURCE_MINHOOK)
    list(APPEND EFZ_THIRD_PARTY_LINKS minhook)
else()
    list(APPEND EFZ_THIRD_PARTY_LINKS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/minhook/lib/libMinHook.x86.lib")
endif()

# Link required libraries including network libraries, Detours, and DirectX
target_link_libraries(efz_training_mode PRIVATE ${EFZ_THIRD_PARTY_LINKS})

# Add explicit LTCG flag and resolve library conflicts
if(MSVC)
    # Enable Link Time Code Generation explicitly
    set_target_properties(efz_training_mode PROPERTIES
        STATIC_LIBRARY_FLAGS "/LTCG")

    target_compile_options(efz_training_mode PRIVATE
        /FS
        # Embed Release object debug records. This still produces the linked
        # DLL PDB, while avoiding per-object references to a shared vc*.pdb
        # (and the resulting LNK4099 warnings in parallel builds).
        "$<$<CONFIG:Release>:/Z7>"
    )

    target_link_options(efz_training_mode PRIVATE
        "$<$<CONFIG:Release>:/DEBUG:FULL>"
        "$<$<CONFIG:Release>:/OPT:REF>"
        "$<$<CONFIG:Release>:/OPT:ICF>"
    )

    if(EFZ_XP_COMPAT)
        # dinput.h auto-links dxguid.lib, but the XP toolchain on this machine
        # does not ship it. We provide the needed DirectInput GUIDs in-tree.
        target_link_options(efz_training_mode PRIVATE "/NODEFAULTLIB:dxguid.lib")
    endif()
    
    # If needed, exclude conflicting libraries
    # Uncomment if build issues persist
    # target_link_options(efz_training_mode PRIVATE "/NODEFAULTLIB:MSVCRT")
endif()

# Set output directories
set_target_properties(efz_training_mode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
    PDB_NAME "efz_training_mode"
)

# Set the runtime library to static
# This ensures all dependencies are statically linked
set_property(TARGET efz_training_mode PROPERTY
    MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")

include(CTest)
if(BUILD_TESTING)
    add_executable(revival_profile_tests
        tests/revival_profile_tests.cpp)
    target_include_directories(revival_profile_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET revival_profile_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME revival_profile_tests
             COMMAND revival_profile_tests)

    add_executable(character_hotswap_transition_tests
        tests/character_hotswap_transition_tests.cpp)
    target_include_directories(character_hotswap_transition_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET character_hotswap_transition_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME character_hotswap_transition_tests
             COMMAND character_hotswap_transition_tests)

    add_executable(mission_sequence_policy_tests
        tests/mission_sequence_policy_tests.cpp)
    target_include_directories(mission_sequence_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET mission_sequence_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_sequence_policy_tests
             COMMAND mission_sequence_policy_tests)

    add_executable(macro_playback_start_policy_tests
        tests/macro_playback_start_policy_tests.cpp)
    target_include_directories(macro_playback_start_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET macro_playback_start_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME macro_playback_start_policy_tests
             COMMAND macro_playback_start_policy_tests)

    add_executable(mission_contact_attribution_policy_tests
        tests/mission_contact_attribution_policy_tests.cpp)
    target_include_directories(mission_contact_attribution_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET mission_contact_attribution_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_contact_attribution_policy_tests
             COMMAND mission_contact_attribution_policy_tests)

    add_executable(mission_entity_schedule_policy_tests
        tests/mission_entity_schedule_policy_tests.cpp)
    target_include_directories(mission_entity_schedule_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET mission_entity_schedule_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_entity_schedule_policy_tests
             COMMAND mission_entity_schedule_policy_tests)

    add_executable(mission_entity_lifecycle_policy_tests
        tests/mission_entity_lifecycle_policy_tests.cpp)
    target_include_directories(mission_entity_lifecycle_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET mission_entity_lifecycle_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_entity_lifecycle_policy_tests
             COMMAND mission_entity_lifecycle_policy_tests)

    add_executable(mission_entity_review_policy_tests
        tests/mission_entity_review_policy_tests.cpp)
    target_include_directories(mission_entity_review_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET mission_entity_review_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_entity_review_policy_tests
             COMMAND mission_entity_review_policy_tests)

    add_executable(mission_legacy_entity_migration_tests
        tests/mission_legacy_entity_migration_tests.cpp
        src/game/mission/mission_legacy_entity_migration.cpp
        src/game/mission/mission_data.cpp
        src/game/mission/tutorial_support.cpp)
    target_include_directories(mission_legacy_entity_migration_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)
    set_property(TARGET mission_legacy_entity_migration_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_legacy_entity_migration_tests
             COMMAND mission_legacy_entity_migration_tests)

    add_executable(mission_entity_presentation_policy_tests
        tests/mission_entity_presentation_policy_tests.cpp)
    target_include_directories(mission_entity_presentation_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET mission_entity_presentation_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_entity_presentation_policy_tests
             COMMAND mission_entity_presentation_policy_tests)

    add_executable(mission_render_snapshot_tests
        tests/mission_render_snapshot_tests.cpp)
    target_include_directories(mission_render_snapshot_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET mission_render_snapshot_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_render_snapshot_tests
             COMMAND mission_render_snapshot_tests)

    add_executable(mission_entity_fanout_policy_tests
        tests/mission_entity_fanout_policy_tests.cpp)
    target_include_directories(mission_entity_fanout_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET mission_entity_fanout_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_entity_fanout_policy_tests
             COMMAND mission_entity_fanout_policy_tests)

    add_executable(snowbunny_transition_policy_tests
        tests/snowbunny_transition_policy_tests.cpp)
    target_include_directories(snowbunny_transition_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET snowbunny_transition_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME snowbunny_transition_policy_tests
             COMMAND snowbunny_transition_policy_tests)

    add_executable(entity_contact_phase_bridge_policy_tests
        tests/entity_contact_phase_bridge_policy_tests.cpp)
    target_include_directories(entity_contact_phase_bridge_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET entity_contact_phase_bridge_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME entity_contact_phase_bridge_policy_tests
             COMMAND entity_contact_phase_bridge_policy_tests)

    add_executable(entity_command_origin_policy_tests
        tests/entity_command_origin_policy_tests.cpp)
    target_include_directories(entity_command_origin_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET entity_command_origin_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME entity_command_origin_policy_tests
             COMMAND entity_command_origin_policy_tests)

    add_executable(tutorial_session_policy_tests
        tests/tutorial_session_policy_tests.cpp)
    target_include_directories(tutorial_session_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET tutorial_session_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME tutorial_session_policy_tests
             COMMAND tutorial_session_policy_tests)

    add_executable(update_check_policy_tests
        tests/update_check_policy_tests.cpp
        src/utils/version_compare.cpp)
    target_include_directories(update_check_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)
    set_property(TARGET update_check_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME update_check_policy_tests
             COMMAND update_check_policy_tests)

    add_executable(version_compare_tests
        tests/version_compare_tests.cpp
        src/utils/version_compare.cpp)
    target_include_directories(version_compare_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET version_compare_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME version_compare_tests
             COMMAND version_compare_tests)

    add_executable(normal_input_policy_tests
        tests/normal_input_policy_tests.cpp)
    target_include_directories(normal_input_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET normal_input_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME normal_input_policy_tests
             COMMAND normal_input_policy_tests)

    add_executable(generic_pad_axis_policy_tests
        tests/generic_pad_axis_policy_tests.cpp)
    target_include_directories(generic_pad_axis_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET generic_pad_axis_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME generic_pad_axis_policy_tests
             COMMAND generic_pad_axis_policy_tests)

    add_executable(efz_input_bindings_tests
        tests/efz_input_bindings_tests.cpp)
    target_include_directories(efz_input_bindings_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET efz_input_bindings_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME efz_input_bindings_tests
             COMMAND efz_input_bindings_tests)

    add_executable(input_poll_attack_edge_journal_tests
        tests/input_poll_attack_edge_journal_tests.cpp)
    target_include_directories(input_poll_attack_edge_journal_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET input_poll_attack_edge_journal_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME input_poll_attack_edge_journal_tests
             COMMAND input_poll_attack_edge_journal_tests)

    add_executable(character_action_catalog_tests
        tests/character_action_catalog_tests.cpp)
    target_include_directories(character_action_catalog_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET character_action_catalog_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME character_action_catalog_tests
             COMMAND character_action_catalog_tests)

    add_executable(move_notation_tables_tests
        tests/move_notation_tables_tests.cpp)
    target_include_directories(move_notation_tables_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET move_notation_tables_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME move_notation_tables_tests
             COMMAND move_notation_tables_tests)

    add_executable(entity_semantic_catalog_tests
        tests/entity_semantic_catalog_tests.cpp)
    target_include_directories(entity_semantic_catalog_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)
    set_property(TARGET entity_semantic_catalog_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME entity_semantic_catalog_tests
             COMMAND entity_semantic_catalog_tests
                     "${CMAKE_CURRENT_SOURCE_DIR}/assets/movedata/entity_manifest.json")

    add_executable(motion_pattern_tests
        tests/motion_pattern_tests.cpp)
    target_include_directories(motion_pattern_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET motion_pattern_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME motion_pattern_tests
             COMMAND motion_pattern_tests)

    add_executable(auto_action_charge_policy_tests
        tests/auto_action_charge_policy_tests.cpp)
    target_include_directories(auto_action_charge_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET auto_action_charge_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME auto_action_charge_policy_tests
             COMMAND auto_action_charge_policy_tests)

    add_executable(kaori_recoil_duck_policy_tests
        tests/kaori_recoil_duck_policy_tests.cpp)
    target_include_directories(kaori_recoil_duck_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_property(TARGET kaori_recoil_duck_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME kaori_recoil_duck_policy_tests
             COMMAND kaori_recoil_duck_policy_tests)

    add_executable(mission_data_tests
        tests/mission_data_tests.cpp
        src/game/mission/mission_data.cpp
        src/game/mission/tutorial_support.cpp)
    target_include_directories(mission_data_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)
    set_property(TARGET mission_data_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_data_tests
             COMMAND mission_data_tests
                     "${CMAKE_CURRENT_SOURCE_DIR}/assets/missions/core_tutorial")

    add_executable(mission_authoring_tests
        tests/mission_authoring_tests.cpp
        src/game/mission/mission_authoring.cpp
        src/game/mission/mission_data.cpp
        src/game/mission/tutorial_support.cpp)
    target_include_directories(mission_authoring_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)
    set_property(TARGET mission_authoring_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_authoring_tests
             COMMAND mission_authoring_tests)

    add_executable(mission_movedata_union_tests
        tests/mission_movedata_union_tests.cpp)
    target_include_directories(mission_movedata_union_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)
    set_property(TARGET mission_movedata_union_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${EFZ_MSVC_RUNTIME_LIBRARY}")
    add_test(NAME mission_movedata_union_tests
             COMMAND mission_movedata_union_tests
                     "${CMAKE_CURRENT_SOURCE_DIR}/assets/movedata/nanase.json"
                     "${CMAKE_CURRENT_SOURCE_DIR}/assets/movedata/nanase2.json")

    find_program(EFZ_PYTHON_EXECUTABLE NAMES python python3)
    if(EFZ_PYTHON_EXECUTABLE)
        add_test(NAME mission_entity_manifest_check
                 COMMAND "${EFZ_PYTHON_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/tools/pat_movedata/entity_census.py"
                         --check)
    endif()
endif()
