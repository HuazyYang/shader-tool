#ifndef SHADER_TOOL_OPTIONS_H
#define SHADER_TOOL_OPTIONS_H

#include <string>
#include <utility>
#include <vector>

#include "CompilerLibrary.h"
#include "ContainerCompiler.h"

// Parsed, validated command line for one ShaderTool invocation.
// Defines are stored raw: values may still contain a single {a,b,c} variant
// group; the driver expands them into the cartesian variant set.
struct ShaderToolOptions {
    ShaderToolBackend Backend = ShaderToolBackend::Dxbc; // unused in container mode
    std::string BackendName;
    // Container mode ('container' / '--container' subcommand): the whole module
    // compiled into one NVRHI shader container; -E, -T and permutation axes do
    // not apply.
    bool Container = false;
    std::vector<ContainerTarget> ContainerTargets; // --container-targets
    std::string OptixIncludeDir;                   // --optix-include
    std::string ShaderModel = "6_5";               // --shader-model (DXIL)
    std::string Source;
    std::string EntryPoint = "main";
    std::string Profile;
    std::vector<std::pair<std::string, std::string>> Defines;
    // Global (unkeyed) defines (-GD): compiled into every variant, excluded
    // from permutation keys and from the raw/NVSP decision. No brace axes.
    // Values are normalized at parse: a valueless define becomes "1".
    std::vector<std::pair<std::string, std::string>> UnkeyedDefines;
    std::vector<std::string> IncludeDirs;
    std::vector<std::string> ExtraArgs;      // canonical dxc-style passthrough tokens
    std::string BinaryOutputPath;            // -Fo (exclusive with -Fh)
    std::string HeaderOutputPath;            // -Fh (requires -Vn)
    std::string HeaderVariableName;          // -Vn
    std::string DepfilePath;                 // -depfile (optional)

    bool HeaderOutput() const { return !HeaderOutputPath.empty(); }

    const std::string &OutputPath() const {
        return HeaderOutput() ? HeaderOutputPath : BinaryOutputPath;
    }
};

enum class OptionsParseStatus {
    Ok = 0,      // options is fully populated and validated
    Help = 1,    // usage was printed; exit 0
    Error = 2    // diagnostic was printed; exit 1
};

OptionsParseStatus ShaderToolOptionsParse(int argc, const char *const *argv,
                                          ShaderToolOptions &options);

#endif
