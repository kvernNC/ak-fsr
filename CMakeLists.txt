cmake_minimum_required(VERSION 3.20)
project(ak-fsr LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # CRT statique : pas de dépendance aux DLL VC++ dans le préfixe

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
  src/ak-fsr.rc)
target_include_directories(ak-fsr PRIVATE
  third_party/reshade/include
  third_party/reshade/deps/imgui
  third_party/Vulkan-Headers/include
  third_party/ffx-api/include
  src)
target_compile_definitions(ak-fsr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(ak-fsr PROPERTIES PREFIX "" SUFFIX ".addon64")

# amd_fidelityfx_vk.dll (scripts/fetch_ffx.sh) doit être déployée à côté de l'addon.
add_custom_command(TARGET ak-fsr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/third_party/ffx-api/bin/amd_fidelityfx_vk.dll $<TARGET_FILE_DIR:ak-fsr>)
