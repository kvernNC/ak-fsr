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
  src/d3d_compute.cpp
  src/highlights.cpp
  src/fsr_dx11.cpp
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
target_compile_definitions(ak-fsr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(ak-fsr PROPERTIES PREFIX "" SUFFIX ".addon64")

# amd_fidelityfx_vk.dll (scripts/fetch_ffx.sh) doit être déployée à côté de l'addon.
add_custom_command(TARGET ak-fsr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/third_party/ffx-api/bin/amd_fidelityfx_vk.dll $<TARGET_FILE_DIR:ak-fsr>)
target_link_libraries(ak-fsr PRIVATE dxguid)

# M8.2 : backend DX11 de FidelityFX (scripts/fetch_ffx_dx11.sh), dans une DLL séparée en CRT dynamique
# (bibliothèques MSVC /MD). Facultatif : sans les bibliothèques, seule l'option « DX11 » est indisponible.
set(AKFSR_FFX_DX11_DIR ${CMAKE_SOURCE_DIR}/third_party/ffx-dx11)
if(EXISTS ${AKFSR_FFX_DX11_DIR}/lib/ffx_backend_dx11_x64.lib)
  add_library(ak-fsr-dx11 SHARED src/dx11/ffx_dx11_bridge.cpp)
  target_include_directories(ak-fsr-dx11 PRIVATE ${AKFSR_FFX_DX11_DIR}/include src)
  target_compile_definitions(ak-fsr-dx11 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  set_target_properties(ak-fsr-dx11 PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  target_link_libraries(ak-fsr-dx11 PRIVATE
    ${AKFSR_FFX_DX11_DIR}/lib/ffx_fsr3upscaler_x64.lib
    ${AKFSR_FFX_DX11_DIR}/lib/ffx_backend_dx11_x64.lib
    d3d11 dxgi)
  message(STATUS "AK-FSR : backend DX11 de FidelityFX trouvé, ak-fsr-dx11.dll sera compilée")
else()
  message(STATUS "AK-FSR : backend DX11 absent (scripts/fetch_ffx_dx11.sh), ak-fsr-dx11.dll non compilée")
endif()
