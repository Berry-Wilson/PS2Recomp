#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopCdvd
    {
    public:
        struct CompletionCallback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            uint32_t reason = 0u;
        };

        IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel);
        ~IopCdvd();

        IopCdvd(const IopCdvd &) = delete;
        IopCdvd &operator=(const IopCdvd &) = delete;

        void reset() noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<CompletionCallback> takeCompletionCallback() noexcept;

        // ioman access to "cdrom0:" files; negative results are -errno.
        [[nodiscard]] int32_t openFile(std::string_view guestPath);
        int32_t closeFile(int32_t fd);
        [[nodiscard]] int32_t readFile(int32_t fd, uint32_t destination, uint32_t size);
        [[nodiscard]] int32_t seekFile(int32_t fd, int32_t offset, uint32_t whence);
        [[nodiscard]] uint32_t errorCode() const noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
