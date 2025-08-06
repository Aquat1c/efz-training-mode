cmake_minimum_required(VERSION 3.10)
project(efz_training_mode)

# Set C++ standard to C++17 for std::filesystem support
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force 32-bit build
if(MSVC)
    # Proper way to force 32-bit build
    set(CMAKE_GENERATOR_PLATFORM Win32)
    # Use appropriate architecture flags for 32-bit
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MACHINE:X86")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MACHINE:X86")
    
    # Static linking of C++ Runtime
    foreach(flag_var
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
file(GLOB GUI_SOURCES "src/gui/*.cpp")
file(GLOB INPUT_SOURCES "src/input/*.cpp")
file(GLOB UTILS_SOURCES "src/utils/*.cpp")

# Create combined SOURCES variable
set(SOURCES
    ${ROOT_SOURCES}
    ${CORE_SOURCES}
    ${GAME_SOURCES}
    ${GUI_SOURCES}
    ${INPUT_SOURCES}
    ${UTILS_SOURCES}
    ${IMGUI_SOURCES}
)

# Create a shared library (DLL)
add_library(efz_training_mode SHARED ${SOURCES})

# Update include directories to include subdirectories
target_include_directories(efz_training_mode PRIVATE
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

# Link required libraries including network libraries, Detours, and DirectX
target_link_libraries(efz_training_mode PRIVATE 
    user32 
    kernel32
    ws2_32
    iphlpapi
    comctl32
    ddraw
    dxguid
    d3d9
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/detours/lib.X86/detours.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/minhook/lib/libMinHook.x86.lib
    legacy_stdio_definitions
)

# Add explicit LTCG flag and resolve library conflicts
if(MSVC)
    # Enable Link Time Code Generation explicitly
    set_target_properties(efz_training_mode PROPERTIES
        STATIC_LIBRARY_FLAGS "/LTCG")
    
    # If needed, exclude conflicting libraries
    # Uncomment if build issues persist
    # target_link_options(efz_training_mode PRIVATE "/NODEFAULTLIB:MSVCRT")
endif()

# Set output directories
set_target_properties(efz_training_mode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Set the runtime library to static
# This ensures all dependencies are statically linked
set_property(TARGET efz_training_mode PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")