# ShaderTool — in-memory shader compiler driver (dxc / d3dcompiler / slang).
# NOTE: the Visual Studio generator needs CMake >= 3.21 for DEPFILE support in
# shadertool_add_shader_objects(); Ninja works with the minimum below.
cmake_minimum_required(VERSION 3.20)

project(ShaderTool VERSION 2.0.0 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# Vendored compiler packages: extraction + IMPORTED targets
# (ShaderTool::dxcompiler, ShaderTool::d3dcompiler, ShaderTool::slang) and
# shadertool_copy_runtime_libraries().
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolDependencies.cmake")

# ---------------------------------------------------------------------------
# ShaderToolBlob — pure C11 static library (NVSP blob read/write + key policy).
# Deliberately no MSVC_RUNTIME_LIBRARY property: consumers control their CRT.
# ---------------------------------------------------------------------------
add_library(ShaderToolBlob STATIC
    ShaderToolBlob.c
    ShaderToolBlob.h)
add_library(ShaderTool::Blob ALIAS ShaderToolBlob)
target_compile_features(ShaderToolBlob PRIVATE c_std_11)
set_target_properties(ShaderToolBlob PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME Blob) # exported/installed as ShaderTool::Blob
target_include_directories(ShaderToolBlob PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# ---------------------------------------------------------------------------
# ShaderTool executable. ShaderToolBlob.c is compiled into the executable
# directly so the static-CRT choice below stays private to the exe (the blob
# STATIC library keeps the default runtime for consumers).
# ---------------------------------------------------------------------------
add_executable(ShaderTool
    ShaderTool.cpp
    ShaderToolOptions.cpp
    CompilerLibrary.cpp
    ContainerCompiler.cpp
    ContainerWriter.cpp
    Depfile.cpp
    HeaderOutput.cpp
    ShaderToolBlob.c
    ShaderToolOptions.h
    CompilerLibrary.h
    ContainerCompiler.h
    ContainerWriter.h
    Depfile.h
    HeaderOutput.h
    ShaderToolBlob.h
    include/nvrhi-shader-container.h)
add_executable(ShaderTool::ShaderTool ALIAS ShaderTool)
target_compile_features(ShaderTool PRIVATE cxx_std_17 c_std_11)
# include/nvrhi-shader-container.h: byte-identical copy of NVRHI's
# <nvrhi/shader-container.h> (the container format; the aggregate checks the copies).
target_include_directories(ShaderTool PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ShaderTool PRIVATE
    ShaderTool::dxcompiler
    ShaderTool::slang)
if(WIN32)
    # The DXBC backend (D3DCompile) is Windows-only; sources guard it with
    # #ifdef _WIN32.
    target_link_libraries(ShaderTool PRIVATE ShaderTool::d3dcompiler)
    target_compile_definitions(ShaderTool PRIVATE
        WIN32_LEAN_AND_MEAN UNICODE NOMINMAX)
    # dxcompiler/slang are DLLs with their own CRT, so a static CRT for the
    # exe is safe and keeps the tool self-contained. The debug CRT in Debug:
    # a parent project may define _DEBUG there (Donut does), and the STL then
    # needs the debug CRT's functions (_CrtDbgReport, _malloc_dbg, ...).
    set_target_properties(ShaderTool PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

shadertool_copy_runtime_libraries(ShaderTool)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
endif()
add_executable(ShaderToolBlobTests tests/BlobTests.c)
target_link_libraries(ShaderToolBlobTests PRIVATE ShaderTool::Blob)
add_test(NAME ShaderToolBlobTests COMMAND ShaderToolBlobTests)

# Shader container tests (standalone builds only: they use
# shadertool_add_shader_containers(), whose module a parent project includes
# itself). The containers are built by a ctest fixture so that the default
# build needs neither NVRTC nor the OptiX SDK; ptx/optix are tested when
# OptiX_INSTALL_DIR is set (NVRTC must then be on PATH as well).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolFunctions.cmake")

    if(DEFINED ENV{OptiX_INSTALL_DIR})
        set(_shadertool_test_targets_default "ptx;optix;dxil;spirv")
    else()
        set(_shadertool_test_targets_default "dxil;spirv")
    endif()
    set(SHADERTOOL_CONTAINER_TEST_TARGETS "${_shadertool_test_targets_default}"
        CACHE STRING "Container targets compiled by the ShaderTool container tests")

    add_executable(ShaderToolContainerTests tests/ContainerTests.cpp)
    target_compile_features(ShaderToolContainerTests PRIVATE cxx_std_17)
    target_include_directories(ShaderToolContainerTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

    set(_shadertool_test_dir "${CMAKE_CURRENT_BINARY_DIR}/container-tests")
    shadertool_add_shader_containers(
        TARGET ShaderToolContainerTestShaders
        SOURCES tests/container/compute.slang tests/container/raytracing.slang
        BASE_DIRECTORY tests/container
        TARGETS ${SHADERTOOL_CONTAINER_TEST_TARGETS}
        OUTPUT_DIR "${_shadertool_test_dir}")

    add_test(NAME ShaderToolContainerBuild
        COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
            --target ShaderToolContainerTestShaders --config "$<CONFIG>")
    set_tests_properties(ShaderToolContainerBuild PROPERTIES
        FIXTURES_SETUP ShaderToolContainers)
    string(REPLACE ";" "," _shadertool_test_target_list "${SHADERTOOL_CONTAINER_TEST_TARGETS}")
    add_test(NAME ShaderToolContainerTests
        COMMAND ShaderToolContainerTests
            --targets "${_shadertool_test_target_list}"
            --compute "${_shadertool_test_dir}/compute.nvsc"
            --raytracing "${_shadertool_test_dir}/raytracing.nvsc")
    set_tests_properties(ShaderToolContainerTests PROPERTIES
        FIXTURES_REQUIRED ShaderToolContainers)
endif()

# ---------------------------------------------------------------------------
# Package config (build tree + install tree)
# ---------------------------------------------------------------------------
set(ShaderTool_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ShaderTool")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolConfig.cmake"
    INSTALL_DESTINATION "${ShaderTool_CMAKE_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolFunctions.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolFunctions.cmake"
    COPYONLY)
# Build-tree copy of the dependencies module: a wrapper that pins the package
# directory to this source tree (the installed copy locates the packages
# relative to itself instead).
set(_shadertool_source_thirdparty "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
set(_shadertool_source_deps_module
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolDependencies.cmake")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolDependencies.cmake"
    CONTENT [[if(NOT SHADERTOOL_THIRDPARTY_PACKAGE_DIR)
    set(SHADERTOOL_THIRDPARTY_PACKAGE_DIR "@_shadertool_source_thirdparty@")
endif()
include("@_shadertool_source_deps_module@")
]] @ONLY)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS ShaderTool ShaderToolBlob EXPORT ShaderToolTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES ShaderToolBlob.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# Make config-mode discovery work directly from the build tree as well as
# from an installed prefix (useful to parent projects that vendor ShaderTool).
export(EXPORT ShaderToolTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolTargets.cmake"
    NAMESPACE ShaderTool::)
install(EXPORT ShaderToolTargets
    FILE ShaderToolTargets.cmake
    NAMESPACE ShaderTool::
    DESTINATION "${ShaderTool_CMAKE_DIR}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolConfigVersion.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolFunctions.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolDependencies.cmake"
    DESTINATION "${ShaderTool_CMAKE_DIR}")

# The vendored packages travel with the installed package so the installed
# ShaderToolDependencies.cmake can re-extract them in consumer build trees.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/VERSIONS"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxc-win-x64.tgz"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxc-linux-x64.tgz"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/d3dcompiler-win-x64.tgz"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/slang-win-x64.tgz"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/slang-linux-x64.tgz"
    DESTINATION "share/ShaderTool/thirdparty")

# Runtime libraries land next to the installed executable too (the exe's
# INSTALL_RPATH is $ORIGIN on non-Windows platforms).
get_property(_shadertool_runtime_libraries GLOBAL PROPERTY
    SHADERTOOL_RUNTIME_LIBRARIES)
get_property(_shadertool_slang_module_dir GLOBAL PROPERTY
    SHADERTOOL_SLANG_STANDARD_MODULE_DIR)
install(PROGRAMS ${_shadertool_runtime_libraries}
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${_shadertool_slang_module_dir}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
