#include "runtime/ps2x_config.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
    std::string trimmed(const std::string &text)
    {
        const char *blank = " \t\r\n";
        const size_t first = text.find_first_not_of(blank);
        if (first == std::string::npos)
            return {};
        return text.substr(first, text.find_last_not_of(blank) - first + 1u);
    }

    void setEnvironment(const std::string &name, const std::string &value)
    {
#if defined(_WIN32)
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }
}

int ps2xLoadConfigFile(const std::filesystem::path &gameDirectory)
{
    std::filesystem::path path;
    if (const char *named = std::getenv("PS2X_CONFIG"); named && named[0])
        path = named;
    else
        path = gameDirectory / "ps2x.ini";

    std::ifstream file(path);
    if (!file)
    {
        if (std::getenv("PS2X_CONFIG"))
            std::cerr << "[config] cannot read " << path.string() << std::endl;
        return 0;
    }

    int applied = 0;
    int lineNumber = 0;
    std::string line;
    while (std::getline(file, line))
    {
        ++lineNumber;
        line = trimmed(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        const size_t equals = line.find('=');
        const std::string name = trimmed(line.substr(0, equals));
        if (equals == std::string::npos || name.rfind("PS2X_", 0) != 0)
        {
            std::cerr << "[config] " << path.string() << ":" << lineNumber << ": expected PS2X_NAME=value" << std::endl;
            continue;
        }
        const std::string value = trimmed(line.substr(equals + 1u));
        if (const char *existing = std::getenv(name.c_str()); existing && existing[0])
            continue; // the environment wins
        if (value.empty())
            continue;
        setEnvironment(name, value);
        ++applied;
    }
    std::cerr << "[config] " << applied << " option(s) from " << path.string() << std::endl;
    return applied;
}
