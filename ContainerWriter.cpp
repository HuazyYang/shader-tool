/* ContainerWriter.cpp -- byte layout of NVRHI shader containers (see ContainerWriter.h and
   include/nvrhi-shader-container.h). */

#include "ContainerWriter.h"

#include <cstring>

static_assert(sizeof(NvrhiScHeader) == 24, "NvrhiScHeader layout");
static_assert(sizeof(NvrhiScSection) == 24, "NvrhiScSection layout");
static_assert(sizeof(NvrhiScReflectionHeader) == 56, "NvrhiScReflectionHeader layout");
static_assert(sizeof(NvrhiScEntryPoint) == 40, "NvrhiScEntryPoint layout");
static_assert(sizeof(NvrhiScParameter) == 48, "NvrhiScParameter layout");
static_assert(sizeof(NvrhiScType) == 48, "NvrhiScType layout");
static_assert(sizeof(NvrhiScField) == 16, "NvrhiScField layout");

namespace {

uint64_t AlignTo8(uint64_t value) {
    return (value + 7u) & ~uint64_t(7u);
}

bool HostIsLittleEndian() {
    const uint16_t probe = 1;
    uint8_t first = 0;
    memcpy(&first, &probe, 1);
    return first == 1;
}

template <typename T>
void AppendBytes(std::string &key, const T &value) {
    key.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

/* Copies a record table into 'out' at 'offset' (the records are plain uint32_t structs). */
template <typename T>
void CopyTable(std::vector<uint8_t> &out, uint64_t offset, const std::vector<T> &table) {
    if (!table.empty())
        memcpy(out.data() + offset, table.data(), table.size() * sizeof(T));
}

} // namespace

ContainerReflection::ContainerReflection() {
    Strings.push_back('\0');
    StringOffsets.emplace(std::string(), 0u);
}

uint32_t ContainerReflection::AddString(const std::string &text) {
    const auto found = StringOffsets.find(text);
    if (found != StringOffsets.end())
        return found->second;
    const uint32_t offset = (uint32_t)Strings.size();
    Strings.append(text.c_str()); /* stops at an embedded NUL, like any reader would */
    Strings.push_back('\0');
    StringOffsets.emplace(text, offset);
    return offset;
}

uint32_t ContainerReflection::AddType(NvrhiScType type,
                                      const std::vector<NvrhiScField> &fields) {
    type.firstField = 0;
    type.fieldCount = (uint32_t)fields.size();

    std::string key;
    AppendBytes(key, type);
    for (const NvrhiScField &field : fields)
        AppendBytes(key, field);
    const auto found = TypeIndices.find(key);
    if (found != TypeIndices.end())
        return found->second;

    type.firstField = fields.empty() ? 0u : (uint32_t)Fields.size();
    Fields.insert(Fields.end(), fields.begin(), fields.end());
    const uint32_t index = (uint32_t)Types.size();
    Types.push_back(type);
    TypeIndices.emplace(std::move(key), index);
    return index;
}

int ContainerReflection::Serialize(std::vector<uint8_t> &section, std::string &error) const {
    NvrhiScReflectionHeader header;
    memset(&header, 0, sizeof(header));

    uint64_t offset = AlignTo8(sizeof(NvrhiScReflectionHeader));
    const uint64_t entry_point_offset = offset;
    offset = AlignTo8(offset + EntryPoints.size() * sizeof(NvrhiScEntryPoint));
    const uint64_t parameter_offset = offset;
    offset = AlignTo8(offset + Parameters.size() * sizeof(NvrhiScParameter));
    const uint64_t type_offset = offset;
    offset = AlignTo8(offset + Types.size() * sizeof(NvrhiScType));
    const uint64_t field_offset = offset;
    offset = AlignTo8(offset + Fields.size() * sizeof(NvrhiScField));
    const uint64_t string_offset = offset;
    const uint64_t total = AlignTo8(offset + Strings.size());
    if (total > UINT32_MAX) {
        error = "the reflection section exceeds 4 GiB";
        return -1;
    }

    header.entryPointCount = (uint32_t)EntryPoints.size();
    header.entryPointOffset = (uint32_t)entry_point_offset;
    header.parameterCount = (uint32_t)Parameters.size();
    header.parameterOffset = (uint32_t)parameter_offset;
    header.typeCount = (uint32_t)Types.size();
    header.typeOffset = (uint32_t)type_offset;
    header.fieldCount = (uint32_t)Fields.size();
    header.fieldOffset = (uint32_t)field_offset;
    header.stringTableSize = (uint32_t)Strings.size();
    header.stringTableOffset = (uint32_t)string_offset;
    header.globalParameterCount = GlobalParameterCount;
    header.globalParamsSize = GlobalParamsSize;
    header.globalParamsAlignment = GlobalParamsAlignment;

    section.assign((size_t)total, 0);
    memcpy(section.data(), &header, sizeof(header));
    CopyTable(section, entry_point_offset, EntryPoints);
    CopyTable(section, parameter_offset, Parameters);
    CopyTable(section, type_offset, Types);
    CopyTable(section, field_offset, Fields);
    memcpy(section.data() + string_offset, Strings.data(), Strings.size());
    return 0;
}

int WriteShaderContainer(const std::vector<ContainerSection> &sections,
                         std::vector<uint8_t> &container, std::string &error) {
    if (!HostIsLittleEndian()) {
        error = "NVRHI shader containers are little-endian; this host is not";
        return -1;
    }
    if (sections.size() > UINT32_MAX) {
        error = "too many container sections";
        return -1;
    }

    std::vector<NvrhiScSection> table(sections.size());
    uint64_t offset = AlignTo8(sizeof(NvrhiScHeader) + sections.size() * sizeof(NvrhiScSection));
    for (size_t i = 0; i < sections.size(); ++i) {
        table[i].kind = sections[i].Kind;
        table[i].target = sections[i].Target;
        table[i].offset = offset;
        table[i].size = sections[i].Data.size();
        offset = AlignTo8(offset + sections[i].Data.size());
    }

    NvrhiScHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = NVRHI_SC_MAGIC;
    header.versionMajor = (uint16_t)NVRHI_SC_VERSION_MAJOR;
    header.versionMinor = (uint16_t)NVRHI_SC_VERSION_MINOR;
    header.headerSize = (uint32_t)sizeof(NvrhiScHeader);
    header.sectionCount = (uint32_t)sections.size();
    header.totalSize = offset;

    container.assign((size_t)offset, 0);
    memcpy(container.data(), &header, sizeof(header));
    if (!table.empty())
        memcpy(container.data() + sizeof(header), table.data(),
               table.size() * sizeof(NvrhiScSection));
    for (size_t i = 0; i < sections.size(); ++i) {
        if (!sections[i].Data.empty())
            memcpy(container.data() + table[i].offset, sections[i].Data.data(),
                   sections[i].Data.size());
    }
    return 0;
}
