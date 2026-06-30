cmake_minimum_required(VERSION 3.10)
project(efz_training_mode)

option(EFZ_XP_COMPAT "Enable Windows XP-oriented compile settings (requires v141_xp-compatible toolset)" OFF)
option(EFZ_ENABLE_INPUT_LOGS "Enable verbose input/menu navigation trace logging" OFF)
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
file(GLOB ROOT_SOURCES "src/*.cpp")
file(GLOB CORE_SOURCES "src/core/*.cpp")
file(GLOB GAME_SOURCES "src/game/*.cpp")
file(GLOB GUI_SOURCES CONFIGURE_DEPENDS "src/gui/*.cpp")
file(GLOB GUI_CUSTOM_MENU_SOURCES CONFIGURE_DEPENDS "src/gui/custom_menu/*.cpp")
file(GLOB INPUT_SOURCES "src/input/*.cpp")
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
        "$<$<CONFIG:Release>:/Zi>"
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
