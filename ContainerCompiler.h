#ifndef SHADER_TOOL_CONTAINER_COMPILER_H
#define SHADER_TOOL_CONTAINER_COMPILER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/* Offline compilation of one Slang module into an NVRHI shader container
   (include/nvrhi-shader-container.h): one CODE and one REFLECTION section per target,
   module-level (every [shader(...)] entry point of the module that the target can hold).
   Uses the modern Slang API (slang::IGlobalSession / ISession / ProgramLayout) and is
   independent of the CompilerLibraryCompile seam. 0 on success, -1 on failure;
   Diagnostics is filled either way when non-empty. No exceptions escape this API. */

enum class ContainerTarget {
    Dxbc,     /* NVRHI_SC_TARGET_DXBC: one VS/HS/DS/GS/PS/CS entry point, shader model 5_0 */
    Dxil,     /* NVRHI_SC_TARGET_DXIL: one non-ray-tracing entry point, or a library of
                 ray-tracing entry points */
    Spirv,    /* NVRHI_SC_TARGET_SPIRV: every entry point in one module */
    CudaPtx,  /* NVRHI_SC_TARGET_CUDA_PTX: the compute entry points as CUDA kernels */
    OptixPtx  /* NVRHI_SC_TARGET_OPTIX_PTX: the ray-tracing entry points as OptiX programs */
};

/* CLI spellings: dxbc, dxil, spirv, ptx, optix. */
const char *ContainerTargetName(ContainerTarget target);
bool ParseContainerTarget(const std::string &name, ContainerTarget &target);

struct ContainerCompileRequest {
    std::string Source;                                       // path as given
    std::vector<std::pair<std::string, std::string>> Defines; // valueless normalized to "1"
    std::vector<std::string> IncludeDirs;
    std::vector<std::string> ExtraArgs;   // canonical dxc-style tokens from the options layer
    std::vector<ContainerTarget> Targets; // section order, no duplicates
    std::string ShaderModel = "6_5";      // DXIL (and the D3D register layout of PTX targets)
    std::string OptixIncludeDir;          // OptiX SDK include directory; empty: the
                                          // OptiX_INSTALL_DIR environment variable + /include
};

struct ContainerCompileResult {
    std::vector<uint8_t> Container;       // the .nvsc bytes
    std::vector<std::string> Includes;    // files the module depends on (depfile input)
    std::string Diagnostics;              // UTF-8 warnings/errors
};

int CompileShaderContainer(const ContainerCompileRequest &request,
                           ContainerCompileResult &result);

#endif
