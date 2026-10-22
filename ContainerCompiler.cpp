/* ContainerCompiler.cpp -- one Slang module -> one NVRHI shader container (ContainerCompiler.h):
     [Arguments]   canonical dxc-style tokens -> slangc tokens -> slang::CompilerOptionEntry
     [Code]        entry-point symbols in PTX and SPIR-V, the PTX global-parameter block
     [Reflection]  slang::ProgramLayout -> ContainerReflection (parameters, types, entry points)
     [Sessions]    a discovery session, then one session per container target
   Uses the modern Slang API of the vendored slang (pinned 2026.10.2, thirdparty/VERSIONS);
   the other ShaderTool backends keep the sp* C API in CompilerLibrary.cpp. */

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include "ContainerCompiler.h"

#include "ContainerWriter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <set>
#include <system_error>

#include <slang-com-ptr.h>
#include <slang.h>

namespace {

using Slang::ComPtr;

constexpr uint32_t kInvalidIndex = NVRHI_SC_INVALID_INDEX;

struct TargetInfo {
    ContainerTarget Target;
    const char *Name;
    uint32_t SectionTarget; /* NVRHI_SC_TARGET_* */
};

const TargetInfo TargetInfos[] = {
    {ContainerTarget::Dxbc, "dxbc", NVRHI_SC_TARGET_DXBC},
    {ContainerTarget::Dxil, "dxil", NVRHI_SC_TARGET_DXIL},
    {ContainerTarget::Spirv, "spirv", NVRHI_SC_TARGET_SPIRV},
    {ContainerTarget::CudaPtx, "ptx", NVRHI_SC_TARGET_CUDA_PTX},
    {ContainerTarget::OptixPtx, "optix", NVRHI_SC_TARGET_OPTIX_PTX},
};

const TargetInfo &InfoOf(ContainerTarget target) {
    for (const TargetInfo &info : TargetInfos)
        if (info.Target == target)
            return info;
    return TargetInfos[0];
}

bool IsPtx(ContainerTarget target) {
    return target == ContainerTarget::CudaPtx || target == ContainerTarget::OptixPtx;
}

bool StartsWith(const std::string &text, const char *prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string BlobText(slang::IBlob *blob) {
    if (!blob || !blob->getBufferSize())
        return std::string();
    std::string text(static_cast<const char *>(blob->getBufferPointer()), blob->getBufferSize());
    while (!text.empty() && text.back() == '\0')
        text.pop_back();
    return text;
}

/* Collects Slang diagnostics and ShaderTool errors. Every target session parses the module
   again, so identical front-end diagnostics would repeat once per target: each distinct
   Slang message is kept once. */
class DiagnosticSink {
public:
    explicit DiagnosticSink(std::string &text) : Text(text) {}

    void Add(slang::IBlob *blob) {
        const std::string text = BlobText(blob);
        if (text.empty() || !Seen.insert(text).second)
            return;
        Text += text;
        if (Text.back() != '\n')
            Text += '\n';
    }

    void Error(const std::string &message) {
        Text += "ShaderTool: ";
        Text += message;
        Text += '\n';
    }

private:
    std::string &Text;
    std::set<std::string> Seen;
};

slang::IGlobalSession *GetGlobalSession() {
    static ComPtr<slang::IGlobalSession> session;
    if (!session)
        slang::createGlobalSession(session.writeRef());
    return session.get();
}

uint32_t SizeToU32(size_t value) {
    if (value == SLANG_UNBOUNDED_SIZE || value == SLANG_UNKNOWN_SIZE)
        return 0;
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    if (alignment <= 1)
        return value;
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

#pragma region[ Arguments ]

namespace {

/* slangc spellings of the canonical tokens. Session-wide options go to every session; the
   SPIR-V ones only to SPIR-V targets (as target options): Slang applies register shifts
   to every target of a session, D3D register layouts included, when they are session
   options. */
struct SlangArguments {
    std::vector<std::string> Common;
    std::vector<std::string> Spirv;
    std::string SpirvProfile = "spirv_1_5"; /* Vulkan 1.2, ShaderTool's default target env */
    /* Row-major unless -Zpc: the default of both Slang APIs and therefore of ShaderTool's
       other slang backends (only slangc's command line defaults to column-major). */
    bool RowMajor = true;
};

bool SpirvProfileForTargetEnv(const std::string &environment, std::string &profile) {
    struct Mapping {
        const char *Environment;
        const char *Profile;
    };
    static const Mapping mappings[] = {
        {"vulkan1.0", "spirv_1_0"},          {"vulkan1.1", "spirv_1_3"},
        {"vulkan1.1spirv1.4", "spirv_1_4"},  {"vulkan1.2", "spirv_1_5"},
        {"vulkan1.3", "spirv_1_6"},          {"vulkan1.4", "spirv_1_6"},
        {"universal1.5", "spirv_1_5"},
    };
    for (const Mapping &mapping : mappings) {
        if (environment == mapping.Environment) {
            profile = mapping.Profile;
            return true;
        }
    }
    return false;
}

int TranslateExtraArgs(const std::vector<std::string> &extra_args, SlangArguments &out,
                       DiagnosticSink &sink) {
    for (size_t i = 0; i < extra_args.size(); ++i) {
        const std::string &argument = extra_args[i];
        const bool is_vk_shift =
            argument == "-fvk-b-shift" || argument == "-fvk-s-shift" ||
            argument == "-fvk-t-shift" || argument == "-fvk-u-shift";
        if (argument == "-WX") {
            out.Common.push_back("-warnings-as-errors");
            out.Common.push_back("all");
        } else if (argument == "-Zi" || argument == "-g") {
            out.Common.push_back("-g");
        } else if (argument == "-Zpr") {
            out.RowMajor = true;
        } else if (argument == "-Zpc") {
            out.RowMajor = false;
        } else if (argument == "-O0" || argument == "-O1" || argument == "-O2" ||
                   argument == "-O3") {
            out.Common.push_back(argument);
        } else if (argument == "-lang") {
            if (i + 1 >= extra_args.size()) {
                sink.Error("-lang requires a language name");
                return -1;
            }
            const std::string &name = extra_args[++i];
            if (name != "slang") {
                sink.Error("container mode compiles Slang modules only (got -lang " + name +
                           ")");
                return -1;
            }
        } else if (argument == "-unscoped-enum") {
            out.Common.push_back(argument);
        } else if (argument == "-capability") {
            if (i + 1 >= extra_args.size()) {
                sink.Error("-capability requires a value");
                return -1;
            }
            out.Common.push_back(argument);
            out.Common.push_back(extra_args[++i]);
        } else if (argument == "-fvk-use-entrypoint-name" ||
                   argument == "-fvk-use-dx-layout" || argument == "-fvk-use-gl-layout") {
            out.Spirv.push_back(argument);
        } else if (argument == "-fvk-use-scalar-layout") {
            out.Spirv.push_back("-force-glsl-scalar-layout");
        } else if (is_vk_shift) {
            if (i + 2 >= extra_args.size()) {
                sink.Error(argument + " requires a shift and a space argument");
                return -1;
            }
            out.Spirv.push_back(argument);
            out.Spirv.push_back(extra_args[++i]);
            out.Spirv.push_back(extra_args[++i]);
        } else if (StartsWith(argument, "-fspv-target-env=")) {
            const std::string environment = argument.substr(strlen("-fspv-target-env="));
            if (!SpirvProfileForTargetEnv(environment, out.SpirvProfile)) {
                sink.Error("unsupported SPIR-V target environment '" + environment + "'");
                return -1;
            }
        } else if (StartsWith(argument, "-fspv-extension=") ||
                   argument == "-enable-16bit-types" || argument == "-all-resources-bound" ||
                   argument == "-Qembed_debug") {
            /* No-op, as in the slang backends: slang enables extensions and 16-bit types on
               demand and embeds debug information whenever it emits it. */
        } else {
            sink.Error("option '" + argument + "' is not supported in container mode");
            return -1;
        }
    }
    out.Common.push_back(out.RowMajor ? "-matrix-layout-row-major"
                                      : "-matrix-layout-column-major");
    return 0;
}

/* Compiler option entries parsed by IGlobalSession::parseCommandLineArguments. The strings
   they point to live in Allocation. */
struct CompilerOptions {
    ComPtr<ISlangUnknown> Allocation;
    std::vector<slang::CompilerOptionEntry> Entries;
};

int ParseCompilerOptions(const std::vector<std::string> &tokens, CompilerOptions &out,
                         DiagnosticSink &sink) {
    out.Entries.clear();
    if (tokens.empty())
        return 0;
    std::vector<const char *> argv;
    for (const std::string &token : tokens)
        argv.push_back(token.c_str());
    slang::SessionDesc parsed = {};
    if (SLANG_FAILED(GetGlobalSession()->parseCommandLineArguments(
            (int)argv.size(), argv.data(), &parsed, out.Allocation.writeRef()))) {
        std::string joined;
        for (const std::string &token : tokens)
            joined += " " + token;
        sink.Error("slang rejected the translated options:" + joined);
        return -1;
    }
    for (uint32_t i = 0; i < parsed.compilerOptionEntryCount; ++i) {
        const slang::CompilerOptionEntry &entry = parsed.compilerOptionEntries[i];
        /* The parser lists an empty argument set for every downstream compiler; dropping
           them keeps them from replacing the NVRTC arguments of the OptiX target. */
        if (entry.name == slang::CompilerOptionName::DownstreamArgs &&
            (!entry.value.stringValue1 || !*entry.value.stringValue1))
            continue;
        out.Entries.push_back(entry);
    }
    return 0;
}

} // namespace

#pragma endregion[Arguments]

#pragma region[ Code ]

namespace {

/* Names of the kernels/programs a PTX module defines (".entry <name>("). */
std::vector<std::string> PtxEntryNames(const std::string &ptx) {
    std::vector<std::string> names;
    size_t position = 0;
    while ((position = ptx.find(".entry", position)) != std::string::npos) {
        position += strlen(".entry");
        if (position >= ptx.size() || (ptx[position] != ' ' && ptx[position] != '\t'))
            continue;
        while (position < ptx.size() && (ptx[position] == ' ' || ptx[position] == '\t'))
            ++position;
        const size_t begin = position;
        while (position < ptx.size() &&
               (isalnum((unsigned char)ptx[position]) || ptx[position] == '_' ||
                ptx[position] == '$'))
            ++position;
        if (position > begin)
            names.push_back(ptx.substr(begin, position - begin));
    }
    return names;
}

/* The declaration of the global-parameter block, e.g.
   ".const .align 8 .b8 SLANG_globalParams[152];". */
bool FindPtxGlobalParams(const std::string &ptx, uint32_t &size, uint32_t &alignment) {
    const std::string symbol = "SLANG_globalParams[";
    size_t position = 0;
    while ((position = ptx.find(symbol, position)) != std::string::npos) {
        const size_t line_begin = ptx.rfind('\n', position);
        const std::string line =
            ptx.substr(line_begin == std::string::npos ? 0 : line_begin + 1,
                       position - (line_begin == std::string::npos ? 0 : line_begin + 1));
        const size_t align = line.find(".align");
        if (line.find(".b8") != std::string::npos && align != std::string::npos) {
            alignment = (uint32_t)strtoul(line.c_str() + align + strlen(".align"), nullptr, 10);
            size = (uint32_t)strtoul(ptx.c_str() + position + symbol.size(), nullptr, 10);
            return true;
        }
        position += symbol.size();
    }
    return false;
}

struct SpirvEntryPoint {
    uint32_t ExecutionModel = 0;
    std::string Name;
};

/* OpEntryPoint instructions of a SPIR-V module. */
std::vector<SpirvEntryPoint> SpirvEntryPoints(const std::vector<uint8_t> &code) {
    std::vector<SpirvEntryPoint> entries;
    const size_t word_count = code.size() / 4;
    if (word_count < 5)
        return entries;
    std::vector<uint32_t> words(word_count);
    memcpy(words.data(), code.data(), word_count * 4);
    if (words[0] != 0x07230203u)
        return entries;
    size_t index = 5;
    while (index < word_count) {
        const uint32_t length = words[index] >> 16;
        const uint32_t opcode = words[index] & 0xFFFFu;
        if (length == 0 || index + length > word_count)
            break;
        if (opcode == 15 /* OpEntryPoint */ && length >= 4) {
            SpirvEntryPoint entry;
            entry.ExecutionModel = words[index + 1];
            const char *name = reinterpret_cast<const char *>(&words[index + 3]);
            const size_t limit = (length - 3) * 4;
            entry.Name.assign(name, strnlen(name, limit));
            entries.push_back(entry);
        }
        index += length;
    }
    return entries;
}

/* Whether a SPIR-V execution model is the one of a Slang stage. */
bool SpirvModelMatches(uint32_t model, SlangStage stage) {
    switch (stage) {
    case SLANG_STAGE_VERTEX: return model == 0;
    case SLANG_STAGE_HULL: return model == 1;
    case SLANG_STAGE_DOMAIN: return model == 2;
    case SLANG_STAGE_GEOMETRY: return model == 3;
    case SLANG_STAGE_FRAGMENT: return model == 4;
    case SLANG_STAGE_COMPUTE: return model == 5;
    case SLANG_STAGE_RAY_GENERATION: return model == 5313;
    case SLANG_STAGE_INTERSECTION: return model == 5314;
    case SLANG_STAGE_ANY_HIT: return model == 5315;
    case SLANG_STAGE_CLOSEST_HIT: return model == 5316;
    case SLANG_STAGE_MISS: return model == 5317;
    case SLANG_STAGE_CALLABLE: return model == 5318;
    case SLANG_STAGE_AMPLIFICATION: return model == 5364 || model == 5267; /* TaskEXT/NV */
    case SLANG_STAGE_MESH: return model == 5365 || model == 5268;          /* MeshEXT/NV */
    default: return false;
    }
}

/* OptiX program-name prefixes (optixProgramGroupCreate looks programs up by them). */
std::vector<std::string> OptixPrefixes(SlangStage stage) {
    switch (stage) {
    case SLANG_STAGE_RAY_GENERATION: return {"__raygen__"};
    case SLANG_STAGE_MISS: return {"__miss__"};
    case SLANG_STAGE_CLOSEST_HIT: return {"__closesthit__"};
    case SLANG_STAGE_ANY_HIT: return {"__anyhit__"};
    case SLANG_STAGE_INTERSECTION: return {"__intersection__"};
    case SLANG_STAGE_CALLABLE: return {"__direct_callable__", "__continuation_callable__"};
    default: return {};
    }
}

bool Contains(const std::vector<std::string> &names, const std::string &name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

/* The name of an entry point in the compiled code: CUDA kernels and OptiX programs are
   looked up in the PTX (Slang renames 'main' to 'main_0' and prefixes OptiX programs),
   SPIR-V entry points in its OpEntryPoint instructions. D3D shaders use the Slang name
   (a DXIL library exports it). */
int ResolveSymbolName(ContainerTarget target, slang::EntryPointReflection *entry,
                      slang::ProgramLayout *layout, const std::vector<std::string> &ptx_entries,
                      const std::vector<SpirvEntryPoint> &spirv_entries, std::string &symbol,
                      DiagnosticSink &sink) {
    const std::string name = entry->getName() ? entry->getName() : "";
    const char *override_text = entry->getNameOverride();
    std::vector<std::string> bases;
    if (override_text && *override_text)
        bases.push_back(override_text);
    if (!Contains(bases, name))
        bases.push_back(name);

    switch (target) {
    case ContainerTarget::Dxbc:
    case ContainerTarget::Dxil:
        symbol = bases.front();
        return 0;
    case ContainerTarget::CudaPtx:
        for (const char *suffix : {"", "_0"})
            for (const std::string &base : bases)
                if (Contains(ptx_entries, base + suffix)) {
                    symbol = base + suffix;
                    return 0;
                }
        break;
    case ContainerTarget::OptixPtx:
        for (const std::string &prefix : OptixPrefixes(entry->getStage()))
            for (const char *suffix : {"", "_0"})
                for (const std::string &base : bases)
                    if (Contains(ptx_entries, prefix + base + suffix)) {
                        symbol = prefix + base + suffix;
                        return 0;
                    }
        break;
    case ContainerTarget::Spirv: {
        const SlangStage stage = entry->getStage();
        for (const std::string &base : bases)
            for (const SpirvEntryPoint &spirv : spirv_entries)
                if (SpirvModelMatches(spirv.ExecutionModel, stage) && spirv.Name == base) {
                    symbol = base;
                    return 0;
                }
        /* A lone entry point of its stage may have been renamed (Slang names a single
           SPIR-V entry point "main"). */
        size_t spirv_matches = 0, slang_matches = 0;
        const SpirvEntryPoint *match = nullptr;
        for (const SpirvEntryPoint &spirv : spirv_entries)
            if (SpirvModelMatches(spirv.ExecutionModel, stage)) {
                ++spirv_matches;
                match = &spirv;
            }
        for (SlangUInt i = 0; i < layout->getEntryPointCount(); ++i)
            if (layout->getEntryPointByIndex(i)->getStage() == entry->getStage())
                ++slang_matches;
        if (spirv_matches == 1 && slang_matches == 1) {
            symbol = match->Name;
            return 0;
        }
        break;
    }
    }
    sink.Error("cannot find entry point '" + name + "' in the generated " +
               InfoOf(target).Name + " code");
    return -1;
}

} // namespace

#pragma endregion[Code]

#pragma region[ Reflection ]

namespace {

uint32_t StageBits(SlangStage stage) {
    switch (stage) {
    case SLANG_STAGE_VERTEX: return NVRHI_SC_STAGE_VERTEX;
    case SLANG_STAGE_HULL: return NVRHI_SC_STAGE_HULL;
    case SLANG_STAGE_DOMAIN: return NVRHI_SC_STAGE_DOMAIN;
    case SLANG_STAGE_GEOMETRY: return NVRHI_SC_STAGE_GEOMETRY;
    case SLANG_STAGE_FRAGMENT: return NVRHI_SC_STAGE_PIXEL;
    case SLANG_STAGE_COMPUTE: return NVRHI_SC_STAGE_COMPUTE;
    case SLANG_STAGE_AMPLIFICATION: return NVRHI_SC_STAGE_AMPLIFICATION;
    case SLANG_STAGE_MESH: return NVRHI_SC_STAGE_MESH;
    case SLANG_STAGE_RAY_GENERATION: return NVRHI_SC_STAGE_RAY_GENERATION;
    case SLANG_STAGE_ANY_HIT: return NVRHI_SC_STAGE_ANY_HIT;
    case SLANG_STAGE_CLOSEST_HIT: return NVRHI_SC_STAGE_CLOSEST_HIT;
    case SLANG_STAGE_MISS: return NVRHI_SC_STAGE_MISS;
    case SLANG_STAGE_INTERSECTION: return NVRHI_SC_STAGE_INTERSECTION;
    case SLANG_STAGE_CALLABLE: return NVRHI_SC_STAGE_CALLABLE;
    default: return 0;
    }
}

uint32_t ScalarTypeOf(slang::TypeReflection::ScalarType scalar) {
    using Scalar = slang::TypeReflection::ScalarType;
    switch (scalar) {
    case Scalar::Bool: return NVRHI_SC_SCALAR_BOOL;
    case Scalar::Int8: return NVRHI_SC_SCALAR_INT8;
    case Scalar::UInt8: return NVRHI_SC_SCALAR_UINT8;
    case Scalar::Int16: return NVRHI_SC_SCALAR_INT16;
    case Scalar::UInt16: return NVRHI_SC_SCALAR_UINT16;
    case Scalar::Int32: return NVRHI_SC_SCALAR_INT32;
    case Scalar::UInt32: return NVRHI_SC_SCALAR_UINT32;
    case Scalar::Int64:
    case Scalar::IntPtr: return NVRHI_SC_SCALAR_INT64;
    case Scalar::UInt64:
    case Scalar::UIntPtr: return NVRHI_SC_SCALAR_UINT64;
    case Scalar::Float16: return NVRHI_SC_SCALAR_FLOAT16;
    case Scalar::Float32: return NVRHI_SC_SCALAR_FLOAT32;
    case Scalar::Float64: return NVRHI_SC_SCALAR_FLOAT64;
    default: return NVRHI_SC_SCALAR_NONE;
    }
}

uint32_t ScalarSize(uint32_t scalar) {
    switch (scalar) {
    case NVRHI_SC_SCALAR_INT8:
    case NVRHI_SC_SCALAR_UINT8: return 1;
    case NVRHI_SC_SCALAR_INT16:
    case NVRHI_SC_SCALAR_UINT16:
    case NVRHI_SC_SCALAR_FLOAT16: return 2;
    case NVRHI_SC_SCALAR_INT64:
    case NVRHI_SC_SCALAR_UINT64:
    case NVRHI_SC_SCALAR_FLOAT64: return 8;
    case NVRHI_SC_SCALAR_NONE: return 0;
    default: return 4;
    }
}

/* The scalar type of a scalar, vector or matrix (vectors and matrices report it through
   their element types). */
uint32_t ElementScalarType(slang::TypeReflection *type) {
    for (int depth = 0; type && depth < 3; ++depth) {
        const uint32_t scalar = ScalarTypeOf(type->getScalarType());
        if (scalar != NVRHI_SC_SCALAR_NONE)
            return scalar;
        type = type->getElementType();
    }
    return NVRHI_SC_SCALAR_NONE;
}

std::string TypeName(slang::TypeReflection *type) {
    if (!type)
        return std::string();
    ComPtr<slang::IBlob> blob;
    if (SLANG_SUCCEEDED(type->getFullName(blob.writeRef())) && blob) {
        const std::string text = BlobText(blob);
        if (!text.empty())
            return text;
    }
    const char *name = type->getName();
    return name ? name : "";
}

bool HasCategory(slang::VariableLayoutReflection *var, slang::ParameterCategory category) {
    if (!var)
        return false;
    for (unsigned i = 0; i < var->getCategoryCount(); ++i)
        if (var->getCategoryByIndex(i) == category)
            return true;
    return false;
}

/* The alignment of a type in its layout. Slang reports 1 for the structs it synthesizes
   (the global and entry-point parameter blocks), so structs take the largest alignment of
   their fields that occupy ordinary data. */
uint32_t LayoutAlignment(slang::TypeLayoutReflection *layout, int depth = 0) {
    if (!layout || depth > 32)
        return 1;
    uint32_t alignment = (uint32_t)std::max(1, layout->getAlignment(slang::ParameterCategory::Uniform));
    if (layout->getKind() == slang::TypeReflection::Kind::Struct) {
        for (unsigned i = 0; i < layout->getFieldCount(); ++i) {
            slang::TypeLayoutReflection *field = layout->getFieldByIndex(i)->getTypeLayout();
            if (field && field->getSize(slang::ParameterCategory::Uniform) != 0)
                alignment = std::max(alignment, LayoutAlignment(field, depth + 1));
        }
    } else if (layout->getKind() == slang::TypeReflection::Kind::Array) {
        alignment = std::max(alignment, LayoutAlignment(layout->getElementTypeLayout(), depth + 1));
    }
    return alignment;
}

/* A ConstantBuffer<T> or ParameterBlock<T> type layout -> the layout of T (anything else is
   returned unchanged). */
slang::TypeLayoutReflection *UnwrapParameterGroup(slang::TypeLayoutReflection *layout) {
    if (layout && (layout->getKind() == slang::TypeReflection::Kind::ConstantBuffer ||
                   layout->getKind() == slang::TypeReflection::Kind::ParameterBlock)) {
        slang::TypeLayoutReflection *element = layout->getElementTypeLayout();
        return element ? element : layout;
    }
    return layout;
}

bool ContainsUnboundedArray(slang::TypeLayoutReflection *layout, int depth = 0) {
    if (!layout || depth > 32)
        return false;
    switch (layout->getKind()) {
    case slang::TypeReflection::Kind::Array: {
        const size_t count = layout->getElementCount();
        return count == 0 || count == SLANG_UNBOUNDED_SIZE ||
               ContainsUnboundedArray(layout->getElementTypeLayout(), depth + 1);
    }
    case slang::TypeReflection::Kind::Struct:
        for (unsigned i = 0; i < layout->getFieldCount(); ++i)
            if (ContainsUnboundedArray(layout->getFieldByIndex(i)->getTypeLayout(), depth + 1))
                return true;
        return false;
    case slang::TypeReflection::Kind::ConstantBuffer:
    case slang::TypeReflection::Kind::ParameterBlock:
        return ContainsUnboundedArray(layout->getElementTypeLayout(), depth + 1);
    default:
        return false;
    }
}

/* A D3D register (space, slot) or Vulkan (set, binding). */
struct Binding {
    uint32_t Space = 0;
    uint32_t Slot = 0;
};

/* Where a parameter is bound in a D3D or Vulkan layout. A parameter block owns a register
   space / descriptor set, and its constant buffer (if it has ordinary data) is the first
   binding in it; push constants have no binding. */
Binding FindBinding(slang::VariableLayoutReflection *var) {
    using Category = slang::ParameterCategory;
    Binding binding;
    if (!var)
        return binding;
    for (unsigned i = 0; i < var->getCategoryCount(); ++i) {
        const Category category = var->getCategoryByIndex(i);
        if (category == Category::SubElementRegisterSpace || category == Category::RegisterSpace) {
            binding.Space = SizeToU32(var->getOffset(category));
            slang::TypeLayoutReflection *layout = var->getTypeLayout();
            slang::VariableLayoutReflection *container =
                layout ? layout->getContainerVarLayout() : nullptr;
            if (container) {
                for (unsigned j = 0; j < container->getCategoryCount(); ++j) {
                    const Category inner = container->getCategoryByIndex(j);
                    if (inner == Category::ConstantBuffer || inner == Category::DescriptorTableSlot) {
                        binding.Slot = SizeToU32(container->getOffset(inner));
                        break;
                    }
                }
            }
            return binding;
        }
    }
    for (unsigned i = 0; i < var->getCategoryCount(); ++i) {
        const Category category = var->getCategoryByIndex(i);
        switch (category) {
        case Category::ConstantBuffer:
        case Category::ShaderResource:
        case Category::UnorderedAccess:
        case Category::SamplerState:
        case Category::DescriptorTableSlot:
            binding.Slot = SizeToU32(var->getOffset(category));
            binding.Space = SizeToU32(var->getBindingSpace(category));
            return binding;
        default:
            break;
        }
    }
    return binding;
}

/* True for entry-point parameters that are uniform (kernel arguments, SBT record data,
   entry-point constant buffers and resources), false for varyings and payloads. */
bool IsUniformEntryParameter(slang::VariableLayoutReflection *var) {
    using Category = slang::ParameterCategory;
    bool uniform = false;
    for (unsigned i = 0; i < var->getCategoryCount(); ++i) {
        switch (var->getCategoryByIndex(i)) {
        case Category::Uniform:
        case Category::ConstantBuffer:
        case Category::ShaderResource:
        case Category::UnorderedAccess:
        case Category::SamplerState:
        case Category::DescriptorTableSlot:
        case Category::PushConstantBuffer:
        case Category::RegisterSpace:
        case Category::SubElementRegisterSpace:
        case Category::ShaderRecord:
            uniform = true;
            break;
        case Category::None:
        case Category::Mixed:
            break;
        default:
            return false; /* varyings, payloads, attributes, specialization constants */
        }
    }
    return uniform;
}

uint32_t TextureDimensionOf(SlangResourceShape shape) {
    const unsigned base = shape & SLANG_RESOURCE_BASE_SHAPE_MASK;
    const bool array = (shape & SLANG_TEXTURE_ARRAY_FLAG) != 0;
    const bool multisample = (shape & SLANG_TEXTURE_MULTISAMPLE_FLAG) != 0;
    /* nvrhi::TextureDimension: Unknown, Texture1D, Texture1DArray, Texture2D,
       Texture2DArray, TextureCube, TextureCubeArray, Texture2DMS, Texture2DMSArray,
       Texture3D */
    switch (base) {
    case SLANG_TEXTURE_1D: return array ? 2u : 1u;
    case SLANG_TEXTURE_2D:
        if (multisample)
            return array ? 8u : 7u;
        return array ? 4u : 3u;
    case SLANG_TEXTURE_CUBE: return array ? 6u : 5u;
    case SLANG_TEXTURE_3D: return 9u;
    default: return 0u;
    }
}

class ReflectionBuilder {
public:
    ReflectionBuilder(ContainerTarget target, ContainerReflection &out, DiagnosticSink &sink)
        : Target(target), Out(out), Sink(sink) {}

    /* The global parameters of 'layout' (the code target). 'register_layout' is the layout
       the space/slot values come from: the code target itself, or for PTX the D3D layout
       of the same program (the HLSL-to-CUDA cross-map). */
    int AddGlobals(slang::ProgramLayout *layout, slang::ProgramLayout *register_layout) {
        const Binding uniform_binding = FindBinding(register_layout->getGlobalParamsVarLayout());
        for (unsigned i = 0; i < layout->getParameterCount(); ++i) {
            slang::VariableLayoutReflection *var = layout->getParameterByIndex(i);
            slang::VariableLayoutReflection *register_var = var;
            if (register_layout != layout) {
                register_var = FindGlobal(register_layout, var->getName());
                if (!register_var) {
                    Sink.Error(std::string("global parameter '") + Name(var) +
                               "' has no D3D register layout counterpart");
                    return -1;
                }
            }
            if (IsPtx(Target) && ContainsUnboundedArray(var->getTypeLayout())) {
                Sink.Error(std::string("global parameter '") + Name(var) + "': unsized arrays "
                           "are not supported on the CUDA/OptiX targets (Slang moves them to "
                           "the end of SLANG_globalParams, which its reflection does not show)");
                return -1;
            }
            if (AddParameter(var, register_var, uniform_binding, 0))
                return -1;
        }
        Out.GlobalParameterCount = (uint32_t)Out.Parameters.size();
        return 0;
    }

    int AddEntryPoint(slang::EntryPointReflection *entry,
                      slang::EntryPointReflection *register_entry, const std::string &symbol) {
        NvrhiScEntryPoint record;
        memset(&record, 0, sizeof(record));
        record.nameOffset = Out.AddString(entry->getName() ? entry->getName() : "");
        record.symbolNameOffset = Out.AddString(symbol);
        const SlangStage stage = entry->getStage();
        record.stage = StageBits(stage);
        if (stage == SLANG_STAGE_COMPUTE || stage == SLANG_STAGE_MESH ||
            stage == SLANG_STAGE_AMPLIFICATION) {
            SlangUInt sizes[3] = {0, 0, 0};
            entry->getComputeThreadGroupSize(3, sizes);
            for (int axis = 0; axis < 3; ++axis)
                record.threadGroupSize[axis] = (uint32_t)sizes[axis];
        }

        record.firstParameter = (uint32_t)Out.Parameters.size();
        const Binding block_binding =
            FindBinding(register_entry ? register_entry->getVarLayout() : nullptr);
        for (unsigned i = 0; i < entry->getParameterCount(); ++i) {
            slang::VariableLayoutReflection *var = entry->getParameterByIndex(i);
            if (!IsUniformEntryParameter(var))
                continue;
            slang::VariableLayoutReflection *register_var = var;
            if (register_entry != entry) {
                register_var = FindEntryParameter(register_entry, var->getName());
                if (!register_var) {
                    Sink.Error(std::string("parameter '") + Name(var) + "' of entry point '" +
                               entry->getName() + "' has no D3D register layout counterpart");
                    return -1;
                }
            }
            if (AddParameter(var, register_var, block_binding, NVRHI_SC_PARAM_FLAG_ENTRY_POINT))
                return -1;
        }
        record.parameterCount = (uint32_t)Out.Parameters.size() - record.firstParameter;

        if (record.parameterCount) {
            slang::TypeLayoutReflection *block = UnwrapParameterGroup(entry->getTypeLayout());
            const uint32_t size = SizeToU32(block->getSize(slang::ParameterCategory::Uniform));
            if (size) {
                record.paramsAlignment = LayoutAlignment(block);
                record.paramsSize = AlignUp(size, record.paramsAlignment);
            }
        }
        Out.EntryPoints.push_back(record);
        return 0;
    }

private:
    static const char *Name(slang::VariableLayoutReflection *var) {
        const char *name = var ? var->getName() : nullptr;
        return name ? name : "";
    }

    static slang::VariableLayoutReflection *FindGlobal(slang::ProgramLayout *layout,
                                                       const char *name) {
        for (unsigned i = 0; name && i < layout->getParameterCount(); ++i) {
            slang::VariableLayoutReflection *var = layout->getParameterByIndex(i);
            if (var->getName() && !strcmp(var->getName(), name))
                return var;
        }
        return nullptr;
    }

    static slang::VariableLayoutReflection *FindEntryParameter(slang::EntryPointReflection *entry,
                                                               const char *name) {
        for (unsigned i = 0; entry && name && i < entry->getParameterCount(); ++i) {
            slang::VariableLayoutReflection *var = entry->getParameterByIndex(i);
            if (var->getName() && !strcmp(var->getName(), name))
                return var;
        }
        return nullptr;
    }

    static bool IsPushConstant(slang::TypeLayoutReflection *layout,
                               slang::VariableLayoutReflection *var,
                               slang::VariableLayoutReflection *register_var) {
        if (HasCategory(var, slang::ParameterCategory::PushConstantBuffer) ||
            HasCategory(register_var, slang::ParameterCategory::PushConstantBuffer))
            return true;
        /* D3D and CUDA layouts do not mark [[vk::push_constant]], but its type does:
           ConstantBuffer<T, DefaultPushConstantDataLayout>. */
        return TypeName(layout->getType()).find("PushConstant") != std::string::npos;
    }

    /* NVRHI_SC_PARAM_* (and the texture dimension) of a non-array parameter type. */
    bool Classify(slang::TypeLayoutReflection *element, slang::VariableLayoutReflection *var,
                  slang::VariableLayoutReflection *register_var, uint32_t &kind,
                  uint32_t &dimension, std::string &problem) {
        using Kind = slang::TypeReflection::Kind;
        dimension = 0;
        switch (element->getKind()) {
        case Kind::ConstantBuffer:
            kind = IsPushConstant(element, var, register_var) ? NVRHI_SC_PARAM_PUSH_CONSTANTS
                                                              : NVRHI_SC_PARAM_CONSTANT_BUFFER;
            return true;
        case Kind::ParameterBlock:
            kind = NVRHI_SC_PARAM_CONSTANT_BUFFER;
            return true;
        case Kind::SamplerState:
            kind = NVRHI_SC_PARAM_SAMPLER;
            return true;
        case Kind::Resource: {
            slang::TypeReflection *type = element->getType();
            const SlangResourceShape shape = type->getResourceShape();
            const SlangResourceAccess access = type->getResourceAccess();
            if (shape & (SLANG_TEXTURE_FEEDBACK_FLAG | SLANG_TEXTURE_COMBINED_FLAG)) {
                problem = "feedback textures and combined texture-samplers";
                return false;
            }
            const bool writable = access != SLANG_RESOURCE_ACCESS_READ;
            switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK) {
            case SLANG_TEXTURE_1D:
            case SLANG_TEXTURE_2D:
            case SLANG_TEXTURE_3D:
            case SLANG_TEXTURE_CUBE:
                kind = writable ? NVRHI_SC_PARAM_TEXTURE_UAV : NVRHI_SC_PARAM_TEXTURE_SRV;
                dimension = TextureDimensionOf(shape);
                return true;
            case SLANG_TEXTURE_BUFFER:
                kind = writable ? NVRHI_SC_PARAM_TYPED_BUFFER_UAV : NVRHI_SC_PARAM_TYPED_BUFFER_SRV;
                return true;
            case SLANG_STRUCTURED_BUFFER:
                kind = writable ? NVRHI_SC_PARAM_STRUCTURED_BUFFER_UAV
                                : NVRHI_SC_PARAM_STRUCTURED_BUFFER_SRV;
                return true;
            case SLANG_BYTE_ADDRESS_BUFFER:
                kind = writable ? NVRHI_SC_PARAM_RAW_BUFFER_UAV : NVRHI_SC_PARAM_RAW_BUFFER_SRV;
                return true;
            case SLANG_ACCELERATION_STRUCTURE:
                kind = NVRHI_SC_PARAM_ACCEL_STRUCT;
                return true;
            default:
                problem = "this resource shape";
                return false;
            }
        }
        case Kind::Scalar:
        case Kind::Vector:
        case Kind::Matrix:
        case Kind::Struct:
        case Kind::Pointer:
        case Kind::Enum:
            kind = NVRHI_SC_PARAM_UNIFORM;
            return true;
        default:
            problem = "this type kind";
            return false;
        }
    }

    int AddParameter(slang::VariableLayoutReflection *var,
                     slang::VariableLayoutReflection *register_var,
                     const Binding &uniform_binding, uint32_t flags) {
        using Category = slang::ParameterCategory;
        if (HasCategory(var, Category::SpecializationConstant)) {
            Sink.Error(std::string("warning: specialization constant '") + Name(var) +
                       "' is not described in the container reflection");
            return 0;
        }
        if (!var->getCategoryCount())
            return 0; /* occupies nothing in this target */

        NvrhiScParameter record;
        memset(&record, 0, sizeof(record));
        record.nameOffset = Out.AddString(Name(var));
        record.typeIndex = kInvalidIndex;
        record.flags = flags;

        /* Arrays (of any rank) are flattened: arraySize counts every element, the stride is
           the one between innermost elements. */
        slang::TypeLayoutReflection *layout = var->getTypeLayout();
        slang::TypeLayoutReflection *element = layout;
        slang::TypeLayoutReflection *innermost_array = nullptr;
        uint64_t count = 1;
        bool unbounded = false;
        while (element && element->getKind() == slang::TypeReflection::Kind::Array) {
            const size_t elements = element->getElementCount();
            if (elements == 0 || elements == SLANG_UNBOUNDED_SIZE)
                unbounded = true;
            else
                count *= elements;
            innermost_array = element;
            element = element->getElementTypeLayout();
        }
        if (!element) {
            Sink.Error(std::string("parameter '") + Name(var) + "' has no type layout");
            return -1;
        }
        if (innermost_array)
            record.arraySize = unbounded ? NVRHI_SC_UNBOUNDED_ARRAY
                                         : (uint32_t)std::min<uint64_t>(count, UINT32_MAX - 1);

        uint32_t kind = 0, dimension = 0;
        std::string problem;
        if (!Classify(element, var, register_var, kind, dimension, problem)) {
            Sink.Error(std::string("parameter '") + Name(var) + "' of type '" +
                       TypeName(element->getType()) + "': " + problem +
                       " cannot be described in an NVRHI shader container");
            return -1;
        }
        record.kind = kind;
        record.textureDimension = dimension;

        switch (kind) {
        case NVRHI_SC_PARAM_CONSTANT_BUFFER:
        case NVRHI_SC_PARAM_PUSH_CONSTANTS:
        case NVRHI_SC_PARAM_STRUCTURED_BUFFER_SRV:
        case NVRHI_SC_PARAM_STRUCTURED_BUFFER_UAV:
            record.typeIndex = AddTypeLayout(element->getElementTypeLayout());
            break;
        case NVRHI_SC_PARAM_TEXTURE_SRV:
        case NVRHI_SC_PARAM_TEXTURE_UAV:
        case NVRHI_SC_PARAM_TYPED_BUFFER_SRV:
        case NVRHI_SC_PARAM_TYPED_BUFFER_UAV:
            record.typeIndex = AddValueType(element->getType()->getResourceResultType());
            break;
        case NVRHI_SC_PARAM_UNIFORM:
            record.typeIndex = AddTypeLayout(element);
            break;
        default:
            break;
        }

        if (kind == NVRHI_SC_PARAM_UNIFORM) {
            /* Ordinary data lives in the constant buffer of its scope (the default constant
               buffer of the globals, or of the entry point); a struct that also holds
               resources would need registers this format cannot describe. */
            for (unsigned i = 0; i < register_var->getCategoryCount(); ++i) {
                const Category category = register_var->getCategoryByIndex(i);
                if (category != Category::Uniform && category != Category::ShaderRecord &&
                    category != Category::None) {
                    Sink.Error(std::string("parameter '") + Name(var) + "' mixes resources and "
                               "ordinary data; declare it as a ParameterBlock<T> or split it");
                    return -1;
                }
            }
            record.space = uniform_binding.Space;
            record.slot = uniform_binding.Slot;
        } else {
            const Binding binding = FindBinding(register_var);
            record.space = binding.Space;
            record.slot = binding.Slot;
        }

        record.uniformOffset = SizeToU32(var->getOffset(Category::Uniform));
        record.uniformSize = SizeToU32(layout->getSize(Category::Uniform));
        if (innermost_array)
            record.uniformStride =
                SizeToU32(innermost_array->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM));
        Out.Parameters.push_back(record);
        return 0;
    }

    /* The element type of a texture or typed buffer (no layout of its own). */
    uint32_t AddValueType(slang::TypeReflection *type) {
        if (!type)
            return kInvalidIndex;
        NvrhiScType record;
        memset(&record, 0, sizeof(record));
        record.nameOffset = Out.AddString(TypeName(type));
        record.rowCount = 1;
        record.columnCount = 1;
        record.elementType = kInvalidIndex;
        record.scalarType = ElementScalarType(type);
        const uint32_t scalar_size = ScalarSize(record.scalarType);
        switch (type->getKind()) {
        case slang::TypeReflection::Kind::Scalar:
            record.kind = NVRHI_SC_TYPE_SCALAR;
            break;
        case slang::TypeReflection::Kind::Vector:
            record.kind = NVRHI_SC_TYPE_VECTOR;
            record.columnCount = (uint32_t)type->getElementCount();
            break;
        default:
            return kInvalidIndex;
        }
        record.size = scalar_size * record.columnCount;
        record.alignment = scalar_size;
        record.stride = record.size;
        return Out.AddType(record, {});
    }

    uint32_t AddTypeLayout(slang::TypeLayoutReflection *layout) {
        using Kind = slang::TypeReflection::Kind;
        if (!layout || TypeStack.size() > 64)
            return kInvalidIndex;
        slang::TypeReflection *type = layout->getType();

        NvrhiScType record;
        memset(&record, 0, sizeof(record));
        record.nameOffset = Out.AddString(TypeName(type));
        record.rowCount = 1;
        record.columnCount = 1;
        record.elementType = kInvalidIndex;
        record.size = SizeToU32(layout->getSize(slang::ParameterCategory::Uniform));
        record.alignment = LayoutAlignment(layout);
        record.stride = std::max(SizeToU32(layout->getStride(slang::ParameterCategory::Uniform)),
                                 AlignUp(record.size, record.alignment));

        std::vector<NvrhiScField> fields;
        TypeStack.push_back(type);
        switch (layout->getKind()) {
        case Kind::Scalar:
        case Kind::Enum:
            record.kind = NVRHI_SC_TYPE_SCALAR;
            record.scalarType = ElementScalarType(type);
            if (record.scalarType == NVRHI_SC_SCALAR_NONE && layout->getKind() == Kind::Enum)
                record.scalarType = NVRHI_SC_SCALAR_INT32;
            break;
        case Kind::Vector:
            record.kind = NVRHI_SC_TYPE_VECTOR;
            record.scalarType = ElementScalarType(type);
            record.columnCount = type ? (uint32_t)type->getElementCount() : 1;
            break;
        case Kind::Matrix:
            record.kind = NVRHI_SC_TYPE_MATRIX;
            record.scalarType = ElementScalarType(type);
            record.rowCount = type ? type->getRowCount() : 1;
            record.columnCount = type ? type->getColumnCount() : 1;
            break;
        case Kind::Struct:
            record.kind = NVRHI_SC_TYPE_STRUCT;
            for (unsigned i = 0; i < layout->getFieldCount(); ++i) {
                slang::VariableLayoutReflection *field_layout = layout->getFieldByIndex(i);
                NvrhiScField field;
                memset(&field, 0, sizeof(field));
                field.nameOffset = Out.AddString(Name(field_layout));
                field.typeIndex = AddTypeLayout(field_layout->getTypeLayout());
                field.offset = SizeToU32(field_layout->getOffset(slang::ParameterCategory::Uniform));
                fields.push_back(field);
            }
            break;
        case Kind::Array: {
            record.kind = NVRHI_SC_TYPE_ARRAY;
            const size_t elements = layout->getElementCount();
            record.elementCount = (elements == 0 || elements == SLANG_UNBOUNDED_SIZE)
                                      ? NVRHI_SC_UNBOUNDED_ARRAY
                                      : (uint32_t)std::min<size_t>(elements, UINT32_MAX - 1);
            record.elementType = AddTypeLayout(layout->getElementTypeLayout());
            record.stride = SizeToU32(layout->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM));
            break;
        }
        case Kind::Pointer: {
            record.kind = NVRHI_SC_TYPE_POINTER;
            slang::TypeLayoutReflection *pointee = layout->getElementTypeLayout();
            /* A pointer back into a type being built (struct Node { Node* next; }) is left
               without an element type rather than recursing forever. */
            if (pointee && std::find(TypeStack.begin(), TypeStack.end(), pointee->getType()) ==
                               TypeStack.end())
                record.elementType = AddTypeLayout(pointee);
            break;
        }
        case Kind::Resource:
        case Kind::SamplerState:
        case Kind::ConstantBuffer:
        case Kind::ParameterBlock:
        case Kind::TextureBuffer:
        case Kind::ShaderStorageBuffer:
        case Kind::DynamicResource:
            record.kind = NVRHI_SC_TYPE_RESOURCE;
            break;
        default:
            record.kind = NVRHI_SC_TYPE_UNKNOWN;
            break;
        }
        TypeStack.pop_back();
        return Out.AddType(record, fields);
    }

    ContainerTarget Target;
    ContainerReflection &Out;
    DiagnosticSink &Sink;
    std::vector<slang::TypeReflection *> TypeStack;
};

/* The PTX global-parameter block: its size and alignment come from the PTX declaration, and
   must agree with the reflected layout the parameter offsets come from. */
int SetPtxGlobalParams(slang::ProgramLayout *layout, const std::string &ptx,
                       ContainerReflection &reflection, DiagnosticSink &sink) {
    slang::TypeLayoutReflection *block =
        UnwrapParameterGroup(layout->getGlobalParamsVarLayout()->getTypeLayout());
    const uint32_t reflected_alignment = LayoutAlignment(block);
    const uint32_t reflected_size =
        AlignUp(SizeToU32(block->getSize(slang::ParameterCategory::Uniform)), reflected_alignment);

    uint32_t size = 0, alignment = 0;
    if (!FindPtxGlobalParams(ptx, size, alignment)) {
        /* No SLANG_globalParams symbol: there is nothing to upload. */
        reflection.GlobalParamsSize = 0;
        reflection.GlobalParamsAlignment = 0;
        return 0;
    }
    if (size != reflected_size) {
        sink.Error("the PTX declares SLANG_globalParams[" + std::to_string(size) +
                   "] but the reflected layout has " + std::to_string(reflected_size) +
                   " bytes; refusing to write a reflection that does not match the code");
        return -1;
    }
    for (uint32_t i = 0; i < reflection.GlobalParameterCount; ++i) {
        const NvrhiScParameter &parameter = reflection.Parameters[i];
        if ((uint64_t)parameter.uniformOffset + parameter.uniformSize > size) {
            sink.Error("global parameter #" + std::to_string(i) +
                       " lies outside SLANG_globalParams");
            return -1;
        }
    }
    reflection.GlobalParamsSize = size;
    reflection.GlobalParamsAlignment = alignment ? alignment : reflected_alignment;
    return 0;
}

} // namespace

#pragma endregion[Reflection]

#pragma region[ Sessions ]

namespace {

struct ModuleInputs {
    std::string ModuleName;
    std::string Path;   /* absolute: Slang resolves relative #include/import against it */
    std::string Source;
    std::vector<std::string> SearchPathStorage;
    std::vector<const char *> SearchPaths;
    std::vector<slang::PreprocessorMacroDesc> Macros;
    CompilerOptions Common;
    CompilerOptions Spirv;
    CompilerOptions Ptx;
    std::string SpirvProfile;
    std::string D3DProfile; /* sm_<model> */
    bool RowMajor = false;
    std::string OptixArguments;
    std::vector<slang::CompilerOptionEntry> OptixEntries;
};

struct SessionTarget {
    SlangCompileTarget Format = SLANG_TARGET_UNKNOWN;
    std::string Profile;
    SlangTargetFlags ExtraFlags = 0;
    bool SpirvOptions = false;
};

/* 'target' selects the session-wide extras of the PTX targets (none for the discovery
   session, which passes ContainerTarget::Dxil). */
int CreateSession(const ModuleInputs &inputs, const std::vector<SessionTarget> &targets,
                  ContainerTarget target, ComPtr<slang::ISession> &session,
                  DiagnosticSink &sink) {
    slang::IGlobalSession *global = GetGlobalSession();
    std::vector<slang::TargetDesc> descs(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        descs[i].format = targets[i].Format;
        if (!targets[i].Profile.empty()) {
            descs[i].profile = global->findProfile(targets[i].Profile.c_str());
            if (descs[i].profile == SLANG_PROFILE_UNKNOWN) {
                sink.Error("slang does not recognize profile '" + targets[i].Profile + "'");
                return -1;
            }
        }
        descs[i].flags |= targets[i].ExtraFlags;
        if (targets[i].SpirvOptions && !inputs.Spirv.Entries.empty()) {
            descs[i].compilerOptionEntries = inputs.Spirv.Entries.data();
            descs[i].compilerOptionEntryCount = (uint32_t)inputs.Spirv.Entries.size();
        }
    }

    std::vector<slang::CompilerOptionEntry> entries = inputs.Common.Entries;
    if (IsPtx(target))
        entries.insert(entries.end(), inputs.Ptx.Entries.begin(), inputs.Ptx.Entries.end());
    if (target == ContainerTarget::OptixPtx)
        entries.insert(entries.end(), inputs.OptixEntries.begin(), inputs.OptixEntries.end());

    slang::SessionDesc desc = {};
    desc.targets = descs.data();
    desc.targetCount = (SlangInt)descs.size();
    desc.defaultMatrixLayoutMode =
        inputs.RowMajor ? SLANG_MATRIX_LAYOUT_ROW_MAJOR : SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    desc.searchPaths = inputs.SearchPaths.empty() ? nullptr : inputs.SearchPaths.data();
    desc.searchPathCount = (SlangInt)inputs.SearchPaths.size();
    desc.preprocessorMacros = inputs.Macros.empty() ? nullptr : inputs.Macros.data();
    desc.preprocessorMacroCount = (SlangInt)inputs.Macros.size();
    desc.compilerOptionEntries = entries.empty() ? nullptr : entries.data();
    desc.compilerOptionEntryCount = (uint32_t)entries.size();
    if (SLANG_FAILED(global->createSession(desc, session.writeRef())) || !session) {
        sink.Error("failed to create a slang session");
        return -1;
    }
    return 0;
}

slang::IModule *LoadModule(slang::ISession *session, const ModuleInputs &inputs,
                           DiagnosticSink &sink) {
    ComPtr<slang::IBlob> diagnostics;
    slang::IModule *module = session->loadModuleFromSourceString(
        inputs.ModuleName.c_str(), inputs.Path.c_str(), inputs.Source.c_str(),
        diagnostics.writeRef());
    sink.Add(diagnostics);
    if (!module)
        sink.Error("slang failed to load '" + inputs.Path + "'");
    return module;
}

struct EntryPointInfo {
    ComPtr<slang::IEntryPoint> EntryPoint;
    std::string Name;
    SlangStage Stage = SLANG_STAGE_NONE;
};

/* The [shader(...)] entry points the module defines, in declaration order. */
int GetEntryPoints(slang::IModule *module, std::vector<EntryPointInfo> &entries,
                   DiagnosticSink &sink) {
    entries.clear();
    const SlangInt32 count = module->getDefinedEntryPointCount();
    for (SlangInt32 i = 0; i < count; ++i) {
        EntryPointInfo info;
        if (SLANG_FAILED(module->getDefinedEntryPoint(i, info.EntryPoint.writeRef())) ||
            !info.EntryPoint) {
            sink.Error("failed to get entry point #" + std::to_string(i));
            return -1;
        }
        ComPtr<slang::IBlob> diagnostics;
        slang::ProgramLayout *layout = info.EntryPoint->getLayout(0, diagnostics.writeRef());
        sink.Add(diagnostics);
        slang::EntryPointReflection *reflection =
            layout ? layout->getEntryPointByIndex(0) : nullptr;
        if (!reflection) {
            sink.Error("failed to reflect entry point #" + std::to_string(i));
            return -1;
        }
        info.Name = reflection->getName() ? reflection->getName() : "";
        info.Stage = reflection->getStage();
        entries.push_back(std::move(info));
    }
    return 0;
}

bool IsRayTracingStage(SlangStage stage) {
    return stage >= SLANG_STAGE_RAY_GENERATION && stage <= SLANG_STAGE_CALLABLE;
}

bool TargetAccepts(ContainerTarget target, SlangStage stage) {
    switch (target) {
    case ContainerTarget::Dxbc:
        return stage >= SLANG_STAGE_VERTEX && stage <= SLANG_STAGE_COMPUTE;
    case ContainerTarget::Dxil:
    case ContainerTarget::Spirv:
        return true;
    case ContainerTarget::CudaPtx:
        return stage == SLANG_STAGE_COMPUTE;
    case ContainerTarget::OptixPtx:
        return IsRayTracingStage(stage);
    }
    return false;
}

/* Compiles the module for one target and appends its CODE and REFLECTION sections. A target
   with no entry points it can hold is skipped (*skipped set). */
int CompileTarget(const ModuleInputs &inputs, ContainerTarget target,
                  const std::vector<EntryPointInfo> &discovered,
                  std::vector<ContainerSection> &sections, bool &skipped,
                  DiagnosticSink &sink) {
    const TargetInfo &info = InfoOf(target);
    skipped = false;

    std::vector<std::string> selected;
    bool any_ray_tracing = false;
    for (const EntryPointInfo &entry : discovered) {
        if (!TargetAccepts(target, entry.Stage))
            continue;
        selected.push_back(entry.Name);
        any_ray_tracing = any_ray_tracing || IsRayTracingStage(entry.Stage);
    }
    if (selected.empty()) {
        skipped = true;
        return 0;
    }

    /* One code blob per section: D3D holds a single shader, or (DXIL) a library of
       ray-tracing entry points. */
    bool library = false;
    if (target == ContainerTarget::Dxil && any_ray_tracing) {
        for (const EntryPointInfo &entry : discovered)
            if (!IsRayTracingStage(entry.Stage)) {
                sink.Error("the dxil target cannot hold ray-tracing entry points together "
                           "with '" + entry.Name + "' in one code section");
                return -1;
            }
        library = true;
    } else if ((target == ContainerTarget::Dxil || target == ContainerTarget::Dxbc) &&
               selected.size() != 1) {
        sink.Error(std::string("the ") + info.Name + " target holds one shader per container, "
                   "but the module has " + std::to_string(selected.size()) +
                   " entry points for it; split the module or drop the target");
        return -1;
    }

    std::vector<SessionTarget> targets(1);
    switch (target) {
    case ContainerTarget::Dxbc:
        targets[0].Format = SLANG_DXBC;
        targets[0].Profile = "sm_5_0";
        break;
    case ContainerTarget::Dxil:
        targets[0].Format = SLANG_DXIL;
        targets[0].Profile = inputs.D3DProfile;
        if (library)
            targets[0].ExtraFlags = SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM;
        break;
    case ContainerTarget::Spirv:
        targets[0].Format = SLANG_SPIRV;
        targets[0].Profile = inputs.SpirvProfile;
        targets[0].SpirvOptions = true;
        break;
    case ContainerTarget::CudaPtx:
    case ContainerTarget::OptixPtx:
        targets[0].Format = SLANG_PTX;
        /* Layout only: the D3D registers of the same program (the cross-map). */
        targets.resize(2);
        targets[1].Format = SLANG_DXIL;
        targets[1].Profile = inputs.D3DProfile;
        break;
    }

    ComPtr<slang::ISession> session;
    if (CreateSession(inputs, targets, target, session, sink))
        return -1;
    slang::IModule *module = LoadModule(session, inputs, sink);
    if (!module)
        return -1;
    std::vector<EntryPointInfo> entries;
    if (GetEntryPoints(module, entries, sink))
        return -1;

    std::vector<slang::IComponentType *> components{module};
    for (const EntryPointInfo &entry : entries)
        if (TargetAccepts(target, entry.Stage))
            components.push_back(entry.EntryPoint.get());

    ComPtr<slang::IComponentType> composite, linked;
    ComPtr<slang::IBlob> diagnostics;
    if (SLANG_FAILED(session->createCompositeComponentType(
            components.data(), (SlangInt)components.size(), composite.writeRef(),
            diagnostics.writeRef()))) {
        sink.Add(diagnostics);
        sink.Error(std::string("failed to compose the program for the ") + info.Name + " target");
        return -1;
    }
    sink.Add(diagnostics);
    if (SLANG_FAILED(composite->link(linked.writeRef(), diagnostics.writeRef())) || !linked) {
        sink.Add(diagnostics);
        sink.Error(std::string("failed to link the program for the ") + info.Name + " target");
        return -1;
    }
    sink.Add(diagnostics);

    ComPtr<slang::IBlob> code;
    const bool single_shader = target == ContainerTarget::Dxbc ||
                               (target == ContainerTarget::Dxil && !library);
    const SlangResult code_result =
        single_shader ? linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef())
                      : linked->getTargetCode(0, code.writeRef(), diagnostics.writeRef());
    sink.Add(diagnostics);
    if (SLANG_FAILED(code_result) || !code || !code->getBufferSize()) {
        sink.Error(std::string("slang produced no ") + info.Name + " code");
        return -1;
    }

    ContainerSection code_section;
    code_section.Kind = NVRHI_SC_SECTION_CODE;
    code_section.Target = info.SectionTarget;
    const uint8_t *bytes = static_cast<const uint8_t *>(code->getBufferPointer());
    code_section.Data.assign(bytes, bytes + code->getBufferSize());
    std::string ptx;
    if (IsPtx(target)) {
        /* cuModuleLoadData and optixModuleCreate take NUL-terminated PTX; the section size
           counts the terminator. */
        while (!code_section.Data.empty() && code_section.Data.back() == 0)
            code_section.Data.pop_back();
        ptx.assign(code_section.Data.begin(), code_section.Data.end());
        code_section.Data.push_back(0);
    }

    slang::ProgramLayout *layout = linked->getLayout(0, diagnostics.writeRef());
    sink.Add(diagnostics);
    slang::ProgramLayout *register_layout = layout;
    if (IsPtx(target)) {
        register_layout = linked->getLayout(1, diagnostics.writeRef());
        sink.Add(diagnostics);
    }
    if (!layout || !register_layout) {
        sink.Error(std::string("slang produced no layout for the ") + info.Name + " target");
        return -1;
    }

    ContainerReflection reflection;
    ReflectionBuilder builder(target, reflection, sink);
    if (builder.AddGlobals(layout, register_layout))
        return -1;
    if (IsPtx(target) && SetPtxGlobalParams(layout, ptx, reflection, sink))
        return -1;

    const std::vector<std::string> ptx_entries = PtxEntryNames(ptx);
    const std::vector<SpirvEntryPoint> spirv_entries =
        target == ContainerTarget::Spirv ? SpirvEntryPoints(code_section.Data)
                                         : std::vector<SpirvEntryPoint>();
    for (SlangUInt i = 0; i < layout->getEntryPointCount(); ++i) {
        slang::EntryPointReflection *entry = layout->getEntryPointByIndex(i);
        slang::EntryPointReflection *register_entry =
            register_layout == layout ? entry : register_layout->getEntryPointByIndex(i);
        std::string symbol;
        if (ResolveSymbolName(target, entry, layout, ptx_entries, spirv_entries, symbol, sink))
            return -1;
        if (builder.AddEntryPoint(entry, register_entry, symbol))
            return -1;
    }

    ContainerSection reflection_section;
    reflection_section.Kind = NVRHI_SC_SECTION_REFLECTION;
    reflection_section.Target = info.SectionTarget;
    std::string error;
    if (reflection.Serialize(reflection_section.Data, error)) {
        sink.Error(error);
        return -1;
    }
    sections.push_back(std::move(code_section));
    sections.push_back(std::move(reflection_section));
    return 0;
}

int ReadSource(const std::string &path, std::string &text, DiagnosticSink &sink) {
#ifdef _WIN32
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
#else
    std::ifstream input(path, std::ios::binary);
#endif
    if (!input) {
        sink.Error("cannot open '" + path + "'");
        return -1;
    }
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        sink.Error("failed to read '" + path + "'");
        return -1;
    }
    return 0;
}

int Compile(const ContainerCompileRequest &request, ContainerCompileResult &result,
            DiagnosticSink &sink) {
    if (request.Targets.empty()) {
        sink.Error("no container targets requested");
        return -1;
    }
    if (!GetGlobalSession()) {
        sink.Error("failed to create the slang global session");
        return -1;
    }

    ModuleInputs inputs;
    std::error_code error_code;
    std::filesystem::path path = std::filesystem::absolute(
#ifdef _WIN32
        std::filesystem::u8path(request.Source),
#else
        std::filesystem::path(request.Source),
#endif
        error_code);
    if (error_code)
        path = request.Source;
    inputs.Path = path.lexically_normal().u8string();
    inputs.ModuleName = path.stem().u8string();
    if (ReadSource(request.Source, inputs.Source, sink))
        return -1;

    inputs.SearchPathStorage = request.IncludeDirs;
    for (const std::string &directory : inputs.SearchPathStorage)
        inputs.SearchPaths.push_back(directory.c_str());
    for (const auto &define : request.Defines)
        inputs.Macros.push_back({define.first.c_str(), define.second.c_str()});

    SlangArguments arguments;
    if (TranslateExtraArgs(request.ExtraArgs, arguments, sink))
        return -1;
    /* The reflection records the renamed CUDA kernel ('main' -> 'main_0') as the entry
       point's symbol name, so Slang's rename warning 40100 is noise here (and -WX would
       print it as an error). */
    if (ParseCompilerOptions(arguments.Common, inputs.Common, sink) ||
        ParseCompilerOptions(arguments.Spirv, inputs.Spirv, sink) ||
        ParseCompilerOptions({"-warnings-disable", "40100"}, inputs.Ptx, sink))
        return -1;
    inputs.SpirvProfile = arguments.SpirvProfile;
    inputs.RowMajor = arguments.RowMajor;
    inputs.D3DProfile = "sm_" + request.ShaderModel;

    const bool wants_optix = std::find(request.Targets.begin(), request.Targets.end(),
                                       ContainerTarget::OptixPtx) != request.Targets.end();
    if (wants_optix) {
        std::string include_dir = request.OptixIncludeDir;
        if (include_dir.empty()) {
            const char *install_dir = getenv("OptiX_INSTALL_DIR");
            if (install_dir && *install_dir)
                include_dir = std::string(install_dir) + "/include";
        }
        if (include_dir.empty()) {
            sink.Error("the optix target needs the OptiX SDK headers: pass --optix-include "
                       "<dir> or set OptiX_INSTALL_DIR");
            return -1;
        }
        /* NVRTC arguments, one per line (what slangc makes of -Xnvrtc... -I<dir> -X.). */
        inputs.OptixArguments = "-I" + include_dir;
        slang::CompilerOptionEntry entry = {};
        entry.name = slang::CompilerOptionName::DownstreamArgs;
        entry.value.kind = slang::CompilerOptionValueKind::String;
        entry.value.stringValue0 = "nvrtc";
        entry.value.stringValue1 = inputs.OptixArguments.c_str();
        inputs.OptixEntries.push_back(entry);
    }

    /* Discovery: parse and check the module once (front-end errors are reported here, once),
       collect its entry points and the files it depends on. */
    std::vector<EntryPointInfo> discovered;
    {
        std::vector<SessionTarget> targets(1);
        targets[0].Format = SLANG_DXIL;
        targets[0].Profile = inputs.D3DProfile;
        ComPtr<slang::ISession> session;
        if (CreateSession(inputs, targets, ContainerTarget::Dxil, session, sink))
            return -1;
        slang::IModule *module = LoadModule(session, inputs, sink);
        if (!module)
            return -1;
        if (GetEntryPoints(module, discovered, sink))
            return -1;
        if (discovered.empty()) {
            sink.Error("'" + request.Source + "' defines no [shader(\"...\")] entry points");
            return -1;
        }
        for (SlangInt32 i = 0; i < module->getDependencyFileCount(); ++i) {
            const char *dependency = module->getDependencyFilePath(i);
            if (dependency && *dependency)
                result.Includes.push_back(dependency);
        }
    }

    std::vector<ContainerSection> sections;
    for (ContainerTarget target : request.Targets) {
        bool skipped = false;
        if (CompileTarget(inputs, target, discovered, sections, skipped, sink))
            return -1;
    }
    if (sections.empty()) {
        sink.Error("none of the requested targets can hold an entry point of '" +
                   request.Source + "'");
        return -1;
    }

    std::string error;
    if (WriteShaderContainer(sections, result.Container, error)) {
        sink.Error(error);
        return -1;
    }
    return 0;
}

} // namespace

#pragma endregion[Sessions]

const char *ContainerTargetName(ContainerTarget target) {
    return InfoOf(target).Name;
}

bool ParseContainerTarget(const std::string &name, ContainerTarget &target) {
    for (const TargetInfo &info : TargetInfos) {
        if (name == info.Name) {
            target = info.Target;
            return true;
        }
    }
    return false;
}

int CompileShaderContainer(const ContainerCompileRequest &request,
                           ContainerCompileResult &result) {
    result.Container.clear();
    result.Includes.clear();
    result.Diagnostics.clear();
    try {
        DiagnosticSink sink(result.Diagnostics);
        const int status = Compile(request, result, sink);
        if (status)
            result.Container.clear();
        return status;
    } catch (const std::bad_alloc &) {
        result.Diagnostics += "ShaderTool: out of memory during container compilation\n";
    } catch (...) {
        result.Diagnostics += "ShaderTool: unexpected internal error during container compilation\n";
    }
    result.Container.clear();
    return -1;
}
