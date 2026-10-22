#ifndef SHADER_TOOL_CONTAINER_WRITER_H
#define SHADER_TOOL_CONTAINER_WRITER_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nvrhi-shader-container.h" /* include/, identical to <nvrhi/shader-container.h> */

/* Serialization of NVRHI shader containers (include/nvrhi-shader-container.h, the format
   definition owned by NVRHI). Nothing here knows about Slang: ContainerCompiler.cpp fills a
   ContainerReflection per target and hands the sections to WriteShaderContainer. */

/* The reflection of one code target, serialized as one REFLECTION section. Records are
   stored in their final on-disk form; names are offsets into the string table, which
   AddString maintains (offset 0 is the empty string). */
class ContainerReflection {
public:
    ContainerReflection();

    /* Offset of a NUL-terminated copy of 'text' in the string table (deduplicated). */
    uint32_t AddString(const std::string &text);

    /* Appends a type with its fields (contiguous, 'type.firstField'/'fieldCount' are set
       here) and returns its index; a structurally identical type already in the table is
       reused instead. */
    uint32_t AddType(NvrhiScType type, const std::vector<NvrhiScField> &fields);

    /* The section bytes: NvrhiScReflectionHeader, the record tables and the string table,
       each table aligned to 8 bytes. 0 on success, -1 (with 'error' set) if a table offset
       would not fit 32 bits. */
    int Serialize(std::vector<uint8_t> &section, std::string &error) const;

    std::vector<NvrhiScEntryPoint> EntryPoints;
    std::vector<NvrhiScParameter> Parameters; /* global parameters first */
    std::vector<NvrhiScType> Types;
    std::vector<NvrhiScField> Fields;
    uint32_t GlobalParameterCount = 0;
    uint32_t GlobalParamsSize = 0;
    uint32_t GlobalParamsAlignment = 0;

private:
    std::string Strings;
    std::map<std::string, uint32_t> StringOffsets;
    std::map<std::string, uint32_t> TypeIndices; /* structural key -> type index */
};

struct ContainerSection {
    uint32_t Kind = 0;   /* NVRHI_SC_SECTION_* */
    uint32_t Target = 0; /* NVRHI_SC_TARGET_* */
    std::vector<uint8_t> Data;
};

/* The container bytes: NvrhiScHeader, the section table and the payloads, each payload
   aligned to 8 bytes. 0 on success, -1 (with 'error' set) if the result would not fit the
   format's 32-bit/64-bit fields or the host is not little-endian. */
int WriteShaderContainer(const std::vector<ContainerSection> &sections,
                         std::vector<uint8_t> &container, std::string &error);

#endif
