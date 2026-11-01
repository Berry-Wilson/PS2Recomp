#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    // Scripted input for headless testing.
    // PS2X_AUTOPAD="startSec:durSec:hexMask[;startSec:durSec:hexMask...]"
    // Times are seconds since the first pad read; mask uses the PAD_* bit values above
    // (e.g. 4000 = CROSS, 0008 = START). Example: PS2X_AUTOPAD="8:0.2:4000;12:0.2:0008"
    struct AutoPadEvent
    {
        double start;
        double duration;
        uint16_t mask;
    };

    const std::vector<AutoPadEvent> &autoPadEvents()
    {
        static const std::vector<AutoPadEvent> events = []()
        {
            std::vector<AutoPadEvent> out;
            const char *env = std::getenv("PS2X_AUTOPAD");
            if (!env)
                return out;
            std::string spec(env);
            size_t pos = 0;
            while (pos < spec.size())
            {
                size_t end = spec.find(';', pos);
                if (end == std::string::npos)
                    end = spec.size();
                const std::string item = spec.substr(pos, end - pos);
                double s = 0.0, d = 0.0;
                unsigned int m = 0;
                if (std::sscanf(item.c_str(), "%lf:%lf:%x", &s, &d, &m) == 3)
                {
                    out.push_back({s, d, static_cast<uint16_t>(m)});
                }
                pos = end + 1;
            }
            return out;
        }();
        return events;
    }

    // Input recording and replay, keyed by the number of port-0 pad reads (one per game frame), so a
    // replay stays in step with the game's frames whatever the host speed.
    // PS2X_INPUT_RECORD=<file>: appends "<read> <buttons hex> <lx> <ly> <rx> <ry> <seconds>" each time
    //   the pad state changes.
    // PS2X_INPUT_REPLAY=<file>: plays such a file back instead of live input, then hands control back.
    struct PadSample
    {
        uint64_t read;
        uint16_t buttons;
        uint8_t sticks[4]; // lx, ly, rx, ry
    };

    struct InputLog
    {
        std::FILE *record = nullptr;
        std::vector<PadSample> replay;
        size_t replayPos = 0;
        uint64_t reads = 0;
        bool haveLast = false;
        PadSample last{};

        InputLog()
        {
            if (const char *path = std::getenv("PS2X_INPUT_REPLAY"))
            {
                if (std::FILE *f = std::fopen(path, "r"))
                {
                    char line[256];
                    while (std::fgets(line, sizeof(line), f))
                    {
                        unsigned long long read = 0;
                        unsigned int buttons = 0, lx = 0, ly = 0, rx = 0, ry = 0;
                        if (std::sscanf(line, "%llu %x %u %u %u %u", &read, &buttons, &lx, &ly, &rx, &ry) == 6)
                            replay.push_back({read, static_cast<uint16_t>(buttons),
                                              {static_cast<uint8_t>(lx), static_cast<uint8_t>(ly),
                                               static_cast<uint8_t>(rx), static_cast<uint8_t>(ry)}});
                    }
                    std::fclose(f);
                    std::fprintf(stderr, "[input] replaying %zu pad changes from %s\n", replay.size(), path);
                }
                else
                    std::fprintf(stderr, "[input] cannot open replay file %s\n", path);
            }
            if (const char *path = std::getenv("PS2X_INPUT_RECORD"))
            {
                record = std::fopen(path, "w");
                if (record)
                {
                    std::fprintf(record, "# ps2x pad input v1: read buttons(hex, active low) lx ly rx ry seconds\n");
                    std::fflush(record);
                    std::fprintf(stderr, "[input] recording pad input to %s\n", path);
                }
                else
                    std::fprintf(stderr, "[input] cannot create record file %s\n", path);
            }
        }

        // Replays (when a recording covers this read), then records the state the game will see.
        void apply(uint8_t *data)
        {
            if (replayPos < replay.size())
            {
                // The latest recorded state at or before this read; live input once the last change is past.
                while (replayPos + 1 < replay.size() && replay[replayPos + 1].read <= reads)
                    ++replayPos;
                if (replayPos + 1 == replay.size() && reads > replay[replayPos].read)
                {
                    std::fprintf(stderr, "[input] replay finished at pad read %llu; live input from here\n",
                                 static_cast<unsigned long long>(reads));
                    replayPos = replay.size();
                }
                else if (replay[replayPos].read <= reads)
                {
                    const PadSample &s = replay[replayPos];
                    data[2] = static_cast<uint8_t>(s.buttons & 0xFF);
                    data[3] = static_cast<uint8_t>(s.buttons >> 8);
                    data[6] = s.sticks[0];
                    data[7] = s.sticks[1];
                    data[4] = s.sticks[2];
                    data[5] = s.sticks[3];
                }
            }
            if (record)
            {
                const PadSample s{reads, static_cast<uint16_t>(data[2] | (data[3] << 8)), {data[6], data[7], data[4], data[5]}};
                if (!haveLast || s.buttons != last.buttons || std::memcmp(s.sticks, last.sticks, 4) != 0)
                {
                    std::fprintf(record, "%llu %04x %u %u %u %u %.3f\n", static_cast<unsigned long long>(s.read), s.buttons,
                                 s.sticks[0], s.sticks[1], s.sticks[2], s.sticks[3], GetTime());
                    std::fflush(record);
                    last = s;
                    haveLast = true;
                }
            }
            ++reads;
        }
    };

    InputLog &inputLog()
    {
        static InputLog log;
        return log;
    }

    uint16_t autoPadPressedMask()
    {
        const auto &events = autoPadEvents();
        if (events.empty())
            return 0u;
        static const double s_t0 = GetTime();
        const double t = GetTime() - s_t0;
        uint16_t mask = 0u;
        for (const auto &e : events)
        {
            if (t >= e.start && t < e.start + e.duration)
                mask |= e.mask;
        }
        return mask;
    }
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // Each stick axis takes whichever source is pushed furthest from centre, so the keyboard and
    // every connected gamepad can be used together (an idle or wrongly mapped pad in slot 0 must
    // not shut out the others).
    float axes[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // lx, ly, rx, ry in [-1, 1]
    auto mergeAxis = [&axes](int index, float value)
    {
        if (std::fabs(value) > std::fabs(axes[index]))
            axes[index] = value;
    };

    constexpr int kMaxGamepads = 4;
    static bool s_padConnected[kMaxGamepads] = {};
    for (int pad = 0; pad < kMaxGamepads; ++pad)
    {
        const bool available = IsGamepadAvailable(pad);
        if (available != s_padConnected[pad])
        {
            s_padConnected[pad] = available;
            if (available)
                std::fprintf(stderr, "[pad] gamepad %d connected: %s\n", pad, GetGamepadName(pad));
            else
                std::fprintf(stderr, "[pad] gamepad %d disconnected\n", pad);
        }
        if (!available)
            continue;
        static constexpr struct
        {
            int button;
            uint16_t mask;
        } kButtonMap[] = {
            {GAMEPAD_BUTTON_LEFT_FACE_UP, PAD_UP},
            {GAMEPAD_BUTTON_LEFT_FACE_DOWN, PAD_DOWN},
            {GAMEPAD_BUTTON_LEFT_FACE_LEFT, PAD_LEFT},
            {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, PAD_RIGHT},
            {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, PAD_CROSS},
            {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, PAD_CIRCLE},
            {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, PAD_SQUARE},
            {GAMEPAD_BUTTON_RIGHT_FACE_UP, PAD_TRIANGLE},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_1, PAD_L1},
            {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, PAD_R1},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_2, PAD_L2},
            {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, PAD_R2},
            {GAMEPAD_BUTTON_MIDDLE_RIGHT, PAD_START},
            {GAMEPAD_BUTTON_MIDDLE_LEFT, PAD_SELECT},
            {GAMEPAD_BUTTON_LEFT_THUMB, PAD_L3},
            {GAMEPAD_BUTTON_RIGHT_THUMB, PAD_R3},
        };
        for (const auto &entry : kButtonMap)
        {
            if (IsGamepadButtonDown(pad, entry.button))
                clearBit(entry.mask);
        }
        mergeAxis(0, GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X));
        mergeAxis(1, GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y));
        mergeAxis(2, GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X));
        mergeAxis(3, GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y));
    }

    // Keyboard. D-pad: arrow keys. Left stick: WASD. Right stick: IJKL.
    if (IsKeyDown(KEY_UP))
        clearBit(PAD_UP);
    if (IsKeyDown(KEY_DOWN))
        clearBit(PAD_DOWN);
    if (IsKeyDown(KEY_LEFT))
        clearBit(PAD_LEFT);
    if (IsKeyDown(KEY_RIGHT))
        clearBit(PAD_RIGHT);

    auto keyAxis = [](int negativeKey, int positiveKey) -> float
    {
        float value = 0.0f;
        if (IsKeyDown(negativeKey))
            value -= 1.0f;
        if (IsKeyDown(positiveKey))
            value += 1.0f;
        return value;
    };
    mergeAxis(0, keyAxis(KEY_A, KEY_D));
    mergeAxis(1, keyAxis(KEY_W, KEY_S));
    mergeAxis(2, keyAxis(KEY_J, KEY_L));
    mergeAxis(3, keyAxis(KEY_I, KEY_K));

    if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
        clearBit(PAD_CROSS);
    if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
        clearBit(PAD_CIRCLE);
    if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
        clearBit(PAD_SQUARE);
    if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
        clearBit(PAD_TRIANGLE);
    if (IsKeyDown(KEY_Q))
        clearBit(PAD_L1);
    if (IsKeyDown(KEY_E))
        clearBit(PAD_R1);
    if (IsKeyDown(KEY_LEFT_SHIFT))
        clearBit(PAD_L2);
    if (IsKeyDown(KEY_RIGHT_SHIFT))
        clearBit(PAD_R2);
    if (IsKeyDown(KEY_ENTER))
        clearBit(PAD_START);
    if (IsKeyDown(KEY_TAB))
        clearBit(PAD_SELECT);

    auto stickByte = [](float value) -> uint8_t
    {
        value = std::fmax(-1.0f, std::fmin(1.0f, value));
        return static_cast<uint8_t>(kPadStickCenter + static_cast<int>(value * 127.0f));
    };
    data[6] = stickByte(axes[0]);
    data[7] = stickByte(axes[1]);
    data[4] = stickByte(axes[2]);
    data[5] = stickByte(axes[3]);

    btns &= static_cast<uint16_t>(~autoPadPressedMask());

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    if (port == 0)
        inputLog().apply(data);
    return true;
}
