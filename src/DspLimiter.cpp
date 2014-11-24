#include "pch.h"
#include "DspLimiter.h"

#include <string>

namespace SaneAudioRenderer
{
    namespace
    {
        float f(int64_t leftPos, float leftValue, int64_t rightPos, float rightValue, uint64_t pos)
        {
            return leftValue + (float)(pos - leftPos) / (rightPos - leftPos) * (rightValue - leftValue);
        }

        float f(const std::pair<int64_t, float>& left, const std::pair<int64_t, float>& right, uint64_t pos)
        {
            return f(left.first, left.second, right.first, right.second, pos);
        }
    }

    void DspLimiter::Initialize(uint32_t rate, bool exclusive)
    {
        m_limit = (exclusive ? 1.0f : 0.98f);
        m_attackFrames = rate / 100; // 10ms
        m_releaseFrames = rate / 100; // 10ms
        m_windowFrames = m_attackFrames + m_releaseFrames + 1;

        m_buffer.clear();
        m_bufferFrameCount = 0;
        m_bufferFirstFrame = 0;

        m_peaks.clear();
    }

    void DspLimiter::Process(DspChunk& chunk)
    {
        if (!chunk.IsEmpty() && chunk.GetFormat() == DspFormat::Float)
        {
            m_bufferFrameCount += chunk.GetFrameCount();
            m_buffer.push_back(std::move(chunk));

            AnalyzeLastChunk();

            size_t bufferFrontFrames = m_buffer.front().GetFrameCount();
            if (m_bufferFrameCount - bufferFrontFrames >= m_windowFrames)
            {
                ModifyFirstChunk();

                m_bufferFrameCount -= bufferFrontFrames;
                m_bufferFirstFrame += bufferFrontFrames;
                chunk = std::move(m_buffer.front());
                m_buffer.pop_front();
            }
        }
    }

    void DspLimiter::Finish(DspChunk& chunk)
    {
        Process(chunk);
    }

    void DspLimiter::AnalyzeLastChunk()
    {
        assert(m_limit > 0);

        const DspChunk& chunk = m_buffer.back();
        assert(chunk.GetFormat() == DspFormat::Float);

        const uint64_t chunkFirstFrame = m_bufferFirstFrame + m_bufferFrameCount - chunk.GetFrameCount();
        const uint32_t channels = chunk.GetChannelCount();

        auto data = reinterpret_cast<const float*>(chunk.GetConstData());
        for (size_t frame = 0, frameCount = chunk.GetFrameCount(); frame < frameCount; frame++)
        {
            float sample = 0.0f;
            for (size_t i = 0; i < channels; i++)
                sample = fmax(fabs(data[frame * channels + i]), sample);

            if (sample > m_limit)
            {
                const uint64_t peakFrame = chunkFirstFrame + frame;
                if (m_peaks.empty())
                {
                    m_peaks.emplace_back(peakFrame > m_attackFrames ? peakFrame - m_attackFrames : 0, m_limit);
                    m_peaks.emplace_back(peakFrame, sample);
                    m_peaks.emplace_back(peakFrame + m_releaseFrames, m_limit);
                    DbgOutString((std::wstring(L"start ") + std::to_wstring(peakFrame) + L" " + std::to_wstring(sample) + L"\n").c_str());
                }
                else
                {
                    assert(m_peaks.size() > 1);
                    assert(m_peaks.back().second == m_limit);
                    auto back = m_peaks.rbegin();
                    auto nextToBack = back + 1;
                    if (peakFrame > back->first || f(*nextToBack, *back, peakFrame) < sample)
                    {
                        while (nextToBack != m_peaks.rend())
                        {
                            if (sample >= back->second &&
                                nextToBack->first >= peakFrame - m_attackFrames - m_releaseFrames &&
                                f(nextToBack->first, nextToBack->second, peakFrame, sample, back->first) >= back->second)
                            {
                                DbgOutString((std::wstring(L"drop ") + std::to_wstring(back->first) + L" " + std::to_wstring(back->second) + L"\n").c_str());
                                m_peaks.pop_back();
                                back = m_peaks.rbegin();
                                nextToBack = back + 1;
                                continue;
                            }
                            break;
                        }
                        {
                            DbgOutString((std::wstring(L"add ") + std::to_wstring(peakFrame) + L" " + std::to_wstring(sample) + L"\n").c_str());
                            m_peaks.emplace_back(peakFrame, sample);
                            m_peaks.emplace_back(peakFrame + m_releaseFrames, m_limit);
                        }
                    }
                    else
                    {
                        DbgOutString((std::wstring(L"consume ") + std::to_wstring(peakFrame) + L" " + std::to_wstring(sample) + L"\n").c_str());
                    }
                }
            }
        }
    }

    void DspLimiter::ModifyFirstChunk()
    {
        if (!m_peaks.empty())
        {
            DspChunk& chunk = m_buffer.front();
            assert(chunk.GetFormat() == DspFormat::Float);

            const uint64_t chunkFirstFrame = m_bufferFirstFrame;
            const size_t chunkFrameCount = chunk.GetFrameCount();
            const uint32_t channels = chunk.GetChannelCount();

            const size_t firstFrameOffset = chunkFirstFrame > m_peaks.front().first ?
                                                0 : m_peaks.front().first - chunkFirstFrame;

            auto data = reinterpret_cast<float*>(chunk.GetData());
            for (size_t frame = firstFrameOffset; frame < chunkFrameCount; frame++)
            {
                assert(m_peaks.size() >= 2);
                const auto& left = m_peaks.front();
                const auto& right = m_peaks[1];
                assert(right.first > left.first);

                float divisor = f(left, right, chunkFirstFrame + frame);

                for (size_t i = 0; i < channels; i++)
                {
                    float& sample = data[frame * channels + i];
                    sample *= (double)m_limit / divisor;
                    assert(fabs(sample) <= m_limit);
                }

                if (right.first <= frame + chunkFirstFrame)
                {
                    assert(right.first == frame + chunkFirstFrame);
                    m_peaks.pop_front();
                    if (m_peaks.size() == 1)
                    {
                        DbgOutString(L"clear\n");
                        m_peaks.clear();
                        break;
                    }
                }
            }
        }
    }
}
