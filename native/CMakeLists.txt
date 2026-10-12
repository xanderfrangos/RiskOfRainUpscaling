cmake_minimum_required(VERSION 3.24)
project(RiskOfRainUpscaling LANGUAGES C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "RiskOfRainUpscaling requires Windows x64")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
find_program(FXC_EXECUTABLE fxc HINTS "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64" REQUIRED)
# Sources include each other relative to native/; compiled shaders land in shaders/<Entry>.h.
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
set(SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_DIR})

# compile_shader(<source.hlsl> <profile> <entry>...) appends shaders/<Entry>.h (variable <Entry>Shader) to SHADER_HEADERS.
function(compile_shader SOURCE PROFILE)
  foreach(ENTRY ${ARGN})
    add_custom_command(OUTPUT ${SHADER_DIR}/${ENTRY}.h
      COMMAND ${FXC_EXECUTABLE} /nologo /WX /O3 /T ${PROFILE} /E ${ENTRY} /I ${CMAKE_CURRENT_SOURCE_DIR} /Fh ${SHADER_DIR}/${ENTRY}.h /Vn ${ENTRY}Shader ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
      DEPENDS ${SOURCE} Core/Fullscreen.hlsli ../third_party/FidelityFX-FSR/ffx_a.h ../third_party/FidelityFX-FSR/ffx_fsr1.h VERBATIM)
    list(APPEND SHADER_HEADERS ${SHADER_DIR}/${ENTRY}.h)
  endforeach()
  set(SHADER_HEADERS ${SHADER_HEADERS} PARENT_SCOPE)
endfunction()
compile_shader(Core/Fullscreen.hlsl vs_5_0 Fullscreen)
compile_shader(SceneInputs/SceneInputs.hlsl ps_5_0 RawDepth)
compile_shader(Diagnostics/Diagnostics.hlsl ps_5_0 DepthPreview MotionPreview FinalCopy)
compile_shader(Upscaling/UpscalerInputs.hlsl ps_5_0 DepthInput MotionInput EncodeSrgb DecodeSrgb SanitizeHdr PreserveAlpha Upscale UpscaleHdr)
compile_shader(Upscaling/Rcas.hlsl ps_5_0 Rcas)
compile_shader(Upscaling/Outlines.hlsl ps_5_0 OutlineUpscale OutlineWeightInput OutlineResolve OutlineResolveFallback OutlineComposite OutlineOcclusionDepth OutlineSobelDepth OutlineSobel)
compile_shader(FrameGeneration/FrameGenerationInputs.hlsl ps_5_0 FgDepthInput FgMotionInput)
compile_shader(FrameGeneration/HudOpacity.hlsl ps_5_0 UiAlpha UiAlphaFlipped)
compile_shader(FrameGeneration/UiLayer.hlsl ps_5_0 UiLayer UiMask UiMaskAlpha FlipRows Letterbox)
compile_shader(../tests/native/HudFixture.hlsl ps_5_0 HudFixture)

# AMD FSR 3.1 upscaler on D3D11, from OptiScaler's FidelityFX-SDK-DX11 port: only
# the upscaler, its D3D11 backend and its shader permutations, linked statically.
set(FFX_DX11_SDK ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/FidelityFX-SDK-DX11/sdk)
set(FFX_DX11_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/ffx-dx11-shaders)
file(MAKE_DIRECTORY ${FFX_DX11_SHADER_DIR})
file(GLOB FFX_DX11_PASSES ${FFX_DX11_SDK}/src/backends/dx11/shaders/fsr3upscaler/*.hlsl)
file(GLOB FFX_DX11_GPU_HEADERS ${FFX_DX11_SDK}/include/FidelityFX/gpu/*.h ${FFX_DX11_SDK}/include/FidelityFX/gpu/fsr3upscaler/*.h
  ${FFX_DX11_SDK}/include/FidelityFX/gpu/fsr1/*.h ${FFX_DX11_SDK}/include/FidelityFX/gpu/spd/*.h)
# The fork's DX11 shader arguments (CMakeShadersFSR3Upscaler.txt): fxc, Shader Model 5.0, no wave operations.
set(FFX_DX11_SC_ARGS -reflection -DFFX_GPU=1
  -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
  -E CS -DFFX_HLSL=1 -DFFX_HLSL_5_0=1 -compiler=fxc -DFFX_SPD_NO_WAVE_OPERATIONS
  -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1} -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}
  -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1} -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}
  -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1} -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}
  -disable-logs -T cs_5_0 -DFFX_HLSL_SM=50 -I ${FFX_DX11_SDK}/include/FidelityFX/gpu -I ${FFX_DX11_SDK}/include/FidelityFX/gpu/fsr3upscaler)
foreach(PASS ${FFX_DX11_PASSES})
  get_filename_component(PASS_NAME ${PASS} NAME_WE)
  # The blob tables also index wave64 and 16-bit tables; on D3D11 wave64 is the same code.
  foreach(VARIANT "" _wave64 _16bit _wave64_16bit)
    if(VARIANT MATCHES "16bit")
      set(HALF 1)
    else()
      set(HALF 0)
    endif()
    add_custom_command(OUTPUT ${FFX_DX11_SHADER_DIR}/${PASS_NAME}${VARIANT}_permutations.h
      COMMAND ${FFX_DX11_SDK}/tools/binary_store/FidelityFX_SC.exe ${FFX_DX11_SC_ARGS} -name=${PASS_NAME}${VARIANT} -DFFX_HALF=${HALF} -output=${FFX_DX11_SHADER_DIR} ${PASS}
      WORKING_DIRECTORY ${FFX_DX11_SHADER_DIR} DEPENDS ${PASS} ${FFX_DX11_GPU_HEADERS} VERBATIM)
    list(APPEND FFX_DX11_SHADER_HEADERS ${FFX_DX11_SHADER_DIR}/${PASS_NAME}${VARIANT}_permutations.h)
  endforeach()
endforeach()
add_library(FidelityFX.Fsr3Dx11 STATIC
  ${FFX_DX11_SDK}/src/shared/ffx_assert.cpp ${FFX_DX11_SDK}/src/shared/ffx_object_management.cpp
  ${FFX_DX11_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
  ${FFX_DX11_SDK}/src/backends/dx11/ffx_dx11.cpp ${FFX_DX11_SDK}/src/backends/dx11/DXBCChecksum.c ${FFX_DX11_SDK}/src/backends/dx11/md5.c
  ${FFX_DX11_SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
  Upscaling/FsrDx11ShaderBlobs.cpp ${FFX_DX11_SHADER_HEADERS})
target_include_directories(FidelityFX.Fsr3Dx11 PUBLIC ${FFX_DX11_SDK}/include
  PRIVATE ${FFX_DX11_SDK}/src/shared ${FFX_DX11_SDK}/src/components ${FFX_DX11_SDK}/src/backends/shared ${FFX_DX11_SHADER_DIR})
# FFX_GCC only drops __declspec(dllexport) from FFX_API, so the native DLL exports none of it.
target_compile_definitions(FidelityFX.Fsr3Dx11 PUBLIC FFX_GCC PRIVATE FFX_FSR FFX_FSR3 UNICODE _UNICODE _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)

set(CORE_SOURCES Core/Native.cpp Core/FullscreenPass.cpp Core/FfxRuntime.cpp)
set(PRESENTATION_SOURCES Presentation/GraphicsHooks.cpp Presentation/StreamlinePresentation.cpp)
set(SCENE_INPUT_SOURCES SceneInputs/SceneInputs.cpp)
set(DIAGNOSTICS_SOURCES Diagnostics/Diagnostics.cpp Diagnostics/Profiler.cpp)
set(UPSCALING_SOURCES Upscaling/UpscalerContext.cpp Upscaling/Sharpening.cpp Upscaling/Outlines.cpp Upscaling/Upscaler.cpp Upscaling/DlssUpscaler.cpp Upscaling/FsrUpscaler.cpp
  Upscaling/FsrDx11Upscaler.cpp)
set(FRAME_GENERATION_SOURCES FrameGeneration/FrameBridge.cpp FrameGeneration/FrameGeneration.cpp FrameGeneration/DlssFrameGenerator.cpp
  FrameGeneration/FsrFrameGenerator.cpp FrameGeneration/HudOpacity.cpp)
set(PACING_SOURCES Pacing/FramePacing.cpp)
add_custom_target(RiskOfRainUpscaling.Shaders DEPENDS ${SHADER_HEADERS})

add_library(RiskOfRainUpscaling.Native SHARED ${CORE_SOURCES} ${PRESENTATION_SOURCES} ${SCENE_INPUT_SOURCES} ${DIAGNOSTICS_SOURCES}
  ${UPSCALING_SOURCES} ${FRAME_GENERATION_SOURCES} ${PACING_SOURCES})
add_dependencies(RiskOfRainUpscaling.Native RiskOfRainUpscaling.Shaders)
target_include_directories(RiskOfRainUpscaling.Native PRIVATE ../third_party/Streamline/include ../third_party/FidelityFX-SDK/Kits/FidelityFX)
target_compile_definitions(RiskOfRainUpscaling.Native PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(RiskOfRainUpscaling.Native PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(RiskOfRainUpscaling.Native PRIVATE FidelityFX.Fsr3Dx11 d3d11 d3d12 dxgi dxguid wintrust crypt32 user32)

enable_testing()
# native_test(<name> <source> <libraries>...): tests link the DLL's C and C++ test exports.
function(native_test NAME SOURCE)
  add_executable(RiskOfRainUpscaling.${NAME} ${SOURCE})
  add_dependencies(RiskOfRainUpscaling.${NAME} RiskOfRainUpscaling.Shaders)
  target_compile_definitions(RiskOfRainUpscaling.${NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(RiskOfRainUpscaling.${NAME} PRIVATE /W4 /permissive- /EHsc)
  target_include_directories(RiskOfRainUpscaling.${NAME} PRIVATE ../third_party/Streamline/include)
  target_link_libraries(RiskOfRainUpscaling.${NAME} PRIVATE ${ARGN})
endfunction()
native_test(GraphicsHooksTests ../tests/native/GraphicsHooksTests.cpp RiskOfRainUpscaling.Native d3d11 dxgi)
native_test(LoggingTests ../tests/native/LoggingTests.cpp RiskOfRainUpscaling.Native)
native_test(HudOpacityTests ../tests/native/HudOpacityTests.cpp RiskOfRainUpscaling.Native d3d11)
native_test(FramePacingTests ../tests/native/FramePacingTests.cpp RiskOfRainUpscaling.Native)
native_test(SceneInputsTests ../tests/native/SceneInputsTests.cpp RiskOfRainUpscaling.Native d3d11 dxgi user32)
native_test(DiagnosticsTests ../tests/native/DiagnosticsTests.cpp RiskOfRainUpscaling.Native d3d11 dxgi)
native_test(PipelineTests ../tests/native/PipelineTests.cpp RiskOfRainUpscaling.Native d3d11 dxgi user32)
native_test(RcasTests ../tests/native/RcasTests.cpp d3d11)
native_test(FrameBridgeTests ../tests/native/FrameBridgeTests.cpp RiskOfRainUpscaling.Native d3d11 d3d12 dxgi user32)
add_test(NAME Logging COMMAND RiskOfRainUpscaling.LoggingTests)
add_test(NAME HudOpacity COMMAND RiskOfRainUpscaling.HudOpacityTests)
add_test(NAME FramePacing COMMAND RiskOfRainUpscaling.FramePacingTests)
add_test(NAME GraphicsHooks COMMAND RiskOfRainUpscaling.GraphicsHooksTests --warp)

# Fake Unity exercises preloader-style initialization and prior hook chaining.
add_library(StartupFixtureUnity SHARED ../tests/startup/FixtureUnity.cpp)
set_target_properties(StartupFixtureUnity PROPERTIES OUTPUT_NAME UnityPlayer RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/startup-test)
target_compile_definitions(StartupFixtureUnity PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(StartupFixtureUnity PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(StartupFixtureUnity PRIVATE d3d11 dxgi)
add_executable(RiskOfRainUpscaling.StartupTests ../tests/startup/StartupTests.cpp)
set_target_properties(RiskOfRainUpscaling.StartupTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/startup-test)
target_compile_definitions(RiskOfRainUpscaling.StartupTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(RiskOfRainUpscaling.StartupTests PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(RiskOfRainUpscaling.StartupTests PRIVATE StartupFixtureUnity)
add_dependencies(RiskOfRainUpscaling.StartupTests RiskOfRainUpscaling.Native)
