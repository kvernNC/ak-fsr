cmake_minimum_required(VERSION 3.20)
project(ak-fsr LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # CRT statique : pas de dépendance aux DLL VC++ dans le préfixe

# Outils de développement (profils, rapport, pilotage par fichier, captures, compteurs) : ON sur la branche dev,
# OFF sur main (version publiée).
option(AKFSR_DEV_TOOLS "Compiler les outils de développement" ON)

add_library(ak-fsr SHARED
  src/main.cpp
  src/frame_dump.cpp
  src/passes.cpp
  src/insertion.cpp
  src/vk_bridge.cpp
  src/vk_context.cpp
  src/fsr.cpp
  src/jitter.cpp
  src/shader_patch.cpp
  src/d3d_compute.cpp
  src/gpu_timer.cpp
  src/shader_explorer.cpp
  src/exposure.cpp
  src/highlights.cpp
  src/rain.cpp
  src/lod_bias.cpp
  src/detour.cpp
  src/vk_device_hook.cpp
  src/spirv_half_image.cpp
  src/fsr4_driver.cpp
  src/ak-fsr.rc)
target_include_directories(ak-fsr PRIVATE
  third_party/reshade/include
  third_party/reshade/deps/imgui
  third_party/Vulkan-Headers/include
  third_party/ffx-api/include
  src)
target_compile_definitions(ak-fsr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN AKFSR_DEV_TOOLS=$<BOOL:${AKFSR_DEV_TOOLS}>)
if(AKFSR_DEV_TOOLS)
  target_sources(ak-fsr PRIVATE src/dev_tools.cpp src/screenshot.cpp)
endif()
message(STATUS "AK-FSR : outils de développement ${AKFSR_DEV_TOOLS}")
set_target_properties(ak-fsr PROPERTIES PREFIX "" SUFFIX ".addon64")
target_link_libraries(ak-fsr PRIVATE dxguid)

