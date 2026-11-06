#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

struct PS2AudioBackend::Impl
{
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;

    // The sound chip's output on its way to the audio device: one thread pushes as the guest runs, the
    // device's thread pulls. Guest time and the device's clock drift apart, so playback speed follows
    // the buffer's fill level by up to 2%, keeping it near kTargetFrames without an audible glitch.
    static constexpr uint64_t kRingFrames = 1u << 15u;
    static constexpr uint64_t kTargetFrames = 3072u;
    static constexpr int kDeviceBufferFrames = 1024;

    std::vector<int16_t> ring = std::vector<int16_t>(kRingFrames * 2u);
    std::atomic<uint64_t> writePos{0};
    std::atomic<uint64_t> readPos{0};
    double fraction = 0.0;
    bool playing = false;
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> droppedFrames{0};
    uint64_t pushedFrames = 0;
    std::FILE *dump = nullptr;
#if !defined(PLATFORM_VITA)
    AudioStream stream{};
#endif
    std::atomic<bool> streamOpen{false};

    static std::atomic<Impl *> s_streamOwner;
    void pull(int16_t *out, unsigned int frames);
    static void streamCallback(void *buffer, unsigned int frames);
};

std::atomic<PS2AudioBackend::Impl *> PS2AudioBackend::Impl::s_streamOwner{nullptr};

void PS2AudioBackend::Impl::pull(int16_t *out, unsigned int frames)
{
    uint64_t read = readPos.load(std::memory_order_relaxed);
    uint64_t available = writePos.load(std::memory_order_acquire) - read;
    unsigned int produced = 0;

    if (!playing && available >= kTargetFrames)
    {
        playing = true;
        fraction = 0.0;
    }
    if (playing)
    {
        const double error = (static_cast<double>(available) - kTargetFrames) / kTargetFrames;
        const double rate = 1.0 + std::clamp(error * 0.02, -0.02, 0.02);
        for (; produced < frames; ++produced)
        {
            if (available < 2u)
            {
                playing = false;
                underruns.fetch_add(1u, std::memory_order_relaxed);
                break;
            }
            const size_t a = static_cast<size_t>(read % kRingFrames) * 2u;
            const size_t b = static_cast<size_t>((read + 1u) % kRingFrames) * 2u;
            const float f = static_cast<float>(fraction);
            out[produced * 2u] = static_cast<int16_t>(ring[a] + (ring[b] - ring[a]) * f);
            out[produced * 2u + 1u] = static_cast<int16_t>(ring[a + 1u] + (ring[b + 1u] - ring[a + 1u]) * f);
            fraction += rate;
            const uint64_t whole = static_cast<uint64_t>(fraction);
            fraction -= static_cast<double>(whole);
            read += whole;
            available -= whole;
        }
        readPos.store(read, std::memory_order_release);
    }
    if (produced < frames)
        std::memset(out + produced * 2u, 0, (frames - produced) * 2u * sizeof(int16_t));
}

void PS2AudioBackend::Impl::streamCallback(void *buffer, unsigned int frames)
{
    if (Impl *owner = s_streamOwner.load(std::memory_order_acquire))
        owner->pull(static_cast<int16_t *>(buffer), frames);
    else
        std::memset(buffer, 0, frames * 2u * sizeof(int16_t));
}

void PS2AudioBackend::setAudioReady(bool ready)
{
    m_audioReady = ready;
    Impl &impl = *m_impl;
#if !defined(PLATFORM_VITA)
    if (ready && !impl.streamOpen.load())
    {
        SetAudioStreamBufferSizeDefault(Impl::kDeviceBufferFrames);
        impl.stream = LoadAudioStream(48000, 16, 2);
        Impl::s_streamOwner.store(&impl, std::memory_order_release);
        SetAudioStreamCallback(impl.stream, Impl::streamCallback);
        PlayAudioStream(impl.stream);
        impl.streamOpen.store(true, std::memory_order_release);
    }
    else if (!ready && impl.streamOpen.load())
    {
        impl.streamOpen.store(false, std::memory_order_release);
        Impl::s_streamOwner.store(nullptr, std::memory_order_release);
        StopAudioStream(impl.stream);
        UnloadAudioStream(impl.stream);
    }
#endif
    if (!ready && impl.pushedFrames != 0u)
    {
        std::fprintf(stderr, "[audio] %.1f s of sound chip output, %llu underruns, %llu frames dropped\n",
                     static_cast<double>(impl.pushedFrames) / 48000.0,
                     static_cast<unsigned long long>(impl.underruns.load()),
                     static_cast<unsigned long long>(impl.droppedFrames.load()));
    }
    if (!ready && impl.dump)
    {
        std::fclose(impl.dump);
        impl.dump = nullptr;
    }
}

void PS2AudioBackend::pushOutput(const int16_t *stereo, size_t frames)
{
    Impl &impl = *m_impl;
    impl.pushedFrames += frames;

    // PS2X_AUDIO_DUMP=<file>: raw 48 kHz 16-bit stereo, as the sound chip made it.
    static const char *const dumpPath = std::getenv("PS2X_AUDIO_DUMP");
    if (dumpPath)
    {
        if (!impl.dump)
            impl.dump = std::fopen(dumpPath, "wb");
        if (impl.dump)
            std::fwrite(stereo, sizeof(int16_t) * 2u, frames, impl.dump);
    }

    if (!impl.streamOpen.load(std::memory_order_acquire))
        return;
    const uint64_t write = impl.writePos.load(std::memory_order_relaxed);
    const uint64_t used = write - impl.readPos.load(std::memory_order_acquire);
    const size_t space = static_cast<size_t>(Impl::kRingFrames - std::min(used, Impl::kRingFrames));
    const size_t count = std::min(frames, space);
    if (count < frames)
        impl.droppedFrames.fetch_add(frames - count, std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i)
    {
        const size_t slot = static_cast<size_t>((write + i) % Impl::kRingFrames) * 2u;
        impl.ring[slot] = stereo[i * 2u];
        impl.ring[slot + 1u] = stereo[i * 2u + 1u];
    }
    impl.writePos.store(write + count, std::memory_order_release);
}

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
        stopAll();
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}
