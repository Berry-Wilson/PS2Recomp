#include "iop_ioman.h"

#include "iop_cdvd.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"

#include <algorithm>
#include <string_view>

namespace ps2x::iop::detail
{
    namespace
    {
        bool isCdromPath(std::string_view path)
        {
            return path.size() >= 6u && (path.substr(0u, 5u) == "cdrom" || path.substr(0u, 5u) == "CDROM") &&
                   path.find(':') != std::string_view::npos;
        }
    }

    IopIoman::IopIoman(IopMemory &memory, IopCdvd &cdvd) noexcept
        : m_memory(memory), m_cdvd(cdvd)
    {
    }

    void IopIoman::reset()
    {
        m_devices.clear();
    }

    bool IopIoman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        constexpr size_t kMaxDevices = 16u;
        const uint32_t a0 = cpu.gpr[4];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        // File calls only reach the cdrom device: the HLE cdvdman never registers it with
        // AddDrv, so ioman forwards to the virtual disc directly.
        case 4: // open(name, flags)
        {
            const std::string name = m_memory.readString(a0, 1024u);
            if (!isCdromPath(name))
                return false;
            setV0(static_cast<uint32_t>(m_cdvd.openFile(name)));
            return true;
        }
        case 5: // close(fd)
            setV0(static_cast<uint32_t>(m_cdvd.closeFile(static_cast<int32_t>(a0))));
            return true;
        case 6: // read(fd, buffer, size)
            setV0(static_cast<uint32_t>(m_cdvd.readFile(static_cast<int32_t>(a0), cpu.gpr[5], cpu.gpr[6])));
            return true;
        case 8: // lseek(fd, offset, whence)
            setV0(static_cast<uint32_t>(
                m_cdvd.seekFile(static_cast<int32_t>(a0), static_cast<int32_t>(cpu.gpr[5]), cpu.gpr[6])));
            return true;
        case 31: // devctl(name, cmd, arg, arglen, buf, buflen)
            return dispatchCdromDevctl(cpu);
        case 20: // AddDrv
        {
            if (a0 == 0u || m_devices.size() >= kMaxDevices)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t nameAddress = m_memory.read32(a0);
            const uint32_t operations = m_memory.read32(a0 + 16u);
            const std::string name = m_memory.readString(nameAddress, 64u);
            if (nameAddress == 0u || operations == 0u || name.empty())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            m_devices.push_back({a0, cpu.gpr[28], name});
            const uint32_t init = m_memory.read32(operations);
            if (init != 0u)
            {
                const int32_t result = static_cast<int32_t>(
                    executor.executeGuestFunction(init, a0, 0u, 0u, 0u, cpu.gpr[28]));
                if (result < 0)
                {
                    m_devices.pop_back();
                    setV0(0xFFFFFFFFu);
                    return true;
                }
            }

            setV0(0u);
            return true;
        }
        case 21: // DelDrv
        {
            const std::string name = m_memory.readString(a0, 64u);
            const auto device = std::find_if(
                m_devices.begin(), m_devices.end(),
                [&](const Device &candidate)
                { return candidate.name == name; });
            if (device == m_devices.end())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t operations = m_memory.read32(device->address + 16u);
            const uint32_t deinit = operations != 0u ? m_memory.read32(operations + 4u) : 0u;
            if (deinit != 0u)
                (void)executor.executeGuestFunction(deinit, device->address, 0u, 0u, 0u, device->gp);
            m_devices.erase(device);
            setV0(0u);
            return true;
        }
        default:
            return false;
        }
    }

    bool IopIoman::dispatchCdromDevctl(IopCpuState &cpu)
    {
        const std::string name = m_memory.readString(cpu.gpr[4], 64u);
        if (!isCdromPath(name))
            return false;

        const uint32_t command = cpu.gpr[5];
        const uint32_t buffer = m_memory.read32(cpu.gpr[29] + 16u);
        const uint32_t bufferLength = m_memory.read32(cpu.gpr[29] + 20u);
        uint32_t value = 0u;
        switch (command)
        {
        case 0x431Fu: // CDIOC_GETDISKTYP
            value = 0x14u; // PS2 DVD
            break;
        case 0x4320u: // CDIOC_GETERROR
            value = m_cdvd.errorCode();
            break;
        case 0x4322u: // CDIOC_STATUS
            value = 0x0Au; // paused
            break;
        case 0x4325u: // CDIOC_DISKRDY
            value = 2u; // ready
            break;
        default: // MMODE, STREAMINIT, TRYCNT, SPIN*, ...: accept
            break;
        }
        if (buffer != 0u && bufferLength >= 4u)
            m_memory.write32(buffer, value);
        cpu.gpr[2] = 0u;
        return true;
    }
}
