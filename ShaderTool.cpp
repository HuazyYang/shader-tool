#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "CompilerLibrary.h"
#include "ContainerCompiler.h"
#include "Depfile.h"
#include "HeaderOutput.h"
#include "ShaderToolBlob.h"
#include "ShaderToolOptions.h"

namespace {

using DefineList = std::vector<std::pair<std::string, std::string>>;

void Error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("ShaderTool: error: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

// Expand -D permutation braces into the cartesian variant set. Only the first
// {...} group in a value is an axis; a valueless or empty define becomes "1".
int ExpandDefineVariants(const DefineList &defines, std::vector<DefineList> &variants) {
    std::vector<DefineList> current(1);

    for (const auto &define : defines) {
        const std::string &name = define.first;
        const std::string &value = define.second;
        std::vector<std::string> choices;

        const size_t open = value.find('{');
        if (open == std::string::npos) {
            if (value.find('}') != std::string::npos) {
                Error("stray '}' in value of define '%s'", name.c_str());
                return -1;
            }
            choices.push_back(value.empty() ? std::string("1") : value);
        } else {
            const size_t close = value.find('}', open + 1);
            if (close == std::string::npos) {
                Error("unterminated '{' in value of define '%s'", name.c_str());
                return -1;
            }
            const std::string prefix = value.substr(0, open);
            const std::string body = value.substr(open + 1, close - open - 1);
            const std::string suffix = value.substr(close + 1);
            if (prefix.find('}') != std::string::npos ||
                body.find('{') != std::string::npos ||
                suffix.find_first_of("{}") != std::string::npos) {
                Error("define '%s' may contain only one {a,b,c} permutation group",
                      name.c_str());
                return -1;
            }
            size_t start = 0;
            while (true) {
                const size_t comma = body.find(',', start);
                const std::string item = body.substr(
                    start, comma == std::string::npos ? std::string::npos
                                                      : comma - start);
                if (item.empty() && prefix.empty() && suffix.empty()) {
                    Error("empty value in permutation list of define '%s'",
                          name.c_str());
                    return -1;
                }
                choices.push_back(prefix + item + suffix);
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
        }

        std::vector<DefineList> next;
        next.reserve(current.size() * choices.size());
        for (const DefineList &base : current) {
            for (const std::string &choice : choices) {
                DefineList expanded = base;
                expanded.emplace_back(name, choice);
                next.push_back(std::move(expanded));
            }
        }
        current = std::move(next);
    }

    variants = std::move(current);
    return 0;
}

// Canonical permutation key via the single blob-library implementation.
int BuildVariantKey(const DefineList &defines, std::string &key) {
    key.clear();
    if (defines.empty())
        return 0;

    std::vector<ShaderToolBlobConstant> constants(defines.size());
    for (size_t i = 0; i < defines.size(); ++i) {
        constants[i].Name = defines[i].first.c_str();
        constants[i].Value = defines[i].second.c_str();
    }
    size_t length = 0;
    if (ShaderToolBlobBuildKey(constants.data(), constants.size(), nullptr, 0, &length))
        return -1;
    std::vector<char> buffer(length + 1);
    if (ShaderToolBlobBuildKey(constants.data(), constants.size(), buffer.data(),
                               buffer.size(), &length))
        return -1;
    key.assign(buffer.data(), length);
    return 0;
}

int AppendBlobBytes(void *context, const void *data, size_t size) {
    try {
        auto *output = static_cast<std::vector<uint8_t> *>(context);
        const auto *bytes = static_cast<const uint8_t *>(data);
        output->insert(output->end(), bytes, bytes + size);
        return 0;
    } catch (...) {
        return -1;
    }
}

int EnsureParentDirectory(const std::string &path) {
    std::error_code error_code;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty())
        return 0;
    std::filesystem::create_directories(parent, error_code);
    if (error_code) {
        Error("cannot create directory '%s': %s", parent.string().c_str(),
              error_code.message().c_str());
        return -1;
    }
    return 0;
}

// Private write-if-changed for the -Fo binary artifact (header and depfile
// outputs go through the HeaderOutput/Depfile seams, which do their own).
int WriteBinaryFileIfChanged(const std::string &path, const void *data, size_t size) {
    {
        std::ifstream input(path, std::ios::binary);
        if (input) {
            std::vector<char> existing((std::istreambuf_iterator<char>(input)),
                                       std::istreambuf_iterator<char>());
            if (!input.bad() && existing.size() == size &&
                (size == 0 || !memcmp(existing.data(), data, size)))
                return 0;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        Error("cannot open '%s' for writing", path.c_str());
        return -1;
    }
    if (size)
        output.write(static_cast<const char *>(data),
                     static_cast<std::streamsize>(size));
    output.close();
    if (!output) {
        Error("failed to write '%s'", path.c_str());
        return -1;
    }
    return 0;
}

void PrintDiagnostics(const std::string &diagnostics, const std::string &key,
                      bool with_key) {
    if (diagnostics.empty())
        return;
    if (with_key)
        fprintf(stderr, "ShaderTool: [%s]\n", key.empty() ? "<default>" : key.c_str());
    fputs(diagnostics.c_str(), stderr);
    if (diagnostics.back() != '\n')
        fputc('\n', stderr);
}

// Writes the artifact (-Fo binary or -Fh header) and, with -depfile, the
// dependency file (the source plus every resolved include).
int WriteOutputs(const ShaderToolOptions &options, const std::vector<uint8_t> &artifact,
                 const std::vector<std::string> &includes) {
    const std::string &target = options.OutputPath();
    if (EnsureParentDirectory(target))
        return 1;
    if (options.HeaderOutput()) {
        if (HeaderOutputWrite(options.HeaderOutputPath, options.HeaderVariableName,
                              artifact.data(), artifact.size())) {
            Error("failed to write header output '%s'",
                  options.HeaderOutputPath.c_str());
            return 1;
        }
    } else {
        if (WriteBinaryFileIfChanged(options.BinaryOutputPath, artifact.data(),
                                     artifact.size()))
            return 1;
    }

    if (!options.DepfilePath.empty()) {
        std::vector<std::string> dependencies;
        // The source is typically invoked with a config-relative path, but ninja
        // resolves depfile entries against the build directory - record it absolute.
        std::error_code absolute_error;
        std::filesystem::path absolute_source =
            std::filesystem::absolute(options.Source, absolute_error);
        dependencies.push_back(absolute_error ? options.Source
                                              : absolute_source.string());
        for (const std::string &include : includes) {
            std::filesystem::path absolute_include =
                std::filesystem::absolute(include, absolute_error);
            dependencies.push_back(absolute_error ? include
                                                  : absolute_include.string());
        }
        if (EnsureParentDirectory(options.DepfilePath))
            return 1;
        if (DepfileWrite(options.DepfilePath, target, dependencies)) {
            Error("failed to write depfile '%s'", options.DepfilePath.c_str());
            return 1;
        }
    }

    return 0;
}

int RunShaderTool(const ShaderToolOptions &options) {
    std::vector<DefineList> variants;
    if (ExpandDefineVariants(options.Defines, variants))
        return 1;

    const bool multi_variant = variants.size() > 1;
    std::vector<std::string> keys(variants.size());
    std::vector<CompileVariantResult> results(variants.size());
    size_t failed_count = 0;

    for (size_t i = 0; i < variants.size(); ++i) {
        if (BuildVariantKey(variants[i], keys[i])) {
            Error("failed to build the permutation key for '%s'",
                  options.Source.c_str());
            return 1;
        }

        CompileVariantRequest request;
        request.Backend = options.Backend;
        request.Source = options.Source;
        request.EntryPoint = options.EntryPoint;
        request.Profile = options.Profile;
        request.Defines = variants[i];
        // Global (-GD) defines reach every compile but stay out of the key and
        // the raw/NVSP decision; ShaderMake appended its globals after the
        // line defines, so keep that order.
        request.Defines.insert(request.Defines.end(), options.UnkeyedDefines.begin(),
                               options.UnkeyedDefines.end());
        request.IncludeDirs = options.IncludeDirs;
        request.ExtraArgs = options.ExtraArgs;

        CompileVariantResult &result = results[i];
        const int status = CompilerLibraryCompile(request, result);
        const bool empty_object = status == 0 && result.Object.empty();

        PrintDiagnostics(result.Diagnostics, keys[i],
                         multi_variant || (status != 0 && !keys[i].empty()));
        if (empty_object)
            Error("the compiler produced an empty object for variant '%s'",
                  keys[i].empty() ? "<default>" : keys[i].c_str());
        if (status != 0 || empty_object) {
            ++failed_count;
            if (multi_variant)
                Error("variant '%s' failed",
                      keys[i].empty() ? "<default>" : keys[i].c_str());
        }
    }

    if (failed_count) {
        Error("%zu of %zu variant(s) of '%s' failed to compile", failed_count,
              variants.size(), options.Source.c_str());
        return 1;
    }

    // Single variant without defines matches ShaderMake's raw output; anything
    // keyed goes into an NVSP blob.
    std::vector<uint8_t> artifact;
    const bool raw_output = variants.size() == 1 && variants[0].empty();
    if (raw_output) {
        artifact = std::move(results[0].Object);
    } else {
        if (ShaderToolBlobWriteFileHeader(AppendBlobBytes, &artifact)) {
            Error("failed to write the NVSP blob header");
            return 1;
        }
        for (size_t i = 0; i < variants.size(); ++i) {
            if (ShaderToolBlobWritePermutation(AppendBlobBytes, &artifact,
                                               keys[i].c_str(), keys[i].size(),
                                               results[i].Object.data(),
                                               results[i].Object.size())) {
                Error("failed to append permutation '%s' to the NVSP blob",
                      keys[i].c_str());
                return 1;
            }
        }
    }

    std::vector<std::string> includes;
    for (const CompileVariantResult &result : results)
        includes.insert(includes.end(), result.Includes.begin(), result.Includes.end());
    return WriteOutputs(options, artifact, includes);
}

// Container mode: the whole module, every listed target, one .nvsc artifact.
int RunContainer(const ShaderToolOptions &options) {
    std::vector<DefineList> variants;
    if (ExpandDefineVariants(options.Defines, variants))
        return 1;
    if (variants.size() != 1) {
        Error("permutation axes (-D NAME={a,b}) are not supported in container mode; "
              "build one container per permutation");
        return 1;
    }

    ContainerCompileRequest request;
    request.Source = options.Source;
    request.Defines = variants[0];
    request.Defines.insert(request.Defines.end(), options.UnkeyedDefines.begin(),
                           options.UnkeyedDefines.end());
    request.IncludeDirs = options.IncludeDirs;
    request.ExtraArgs = options.ExtraArgs;
    request.Targets = options.ContainerTargets;
    request.ShaderModel = options.ShaderModel;
    request.OptixIncludeDir = options.OptixIncludeDir;

    ContainerCompileResult result;
    const int status = CompileShaderContainer(request, result);
    PrintDiagnostics(result.Diagnostics, std::string(), false);
    if (status != 0) {
        Error("failed to compile '%s' into a shader container", options.Source.c_str());
        return 1;
    }
    return WriteOutputs(options, result.Container, result.Includes);
}

} // namespace

int main(int argc, char **argv) {
    try {
        ShaderToolOptions options;
        switch (ShaderToolOptionsParse(argc, argv, options)) {
        case OptionsParseStatus::Help:
            return 0;
        case OptionsParseStatus::Error:
            return 1;
        case OptionsParseStatus::Ok:
            break;
        }
        return options.Container ? RunContainer(options) : RunShaderTool(options);
    } catch (const std::exception &error) {
        fprintf(stderr, "ShaderTool: fatal: %s\n", error.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "ShaderTool: fatal: unknown error\n");
        return 1;
    }
}
