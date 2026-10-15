#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopCdvd;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        IopIoman(IopMemory &memory, IopCdvd &cdvd) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;
        };

        [[nodiscard]] bool dispatchCdromDevctl(IopCpuState &cpu);

        IopMemory &m_memory;
        IopCdvd &m_cdvd;
        std::vector<Device> m_devices;
    };
}
