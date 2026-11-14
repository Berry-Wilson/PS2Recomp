#include "runtime/ps2_guest_clock.h"

#include <cstdio>
#include <cstdlib>
#include <string>

std::time_t ps2xGuestClockStart()
{
    static const std::time_t start = []()
    {
        // A replay continues from its recording's clock.
        if (const char *path = std::getenv("PS2X_INPUT_REPLAY"); path && path[0])
        {
            if (std::FILE *file = std::fopen(path, "r"))
            {
                char line[256];
                long long recorded = 0;
                bool found = false;
                while (!found && std::fgets(line, sizeof(line), file) && line[0] == '#')
                    found = std::sscanf(line, "# clock %lld", &recorded) == 1;
                std::fclose(file);
                if (found)
                    return static_cast<std::time_t>(recorded);
            }
            // An older recording has no clock line. Games seed their random numbers from the clock, so
            // a replay still needs the same start every time: the time in the file's name
            // (input_YYYYMMDD_HHMMSS, when it was made) if it has one, else a fixed date.
            const std::string name = path;
            const size_t stamp = name.rfind("input_");
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            if (stamp != std::string::npos &&
                std::sscanf(name.c_str() + stamp, "input_%4d%2d%2d_%2d%2d%2d", &year, &month, &day, &hour, &minute, &second) == 6)
            {
                std::tm local{};
                local.tm_year = year - 1900;
                local.tm_mon = month - 1;
                local.tm_mday = day;
                local.tm_hour = hour;
                local.tm_min = minute;
                local.tm_sec = second;
                local.tm_isdst = -1;
                if (const std::time_t made = std::mktime(&local); made != static_cast<std::time_t>(-1))
                    return made;
            }
            return static_cast<std::time_t>(1000000000); // 9 September 2001
        }
        return std::time(nullptr);
    }();
    return start;
}

std::time_t ps2xGuestClockNow(uint64_t vsyncTick)
{
    return ps2xGuestClockStart() + static_cast<std::time_t>(vsyncTick / 60u);
}
