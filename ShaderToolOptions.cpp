#include "ShaderToolOptions.h"

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

enum BackendMask : uint32_t {
    MaskDxbc = 1u << 0,
    MaskDxil = 1u << 1,
    MaskSpirv = 1u << 2,
    MaskSlangDxbc = 1u << 3,
    MaskSlangDxil = 1u << 4,
    MaskSlangSpirv = 1u << 5,
    MaskContainer = 1u << 6,   // Slang module -> NVRHI shader container

    MaskDxbcClass = MaskDxbc | MaskSlangDxbc,               // SM 5_0/5_1 targets
    MaskDxcClass = MaskDxil | MaskSpirv,                    // native DXC backends
    MaskSpirvClass = MaskSpirv | MaskSlangSpirv,            // SPIR-V emitting backends
    MaskSlangClass = MaskSlangDxbc | MaskSlangDxil | MaskSlangSpirv,
    MaskSm6Class = MaskDxil | MaskSpirv | MaskSlangDxil | MaskSlangSpirv,
    MaskCompile = MaskDxbc | MaskDxil | MaskSpirv | MaskSlangClass, // one entry point
    MaskSpirvContainer = MaskSpirvClass | MaskContainer,    // the container's SPIR-V too
    MaskSlangContainer = MaskSlangClass | MaskContainer,
    MaskAll = MaskCompile | MaskContainer
};

enum class OptionKind {
    Group,
    Help,
    Depfile,
    Define,
    DefineGlobal,      // -GD: unkeyed define, applied to every variant
    Include,
    Entry,
    Profile,
    OutputBinary,
    OutputHeader,
    HeaderVariable,
    OptimizationLevel,
    ContainerTargets,  // --container-targets LIST
    OptixInclude,      // --optix-include DIR
    ShaderModel,       // --shader-model MAJOR_MINOR
    ForwardFlag,       // emit the option name alone
    ForwardSeparate,   // emit the option name, then each value as its own token
    ForwardJoined      // emit "name=value" as a single token
};

struct OptionSpec {
    OptionKind Kind;
    uint32_t Backends;
    const char *Name;         // null for group headers
    uint8_t NumValues;
    const char *ValueName;
    const char *Description;
};

struct BackendSpec {
    const char *Name;
    ShaderToolBackend Backend;
    uint32_t Mask;
    const char *Description;
};

#define GROUP(backends, title) \
    {OptionKind::Group, (backends), nullptr, 0, nullptr, (title)}

const OptionSpec OptionSpecs[] = {
    GROUP(MaskAll, "ShaderTool control"),
    {OptionKind::Help, MaskAll, "-h", 0, nullptr, "show this help"},
    {OptionKind::Help, MaskAll, "--help", 0, nullptr, "show this help"},
    {OptionKind::Depfile, MaskAll, "-depfile", 1, "PATH",
     "write a make-style dependency file"},

    GROUP(MaskAll, "Output selection"),
    {OptionKind::OutputBinary, MaskAll, "-Fo", 1, "FILE",
     "write the compiled artifact to a binary file"},
    {OptionKind::OutputHeader, MaskAll, "-Fh", 1, "FILE",
     "write the compiled artifact as a C header (requires -Vn)"},
    {OptionKind::HeaderVariable, MaskAll, "-Vn", 1, "NAME",
     "byte-array symbol name used with -Fh"},

    GROUP(MaskAll, "Preprocessor and include paths"),
    {OptionKind::Define, MaskAll, "-D", 1, "NAME[=VALUE]",
     "define a macro; -D NAME={a,b,c} adds a permutation axis"},
    {OptionKind::DefineGlobal, MaskAll, "-GD", 1, "NAME[=VALUE]",
     "define a macro for every variant without keying the permutation"},
    {OptionKind::Include, MaskAll, "-I", 1, "DIRECTORY", "add an include directory"},

    GROUP(MaskCompile, "Entry point and shader profile"),
    {OptionKind::Entry, MaskCompile, "-E", 1, "ENTRY", "entry point name (default: main)"},
    {OptionKind::Profile, MaskCompile, "-T", 1, "PROFILE",
     "full shader profile with model, e.g. ps_6_5 or vs_5_0 (required)"},

    GROUP(MaskContainer, "Shader container"),
    {OptionKind::ContainerTargets, MaskContainer, "--container-targets", 1, "LIST",
     "code targets, comma-separated: dxbc,dxil,spirv,ptx,optix (required)"},
    {OptionKind::OptixInclude, MaskContainer, "--optix-include", 1, "DIRECTORY",
     "OptiX SDK include directory for NVRTC (default: $OptiX_INSTALL_DIR/include)"},
    {OptionKind::ShaderModel, MaskContainer, "--shader-model", 1, "MAJOR_MINOR",
     "DXIL shader model, also used for the D3D registers of ptx/optix (default: 6_5)"},

    GROUP(MaskAll, "Optimization"),
    {OptionKind::OptimizationLevel, MaskAll, "-O0", 0, nullptr, "optimization level 0"},
    {OptionKind::OptimizationLevel, MaskAll, "-O1", 0, nullptr, "optimization level 1"},
    {OptionKind::OptimizationLevel, MaskAll, "-O2", 0, nullptr, "optimization level 2"},
    {OptionKind::OptimizationLevel, MaskAll, "-O3", 0, nullptr,
     "optimization level 3 (default)"},

    GROUP(MaskAll, "Language and validation"),
    {OptionKind::ForwardFlag, MaskAll, "-WX", 0, nullptr, "treat warnings as errors"},
    {OptionKind::ForwardFlag, MaskAll, "-Zpr", 0, nullptr,
     "pack matrices in row-major order"},
    {OptionKind::ForwardFlag, MaskAll, "-Zpc", 0, nullptr,
     "pack matrices in column-major order"},
    {OptionKind::ForwardFlag, MaskDxbc | MaskDxcClass, "-Gec", 0, nullptr,
     "enable backward-compatibility mode"},
    {OptionKind::ForwardFlag, MaskDxbc | MaskDxcClass, "-Ges", 0, nullptr,
     "enable strict mode"},
    {OptionKind::ForwardFlag, MaskDxbc | MaskDxcClass, "-Gis", 0, nullptr,
     "force IEEE strictness"},
    {OptionKind::ForwardFlag, MaskDxbc | MaskDxcClass, "-Vd", 0, nullptr,
     "disable validation"},
    {OptionKind::ForwardSeparate, MaskDxcClass, "-HV", 1, "YEAR",
     "HLSL language version (e.g. 2021)"},
    {OptionKind::ForwardFlag, MaskDxbc | MaskDxcClass, "-all-resources-bound", 0,
     nullptr, "assume all resources are bound"},
    {OptionKind::ForwardFlag, MaskSm6Class | MaskContainer, "-enable-16bit-types", 0, nullptr,
     "enable native 16-bit types (SM 6.2+)"},

    GROUP(MaskAll, "Debug information"),
    {OptionKind::ForwardFlag, MaskAll, "-Zi", 0, nullptr, "enable debug information"},
    // DXBC and slang embed debug info whenever -Zi is set, so they accept the flag as a no-op
    // (CompilerLibrary.cpp); EMBED_PDB passes it to every backend (Aftermath builds D3D11 shaders too).
    {OptionKind::ForwardFlag, MaskAll, "-Qembed_debug", 0, nullptr,
     "embed debug information in the shader"},
    {OptionKind::ForwardFlag, MaskSlangContainer, "-g", 0, nullptr,
     "emit debug information (slang)"},

    GROUP(MaskSpirvContainer, "SPIR-V binding remapping"),
    {OptionKind::ForwardSeparate, MaskSpirvContainer, "-fvk-b-shift", 2, "SHIFT SPACE",
     "shift constant-buffer bindings"},
    {OptionKind::ForwardSeparate, MaskSpirvContainer, "-fvk-s-shift", 2, "SHIFT SPACE",
     "shift sampler bindings"},
    {OptionKind::ForwardSeparate, MaskSpirvContainer, "-fvk-t-shift", 2, "SHIFT SPACE",
     "shift texture bindings"},
    {OptionKind::ForwardSeparate, MaskSpirvContainer, "-fvk-u-shift", 2, "SHIFT SPACE",
     "shift UAV bindings"},
    {OptionKind::ForwardSeparate, MaskSpirvClass, "-fvk-bind-register", 4,
     "TYPE# SPACE BINDING SET", "remap a single register"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fvk-auto-shift-bindings", 0, nullptr,
     "automatically shift Vulkan bindings"},

    GROUP(MaskSpirvContainer, "SPIR-V memory layout and semantics"),
    {OptionKind::ForwardFlag, MaskSpirvContainer, "-fvk-use-dx-layout", 0, nullptr,
     "use DirectX memory layout"},
    {OptionKind::ForwardFlag, MaskSpirvContainer, "-fvk-use-gl-layout", 0, nullptr,
     "use OpenGL std140/std430 memory layout"},
    {OptionKind::ForwardFlag, MaskSpirvContainer, "-fvk-use-scalar-layout", 0, nullptr,
     "use scalar memory layout"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fvk-use-dx-position-w", 0, nullptr,
     "reciprocate SV_Position.w like DirectX"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fvk-invert-y", 0, nullptr,
     "invert the Vulkan Y axis"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fvk-support-nonzero-base-instance", 0,
     nullptr, "support nonzero base instance"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fvk-support-nonzero-base-vertex", 0,
     nullptr, "support nonzero base vertex"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-use-legacy-buffer-matrix-order", 0,
     nullptr, "use legacy buffer matrix order"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-use-unknown-image-format", 0,
     nullptr, "allow unknown storage image formats"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-use-vulkan-memory-model", 0,
     nullptr, "use the Vulkan memory model"},

    GROUP(MaskSpirvContainer, "SPIR-V code generation"),
    {OptionKind::ForwardJoined, MaskSpirvContainer, "-fspv-target-env", 1, "ENVIRONMENT",
     "SPIR-V target environment, e.g. vulkan1.2"},
    {OptionKind::ForwardJoined, MaskSpirvContainer, "-fspv-extension", 1, "EXTENSION",
     "enable a SPIR-V extension (repeatable)"},
    {OptionKind::ForwardJoined, MaskSpirvClass, "-fspv-debug", 1, "MODE",
     "SPIR-V debug information mode"},
    {OptionKind::ForwardJoined, MaskSpirvClass, "-fspv-entrypoint-name", 1, "NAME",
     "override the SPIR-V entry-point name"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-reflect", 0, nullptr,
     "emit reflection decorations"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-reduce-load-size", 0, nullptr,
     "reduce SPIR-V load size"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-flatten-resource-arrays", 0,
     nullptr, "flatten resource arrays"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-preserve-bindings", 0, nullptr,
     "preserve resource bindings"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-preserve-interface", 0, nullptr,
     "preserve the entry-point interface"},
    {OptionKind::ForwardFlag, MaskSpirvClass, "-fspv-enable-maximal-reconvergence", 0,
     nullptr, "enable maximal reconvergence"},
    {OptionKind::ForwardJoined, MaskSpirv, "-Oconfig", 1, "PASSES",
     "custom SPIR-V optimization pass list"},

    GROUP(MaskSlangContainer, "Slang options"),
    {OptionKind::ForwardSeparate, MaskSlangContainer, "-lang", 1, "LANGUAGE",
     "source language, e.g. hlsl or slang"},
    {OptionKind::ForwardSeparate, MaskSlangContainer, "-capability", 1, "CAPABILITY",
     "add a capability atom (repeatable)"},
    {OptionKind::ForwardFlag, MaskSlangContainer, "-unscoped-enum", 0, nullptr,
     "treat enums as unscoped"}
};

#undef GROUP

// The container entry's Backend value is unused (ShaderToolOptions::Container).
const BackendSpec BackendSpecs[] = {
    {"dxbc", ShaderToolBackend::Dxbc, MaskDxbc,
     "compile DXBC with D3DCompile (Shader Model 5_0/5_1)"},
    {"dxil", ShaderToolBackend::Dxil, MaskDxil,
     "compile DXIL with DXC (Shader Model 6_0 or newer)"},
    {"spirv", ShaderToolBackend::Spirv, MaskSpirv,
     "compile SPIR-V with DXC (Shader Model 6_0 or newer)"},
    {"slang-dxbc", ShaderToolBackend::SlangDxbc, MaskSlangDxbc,
     "compile DXBC with Slang (Shader Model 5_0/5_1)"},
    {"slang-dxil", ShaderToolBackend::SlangDxil, MaskSlangDxil,
     "compile DXIL with Slang (Shader Model 6_0 or newer)"},
    {"slang-spirv", ShaderToolBackend::SlangSpirv, MaskSlangSpirv,
     "compile SPIR-V with Slang (Shader Model 6_0 or newer)"},
    {"container", ShaderToolBackend::SlangDxil, MaskContainer,
     "compile a Slang module into an NVRHI shader container (.nvsc)"}
};

const BackendSpec *FindBackend(const char *name) {
    if (!strcmp(name, "--container"))
        name = "container";
    for (const BackendSpec &backend : BackendSpecs)
        if (!strcmp(name, backend.Name))
            return &backend;
    return nullptr;
}

void PrintBackends(FILE *stream) {
    fprintf(stream, "Backends:\n");
    for (const BackendSpec &backend : BackendSpecs)
        fprintf(stream, "  %-12s %s\n", backend.Name, backend.Description);
}

void PrintGlobalUsage(const char *program) {
    printf("Usage: %s <backend> [options] <source>\n"
           "       %s container --container-targets <list> [options] <source.slang>\n\n",
           program, program);
    PrintBackends(stdout);
    printf(
        "\nRequired options:\n"
        "  -T <profile>                 full shader profile, e.g. ps_6_5 or vs_5_0\n"
        "                               (not for 'container')\n"
        "  -Fo <file> | -Fh <file> -Vn <symbol>\n"
        "                               binary or C-header output (exactly one)\n"
        "\nThe single source file must be the last argument. -D NAME={a,b,c} defines\n"
        "a permutation axis; all axes combine into one NVSP blob artifact.\n"
        "'container' (also spelled '--container') compiles every [shader(...)] entry\n"
        "point of the module for each listed target (dxbc, dxil, spirv, ptx, optix)\n"
        "into one container with the code and reflection of each target.\n"
        "Run '%s <backend> -h' for the options of one backend.\n",
        program);
}

void PrintBackendUsage(const char *program, const BackendSpec &backend) {
    const size_t count = sizeof(OptionSpecs) / sizeof(OptionSpecs[0]);
    size_t i = 0;
    printf("Usage: %s %s [options] <source>\n", program, backend.Name);
    while (i < count) {
        const OptionSpec &group = OptionSpecs[i++];
        bool printed_group = false;
        while (i < count && OptionSpecs[i].Kind != OptionKind::Group) {
            const OptionSpec &spec = OptionSpecs[i++];
            if (!(group.Backends & spec.Backends & backend.Mask))
                continue;
            if (!printed_group) {
                printf("\n%s:\n", group.Description);
                printed_group = true;
            }
            if (spec.ValueName)
                printf("  %-22s %-22s %s\n", spec.Name, spec.ValueName,
                       spec.Description);
            else
                printf("  %-45s %s\n", spec.Name, spec.Description);
        }
    }
    printf("\nSingle-value options also accept joined forms: -X=value or -Xvalue.\n");
}

struct OptionMatch {
    const OptionSpec *Spec = nullptr;
    const char *Inline = nullptr;   // joined/attached value for single-value options
};

// Longest-name match wins, so e.g. "-fvk-use-dx-position-w" is never taken as an
// attached-value spelling of a shorter option.
bool MatchOption(const char *argument, OptionMatch &match) {
    size_t best_length = 0;
    for (const OptionSpec &spec : OptionSpecs) {
        if (spec.Kind == OptionKind::Group)
            continue;
        const size_t name_length = strlen(spec.Name);
        if (strncmp(argument, spec.Name, name_length) != 0)
            continue;
        const char *remainder = argument + name_length;
        const char *inline_value = nullptr;
        if (*remainder != 0) {
            if (spec.NumValues != 1)
                continue;
            inline_value = (*remainder == '=') ? remainder + 1 : remainder;
        }
        if (name_length > best_length) {
            best_length = name_length;
            match.Spec = &spec;
            match.Inline = inline_value;
        }
    }
    return match.Spec != nullptr;
}

void Error(const char *format, ...);

void Error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("ShaderTool: error: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

bool IsUnsignedNumber(const std::string &text) {
    if (text.empty())
        return false;
    for (char character : text)
        if (!isdigit(static_cast<unsigned char>(character)))
            return false;
    return true;
}

// --container-targets: comma-separated, known names, no duplicates.
int ParseContainerTargets(const std::string &list, std::vector<ContainerTarget> &targets) {
    targets.clear();
    size_t start = 0;
    while (true) {
        const size_t comma = list.find(',', start);
        const std::string name =
            list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        ContainerTarget target;
        if (!ParseContainerTarget(name, target)) {
            Error("unknown container target '%s'; expected dxbc, dxil, spirv, ptx or optix",
                  name.c_str());
            return -1;
        }
        for (ContainerTarget previous : targets) {
            if (previous == target) {
                Error("container target '%s' is listed twice", name.c_str());
                return -1;
            }
        }
        targets.push_back(target);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return 0;
}

// --shader-model: <major>_<minor>, 6_0 or newer (it selects the DXIL profile sm_<model>).
int ValidateShaderModel(const std::string &model) {
    const size_t underscore = model.find('_');
    if (underscore == std::string::npos || !IsUnsignedNumber(model.substr(0, underscore)) ||
        !IsUnsignedNumber(model.substr(underscore + 1))) {
        Error("invalid shader model '%s'; expected <major>_<minor>, e.g. 6_5", model.c_str());
        return -1;
    }
    if (strtoul(model.c_str(), nullptr, 10) < 6) {
        Error("the container's shader model must be 6_0 or newer (got '%s'); the dxbc "
              "target always uses 5_0", model.c_str());
        return -1;
    }
    return 0;
}

bool IsRegisterShiftOption(const OptionSpec &spec) {
    return spec.Kind == OptionKind::ForwardSeparate && spec.NumValues == 2 &&
           !strncmp(spec.Name, "-fvk-", 5);
}

bool ContainsBrace(const std::string &text) {
    return text.find_first_of("{}") != std::string::npos;
}

bool ContainsWhitespace(const std::string &text) {
    for (char character : text)
        if (isspace(static_cast<unsigned char>(character)))
            return true;
    return false;
}

int ValidateProfile(const ShaderToolOptions &options, uint32_t backend_mask) {
    const std::string &profile = options.Profile;
    const size_t first = profile.find('_');
    const size_t second =
        first == std::string::npos ? std::string::npos : profile.find('_', first + 1);
    std::string stage, major_text, minor_text;
    unsigned long major = 0, minor = 0;

    if (first == std::string::npos || first == 0 || second == std::string::npos ||
        second == first + 1 || second + 1 == profile.size()) {
        Error("invalid shader profile '%s'; expected <stage>_<major>_<minor>, "
              "e.g. ps_6_5", profile.c_str());
        return -1;
    }
    stage = profile.substr(0, first);
    major_text = profile.substr(first + 1, second - first - 1);
    minor_text = profile.substr(second + 1);
    if (!IsUnsignedNumber(major_text) || !IsUnsignedNumber(minor_text)) {
        Error("invalid shader profile '%s'; the shader model must be numeric",
              profile.c_str());
        return -1;
    }
    major = strtoul(major_text.c_str(), nullptr, 10);
    minor = strtoul(minor_text.c_str(), nullptr, 10);

    if (backend_mask & MaskDxbcClass) {
        if (stage == "lib" || stage == "ms" || stage == "as") {
            Error("profile '%s' is not supported by the '%s' backend; library, mesh, "
                  "and amplification shaders require a DXIL-class backend",
                  profile.c_str(), options.BackendName.c_str());
            return -1;
        }
        if (major != 5 || minor > 1) {
            Error("the '%s' backend requires shader model 5_0 or 5_1 (got '%s')",
                  options.BackendName.c_str(), profile.c_str());
            return -1;
        }
    } else if (major < 6) {
        Error("the '%s' backend requires shader model 6_0 or newer (got '%s')",
              options.BackendName.c_str(), profile.c_str());
        return -1;
    }
    return 0;
}

// Braces are reserved for -D permutation values; catch them anywhere else so a
// misplaced permutation axis fails loudly instead of reaching the compiler.
int RejectStrayBraces(const ShaderToolOptions &options) {
    bool found = ContainsBrace(options.Source) || ContainsBrace(options.EntryPoint) ||
                 ContainsBrace(options.Profile) ||
                 ContainsBrace(options.HeaderVariableName) ||
                 ContainsBrace(options.BinaryOutputPath) ||
                 ContainsBrace(options.HeaderOutputPath) ||
                 ContainsBrace(options.DepfilePath);
    for (const std::string &directory : options.IncludeDirs)
        found = found || ContainsBrace(directory);
    for (const std::string &argument : options.ExtraArgs)
        found = found || ContainsBrace(argument);
    for (const auto &define : options.Defines)
        found = found || ContainsBrace(define.first);
    if (found) {
        Error("permutation braces are only supported in -D values; split into "
              "separate config rows");
        return -1;
    }
    return 0;
}

} // namespace

OptionsParseStatus ShaderToolOptionsParse(int argc, const char *const *argv,
                                          ShaderToolOptions &options) {
    const char *program = (argc > 0 && argv[0] && *argv[0]) ? argv[0] : "ShaderTool";
    for (const char *cursor = program; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\')
            program = cursor + 1;
    if (!*program)
        program = "ShaderTool";

    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        PrintGlobalUsage(program);
        return OptionsParseStatus::Help;
    }
    if (argc < 2) {
        Error("a backend subcommand is required; run '%s --help'", program);
        return OptionsParseStatus::Error;
    }

    const BackendSpec *backend = FindBackend(argv[1]);
    if (!backend) {
        Error("unknown backend '%s'", argv[1]);
        PrintBackends(stderr);
        return OptionsParseStatus::Error;
    }
    options.Backend = backend->Backend;
    options.BackendName = backend->Name;
    options.Container = (backend->Mask & MaskContainer) != 0;

    int optimization_level = 3;
    bool seen_entry = false, seen_profile = false, seen_variable = false;
    bool seen_binary = false, seen_header = false, seen_depfile = false;
    bool seen_targets = false, seen_optix = false, seen_model = false;

    int index = 2;
    while (index < argc) {
        const char *argument = argv[index++];

        if (argument[0] != '-') {
            if (!options.Source.empty() || index != argc) {
                Error("exactly one source file must be the final argument");
                return OptionsParseStatus::Error;
            }
            options.Source = argument;
            continue;
        }

        OptionMatch match;
        if (!MatchOption(argument, match)) {
            Error("unknown option '%s'; run '%s %s -h'", argument, program,
                  backend->Name);
            return OptionsParseStatus::Error;
        }
        const OptionSpec &spec = *match.Spec;
        if (!(spec.Backends & backend->Mask)) {
            Error("option '%s' is not supported by the '%s' backend", spec.Name,
                  backend->Name);
            return OptionsParseStatus::Error;
        }

        std::vector<std::string> values;
        if (spec.NumValues == 1) {
            if (match.Inline) {
                if (!*match.Inline) {
                    Error("option '%s' requires a value", spec.Name);
                    return OptionsParseStatus::Error;
                }
                values.emplace_back(match.Inline);
            } else {
                if (index >= argc) {
                    Error("option '%s' requires a value (%s)", spec.Name,
                          spec.ValueName ? spec.ValueName : "VALUE");
                    return OptionsParseStatus::Error;
                }
                values.emplace_back(argv[index++]);
            }
        } else if (spec.NumValues > 1) {
            if (argc - index < spec.NumValues) {
                Error("option '%s' requires %u values (%s)", spec.Name,
                      (unsigned)spec.NumValues,
                      spec.ValueName ? spec.ValueName : "VALUES");
                return OptionsParseStatus::Error;
            }
            for (uint8_t value_index = 0; value_index < spec.NumValues; ++value_index)
                values.emplace_back(argv[index++]);
        }

        if (IsRegisterShiftOption(spec) &&
            (!IsUnsignedNumber(values[0]) ||
             (values[1] != "all" && !IsUnsignedNumber(values[1])))) {
            Error("option '%s' expects '<shift> <space>' with a numeric shift and a "
                  "numeric space or 'all'", spec.Name);
            return OptionsParseStatus::Error;
        }

        switch (spec.Kind) {
        case OptionKind::Group:
            break;
        case OptionKind::Help:
            PrintBackendUsage(program, *backend);
            return OptionsParseStatus::Help;
        case OptionKind::Depfile:
            if (seen_depfile) {
                Error("-depfile may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_depfile = true;
            options.DepfilePath = values[0];
            break;
        case OptionKind::Define: {
            const size_t equals = values[0].find('=');
            std::string name = values[0].substr(0, equals);
            std::string value =
                equals == std::string::npos ? std::string() : values[0].substr(equals + 1);
            if (name.empty() || ContainsWhitespace(name) ||
                ContainsWhitespace(value)) {
                Error("invalid define '%s'; expected NAME, NAME=VALUE, or "
                      "NAME={a,b,c} without whitespace", values[0].c_str());
                return OptionsParseStatus::Error;
            }
            options.Defines.emplace_back(std::move(name), std::move(value));
            break;
        }
        case OptionKind::DefineGlobal: {
            const size_t equals = values[0].find('=');
            std::string name = values[0].substr(0, equals);
            std::string value =
                equals == std::string::npos ? std::string("1") : values[0].substr(equals + 1);
            if (name.empty() || ContainsWhitespace(name) || ContainsWhitespace(value) ||
                ContainsBrace(name) || ContainsBrace(value) || value.empty()) {
                Error("invalid global define '%s'; expected NAME or NAME=VALUE without "
                      "whitespace or braces", values[0].c_str());
                return OptionsParseStatus::Error;
            }
            options.UnkeyedDefines.emplace_back(std::move(name), std::move(value));
            break;
        }
        case OptionKind::Include:
            options.IncludeDirs.push_back(values[0]);
            break;
        case OptionKind::Entry:
            if (seen_entry) {
                Error("-E may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_entry = true;
            options.EntryPoint = values[0];
            break;
        case OptionKind::Profile:
            if (seen_profile) {
                Error("-T may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_profile = true;
            options.Profile = values[0];
            break;
        case OptionKind::OutputBinary:
            if (seen_binary) {
                Error("-Fo may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_binary = true;
            options.BinaryOutputPath = values[0];
            break;
        case OptionKind::OutputHeader:
            if (seen_header) {
                Error("-Fh may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_header = true;
            options.HeaderOutputPath = values[0];
            break;
        case OptionKind::HeaderVariable:
            if (seen_variable) {
                Error("-Vn may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_variable = true;
            options.HeaderVariableName = values[0];
            break;
        case OptionKind::OptimizationLevel:
            optimization_level = spec.Name[2] - '0';
            break;
        case OptionKind::ContainerTargets:
            if (seen_targets) {
                Error("--container-targets may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_targets = true;
            if (ParseContainerTargets(values[0], options.ContainerTargets))
                return OptionsParseStatus::Error;
            break;
        case OptionKind::OptixInclude:
            if (seen_optix) {
                Error("--optix-include may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_optix = true;
            options.OptixIncludeDir = values[0];
            break;
        case OptionKind::ShaderModel:
            if (seen_model) {
                Error("--shader-model may be specified at most once");
                return OptionsParseStatus::Error;
            }
            seen_model = true;
            if (ValidateShaderModel(values[0]))
                return OptionsParseStatus::Error;
            options.ShaderModel = values[0];
            break;
        case OptionKind::ForwardFlag:
            options.ExtraArgs.emplace_back(spec.Name);
            break;
        case OptionKind::ForwardSeparate:
            options.ExtraArgs.emplace_back(spec.Name);
            for (std::string &value : values)
                options.ExtraArgs.push_back(std::move(value));
            break;
        case OptionKind::ForwardJoined:
            options.ExtraArgs.push_back(std::string(spec.Name) + "=" + values[0]);
            break;
        }
    }

    if (options.Source.empty()) {
        Error("exactly one source file is required as the final argument");
        return OptionsParseStatus::Error;
    }
    if (options.Container && !seen_targets) {
        Error("--container-targets <list> is required in container mode");
        return OptionsParseStatus::Error;
    }
    if (!options.Container && !seen_profile) {
        Error("-T <profile> is required");
        return OptionsParseStatus::Error;
    }
    if (seen_binary == seen_header) {
        Error("exactly one of -Fo <file> or -Fh <file> is required");
        return OptionsParseStatus::Error;
    }
    if (seen_header && !seen_variable) {
        Error("-Fh requires -Vn <symbol>");
        return OptionsParseStatus::Error;
    }
    if (seen_variable && !seen_header) {
        Error("-Vn is only valid together with -Fh");
        return OptionsParseStatus::Error;
    }
    if (options.EntryPoint.empty()) {
        Error("-E requires a non-empty entry point name");
        return OptionsParseStatus::Error;
    }
    if (!options.Container && ValidateProfile(options, backend->Mask))
        return OptionsParseStatus::Error;
    if (RejectStrayBraces(options))
        return OptionsParseStatus::Error;

    char level_token[4] = {'-', 'O', (char)('0' + optimization_level), 0};
    options.ExtraArgs.emplace_back(level_token);

    return OptionsParseStatus::Ok;
}
