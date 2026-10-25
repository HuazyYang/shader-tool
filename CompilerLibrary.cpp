/* CompilerLibrary.cpp -- all three in-process compiler backends behind the frozen
   CompilerLibraryCompile seam (contract section 8):
     [Shared helpers]  UTF-8/wide conversion, file loading, error text assembly
     [DXC backend]     Dxil + Spirv via IDxcCompiler3 (dxcompiler, implicit link)
     [FXC backend]     Dxbc via D3DCompile (d3dcompiler_47, Windows only)
     [Slang backend]   SlangDxbc/SlangDxil/SlangSpirv via the slang sp* C API
   Vendored headers/import libraries are wired up by the build (contract section 7);
   nothing here loads libraries or discovers compilers at runtime. */

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include "CompilerLibrary.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <new>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3dcompiler.h>
#else
#include "WinAdapter.h" /* from the vendored dxc-linux-x64 package */
#endif

#include <dxcapi.h>

#include <slang.h> /* vendored slang, pinned 2026.10.2 (thirdparty/VERSIONS) */

#pragma region[ Shared helpers ]

namespace {

/* Minimal exception-free COM smart pointer, used for both dxc interfaces and
   d3dcompiler blobs so no <wrl/client.h> dependency leaks to non-Windows builds. */
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;
    ~ComPtr() { Reset(); }

    T *Get() const { return Pointer; }
    T *operator->() const { return Pointer; }
    explicit operator bool() const { return Pointer != nullptr; }

    /* Releases any held interface and exposes the slot for an output parameter. */
    T **GetAddressOf() {
        Reset();
        return &Pointer;
    }

    void Reset() {
        if (Pointer) {
            Pointer->Release();
            Pointer = nullptr;
        }
    }

private:
    T *Pointer = nullptr;
};

void AppendToolError(std::string &diagnostics, const std::string &message) {
    diagnostics += "ShaderTool: ";
    diagnostics += message;
    diagnostics += '\n';
}

void AppendHresultError(std::string &diagnostics, const char *operation, HRESULT code) {
    char line[192];
    snprintf(line, sizeof(line), "ShaderTool: %s failed (HRESULT 0x%08lX)\n", operation,
             (unsigned long)(uint32_t)code);
    diagnostics += line;
}

#ifndef _WIN32
void AppendCodePointUtf8(std::string &text, uint32_t code_point) {
    if (code_point < 0x80u) {
        text += (char)code_point;
    } else if (code_point < 0x800u) {
        text += (char)(0xC0u | (code_point >> 6));
        text += (char)(0x80u | (code_point & 0x3Fu));
    } else if (code_point < 0x10000u) {
        text += (char)(0xE0u | (code_point >> 12));
        text += (char)(0x80u | ((code_point >> 6) & 0x3Fu));
        text += (char)(0x80u | (code_point & 0x3Fu));
    } else {
        text += (char)(0xF0u | (code_point >> 18));
        text += (char)(0x80u | ((code_point >> 12) & 0x3Fu));
        text += (char)(0x80u | ((code_point >> 6) & 0x3Fu));
        text += (char)(0x80u | (code_point & 0x3Fu));
    }
}

int DecodeUtf8(const std::string &text, size_t &index, uint32_t &code_point) {
    const unsigned char lead = (unsigned char)text[index];
    size_t extra;
    if (lead < 0x80u) {
        code_point = lead;
        extra = 0;
    } else if ((lead & 0xE0u) == 0xC0u) {
        code_point = lead & 0x1Fu;
        extra = 1;
    } else if ((lead & 0xF0u) == 0xE0u) {
        code_point = lead & 0x0Fu;
        extra = 2;
    } else if ((lead & 0xF8u) == 0xF0u) {
        code_point = lead & 0x07u;
        extra = 3;
    } else {
        return -1;
    }
    if (index + extra >= text.size() + (extra ? 0 : 1))
        return -1;
    for (size_t k = 1; k <= extra; ++k) {
        const unsigned char follow = (unsigned char)text[index + k];
        if ((follow & 0xC0u) != 0x80u)
            return -1;
        code_point = (code_point << 6) | (follow & 0x3Fu);
    }
    index += extra + 1;
    return 0;
}
#endif

/* UTF-8 -> native wide (UTF-16 on Windows / 2-byte wchar_t targets, UTF-32 otherwise). */
int Utf8ToWide(const std::string &text, std::wstring &wide, std::string &diagnostics) {
    wide.clear();
#ifdef _WIN32
    if (text.empty())
        return 0;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
                                          (int)text.size(), NULL, 0);
    if (count <= 0) {
        AppendToolError(diagnostics, "invalid UTF-8 in '" + text + "'");
        return -1;
    }
    wide.resize((size_t)count);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
                             (int)text.size(), &wide[0], count)) {
        AppendToolError(diagnostics, "invalid UTF-8 in '" + text + "'");
        return -1;
    }
    return 0;
#else
    size_t index = 0;
    uint32_t code_point = 0;
    while (index < text.size()) {
        if (DecodeUtf8(text, index, code_point)) {
            AppendToolError(diagnostics, "invalid UTF-8 in '" + text + "'");
            return -1;
        }
        if (sizeof(wchar_t) == 2 && code_point >= 0x10000u) {
            code_point -= 0x10000u;
            wide += (wchar_t)(0xD800u + (code_point >> 10));
            wide += (wchar_t)(0xDC00u + (code_point & 0x3FFu));
        } else {
            wide += (wchar_t)code_point;
        }
    }
    return 0;
#endif
}

std::string WideToUtf8(const wchar_t *text) {
    std::string result;
    if (!text || !*text)
        return result;
#ifdef _WIN32
    const int count =
        WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (count > 1) {
        result.resize((size_t)count - 1);
        if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], count, NULL, NULL))
            result.clear();
    }
#else
    for (size_t i = 0; text[i];) {
        uint32_t code_point = (uint32_t)text[i++];
        if (sizeof(wchar_t) == 2 && code_point >= 0xD800u && code_point < 0xDC00u &&
            text[i] >= 0xDC00u && (uint32_t)text[i] < 0xE000u) {
            code_point =
                0x10000u + ((code_point - 0xD800u) << 10) + ((uint32_t)text[i++] - 0xDC00u);
        }
        AppendCodePointUtf8(result, code_point);
    }
#endif
    return result;
}

int ReadFileBytes(const std::string &path, std::vector<char> &data,
                  std::string &diagnostics) {
    FILE *file = NULL;
#ifdef _WIN32
    std::wstring wide;
    if (Utf8ToWide(path, wide, diagnostics))
        return -1;
    file = _wfopen(wide.c_str(), L"rb");
#else
    file = fopen(path.c_str(), "rb");
#endif
    if (!file) {
        AppendToolError(diagnostics, "cannot open '" + path + "'");
        return -1;
    }
    data.clear();
    char buffer[16384];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        data.insert(data.end(), buffer, buffer + count);
    const bool failed = ferror(file) != 0;
    fclose(file);
    if (failed) {
        AppendToolError(diagnostics, "failed to read '" + path + "'");
        data.clear();
        return -1;
    }
    return 0;
}

bool IsAbsolutePath(const std::string &path) {
    if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
        return true;
    return path.size() >= 2 && path[1] == ':';
}

std::string DirectoryName(const std::string &path) {
    const size_t position = path.find_last_of("/\\");
    if (position == std::string::npos)
        return ".";
    if (position == 0)
        return path.substr(0, 1);
    return path.substr(0, position);
}

std::string JoinPath(const std::string &directory, const std::string &name) {
    if (directory.empty() || IsAbsolutePath(name))
        return name;
    const char last = directory[directory.size() - 1];
    if (last == '/' || last == '\\')
        return directory + name;
    return directory + "/" + name;
}

bool StartsWith(const std::string &text, const char *prefix) {
    return text.rfind(prefix, 0) == 0;
}

} // namespace

#pragma endregion[Shared helpers]

#pragma region[ DXC backend (Dxil + Spirv) ]

namespace {

/* Wraps the default include handler and records every successfully resolved include
   path (dxc hands the resolved path to LoadSource). Stack-allocated: refcounting is
   inert, dxc only uses the handler for the duration of Compile. */
class DxcIncludeRecorder final : public IDxcIncludeHandler {
public:
    DxcIncludeRecorder(IDxcIncludeHandler *fallback, std::vector<std::string> &includes)
        : Fallback(fallback), Includes(includes) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) noexcept override {
        if (!object)
            return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IDxcIncludeHandler)) ||
            IsEqualIID(riid, __uuidof(IUnknown))) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 2; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR file_name,
                                         IDxcBlob **include_source) noexcept override {
        const HRESULT code = Fallback->LoadSource(file_name, include_source);
        if (SUCCEEDED(code) && include_source && *include_source) {
            try {
                Includes.push_back(WideToUtf8(file_name));
            } catch (...) {
                (*include_source)->Release();
                *include_source = nullptr;
                return E_OUTOFMEMORY;
            }
        }
        return code;
    }

private:
    IDxcIncludeHandler *Fallback;
    std::vector<std::string> &Includes;
};

int CompileWithDxc(const CompileVariantRequest &request, CompileVariantResult &result) {
    std::vector<char> source;
    if (ReadFileBytes(request.Source, source, result.Diagnostics))
        return -1;

    /* The dxc interfaces are stateless across Compile calls; create them once per
       process instead of once per variant (mirrors the cached slang session). */
    static ComPtr<IDxcUtils> utils;
    static ComPtr<IDxcCompiler3> compiler;
    static ComPtr<IDxcIncludeHandler> default_handler;
    HRESULT code = S_OK;
    if (!utils) {
        code = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.GetAddressOf()));
        if (FAILED(code)) {
            AppendHresultError(result.Diagnostics, "create IDxcUtils", code);
            return -1;
        }
    }
    if (!compiler) {
        code = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf()));
        if (FAILED(code)) {
            AppendHresultError(result.Diagnostics, "create IDxcCompiler3", code);
            return -1;
        }
    }
    if (!default_handler) {
        code = utils->CreateDefaultIncludeHandler(default_handler.GetAddressOf());
        if (FAILED(code) || !default_handler) {
            AppendHresultError(result.Diagnostics, "create dxc include handler", code);
            return -1;
        }
    }
    DxcIncludeRecorder include_recorder(default_handler.Get(), result.Includes);

    /* Argument vector. The source path goes first so dxc names the main file and
       resolves relative includes against it; ExtraArgs are canonical dxc tokens and
       pass through verbatim (they already carry -O<n> plus the -fspv-... and
       -fvk-... tokens for SPIR-V). */
    const std::string entry = request.EntryPoint.empty() ? "main" : request.EntryPoint;
    std::vector<std::string> arguments;
    arguments.push_back(request.Source);
    arguments.push_back("-T");
    arguments.push_back(request.Profile);
    if (!StartsWith(request.Profile, "lib")) { /* dxc rejects -E for lib profiles */
        arguments.push_back("-E");
        arguments.push_back(entry);
    }
    for (const auto &define : request.Defines) {
        arguments.push_back("-D");
        arguments.push_back(define.first + "=" + define.second);
    }
    for (const std::string &directory : request.IncludeDirs) {
        arguments.push_back("-I");
        arguments.push_back(directory);
    }
    for (const std::string &extra : request.ExtraArgs)
        arguments.push_back(extra);
    if (request.Backend == ShaderToolBackend::Spirv)
        arguments.push_back("-spirv");

    std::vector<std::wstring> wide_arguments(arguments.size());
    std::vector<LPCWSTR> argument_pointers(arguments.size());
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (Utf8ToWide(arguments[i], wide_arguments[i], result.Diagnostics))
            return -1;
        argument_pointers[i] = wide_arguments[i].c_str();
    }

    static const char empty_source = 0;
    DxcBuffer buffer = {};
    buffer.Ptr = source.empty() ? &empty_source : source.data();
    buffer.Size = source.size();
    buffer.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcResult> results;
    code = compiler->Compile(&buffer, argument_pointers.data(),
                             (UINT32)argument_pointers.size(), &include_recorder,
                             IID_PPV_ARGS(results.GetAddressOf()));
    if (FAILED(code) || !results) {
        AppendHresultError(result.Diagnostics, "IDxcCompiler3::Compile", code);
        return -1;
    }

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(results->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()),
                                     nullptr)) &&
        errors && errors->GetStringLength())
        result.Diagnostics.append(errors->GetStringPointer(), errors->GetStringLength());

    HRESULT status = E_FAIL;
    if (FAILED(results->GetStatus(&status)) || FAILED(status)) {
        if (result.Diagnostics.empty())
            AppendHresultError(result.Diagnostics, "dxc compilation", status);
        return -1;
    }

    ComPtr<IDxcBlob> object;
    code = results->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.GetAddressOf()),
                              nullptr);
    if (FAILED(code) || !object || !object->GetBufferSize()) {
        AppendToolError(result.Diagnostics, "dxc produced no object for '" +
                                                request.Source + "'");
        return -1;
    }
    const uint8_t *bytes = (const uint8_t *)object->GetBufferPointer();
    result.Object.assign(bytes, bytes + object->GetBufferSize());
    return 0;
}

} // namespace

#pragma endregion[DXC backend]

#pragma region[ FXC backend (Dxbc, Windows only) ]

#ifdef _WIN32

namespace {

/* Canonical dxc-style token -> D3DCOMPILE_* flag translation. Tokens that have no
   D3DCompile equivalent are rejected with a clear per-backend error. */
int TranslateFxcExtraArgs(const std::vector<std::string> &extra_args, UINT &flags,
                          std::string &diagnostics) {
    flags = 0;
    for (size_t i = 0; i < extra_args.size(); ++i) {
        const std::string &argument = extra_args[i];
        if (argument == "-WX")
            flags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
        else if (argument == "-Zi")
            flags |= D3DCOMPILE_DEBUG;
        else if (argument == "-Zpr")
            flags |= D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
        else if (argument == "-Zpc")
            flags |= D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR;
        else if (argument == "-O0")
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;
        else if (argument == "-O1")
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL1;
        else if (argument == "-O2")
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL2;
        else if (argument == "-O3")
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
        else if (argument == "-Od")
            flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
        else if (argument == "-Gec")
            flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
        else if (argument == "-Ges")
            flags |= D3DCOMPILE_ENABLE_STRICTNESS;
        else if (argument == "-Gis")
            flags |= D3DCOMPILE_IEEE_STRICTNESS;
        else if (argument == "-Vd")
            flags |= D3DCOMPILE_SKIP_VALIDATION;
        else if (argument == "-all-resources-bound" || argument == "-all_resources_bound")
            flags |= D3DCOMPILE_ALL_RESOURCES_BOUND;
        else if (argument == "-Qembed_debug") {
            /* No-op: D3DCompile embeds debug info in the blob whenever -Zi is set. */
        } else {
            AppendToolError(diagnostics, "option '" + argument +
                                             "' is not supported by the DXBC backend");
            return -1;
        }
    }
    return 0;
}

/* Custom ID3DInclude: resolves include names against (1) the directory of the
   including file (tracked through the pParentData pointer -> path map) and
   (2) request.IncludeDirs in order; records every resolved path. */
class FxcIncludeRecorder final : public ID3DInclude {
public:
    FxcIncludeRecorder(std::string root_directory,
                       const std::vector<std::string> &include_dirs,
                       std::vector<std::string> &includes)
        : RootDirectory(std::move(root_directory)), IncludeDirs(include_dirs),
          Includes(includes) {}

    std::string Error;

    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE include_type, LPCSTR file_name,
                                   LPCVOID parent_data, LPCVOID *out_data,
                                   UINT *out_bytes) noexcept override {
        try {
            if (!file_name || !out_data || !out_bytes)
                return E_INVALIDARG;
            std::string parent_directory = RootDirectory;
            const auto parent = OpenEntries.find(parent_data);
            if (parent != OpenEntries.end())
                parent_directory = DirectoryName(parent->second.Path);

            std::vector<std::string> candidates;
            if (IsAbsolutePath(file_name)) {
                candidates.push_back(file_name);
            } else if (include_type == D3D_INCLUDE_LOCAL) {
                candidates.push_back(JoinPath(parent_directory, file_name));
                for (const std::string &directory : IncludeDirs)
                    candidates.push_back(JoinPath(directory, file_name));
            } else {
                for (const std::string &directory : IncludeDirs)
                    candidates.push_back(JoinPath(directory, file_name));
                candidates.push_back(JoinPath(parent_directory, file_name));
            }

            for (const std::string &candidate : candidates) {
                std::vector<char> data;
                std::string ignored;
                if (ReadFileBytes(candidate, data, ignored))
                    continue;
                const UINT size = (UINT)data.size();
                if (data.empty())
                    data.push_back(0); /* keep a stable non-null pointer */
                const void *pointer = data.data();
                Entry entry;
                entry.Data = std::move(data);
                entry.Path = candidate;
                Includes.push_back(candidate);
                OpenEntries.emplace(pointer, std::move(entry));
                *out_data = pointer;
                *out_bytes = size;
                return S_OK;
            }
            if (Error.empty())
                Error = "ShaderTool: cannot open include file '" +
                        std::string(file_name) + "'\n";
            return E_FAIL;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) noexcept override {
        OpenEntries.erase(data);
        return S_OK;
    }

private:
    struct Entry {
        std::vector<char> Data;
        std::string Path;
    };

    std::string RootDirectory;
    const std::vector<std::string> &IncludeDirs;
    std::vector<std::string> &Includes;
    std::map<LPCVOID, Entry> OpenEntries;
};

int CompileWithFxc(const CompileVariantRequest &request, CompileVariantResult &result) {
    UINT flags = 0;
    if (TranslateFxcExtraArgs(request.ExtraArgs, flags, result.Diagnostics))
        return -1;

    std::vector<char> source;
    if (ReadFileBytes(request.Source, source, result.Diagnostics))
        return -1;

    std::vector<D3D_SHADER_MACRO> macros;
    macros.reserve(request.Defines.size() + 1);
    for (const auto &define : request.Defines) {
        D3D_SHADER_MACRO macro = {define.first.c_str(), define.second.c_str()};
        macros.push_back(macro);
    }
    const D3D_SHADER_MACRO terminator = {NULL, NULL};
    macros.push_back(terminator);

    const std::string entry = request.EntryPoint.empty() ? "main" : request.EntryPoint;
    FxcIncludeRecorder include_recorder(DirectoryName(request.Source),
                                        request.IncludeDirs, result.Includes);

    ComPtr<ID3DBlob> object;
    ComPtr<ID3DBlob> errors;
    const HRESULT code = D3DCompile(
        source.empty() ? "" : source.data(), source.size(), request.Source.c_str(),
        macros.data(), &include_recorder, entry.c_str(), request.Profile.c_str(), flags,
        0, object.GetAddressOf(), errors.GetAddressOf());

    if (!include_recorder.Error.empty())
        result.Diagnostics += include_recorder.Error;
    if (errors && errors->GetBufferSize()) {
        const char *text = (const char *)errors->GetBufferPointer();
        size_t length = errors->GetBufferSize();
        while (length && text[length - 1] == 0)
            --length; /* the error blob is NUL-terminated */
        result.Diagnostics.append(text, length);
    }

    if (FAILED(code)) {
        if (result.Diagnostics.empty())
            AppendHresultError(result.Diagnostics, "D3DCompile", code);
        return -1;
    }
    if (!object || !object->GetBufferSize()) {
        AppendToolError(result.Diagnostics,
                        "D3DCompile produced no object for '" + request.Source + "'");
        return -1;
    }
    const uint8_t *bytes = (const uint8_t *)object->GetBufferPointer();
    result.Object.assign(bytes, bytes + object->GetBufferSize());
    return 0;
}

} // namespace

#else

namespace {

int CompileWithFxc(const CompileVariantRequest &request, CompileVariantResult &result) {
    (void)request;
    AppendToolError(result.Diagnostics, "DXBC backend is only available on Windows");
    return -1;
}

} // namespace

#endif

#pragma endregion[FXC backend]

#pragma region[ Slang backend (SlangDxbc + SlangDxil + SlangSpirv) ]

namespace {

/* The global slang session is expensive to create; keep one for the process. The
   sp* C API is used throughout for its stable ABI (slang pinned at 2026.10.2). */
SlangSession *GetSlangSession() {
    static SlangSession *session = spCreateSession(NULL);
    return session;
}

int SlangStageFromProfile(const std::string &profile, SlangStage &stage,
                          std::string &diagnostics) {
    struct StageMapping {
        const char *Prefix;
        SlangStage Stage;
    };
    static const StageMapping mappings[] = {
        {"vs_", SLANG_STAGE_VERTEX},   {"hs_", SLANG_STAGE_HULL},
        {"ds_", SLANG_STAGE_DOMAIN},   {"gs_", SLANG_STAGE_GEOMETRY},
        {"ps_", SLANG_STAGE_FRAGMENT}, {"cs_", SLANG_STAGE_COMPUTE},
        {"ms_", SLANG_STAGE_MESH},     {"as_", SLANG_STAGE_AMPLIFICATION},
    };
    for (const StageMapping &mapping : mappings) {
        if (StartsWith(profile, mapping.Prefix)) {
            stage = mapping.Stage;
            return 0;
        }
    }
    AppendToolError(diagnostics, "profile '" + profile +
                                     "' has no single entry-point stage supported by "
                                     "the slang backend");
    return -1;
}

/* Canonical dxc-style token translation for slang. Direct API calls where slang has
   one (-Zi, -Zpr/-Zpc, -O<n>); command-line pass-through (spProcessCommandLineArguments)
   for the dxc-compatible Vulkan tokens slang accepts; clear rejection for the rest. */
int TranslateSlangExtraArgs(SlangCompileRequest *compile,
                            const std::vector<std::string> &extra_args,
                            std::vector<std::string> &cli_args,
                            SlangSourceLanguage &language, std::string &diagnostics) {
    language = SLANG_SOURCE_LANGUAGE_SLANG;
    for (size_t i = 0; i < extra_args.size(); ++i) {
        const std::string &argument = extra_args[i];
        const bool is_vk_shift =
            argument == "-fvk-b-shift" || argument == "-fvk-s-shift" ||
            argument == "-fvk-t-shift" || argument == "-fvk-u-shift";
        if (argument == "-WX") {
            cli_args.push_back("-warnings-as-errors");
            cli_args.push_back("all");
        } else if (argument == "-Zi") {
            spSetDebugInfoLevel(compile, SLANG_DEBUG_INFO_LEVEL_STANDARD);
        } else if (argument == "-Qembed_debug") {
            /* No-op: slang embeds debug info in the target code when enabled. */
        } else if (argument == "-Zpr") {
            spSetMatrixLayoutMode(compile, SLANG_MATRIX_LAYOUT_ROW_MAJOR);
        } else if (argument == "-Zpc") {
            spSetMatrixLayoutMode(compile, SLANG_MATRIX_LAYOUT_COLUMN_MAJOR);
        } else if (argument == "-O0") {
            spSetOptimizationLevel(compile, SLANG_OPTIMIZATION_LEVEL_NONE);
        } else if (argument == "-O1") {
            spSetOptimizationLevel(compile, SLANG_OPTIMIZATION_LEVEL_DEFAULT);
        } else if (argument == "-O2") {
            spSetOptimizationLevel(compile, SLANG_OPTIMIZATION_LEVEL_HIGH);
        } else if (argument == "-O3") {
            spSetOptimizationLevel(compile, SLANG_OPTIMIZATION_LEVEL_MAXIMAL);
        } else if (argument == "-lang") {
            if (i + 1 >= extra_args.size()) {
                AppendToolError(diagnostics, "-lang requires a language name");
                return -1;
            }
            const std::string &name = extra_args[++i];
            if (name == "hlsl")
                language = SLANG_SOURCE_LANGUAGE_HLSL;
            else if (name == "slang")
                language = SLANG_SOURCE_LANGUAGE_SLANG;
            else {
                AppendToolError(diagnostics, "unsupported -lang value '" + name + "'");
                return -1;
            }
        } else if (argument == "-unscoped-enum" ||
                   argument == "-fvk-use-entrypoint-name" ||
                   argument == "-fvk-use-dx-layout" ||
                   argument == "-fvk-use-gl-layout") {
            cli_args.push_back(argument);
        } else if (argument == "-fvk-use-scalar-layout") {
            /* Slang spells this one differently (ShaderMake used the same
               translation for its slang path). */
            cli_args.push_back("-force-glsl-scalar-layout");
        } else if (StartsWith(argument, "-fspv-target-env=") ||
                   StartsWith(argument, "-fspv-extension=")) {
            /* No-op: slang has no equivalents; it derives the SPIR-V version
               from the session target and enables extensions on demand.
               ShaderMake likewise never forwarded these to slang. */
        } else if (argument == "-g") {
            spSetDebugInfoLevel(compile, SLANG_DEBUG_INFO_LEVEL_STANDARD);
        } else if (argument == "-capability" || argument == "-fp-mode") {
            if (i + 1 >= extra_args.size()) {
                AppendToolError(diagnostics, argument + " requires a value");
                return -1;
            }
            cli_args.push_back(argument);
            cli_args.push_back(extra_args[++i]);
        } else if (is_vk_shift) {
            if (i + 2 >= extra_args.size()) {
                AppendToolError(diagnostics,
                                argument + " requires a shift and a space argument");
                return -1;
            }
            cli_args.push_back(argument);
            cli_args.push_back(extra_args[++i]);
            cli_args.push_back(extra_args[++i]);
        } else if (argument == "-enable-16bit-types") {
            /* No-op: slang enables 16-bit types from the target profile. */
        } else if (argument == "-all-resources-bound") {
            /* No-op: not meaningful for slang-generated code. */
        } else if (argument == "-HV") {
            AppendToolError(diagnostics, "option '-HV' is not supported by the slang "
                                         "backend; use -lang hlsl for HLSL input");
            return -1;
        } else {
            AppendToolError(diagnostics, "option '" + argument +
                                             "' is not supported by the slang backend");
            return -1;
        }
    }
    return 0;
}

int CompileWithSlang(const CompileVariantRequest &request, CompileVariantResult &result) {
    SlangSession *session = GetSlangSession();
    if (!session) {
        AppendToolError(result.Diagnostics, "failed to create the slang session");
        return -1;
    }
    SlangCompileRequest *compile = spCreateCompileRequest(session);
    if (!compile) {
        AppendToolError(result.Diagnostics, "failed to create a slang compile request");
        return -1;
    }

    int status = -1;
    do {
        SlangCompileTarget target = SLANG_DXIL;
        if (request.Backend == ShaderToolBackend::SlangDxbc)
            target = SLANG_DXBC;
        else if (request.Backend == ShaderToolBackend::SlangSpirv)
            target = SLANG_SPIRV;
        spSetCodeGenTarget(compile, target);

        const SlangProfileID profile = spFindProfile(session, request.Profile.c_str());
        if (profile == SLANG_PROFILE_UNKNOWN) {
            AppendToolError(result.Diagnostics, "slang does not recognize profile '" +
                                                    request.Profile + "'");
            break;
        }
        spSetTargetProfile(compile, 0, profile);

        SlangStage stage = SLANG_STAGE_NONE;
        if (SlangStageFromProfile(request.Profile, stage, result.Diagnostics))
            break;

        std::vector<std::string> cli_args;
        SlangSourceLanguage language = SLANG_SOURCE_LANGUAGE_SLANG;
        if (TranslateSlangExtraArgs(compile, request.ExtraArgs, cli_args, language,
                                    result.Diagnostics))
            break;
        if (!cli_args.empty()) {
            std::vector<const char *> argument_pointers(cli_args.size());
            for (size_t i = 0; i < cli_args.size(); ++i)
                argument_pointers[i] = cli_args[i].c_str();
            const SlangResult processed = spProcessCommandLineArguments(
                compile, argument_pointers.data(), (int)argument_pointers.size());
            if (SLANG_FAILED(processed)) {
                const char *text = spGetDiagnosticOutput(compile);
                if (text && *text)
                    result.Diagnostics += text;
                AppendToolError(result.Diagnostics,
                                "slang rejected the translated argument list");
                break;
            }
        }

        for (const std::string &directory : request.IncludeDirs)
            spAddSearchPath(compile, directory.c_str());
        for (const auto &define : request.Defines)
            spAddPreprocessorDefine(compile, define.first.c_str(),
                                    define.second.c_str());

        const int translation_unit =
            spAddTranslationUnit(compile, language, "shadertool");
        if (translation_unit < 0) {
            AppendToolError(result.Diagnostics, "failed to add a slang translation unit");
            break;
        }
        spAddTranslationUnitSourceFile(compile, translation_unit,
                                       request.Source.c_str());

        const std::string entry =
            request.EntryPoint.empty() ? "main" : request.EntryPoint;
        const int entry_point =
            spAddEntryPoint(compile, translation_unit, entry.c_str(), stage);
        if (entry_point < 0) {
            AppendToolError(result.Diagnostics,
                            "failed to add slang entry point '" + entry + "'");
            break;
        }

        const SlangResult compiled = spCompile(compile);
        const char *diagnostic_text = spGetDiagnosticOutput(compile);
        if (diagnostic_text && *diagnostic_text)
            result.Diagnostics += diagnostic_text;
        if (SLANG_FAILED(compiled))
            break;

        size_t code_size = 0;
        const void *code = spGetEntryPointCode(compile, entry_point, &code_size);
        if (!code || !code_size) {
            AppendToolError(result.Diagnostics, "slang produced no object for '" +
                                                    request.Source + "'");
            break;
        }
        const uint8_t *bytes = (const uint8_t *)code;
        result.Object.assign(bytes, bytes + code_size);

        const int dependency_count = spGetDependencyFileCount(compile);
        for (int i = 0; i < dependency_count; ++i) {
            const char *path = spGetDependencyFilePath(compile, i);
            if (path && *path)
                result.Includes.push_back(path);
        }
        status = 0;
    } while (0);

    spDestroyCompileRequest(compile);
    return status;
}

} // namespace

#pragma endregion[Slang backend]

int CompilerLibraryCompile(const CompileVariantRequest &request,
                           CompileVariantResult &result) {
    result.Object.clear();
    result.Includes.clear();
    result.Diagnostics.clear();
    try {
        switch (request.Backend) {
            case ShaderToolBackend::Dxbc:
                return CompileWithFxc(request, result);
            case ShaderToolBackend::Dxil:
            case ShaderToolBackend::Spirv:
                return CompileWithDxc(request, result);
            case ShaderToolBackend::SlangDxbc:
            case ShaderToolBackend::SlangDxil:
            case ShaderToolBackend::SlangSpirv:
                return CompileWithSlang(request, result);
        }
        AppendToolError(result.Diagnostics, "unknown backend");
        return -1;
    } catch (const std::bad_alloc &) {
        result.Diagnostics = "ShaderTool: out of memory during compilation\n";
        return -1;
    } catch (...) {
        result.Diagnostics = "ShaderTool: unexpected internal error during compilation\n";
        return -1;
    }
}
