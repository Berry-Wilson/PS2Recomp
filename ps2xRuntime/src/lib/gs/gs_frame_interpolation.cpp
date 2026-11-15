#include "gs_frame_interpolation.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace ps2x_interp
{
    namespace
    {
        constexpr uint32_t kPacked = 0u;
        constexpr uint32_t kRegList = 1u;
        constexpr uint32_t kDescAd = 0xEu;
        constexpr uint64_t kRegTex0First = 0x06u;
        constexpr uint64_t kRegTex0Last = 0x07u;
        constexpr uint64_t kRegFrameFirst = 0x4Cu;
        constexpr uint64_t kRegFrameLast = 0x4Du;
        constexpr uint64_t kRegFinish = 0x61u;
        // Further than this in one frame is a cut, a teleport or a wrong pairing: 128 pixels.
        constexpr int kMaxMove = 128 * 16;

        constexpr bool isXyzf(uint32_t desc) { return desc == 0x4u || desc == 0xCu; }
        constexpr bool isXyz(uint32_t desc) { return desc == 0x5u || desc == 0xDu; }

        template <typename T>
        T read(const uint8_t *data, size_t offset)
        {
            T value;
            std::memcpy(&value, data + offset, sizeof(value));
            return value;
        }

        template <typename T>
        void write(uint8_t *data, size_t offset, T value)
        {
            std::memcpy(data + offset, &value, sizeof(value));
        }

        struct Tag
        {
            size_t offset; // of the tag
            uint64_t lo;
            uint64_t hi;
            uint32_t nloop;
            uint32_t format;
            uint32_t nreg;
            size_t dataBytes;
        };

        // Calls each(tag) for every whole GIF tag in a packet.
        template <typename Each>
        void walkTags(const uint8_t *data, size_t begin, size_t size, Each &&each)
        {
            size_t offset = begin;
            const size_t end = begin + size;
            while (offset + 16u <= end)
            {
                Tag tag{};
                tag.offset = offset;
                tag.lo = read<uint64_t>(data, offset);
                tag.hi = read<uint64_t>(data, offset + 8u);
                tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
                tag.format = static_cast<uint32_t>((tag.lo >> 58u) & 3u);
                tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
                if (tag.nreg == 0u)
                    tag.nreg = 16u;
                const size_t words = static_cast<size_t>(tag.nloop) * tag.nreg;
                if (tag.format == kPacked)
                    tag.dataBytes = words * 16u;
                else if (tag.format == kRegList)
                    tag.dataBytes = ((words + 1u) / 2u) * 16u;
                else
                    tag.dataBytes = static_cast<size_t>(tag.nloop) * 16u;
                if (offset + 16u + tag.dataBytes > end)
                    return; // runs past the packet: not something to take apart
                each(tag);
                offset += 16u + tag.dataBytes;
            }
        }

        uint32_t descOf(const Tag &tag, size_t word)
        {
            return static_cast<uint32_t>((tag.hi >> ((word % tag.nreg) * 4u)) & 0xFu);
        }

        bool hasDesc(const Tag &tag, uint32_t wanted)
        {
            for (uint32_t i = 0; i < tag.nreg; ++i)
            {
                if (descOf(tag, i) == wanted)
                    return true;
            }
            return false;
        }

        struct Batch
        {
            Tag tag;
            uint64_t key;
        };

        uint64_t mix(uint64_t hash, uint64_t value)
        {
            hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6u) + (hash >> 2u);
            return hash;
        }

        // The batches of a frame that have vertices, each with the key it is paired by.
        std::vector<Batch> collectBatches(const Frame &frame, Stats *stats)
        {
            std::vector<Batch> batches;
            uint64_t tex0[4] = {};
            for (const Packet &packet : frame.packets)
            {
                const uint32_t path = packet.path & 3u;
                walkTags(frame.data.data(), packet.offset, packet.size, [&](const Tag &tag)
                {
                    if (tag.format != kPacked && tag.format != kRegList)
                        return;
                    bool vertices = false;
                    for (uint32_t i = 0; i < tag.nreg; ++i)
                    {
                        const uint32_t desc = descOf(tag, i);
                        vertices = vertices || isXyz(desc) || isXyzf(desc);
                    }
                    if (tag.format == kPacked && hasDesc(tag, kDescAd))
                    {
                        const size_t words = static_cast<size_t>(tag.nloop) * tag.nreg;
                        for (size_t k = 0; k < words; ++k)
                        {
                            if (descOf(tag, k) != kDescAd)
                                continue;
                            const size_t at = tag.offset + 16u + k * 16u;
                            const uint64_t reg = read<uint64_t>(frame.data.data(), at + 8u) & 0xFFu;
                            const uint64_t value = read<uint64_t>(frame.data.data(), at);
                            if (reg >= kRegTex0First && reg <= kRegTex0Last)
                                tex0[path] = value;
                            if (stats && reg >= kRegFrameFirst && reg <= kRegFrameLast)
                            {
                                const uint32_t fbp = static_cast<uint32_t>(value & 0x1FFu);
                                if (stats->frameFirst == 0xFFFFFFFFu)
                                    stats->frameFirst = fbp;
                                stats->frameLast = fbp;
                            }
                        }
                    }
                    if (!vertices || tag.nloop == 0u)
                        return;
                    uint64_t key = mix(path, tag.lo);
                    key = mix(key, tag.hi);
                    key = mix(key, tex0[path]);
                    batches.push_back({tag, key});
                });
            }
            return batches;
        }

        struct Vertex
        {
            int x;
            int y;
            uint32_t z;
            bool skip; // ADC: moves the pen without drawing
        };

        // Where the vertex at a word of a batch is, and how its depth is stored.
        struct Slot
        {
            size_t at;
            bool packed;
            bool fog; // XYZF: 24 bits of depth
        };

        Vertex readVertex(const uint8_t *data, const Slot &slot)
        {
            Vertex v{};
            if (slot.packed)
            {
                v.x = read<uint16_t>(data, slot.at);
                v.y = read<uint16_t>(data, slot.at + 4u);
                const uint32_t zWord = read<uint32_t>(data, slot.at + 8u);
                v.z = slot.fog ? (zWord >> 4u) & 0xFFFFFFu : zWord;
                v.skip = ((read<uint32_t>(data, slot.at + 12u) >> 15u) & 1u) != 0u;
            }
            else
            {
                v.x = read<uint16_t>(data, slot.at);
                v.y = read<uint16_t>(data, slot.at + 2u);
                const uint32_t zWord = read<uint32_t>(data, slot.at + 4u);
                v.z = slot.fog ? zWord & 0xFFFFFFu : zWord;
                v.skip = false;
            }
            return v;
        }

        void writeVertex(uint8_t *data, const Slot &slot, const Vertex &v)
        {
            write<uint16_t>(data, slot.at, static_cast<uint16_t>(v.x));
            write<uint16_t>(data, slot.at + (slot.packed ? 4u : 2u), static_cast<uint16_t>(v.y));
            const size_t zAt = slot.at + (slot.packed ? 8u : 4u);
            const uint32_t zWord = read<uint32_t>(data, zAt);
            if (!slot.fog)
                write<uint32_t>(data, zAt, v.z);
            else if (slot.packed)
                write<uint32_t>(data, zAt, (zWord & ~0x0FFFFFF0u) | ((v.z & 0xFFFFFFu) << 4u));
            else
                write<uint32_t>(data, zAt, (zWord & 0xFF000000u) | (v.z & 0xFFFFFFu));
        }

        // Calls each(slot index, slot) for the vertices of a batch whose tag is at tagOffset.
        template <typename Each>
        void walkVertices(const Tag &tag, size_t tagOffset, Each &&each)
        {
            const size_t words = static_cast<size_t>(tag.nloop) * tag.nreg;
            const bool packed = tag.format == kPacked;
            for (size_t k = 0; k < words; ++k)
            {
                const uint32_t desc = descOf(tag, k);
                if (isXyz(desc) || isXyzf(desc))
                    each(Slot{tagOffset + 16u + k * (packed ? 16u : 8u), packed, isXyzf(desc)});
            }
        }
    }

    void Frame::add(uint32_t path, const void *bytes, uint32_t size)
    {
        const uint32_t offset = static_cast<uint32_t>(data.size());
        data.resize(data.size() + size);
        std::memcpy(data.data() + offset, bytes, size);
        packets.push_back({path, offset, size});
    }

    bool writesFinish(const void *bytes, uint32_t size)
    {
        const uint8_t *data = static_cast<const uint8_t *>(bytes);
        bool found = false;
        walkTags(data, 0u, size, [&](const Tag &tag)
        {
            if (found || tag.format != kPacked || !hasDesc(tag, kDescAd))
                return;
            const size_t words = static_cast<size_t>(tag.nloop) * tag.nreg;
            for (size_t k = 0; k < words && !found; ++k)
            {
                if (descOf(tag, k) == kDescAd)
                    found = (read<uint64_t>(data, tag.offset + 16u + k * 16u + 8u) & 0xFFu) == kRegFinish;
            }
        });
        return found;
    }

    void buildMiddle(const Frame &older, const Frame &newer, Frame &out, Stats &stats)
    {
        out = newer;
        const std::vector<Batch> olderBatches = collectBatches(older, nullptr);
        const std::vector<Batch> newerBatches = collectBatches(newer, &stats);

        // The older frame's batches by key, in the order they were drawn.
        std::unordered_map<uint64_t, std::vector<uint32_t>> byKey;
        byKey.reserve(olderBatches.size());
        for (uint32_t i = 0; i < olderBatches.size(); ++i)
            byKey[olderBatches[i].key].push_back(i);
        std::unordered_map<uint64_t, uint32_t> used;

        const uint8_t *olderData = older.data.data();
        const uint8_t *newerData = newer.data.data();
        std::vector<Vertex> middle;
        stats.batches = static_cast<uint32_t>(newerBatches.size());
        for (const Batch &batch : newerBatches)
        {
            const auto found = byKey.find(batch.key);
            if (found == byKey.end())
                continue;
            uint32_t &next = used[batch.key];
            if (next >= found->second.size())
                continue;
            const Batch &partner = olderBatches[found->second[next++]];
            if (partner.tag.lo != batch.tag.lo || partner.tag.hi != batch.tag.hi)
                continue;
            ++stats.paired;

            middle.clear();
            bool usable = true;
            bool changed = false;
            const size_t shift = partner.tag.offset - batch.tag.offset; // wraps; only added back
            walkVertices(batch.tag, batch.tag.offset, [&](const Slot &slot)
            {
                if (!usable)
                    return;
                Slot olderSlot = slot;
                olderSlot.at += shift;
                const Vertex a = readVertex(olderData, olderSlot);
                const Vertex b = readVertex(newerData, slot);
                if (a.skip != b.skip || std::abs(a.x - b.x) > kMaxMove || std::abs(a.y - b.y) > kMaxMove)
                {
                    usable = false;
                    return;
                }
                changed = changed || a.x != b.x || a.y != b.y || a.z != b.z;
                Vertex m = b;
                m.x = (a.x + b.x + 1) / 2;
                m.y = (a.y + b.y + 1) / 2;
                m.z = static_cast<uint32_t>((static_cast<uint64_t>(a.z) + b.z + 1u) / 2u);
                middle.push_back(m);
            });
            if (!usable)
            {
                ++stats.rejected;
                continue;
            }
            if (!changed)
                continue;
            ++stats.moved;
            size_t index = 0;
            walkVertices(batch.tag, batch.tag.offset, [&](const Slot &slot) { writeVertex(out.data.data(), slot, middle[index++]); });
        }
    }
}
