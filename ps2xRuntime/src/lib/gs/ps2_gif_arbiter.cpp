#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstring>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
    return flg == 2u || flg == 3u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    if (m_count == m_queue.size())
        m_queue.emplace_back();
    GifArbiterPacket &pkt = m_queue[m_count++];
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.assign(data, data + sizeBytes);
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    const auto before = [](const GifArbiterPacket &a, const GifArbiterPacket &b)
    {
        // DIRECTHL cannot preempt PATH3 IMAGE transfers.
        if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
        {
            if (a.path3Image && b.path2DirectHl)
                return true;
            if (a.path2DirectHl && b.path3Image)
                return false;
        }
        return pathPriority(a.pathId) < pathPriority(b.pathId);
    };
    // The queue is usually already in order (one path); a stable sort would then change nothing.
    const auto end = m_queue.begin() + static_cast<std::ptrdiff_t>(m_count);
    if (!std::is_sorted(m_queue.begin(), end, before))
        std::stable_sort(m_queue.begin(), end, before);

    for (size_t i = 0; i < m_count; ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()), static_cast<uint32_t>(pkt.pathId));
        }
    }
    m_count = 0u;
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
