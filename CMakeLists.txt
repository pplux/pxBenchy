cmake_minimum_required(VERSION 3.20)
project(pxbenchy C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PXB_TRACY "Enable Tracy profiler" ON)

set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/deps)

# lua (everything except standalone programs, amalgamation and test harness)
file(GLOB LUA_SOURCES ${DEPS}/lua/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${DEPS}/lua)

# volk
add_library(volk STATIC ${DEPS}/volk/volk.c)
target_include_directories(volk PUBLIC ${DEPS}/volk ${DEPS}/Vulkan-Headers/include)
if(APPLE)
  target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_METAL_EXT)
elseif(ANDROID)
  target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
endif()

# tinygltf v3 (C11 implementation unit)
add_library(tinygltf STATIC ${DEPS}/tinygltf/tiny_gltf_v3.c)
target_include_directories(tinygltf PUBLIC ${DEPS}/tinygltf)

# stb_image (C implementation unit, PNG/JPEG from memory) + stb_image_write (--screenshot PNG)
add_library(stb_image STATIC src/asset/stb_image.c src/asset/stb_image_write.c)
target_include_directories(stb_image PUBLIC ${DEPS}/stb)

# tracy
add_library(tracy STATIC ${DEPS}/tracy/public/TracyClient.cpp)
target_include_directories(tracy PUBLIC ${DEPS}/tracy/public)
if(PXB_TRACY)
  target_compile_definitions(tracy PUBLIC TRACY_ENABLE)
endif()

# glslang (no SPIRV-Tools, no tools/tests/install)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/glslang EXCLUDE_FROM_ALL)

set(PXB_SOURCES src/main.cpp src/gpu/vkc/device.cpp src/gpu/vk13/device.cpp src/gpu/vkc/vma.cpp src/gpu/vkc/swapchain.cpp
  src/gpu/vk13/swapchain.cpp src/gpu/vkc/shader.cpp
  src/gpu/vkc/timers.cpp src/gpu/vk13/timers.cpp src/gpu/vkc/text.cpp src/gpu/vk13/text.cpp src/gpu/vkc/screenshot.cpp src/gpu/vk13/screenshot.cpp src/asset/gltf.cpp src/lua/vm.cpp src/lua/bind_gpu.cpp src/lua/bind_pxb.cpp
  src/lua/bind_math.cpp src/gpu/vk13/sync.cpp src/gpu/vk13/barriers.cpp src/gpu/vkc/sync.cpp src/gpu/vkc/barriers.cpp
  src/gpu/vkc/resources.cpp src/gpu/vk13/resources.cpp src/gpu/vkc/pipeline.cpp src/gpu/vk13/pipeline.cpp
  src/gpu/vkc/pass.cpp src/gpu/vk13/pass.cpp src/gpu/vkc/descriptors.cpp src/gpu/vk11/sync.cpp src/gpu/vk11/barriers.cpp
  src/gpu/vk11/device.cpp src/gpu/vk11/caches.cpp src/gpu/vk11/resources.cpp src/gpu/vk11/pipeline.cpp
  src/gpu/vk11/pass.cpp src/gpu/vk11/screenshot.cpp src/gpu/vk11/swapchain.cpp src/gpu/vk11/timers.cpp src/gpu/vk11/text.cpp
  src/lua/timers_report.cpp src/lua/gpu_api.cpp src/platform/thermal.cpp)
# src/gpu/vkc is private to src/gpu: src/lua and src/main.cpp go through gpu/vk.h, gpu/vk13.h (+ lua/gpu_api.h)
file(GLOB_RECURSE PXB_GPU_CALLERS CONFIGURE_DEPENDS src/lua/*.h src/lua/*.cpp src/main.cpp)
foreach(f ${PXB_GPU_CALLERS})
  file(STRINGS ${f} vkc_includes REGEX "#include \"gpu/vkc/")
  if(vkc_includes)
    message(FATAL_ERROR "${f} includes a private gpu/vkc header")
  endif()
endforeach()
if(ANDROID)
  add_library(pxbenchy SHARED ${PXB_SOURCES} src/platform/android.cpp)
else()
  add_executable(pxbenchy ${PXB_SOURCES})
endif()
target_include_directories(pxbenchy PRIVATE src)
# git hash stamped at build time (startup line + HUD)
add_custom_target(git_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/git_version.h
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/git_version.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/git_version.h)
add_dependencies(pxbenchy git_version)
target_include_directories(pxbenchy PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(pxbenchy SYSTEM PRIVATE ${DEPS}/vma/include ${DEPS}/font8x8)
if(APPLE)
  # baked Vulkan loader / validation layer paths (used only if the default lookup fails; loader not linked)
  find_library(PXB_VULKAN_LOADER NAMES vulkan.1 vulkan HINTS /opt/homebrew/lib /usr/local/lib)
  find_path(PXB_VK_LAYER_DIR_FOUND VkLayer_khronos_validation.json
    HINTS /opt/homebrew/share/vulkan/explicit_layer.d /usr/local/share/vulkan/explicit_layer.d)
  if(PXB_VULKAN_LOADER)
    target_compile_definitions(pxbenchy PRIVATE PXB_VULKAN_LOADER_PATH="${PXB_VULKAN_LOADER}")
  endif()
  # the Homebrew manifest names the layer dylib without a path, which dyld cannot find
  # outside the default search paths: write a copy pointing at the absolute dylib path
  find_library(PXB_VK_LAYER_LIB NAMES VkLayer_khronos_validation HINTS /opt/homebrew/lib /usr/local/lib)
  if(PXB_VK_LAYER_DIR_FOUND AND PXB_VK_LAYER_LIB)
    set(PXB_VK_LAYER_JSON ${PXB_VK_LAYER_DIR_FOUND}/VkLayer_khronos_validation.json)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PXB_VK_LAYER_JSON})
    file(READ ${PXB_VK_LAYER_JSON} PXB_VK_LAYER_MANIFEST)
    string(REGEX REPLACE "\"library_path\": *\"[^\"]*\"" "\"library_path\": \"${PXB_VK_LAYER_LIB}\""
      PXB_VK_LAYER_MANIFEST "${PXB_VK_LAYER_MANIFEST}")
    file(WRITE ${CMAKE_BINARY_DIR}/vk_layers/VkLayer_khronos_validation.json "${PXB_VK_LAYER_MANIFEST}")
    target_compile_definitions(pxbenchy PRIVATE PXB_VK_LAYER_DIR="${CMAKE_BINARY_DIR}/vk_layers")
  endif()
  target_sources(pxbenchy PRIVATE src/platform/macos.mm)
  set_source_files_properties(src/platform/macos.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(pxbenchy PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Metal")
endif()
if(ANDROID)
  add_library(native_app_glue STATIC ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  target_include_directories(native_app_glue PUBLIC ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue)
  # keep the glue's entry point alive in the shared library
  target_link_libraries(pxbenchy PRIVATE native_app_glue android log)
  target_link_options(pxbenchy PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
endif()
target_link_libraries(pxbenchy PRIVATE lua volk tinygltf stb_image tracy
  glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
