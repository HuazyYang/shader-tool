/* Tests for ShaderTool's NVRHI shader containers (include/nvrhi-shader-container.h).

     ShaderToolContainerTests --targets ptx,optix,dxil,spirv
                              --compute <compute.nvsc> --raytracing <raytracing.nvsc>
     ShaderToolContainerTests --dump <file.nvsc>

   The reader below validates everything NVRHI's parser (src/common/shader-container.cpp)
   validates -- bounds, NUL-terminated strings, index ranges, enum ranges -- plus the
   producer's own invariants (alignment, total size, CODE/REFLECTION pairing). The semantic
   checks compare against the layouts documented in tests/container/*.slang, against the
   PTX (SLANG_globalParams, .entry names) and the SPIR-V (OpEntryPoint names). */

#include "nvrhi-shader-container.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failed_checks = 0;
int g_total_checks = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        g_total_checks++;                                                      \
        if (!(condition)) {                                                    \
            g_failed_checks++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        g_total_checks++;                                                      \
        const unsigned long long actual_value = (unsigned long long)(actual);  \
        const unsigned long long expected_value = (unsigned long long)(expected); \
        if (actual_value != expected_value) {                                  \
            g_failed_checks++;                                                 \
            printf("FAIL %s:%d: %s == %llu, expected %s == %llu\n", __FILE__,  \
                   __LINE__, #actual, actual_value, #expected, expected_value); \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Reader                                                                     */
/* ------------------------------------------------------------------------- */

struct Range {
    const uint8_t *Data = nullptr;
    size_t Size = 0;

    bool Contains(uint64_t offset, uint64_t length) const {
        return offset <= Size && length <= Size - offset;
    }

    template <typename T>
    bool Table(uint64_t offset, uint64_t count, std::vector<T> &out) const {
        out.clear();
        if (count > UINT64_MAX / sizeof(T) || !Contains(offset, count * sizeof(T)))
            return false;
        out.resize((size_t)count);
        if (count)
            memcpy(out.data(), Data + offset, (size_t)count * sizeof(T));
        return true;
    }
};

struct Reflection {
    NvrhiScReflectionHeader Header = {};
    std::vector<NvrhiScEntryPoint> EntryPoints;
    std::vector<NvrhiScParameter> Parameters;
    std::vector<NvrhiScType> Types;
    std::vector<NvrhiScField> Fields;
    std::vector<char> Strings;

    const char *String(uint32_t offset) const {
        return offset < Strings.size() ? Strings.data() + offset : "<bad string>";
    }
};

struct Section {
    NvrhiScSection Record = {};
    std::vector<uint8_t> Data;
};

struct Container {
    NvrhiScHeader Header = {};
    std::vector<Section> Sections;
};

void Fail(std::string &error, const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    error = text;
}

bool ValidString(const std::vector<char> &strings, uint32_t offset) {
    return offset < strings.size() &&
           memchr(strings.data() + offset, 0, strings.size() - offset) != nullptr;
}

bool ValidIndex(uint32_t index, uint32_t count) {
    return index == NVRHI_SC_INVALID_INDEX || index < count;
}

bool ValidRange(uint32_t first, uint32_t count, uint32_t total) {
    return first <= total && count <= total - first;
}

bool ParseReflection(const std::vector<uint8_t> &bytes, Reflection &out, std::string &error) {
    const Range range{bytes.data(), bytes.size()};
    std::vector<NvrhiScReflectionHeader> header;
    if (!range.Table(0, 1, header)) {
        Fail(error, "truncated reflection header");
        return false;
    }
    out.Header = header[0];
    const NvrhiScReflectionHeader &h = out.Header;
    if (!range.Table(h.entryPointOffset, h.entryPointCount, out.EntryPoints) ||
        !range.Table(h.parameterOffset, h.parameterCount, out.Parameters) ||
        !range.Table(h.typeOffset, h.typeCount, out.Types) ||
        !range.Table(h.fieldOffset, h.fieldCount, out.Fields) ||
        !range.Table(h.stringTableOffset, h.stringTableSize, out.Strings) ||
        h.globalParameterCount > h.parameterCount) {
        Fail(error, "reflection tables out of bounds");
        return false;
    }
    /* Producer invariants: 8-aligned tables, the string table ends with a NUL. */
    for (uint32_t offset : {h.entryPointOffset, h.parameterOffset, h.typeOffset, h.fieldOffset,
                            h.stringTableOffset})
        if (offset % 8) {
            Fail(error, "reflection table at offset %u is not 8-byte aligned", offset);
            return false;
        }
    if (out.Strings.empty() || out.Strings.back() != 0) {
        Fail(error, "string table is not NUL-terminated");
        return false;
    }

    for (uint32_t i = 0; i < h.entryPointCount; ++i) {
        const NvrhiScEntryPoint &e = out.EntryPoints[i];
        if (!ValidString(out.Strings, e.nameOffset) ||
            !ValidString(out.Strings, e.symbolNameOffset) ||
            !ValidRange(e.firstParameter, e.parameterCount, h.parameterCount)) {
            Fail(error, "bad entry point %u", i);
            return false;
        }
    }
    for (uint32_t i = 0; i < h.parameterCount; ++i) {
        const NvrhiScParameter &p = out.Parameters[i];
        if (!ValidString(out.Strings, p.nameOffset) || !ValidIndex(p.typeIndex, h.typeCount) ||
            p.kind > NVRHI_SC_PARAM_PUSH_CONSTANTS || p.textureDimension > 9) {
            Fail(error, "bad parameter %u", i);
            return false;
        }
    }
    for (uint32_t i = 0; i < h.typeCount; ++i) {
        const NvrhiScType &t = out.Types[i];
        if (!ValidString(out.Strings, t.nameOffset) || !ValidIndex(t.elementType, h.typeCount) ||
            !ValidRange(t.firstField, t.fieldCount, h.fieldCount) ||
            t.kind > NVRHI_SC_TYPE_POINTER || t.scalarType > NVRHI_SC_SCALAR_FLOAT64 ||
            t.rowCount > 255 || t.columnCount > 255) {
            Fail(error, "bad type %u", i);
            return false;
        }
    }
    for (uint32_t i = 0; i < h.fieldCount; ++i) {
        const NvrhiScField &f = out.Fields[i];
        if (!ValidString(out.Strings, f.nameOffset) || !ValidIndex(f.typeIndex, h.typeCount)) {
            Fail(error, "bad field %u", i);
            return false;
        }
    }
    return true;
}

bool ParseContainer(const std::vector<uint8_t> &bytes, Container &out, std::string &error) {
    const Range all{bytes.data(), bytes.size()};
    std::vector<NvrhiScHeader> header;
    if (!all.Table(0, 1, header) || header[0].magic != NVRHI_SC_MAGIC) {
        Fail(error, "not an NVRHI shader container");
        return false;
    }
    out.Header = header[0];
    if (out.Header.versionMajor != NVRHI_SC_VERSION_MAJOR ||
        out.Header.headerSize != sizeof(NvrhiScHeader) ||
        out.Header.totalSize != bytes.size() || out.Header.totalSize % 8) {
        Fail(error, "bad container header (version %u, header size %u, total size %llu, file %zu)",
             out.Header.versionMajor, out.Header.headerSize,
             (unsigned long long)out.Header.totalSize, bytes.size());
        return false;
    }
    std::vector<NvrhiScSection> table;
    if (!all.Table(out.Header.headerSize, out.Header.sectionCount, table)) {
        Fail(error, "bad section table");
        return false;
    }
    uint64_t previous_end = out.Header.headerSize + table.size() * sizeof(NvrhiScSection);
    for (size_t i = 0; i < table.size(); ++i) {
        const NvrhiScSection &record = table[i];
        if (!all.Contains(record.offset, record.size) || record.offset % 8 ||
            record.offset < previous_end ||
            (record.kind != NVRHI_SC_SECTION_CODE && record.kind != NVRHI_SC_SECTION_REFLECTION) ||
            record.target < NVRHI_SC_TARGET_DXBC || record.target > NVRHI_SC_TARGET_OPTIX_IR) {
            Fail(error, "bad section %zu", i);
            return false;
        }
        previous_end = record.offset + record.size;
        Section section;
        section.Record = record;
        section.Data.assign(bytes.begin() + (size_t)record.offset,
                            bytes.begin() + (size_t)(record.offset + record.size));
        out.Sections.push_back(std::move(section));
    }
    return true;
}

const Section *FindSection(const Container &container, uint32_t kind, uint32_t target) {
    for (const Section &section : container.Sections)
        if (section.Record.kind == kind && section.Record.target == target)
            return &section;
    return nullptr;
}

bool LoadFile(const char *path, std::vector<uint8_t> &bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

/* ------------------------------------------------------------------------- */
/* Lookups                                                                    */
/* ------------------------------------------------------------------------- */

const NvrhiScParameter *FindGlobal(const Reflection &r, const char *name) {
    for (uint32_t i = 0; i < r.Header.globalParameterCount; ++i)
        if (!strcmp(r.String(r.Parameters[i].nameOffset), name))
            return &r.Parameters[i];
    return nullptr;
}

const NvrhiScEntryPoint *FindEntry(const Reflection &r, const char *name) {
    for (const NvrhiScEntryPoint &entry : r.EntryPoints)
        if (!strcmp(r.String(entry.nameOffset), name))
            return &entry;
    return nullptr;
}

const NvrhiScParameter *FindEntryParameter(const Reflection &r, const NvrhiScEntryPoint *entry,
                                           const char *name) {
    for (uint32_t i = 0; entry && i < entry->parameterCount; ++i) {
        const NvrhiScParameter &p = r.Parameters[entry->firstParameter + i];
        if (!strcmp(r.String(p.nameOffset), name))
            return &p;
    }
    return nullptr;
}

const NvrhiScType *TypeAt(const Reflection &r, uint32_t index) {
    return index < r.Types.size() ? &r.Types[index] : nullptr;
}

const NvrhiScField *FindField(const Reflection &r, uint32_t type_index, const char *name) {
    const NvrhiScType *type = TypeAt(r, type_index);
    for (uint32_t i = 0; type && i < type->fieldCount; ++i) {
        const NvrhiScField &field = r.Fields[type->firstField + i];
        if (!strcmp(r.String(field.nameOffset), name))
            return &field;
    }
    return nullptr;
}

/* ------------------------------------------------------------------------- */
/* Code inspection                                                            */
/* ------------------------------------------------------------------------- */

std::vector<std::string> PtxEntries(const std::string &ptx) {
    std::vector<std::string> names;
    size_t position = 0;
    while ((position = ptx.find(".entry ", position)) != std::string::npos) {
        position += 7;
        const size_t end = ptx.find_first_of("( \t\r\n", position);
        names.push_back(ptx.substr(position, end - position));
    }
    return names;
}

bool PtxGlobalParams(const std::string &ptx, uint32_t &size, uint32_t &alignment) {
    const size_t symbol = ptx.find("SLANG_globalParams[");
    if (symbol == std::string::npos)
        return false;
    const size_t line = ptx.rfind('\n', symbol);
    const size_t align = ptx.find(".align ", line == std::string::npos ? 0 : line);
    if (align == std::string::npos || align > symbol)
        return false;
    alignment = (uint32_t)strtoul(ptx.c_str() + align + 7, nullptr, 10);
    size = (uint32_t)strtoul(ptx.c_str() + symbol + strlen("SLANG_globalParams["), nullptr, 10);
    return true;
}

std::vector<std::string> SpirvEntries(const std::vector<uint8_t> &code) {
    std::vector<std::string> names;
    std::vector<uint32_t> words(code.size() / 4);
    if (words.size() < 5)
        return names;
    memcpy(words.data(), code.data(), words.size() * 4);
    for (size_t index = 5; index < words.size();) {
        const uint32_t length = words[index] >> 16;
        if (!length || index + length > words.size())
            break;
        if ((words[index] & 0xFFFFu) == 15 && length >= 4) {
            const char *name = reinterpret_cast<const char *>(&words[index + 3]);
            names.emplace_back(name, strnlen(name, (length - 3) * 4));
        }
        index += length;
    }
    return names;
}

bool Contains(const std::vector<std::string> &names, const std::string &name) {
    for (const std::string &candidate : names)
        if (candidate == name)
            return true;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Dump                                                                       */
/* ------------------------------------------------------------------------- */

const char *TargetName(uint32_t target) {
    switch (target) {
    case NVRHI_SC_TARGET_DXBC: return "dxbc";
    case NVRHI_SC_TARGET_DXIL: return "dxil";
    case NVRHI_SC_TARGET_SPIRV: return "spirv";
    case NVRHI_SC_TARGET_CUDA_PTX: return "ptx";
    case NVRHI_SC_TARGET_OPTIX_PTX: return "optix";
    case NVRHI_SC_TARGET_OPTIX_IR: return "optix-ir";
    default: return "?";
    }
}

void DumpReflection(const Reflection &r) {
    printf("  globals: %u parameters, block %u bytes (align %u)\n", r.Header.globalParameterCount,
           r.Header.globalParamsSize, r.Header.globalParamsAlignment);
    for (size_t i = 0; i < r.Parameters.size(); ++i) {
        const NvrhiScParameter &p = r.Parameters[i];
        printf("  param %2zu %-14s kind=%-2u type=%-3d array=%-3d space=%u slot=%-3u "
               "uniform=[%u +%u stride %u] dim=%u flags=%u\n",
               i, r.String(p.nameOffset), p.kind, (int)p.typeIndex, (int)p.arraySize, p.space,
               p.slot, p.uniformOffset, p.uniformSize, p.uniformStride, p.textureDimension,
               p.flags);
    }
    for (const NvrhiScEntryPoint &e : r.EntryPoints)
        printf("  entry %s -> %s stage=0x%x group=%u,%u,%u params=[%u,+%u] block %u (align %u)\n",
               r.String(e.nameOffset), r.String(e.symbolNameOffset), e.stage,
               e.threadGroupSize[0], e.threadGroupSize[1], e.threadGroupSize[2],
               e.firstParameter, e.parameterCount, e.paramsSize, e.paramsAlignment);
    for (size_t i = 0; i < r.Types.size(); ++i) {
        const NvrhiScType &t = r.Types[i];
        printf("  type %2zu %-28s kind=%u scalar=%u %ux%u size=%u align=%u stride=%u count=%d "
               "element=%d",
               i, r.String(t.nameOffset), t.kind, t.scalarType, t.rowCount, t.columnCount,
               t.size, t.alignment, t.stride, (int)t.elementCount, (int)t.elementType);
        for (uint32_t f = 0; f < t.fieldCount; ++f) {
            const NvrhiScField &field = r.Fields[t.firstField + f];
            printf(" {%s:%d@%u}", r.String(field.nameOffset), (int)field.typeIndex, field.offset);
        }
        printf("\n");
    }
}

int Dump(const char *path) {
    std::vector<uint8_t> bytes;
    if (!LoadFile(path, bytes)) {
        printf("cannot read %s\n", path);
        return 1;
    }
    Container container;
    std::string error;
    if (!ParseContainer(bytes, container, error)) {
        printf("%s: %s\n", path, error.c_str());
        return 1;
    }
    printf("%s: %zu bytes, version %u.%u, %zu sections\n", path, bytes.size(),
           container.Header.versionMajor, container.Header.versionMinor,
           container.Sections.size());
    for (const Section &section : container.Sections) {
        printf("section %s %s: offset %llu, %llu bytes\n",
               section.Record.kind == NVRHI_SC_SECTION_CODE ? "CODE" : "REFLECTION",
               TargetName(section.Record.target), (unsigned long long)section.Record.offset,
               (unsigned long long)section.Record.size);
        if (section.Record.kind == NVRHI_SC_SECTION_REFLECTION) {
            Reflection reflection;
            if (!ParseReflection(section.Data, reflection, error)) {
                printf("  %s\n", error.c_str());
                return 1;
            }
            DumpReflection(reflection);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Checks                                                                     */
/* ------------------------------------------------------------------------- */

/* Loads a container, checks the structure and that 'targets' (and only they) are present,
   each CODE with its REFLECTION. */
bool LoadContainer(const char *path, const std::vector<uint32_t> &targets, Container &container) {
    std::vector<uint8_t> bytes;
    CHECK(LoadFile(path, bytes));
    std::string error;
    const bool parsed = ParseContainer(bytes, container, error);
    if (!parsed)
        printf("FAIL %s: %s\n", path, error.c_str());
    CHECK(parsed);
    if (!parsed)
        return false;
    CHECK_EQ(container.Header.versionMinor, NVRHI_SC_VERSION_MINOR);
    CHECK_EQ(container.Sections.size(), targets.size() * 2);
    for (uint32_t target : targets) {
        CHECK(FindSection(container, NVRHI_SC_SECTION_CODE, target) != nullptr);
        CHECK(FindSection(container, NVRHI_SC_SECTION_REFLECTION, target) != nullptr);
    }
    return true;
}

/* Structural invariants of one reflection that hold for every target. */
void CheckReflectionInvariants(const Reflection &r, uint32_t target) {
    const bool ptx = target == NVRHI_SC_TARGET_CUDA_PTX || target == NVRHI_SC_TARGET_OPTIX_PTX;
    for (uint32_t i = 0; i < r.Header.globalParameterCount; ++i) {
        const NvrhiScParameter &p = r.Parameters[i];
        CHECK_EQ(p.flags & NVRHI_SC_PARAM_FLAG_ENTRY_POINT, 0u);
        if (ptx)
            CHECK((uint64_t)p.uniformOffset + p.uniformSize <= r.Header.globalParamsSize);
    }
    if (!ptx) {
        CHECK_EQ(r.Header.globalParamsSize, 0u);
    }
    uint32_t next = r.Header.globalParameterCount;
    for (const NvrhiScEntryPoint &e : r.EntryPoints) {
        CHECK_EQ(e.firstParameter, next); /* contiguous, after the globals */
        next = e.firstParameter + e.parameterCount;
        for (uint32_t i = 0; i < e.parameterCount; ++i) {
            const NvrhiScParameter &p = r.Parameters[e.firstParameter + i];
            CHECK_EQ(p.flags & NVRHI_SC_PARAM_FLAG_ENTRY_POINT, NVRHI_SC_PARAM_FLAG_ENTRY_POINT);
            if (ptx)
                CHECK((uint64_t)p.uniformOffset + p.uniformSize <= e.paramsSize);
        }
    }
    CHECK_EQ(next, r.Parameters.size());
}

/* PTX code: NUL-terminated, SLANG_globalParams matches the header, symbols are entries. */
void CheckPtx(const Section &code, const Reflection &r) {
    CHECK(!code.Data.empty() && code.Data.back() == 0);
    const std::string ptx(code.Data.begin(), code.Data.end() - (code.Data.empty() ? 0 : 1));
    CHECK(ptx.find('\0') == std::string::npos);
    uint32_t size = 0, alignment = 0;
    CHECK(PtxGlobalParams(ptx, size, alignment));
    CHECK_EQ(r.Header.globalParamsSize, size);
    CHECK_EQ(r.Header.globalParamsAlignment, alignment);
    const std::vector<std::string> entries = PtxEntries(ptx);
    for (const NvrhiScEntryPoint &e : r.EntryPoints) {
        const bool found = Contains(entries, r.String(e.symbolNameOffset));
        if (!found)
            printf("FAIL: PTX has no .entry %s\n", r.String(e.symbolNameOffset));
        CHECK(found);
    }
}

void CheckSpirv(const Section &code, const Reflection &r) {
    uint32_t magic = 0;
    CHECK(code.Data.size() >= 20 && code.Data.size() % 4 == 0);
    if (code.Data.size() >= 4)
        memcpy(&magic, code.Data.data(), 4);
    CHECK_EQ(magic, 0x07230203u);
    const std::vector<std::string> entries = SpirvEntries(code.Data);
    CHECK_EQ(entries.size(), r.EntryPoints.size());
    for (const NvrhiScEntryPoint &e : r.EntryPoints) {
        const bool found = Contains(entries, r.String(e.symbolNameOffset));
        if (!found)
            printf("FAIL: SPIR-V has no OpEntryPoint %s\n", r.String(e.symbolNameOffset));
        CHECK(found);
    }
}

void CheckDxil(const Section &code) {
    CHECK(code.Data.size() >= 4 && !memcmp(code.Data.data(), "DXBC", 4)); /* DXIL container */
}

bool GetReflection(const Container &container, uint32_t target, Reflection &r) {
    const Section *section = FindSection(container, NVRHI_SC_SECTION_REFLECTION, target);
    if (!section)
        return false;
    std::string error;
    const bool parsed = ParseReflection(section->Data, r, error);
    if (!parsed)
        printf("FAIL %s reflection: %s\n", TargetName(target), error.c_str());
    CHECK(parsed);
    if (parsed)
        CheckReflectionInvariants(r, target);
    return parsed;
}

/* The Particle element type of g_Particles: { float3 position @0; float mass @12 }. */
void CheckParticleType(const Reflection &r, uint32_t type_index) {
    const NvrhiScType *particle = TypeAt(r, type_index);
    CHECK(particle != nullptr);
    if (!particle)
        return;
    CHECK(!strcmp(r.String(particle->nameOffset), "Particle"));
    CHECK_EQ(particle->kind, NVRHI_SC_TYPE_STRUCT);
    CHECK_EQ(particle->size, 16u);
    CHECK_EQ(particle->fieldCount, 2u);
    const NvrhiScField *position = FindField(r, type_index, "position");
    const NvrhiScField *mass = FindField(r, type_index, "mass");
    CHECK(position && mass);
    if (!position || !mass)
        return;
    CHECK_EQ(position->offset, 0u);
    CHECK_EQ(mass->offset, 12u);
    const NvrhiScType *float3 = TypeAt(r, position->typeIndex);
    CHECK(float3 && float3->kind == NVRHI_SC_TYPE_VECTOR && float3->columnCount == 3 &&
          float3->scalarType == NVRHI_SC_SCALAR_FLOAT32 && float3->size == 12);
    const NvrhiScType *scalar = TypeAt(r, mass->typeIndex);
    CHECK(scalar && scalar->kind == NVRHI_SC_TYPE_SCALAR &&
          scalar->scalarType == NVRHI_SC_SCALAR_FLOAT32 && scalar->size == 4);
}

/* SimParams: { float4x4 transform @0; float3 gravity @64; uint count @76 }. */
void CheckSimParamsType(const Reflection &r, uint32_t type_index) {
    const NvrhiScType *params = TypeAt(r, type_index);
    CHECK(params && params->kind == NVRHI_SC_TYPE_STRUCT && params->size == 80);
    const NvrhiScField *transform = FindField(r, type_index, "transform");
    const NvrhiScField *gravity = FindField(r, type_index, "gravity");
    const NvrhiScField *count = FindField(r, type_index, "count");
    CHECK(transform && gravity && count);
    if (!transform || !gravity || !count)
        return;
    CHECK_EQ(transform->offset, 0u);
    CHECK_EQ(gravity->offset, 64u);
    CHECK_EQ(count->offset, 76u);
    const NvrhiScType *matrix = TypeAt(r, transform->typeIndex);
    CHECK(matrix && matrix->kind == NVRHI_SC_TYPE_MATRIX && matrix->rowCount == 4 &&
          matrix->columnCount == 4 && matrix->scalarType == NVRHI_SC_SCALAR_FLOAT32 &&
          matrix->size == 64);
    const NvrhiScType *uint_type = TypeAt(r, count->typeIndex);
    CHECK(uint_type && uint_type->scalarType == NVRHI_SC_SCALAR_UINT32);
}

/* tests/container/compute.slang */
void CheckCompute(const char *path, const std::vector<uint32_t> &requested) {
    printf("compute: %s\n", path);
    std::vector<uint32_t> targets;
    for (uint32_t target : requested)
        if (target != NVRHI_SC_TARGET_OPTIX_PTX) /* no ray-tracing entry points */
            targets.push_back(target);
    Container container;
    if (!LoadContainer(path, targets, container))
        return;

    for (uint32_t target : targets) {
        Reflection r;
        if (!GetReflection(container, target, r))
            continue;
        const Section *code = FindSection(container, NVRHI_SC_SECTION_CODE, target);
        const bool ptx = target == NVRHI_SC_TARGET_CUDA_PTX;

        CHECK_EQ(r.Header.globalParameterCount, 8u);
        const NvrhiScParameter *params = FindGlobal(r, "g_Params");
        const NvrhiScParameter *particles = FindGlobal(r, "g_Particles");
        const NvrhiScParameter *output = FindGlobal(r, "g_Output");
        const NvrhiScParameter *raw = FindGlobal(r, "g_Raw");
        const NvrhiScParameter *albedo = FindGlobal(r, "g_Albedo");
        const NvrhiScParameter *image = FindGlobal(r, "g_Image");
        const NvrhiScParameter *sampler = FindGlobal(r, "g_Sampler");
        const NvrhiScParameter *scale = FindGlobal(r, "g_Scale");
        CHECK(params && particles && output && raw && albedo && image && sampler && scale);
        if (!params || !particles || !output || !raw || !albedo || !image || !sampler || !scale)
            continue;

        CHECK_EQ(params->kind, NVRHI_SC_PARAM_CONSTANT_BUFFER);
        CHECK_EQ(particles->kind, NVRHI_SC_PARAM_STRUCTURED_BUFFER_SRV);
        CHECK_EQ(output->kind, NVRHI_SC_PARAM_STRUCTURED_BUFFER_UAV);
        CHECK_EQ(raw->kind, NVRHI_SC_PARAM_RAW_BUFFER_SRV);
        CHECK_EQ(albedo->kind, NVRHI_SC_PARAM_TEXTURE_SRV);
        CHECK_EQ(albedo->arraySize, 2u);
        CHECK_EQ(albedo->textureDimension, 3u); /* nvrhi::TextureDimension::Texture2D */
        CHECK_EQ(image->kind, NVRHI_SC_PARAM_TEXTURE_UAV);
        CHECK_EQ(image->textureDimension, 3u);
        CHECK_EQ(sampler->kind, NVRHI_SC_PARAM_SAMPLER);
        CHECK_EQ(scale->kind, NVRHI_SC_PARAM_UNIFORM);
        CheckParticleType(r, particles->typeIndex);
        CheckSimParamsType(r, params->typeIndex);
        const NvrhiScType *albedo_type = TypeAt(r, albedo->typeIndex);
        CHECK(albedo_type && albedo_type->kind == NVRHI_SC_TYPE_VECTOR &&
              albedo_type->columnCount == 4);

        /* Registers: explicit ones in the source; Vulkan bindings shifted by
           0 (t) / 128 (s) / 256 (b) / 384 (u) per space. */
        if (target == NVRHI_SC_TARGET_SPIRV) {
            CHECK_EQ(particles->space, 1u);
            CHECK_EQ(particles->slot, 3u);
            CHECK_EQ(params->space, 0u);
            CHECK_EQ(params->slot, 256u + 1u);
            CHECK_EQ(output->slot, 384u);
            CHECK_EQ(sampler->slot, 128u);
        } else {
            CHECK_EQ(particles->space, 1u);
            CHECK_EQ(particles->slot, 3u);
            CHECK_EQ(params->space, 0u);
            CHECK_EQ(params->slot, 1u);
            CHECK_EQ(output->space, 0u);
            CHECK_EQ(output->slot, 0u);
            CHECK_EQ(sampler->slot, 0u);
        }

        if (ptx) {
            /* The CUDA global block, documented in compute.slang. */
            CHECK_EQ(r.Header.globalParamsSize, 96u);
            CHECK_EQ(r.Header.globalParamsAlignment, 8u);
            CHECK_EQ(params->uniformOffset, 0u);
            CHECK_EQ(params->uniformSize, 8u);
            CHECK_EQ(particles->uniformOffset, 8u);
            CHECK_EQ(particles->uniformSize, 16u);
            CHECK_EQ(output->uniformOffset, 24u);
            CHECK_EQ(raw->uniformOffset, 40u);
            CHECK_EQ(raw->uniformSize, 16u);
            CHECK_EQ(albedo->uniformOffset, 56u);
            CHECK_EQ(albedo->uniformSize, 16u);
            CHECK_EQ(albedo->uniformStride, 8u);
            CHECK_EQ(image->uniformOffset, 72u);
            CHECK_EQ(sampler->uniformOffset, 80u);
            CHECK_EQ(scale->uniformOffset, 88u);
            CHECK_EQ(scale->uniformSize, 4u);
            CheckPtx(*code, r);
        } else {
            CHECK_EQ(scale->uniformOffset, 0u); /* first in the default constant buffer */
            CHECK_EQ(scale->uniformSize, 4u);
            if (target == NVRHI_SC_TARGET_SPIRV)
                CheckSpirv(*code, r);
            else if (target == NVRHI_SC_TARGET_DXIL)
                CheckDxil(*code);
        }

        CHECK_EQ(r.EntryPoints.size(), 1u);
        const NvrhiScEntryPoint *entry = FindEntry(r, "main");
        CHECK(entry != nullptr);
        if (!entry)
            continue;
        CHECK_EQ(entry->stage, NVRHI_SC_STAGE_COMPUTE);
        CHECK_EQ(entry->threadGroupSize[0], 64u);
        CHECK_EQ(entry->threadGroupSize[1], 1u);
        CHECK_EQ(entry->threadGroupSize[2], 1u);
        /* Slang renames a CUDA kernel called 'main' (a reserved name in C++). */
        CHECK(!strcmp(r.String(entry->symbolNameOffset), ptx ? "main_0" : "main"));
        CHECK_EQ(entry->parameterCount, 2u);
        const NvrhiScParameter *entry_scale = FindEntryParameter(r, entry, "entryScale");
        const NvrhiScParameter *entry_offset = FindEntryParameter(r, entry, "entryOffset");
        CHECK(entry_scale && entry_offset);
        if (entry_scale && entry_offset) {
            CHECK_EQ(entry_scale->kind, NVRHI_SC_PARAM_UNIFORM);
            CHECK_EQ(entry_scale->uniformOffset, 0u);
            CHECK_EQ(entry_offset->uniformOffset, 4u);
        }
        CHECK_EQ(entry->paramsSize, 8u);
        CHECK_EQ(entry->paramsAlignment, 4u);
    }
}

/* tests/container/raytracing.slang */
void CheckRayTracing(const char *path, const std::vector<uint32_t> &requested) {
    printf("raytracing: %s\n", path);
    std::vector<uint32_t> targets;
    for (uint32_t target : requested)
        if (target != NVRHI_SC_TARGET_CUDA_PTX) /* no compute entry points */
            targets.push_back(target);
    Container container;
    if (!LoadContainer(path, targets, container))
        return;

    for (uint32_t target : targets) {
        Reflection r;
        if (!GetReflection(container, target, r))
            continue;
        const Section *code = FindSection(container, NVRHI_SC_SECTION_CODE, target);
        const bool optix = target == NVRHI_SC_TARGET_OPTIX_PTX;

        CHECK_EQ(r.Header.globalParameterCount, 5u);
        const NvrhiScParameter *scene = FindGlobal(r, "g_Scene");
        const NvrhiScParameter *output = FindGlobal(r, "g_Output");
        const NvrhiScParameter *camera = FindGlobal(r, "g_Camera");
        const NvrhiScParameter *colors = FindGlobal(r, "g_Colors");
        const NvrhiScParameter *time = FindGlobal(r, "g_Time");
        CHECK(scene && output && camera && colors && time);
        if (!scene || !output || !camera || !colors || !time)
            continue;
        CHECK_EQ(scene->kind, NVRHI_SC_PARAM_ACCEL_STRUCT);
        CHECK_EQ(scene->space, 2u);
        CHECK_EQ(scene->slot, 0u); /* t0, space2: unshifted on Vulkan too */
        CHECK_EQ(output->kind, NVRHI_SC_PARAM_TEXTURE_UAV);
        CHECK_EQ(camera->kind, NVRHI_SC_PARAM_CONSTANT_BUFFER);
        CHECK_EQ(colors->kind, NVRHI_SC_PARAM_STRUCTURED_BUFFER_SRV);
        CHECK_EQ(time->kind, NVRHI_SC_PARAM_UNIFORM);

        if (optix) {
            CHECK_EQ(r.Header.globalParamsSize, 48u);
            CHECK_EQ(r.Header.globalParamsAlignment, 8u);
            CHECK_EQ(scene->uniformOffset, 0u);
            CHECK_EQ(scene->uniformSize, 8u);
            CHECK_EQ(output->uniformOffset, 8u);
            CHECK_EQ(camera->uniformOffset, 16u);
            CHECK_EQ(colors->uniformOffset, 24u);
            CHECK_EQ(colors->uniformSize, 16u);
            CHECK_EQ(time->uniformOffset, 40u);
            CheckPtx(*code, r);
        } else if (target == NVRHI_SC_TARGET_SPIRV) {
            CheckSpirv(*code, r);
        } else if (target == NVRHI_SC_TARGET_DXIL) {
            CheckDxil(*code);
        }

        struct Expected {
            const char *Name;
            uint32_t Stage;
            const char *OptixSymbol;
        };
        const Expected expected[] = {
            {"rayGen", NVRHI_SC_STAGE_RAY_GENERATION, "__raygen__rayGen"},
            {"miss", NVRHI_SC_STAGE_MISS, "__miss__miss"},
            {"closestHit", NVRHI_SC_STAGE_CLOSEST_HIT, "__closesthit__closestHit"},
            {"anyHit", NVRHI_SC_STAGE_ANY_HIT, "__anyhit__anyHit"},
        };
        CHECK_EQ(r.EntryPoints.size(), 4u);
        for (const Expected &e : expected) {
            const NvrhiScEntryPoint *entry = FindEntry(r, e.Name);
            CHECK(entry != nullptr);
            if (!entry)
                continue;
            CHECK_EQ(entry->stage, e.Stage);
            CHECK(!strcmp(r.String(entry->symbolNameOffset), optix ? e.OptixSymbol : e.Name));
            CHECK_EQ(entry->threadGroupSize[0], 0u);
        }

        const NvrhiScEntryPoint *hit = FindEntry(r, "closestHit");
        const NvrhiScParameter *hit_color = FindEntryParameter(r, hit, "hitColor");
        const NvrhiScParameter *material = FindEntryParameter(r, hit, "materialIndex");
        CHECK(hit && hit->parameterCount == 2 && hit_color && material);
        if (hit_color && material) {
            CHECK_EQ(hit_color->uniformOffset, 0u);
            CHECK_EQ(hit_color->uniformSize, 16u);
            CHECK_EQ(material->uniformOffset, 16u);
        }
        if (optix && hit) {
            /* SBT record data: sizeof({ float4; uint; }) in CUDA. */
            CHECK_EQ(hit->paramsSize, 32u);
            CHECK_EQ(hit->paramsAlignment, 16u);
        }
        const NvrhiScEntryPoint *miss = FindEntry(r, "miss");
        CHECK(miss && miss->parameterCount == 0 && miss->paramsSize == 0);
        const NvrhiScEntryPoint *ray_gen = FindEntry(r, "rayGen");
        CHECK(ray_gen && ray_gen->parameterCount == 1);
        if (ray_gen && optix)
            CHECK_EQ(ray_gen->paramsSize, 4u);
    }
}

bool ParseTargetList(const char *list, std::vector<uint32_t> &targets) {
    std::string text(list);
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string name = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (name == "dxbc") targets.push_back(NVRHI_SC_TARGET_DXBC);
        else if (name == "dxil") targets.push_back(NVRHI_SC_TARGET_DXIL);
        else if (name == "spirv") targets.push_back(NVRHI_SC_TARGET_SPIRV);
        else if (name == "ptx") targets.push_back(NVRHI_SC_TARGET_CUDA_PTX);
        else if (name == "optix") targets.push_back(NVRHI_SC_TARGET_OPTIX_PTX);
        else {
            printf("unknown target '%s'\n", name.c_str());
            return false;
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--dump"))
        return Dump(argv[2]);

    std::vector<uint32_t> targets;
    const char *compute = nullptr;
    const char *raytracing = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--targets")) {
            if (!ParseTargetList(argv[i + 1], targets))
                return 2;
        } else if (!strcmp(argv[i], "--compute")) {
            compute = argv[i + 1];
        } else if (!strcmp(argv[i], "--raytracing")) {
            raytracing = argv[i + 1];
        } else {
            printf("unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (targets.empty() || (!compute && !raytracing)) {
        printf("usage: %s --targets <list> [--compute <file>] [--raytracing <file>]\n"
               "       %s --dump <file>\n", argv[0], argv[0]);
        return 2;
    }

    if (compute)
        CheckCompute(compute, targets);
    if (raytracing)
        CheckRayTracing(raytracing, targets);

    printf("%d/%d checks passed\n", g_total_checks - g_failed_checks, g_total_checks);
    return g_failed_checks ? 1 : 0;
}
