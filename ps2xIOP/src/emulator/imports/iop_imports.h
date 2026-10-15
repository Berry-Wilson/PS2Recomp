#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    struct IopImportCall
    {
        std::string library;
        uint16_t ordinal = 0;
        uint16_t version = 0;
    };

    class IopImportRegistry
    {
    public:
        explicit IopImportRegistry(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] std::optional<IopImportCall> decode(uint32_t pc) const;
        [[nodiscard]] bool registerExportTable(uint32_t address);
        [[nodiscard]] bool releaseExportTable(uint32_t address);
        [[nodiscard]] uint32_t findTable(std::string_view library, std::optional<uint16_t> version = std::nullopt) const;
        [[nodiscard]] uint32_t resolve(std::string_view library, uint16_t ordinal, std::optional<uint16_t> version = std::nullopt) const;
        [[nodiscard]] int32_t setRebootTimeLibraryHandlingMode(uint32_t address, uint32_t mode);
        void eraseRange(uint32_t base, uint32_t size);

    private:
        struct ExportLibrary
        {
            uint32_t tableAddress = 0;
            uint16_t version = 0;
            std::string name;
            std::vector<uint32_t> functions;
        };

        [[nodiscard]] const ExportLibrary *findLibrary(std::string_view name, std::optional<uint16_t> version) const;

        struct CachedStub
        {
            uint32_t table = 0;
            uint32_t delay = 0;
            uint32_t name[2]{};
            IopImportCall call;
        };

        std::optional<IopImportCall> decodeUncached(uint32_t pc, uint32_t delay, uint32_t &table) const;

        IopMemory &m_memory;
        std::map<uint32_t, ExportLibrary> m_libraries;
        // decode() runs before every instruction; the backward table scan is cached per stub
        // address and revalidated against the stub words and the table name on each hit.
        mutable std::unordered_map<uint32_t, CachedStub> m_stubCache;
    };
}
