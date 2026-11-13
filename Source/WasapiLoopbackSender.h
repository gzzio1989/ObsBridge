#pragma once
// =============================================================================
// WasapiLoopbackSender.h
// DAW (ASIO / CoreAudio) のマスタートラック出力を、本線のオーディオスレッドを一切ブロックせず
// 仮想オーディオデバイス（Yamaha SYNCROOM Driver / VB-CABLE / BlackHole 等）へ
// 低遅延・ゼロクリックで流し込むクロスプラットフォーム音声送信エンジン（Windows & macOS対応）。
//
// 特徴:
//  - pushSamples() は完全ロックフリー・ゼロアロケーション（128サンプルでも <1us）
//  - Windows: Win32 Pro Audio スレッド + WASAPI 共有モード イベント駆動送信
//  - macOS  : CoreAudio HALOutput AudioUnit レンダーコールバック送信
//  - DAW側(例: 48000Hz) と ドライバ側(例: 44100Hz / 48000Hz) のサンプルレート差および
//    2デバイス間の水晶クロックドリフトを 4点3次Hermite補間 + FIFO比例サーボで滑らかに吸収
//  - 仮想デバイスが見つからない時は通常スピーカーを誤って鳴らさない安全設計
// =============================================================================

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#elif defined(__APPLE__)
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sobr
{

struct RenderEndpointInfo
{
    std::wstring id;
    std::wstring name;
    int virtualPriority = 0; // 0 = 通常出力デバイス, >0 = 仮想ループバック（小さいほど高優先度）
};

class WasapiLoopbackSender
{
public:
    static constexpr uint32_t kRingFrames = 65536u; // 2のべき乗（約1.36秒 @ 48kHz）
    static constexpr uint32_t kRingMask   = kRingFrames - 1u;

    WasapiLoopbackSender()
        : ringL_ (kRingFrames, 0.0f),
          ringR_ (kRingFrames, 0.0f)
    {
#ifdef _WIN32
        wakeEvent_ = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        audioEvent_ = CreateEventW (nullptr, FALSE, FALSE, nullptr);
#endif
    }

    ~WasapiLoopbackSender()
    {
        stop();
#ifdef _WIN32
        if (wakeEvent_ != nullptr)  { CloseHandle (wakeEvent_);  wakeEvent_  = nullptr; }
        if (audioEvent_ != nullptr) { CloseHandle (audioEvent_); audioEvent_ = nullptr; }
#endif
    }

    // 仮想オーディオデバイスかどうかを判定し、自動選択時の優先順位（1が最優先、0は非仮想デバイス）を返す
    static int classifyVirtualDevicePriority (const std::wstring& name)
    {
        std::wstring low = name;
        std::transform (low.begin(), low.end(), low.begin(),
                        [] (wchar_t c) { return static_cast<wchar_t> (std::towlower (c)); });

        // VRヘッドセットやGPU/HDMI音声、ヘッドホンモニターは実スピーカーなので除外
        if (low.find (L"oculus") != std::wstring::npos
            || low.find (L"quest") != std::wstring::npos
            || low.find (L"steam streaming") != std::wstring::npos
            || low.find (L"nvidia") != std::wstring::npos
            || low.find (L"amd ") != std::wstring::npos
            || low.find (L"wave link monitor") != std::wstring::npos)
            return 0;

        if (low.find (L"yamaha syncroom") != std::wstring::npos) return 1;
        if (low.find (L"syncroom") != std::wstring::npos || low.find (L"netduetto") != std::wstring::npos) return 2;
        if (low.find (L"blackhole") != std::wstring::npos) return 3;
        if (low.find (L"cable input") != std::wstring::npos || low.find (L"vb-audio") != std::wstring::npos || low.find (L"vb-cable") != std::wstring::npos) return 4;
        if (low.find (L"voicemeeter") != std::wstring::npos || low.find (L"vaio") != std::wstring::npos) return 5;
        if (low.find (L"vocalgzzio") != std::wstring::npos || low.find (L"gzzioroom") != std::wstring::npos) return 6;
        if (low.find (L"soundflower") != std::wstring::npos || low.find (L"loopback audio") != std::wstring::npos
            || low.find (L"wave link stream") != std::wstring::npos || low.find (L"broadcast stream mix") != std::wstring::npos) return 7;
        return 0;
    }

#ifdef __APPLE__
    static std::wstring cfStringToWString (CFStringRef cfStr)
    {
        if (cfStr == nullptr)
            return {};
        CFIndex len = CFStringGetLength (cfStr);
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding (len, kCFStringEncodingUTF32LE) + 4;
        std::vector<char> buf (static_cast<size_t> (maxSize), 0);
        CFIndex used = 0;
        CFStringGetBytes (cfStr, CFRangeMake (0, len), kCFStringEncodingUTF32LE, '?', false,
                          reinterpret_cast<UInt8*> (buf.data()), maxSize, &used);
        const size_t wcharCount = static_cast<size_t> (used) / sizeof (wchar_t);
        return std::wstring (reinterpret_cast<const wchar_t*> (buf.data()), wcharCount);
    }

    static CFStringRef wstringToCFString (const std::wstring& ws)
    {
        return CFStringCreateWithBytes (kCFAllocatorDefault,
                                        reinterpret_cast<const UInt8*> (ws.data()),
                                        static_cast<CFIndex> (ws.size() * sizeof (wchar_t)),
                                        kCFStringEncodingUTF32LE,
                                        false);
    }
#endif

    // 有効な再生エンドポイント一覧を取得する（仮想デバイスを先頭に並べる）
    static std::vector<RenderEndpointInfo> enumerateRenderEndpoints()
    {
        std::vector<RenderEndpointInfo> result;
#ifdef _WIN32
        const HRESULT hrCo = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        const bool needUninit = SUCCEEDED (hrCo);

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof (IMMDeviceEnumerator), reinterpret_cast<void**> (&enumerator));
        if (SUCCEEDED (hr) && enumerator != nullptr)
        {
            IMMDeviceCollection* collection = nullptr;
            if (SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &collection)) && collection != nullptr)
            {
                UINT count = 0;
                collection->GetCount (&count);
                for (UINT i = 0; i < count; ++i)
                {
                    IMMDevice* dev = nullptr;
                    if (SUCCEEDED (collection->Item (i, &dev)) && dev != nullptr)
                    {
                        LPWSTR rawId = nullptr;
                        std::wstring idStr, nameStr;
                        if (SUCCEEDED (dev->GetId (&rawId)) && rawId != nullptr)
                        {
                            idStr = rawId;
                            CoTaskMemFree (rawId);
                        }
                        IPropertyStore* props = nullptr;
                        if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &props)) && props != nullptr)
                        {
                            PROPVARIANT pv;
                            PropVariantInit (&pv);
                            if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr)
                                nameStr = pv.pwszVal;
                            PropVariantClear (&pv);
                            props->Release();
                        }
                        if (! idStr.empty())
                        {
                            RenderEndpointInfo info;
                            info.id = idStr;
                            info.name = nameStr.empty() ? idStr : nameStr;
                            info.virtualPriority = classifyVirtualDevicePriority (info.name);
                            result.push_back (std::move (info));
                        }
                        dev->Release();
                    }
                }
                collection->Release();
            }
            enumerator->Release();
        }

        if (needUninit)
            CoUninitialize();
#elif defined(__APPLE__)
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &dataSize) == noErr && dataSize > 0)
        {
            const size_t devCount = dataSize / sizeof (AudioDeviceID);
            std::vector<AudioDeviceID> devIds (devCount, 0);
            if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &dataSize, devIds.data()) == noErr)
            {
                for (AudioDeviceID devId : devIds)
                {
                    // 出力チャンネル数をチェック
                    AudioObjectPropertyAddress streamAddr = {
                        kAudioDevicePropertyStreamConfiguration,
                        kAudioDevicePropertyScopeOutput,
                        kAudioObjectPropertyElementMain
                    };
                    UInt32 cfgSize = 0;
                    if (AudioObjectGetPropertyDataSize (devId, &streamAddr, 0, nullptr, &cfgSize) != noErr || cfgSize == 0)
                        continue;
                    std::vector<uint8_t> cfgBuf (cfgSize, 0);
                    auto* abl = reinterpret_cast<AudioBufferList*> (cfgBuf.data());
                    if (AudioObjectGetPropertyData (devId, &streamAddr, 0, nullptr, &cfgSize, abl) != noErr)
                        continue;
                    UInt32 outCh = 0;
                    for (UInt32 b = 0; b < abl->mNumberBuffers; ++b)
                        outCh += abl->mBuffers[b].mNumberChannels;
                    if (outCh == 0)
                        continue;

                    CFStringRef uidRef = nullptr;
                    UInt32 uidSize = sizeof (uidRef);
                    AudioObjectPropertyAddress uidAddr = {
                        kAudioDevicePropertyDeviceUID,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    AudioObjectGetPropertyData (devId, &uidAddr, 0, nullptr, &uidSize, &uidRef);

                    CFStringRef nameRef = nullptr;
                    UInt32 nameSize = sizeof (nameRef);
                    AudioObjectPropertyAddress nameAddr = {
                        kAudioObjectPropertyName,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    AudioObjectGetPropertyData (devId, &nameAddr, 0, nullptr, &nameSize, &nameRef);

                    std::wstring uidStr  = cfStringToWString (uidRef);
                    std::wstring nameStr = cfStringToWString (nameRef);
                    if (uidRef != nullptr)  CFRelease (uidRef);
                    if (nameRef != nullptr) CFRelease (nameRef);

                    if (! uidStr.empty())
                    {
                        RenderEndpointInfo info;
                        info.id = uidStr;
                        info.name = nameStr.empty() ? uidStr : nameStr;
                        info.virtualPriority = classifyVirtualDevicePriority (info.name);
                        result.push_back (std::move (info));
                    }
                }
            }
        }
#endif
        std::stable_sort (result.begin(), result.end(), [] (const RenderEndpointInfo& a, const RenderEndpointInfo& b)
        {
            const int pa = (a.virtualPriority > 0) ? a.virtualPriority : 999;
            const int pb = (b.virtualPriority > 0) ? b.virtualPriority : 999;
            return pa < pb;
        });
        return result;
    }

    // 送信スレッドを開始する。targetEndpointId が空文字なら仮想ドライバを自動選択する
    void start (double hostSampleRate, const std::wstring& targetEndpointId = L"")
    {
        setHostSampleRate (hostSampleRate);
        {
            std::lock_guard<std::mutex> lk (configMutex_);
            requestedDeviceId_ = targetEndpointId;
        }
        reconnectRequested_.store (true, std::memory_order_release);

        if (! threadRunning_.load (std::memory_order_acquire))
        {
            stopRequested_.store (false, std::memory_order_release);
            threadRunning_.store (true, std::memory_order_release);
            worker_ = std::thread ([this] { workerLoop(); });
        }
        else
        {
            notifyWake();
        }
    }

    void stop()
    {
        stopRequested_.store (true, std::memory_order_release);
        notifyWake();
        if (worker_.joinable())
            worker_.join();
        threadRunning_.store (false, std::memory_order_release);
        streamConnected_.store (false, std::memory_order_release);
    }

    void requestDeviceChange (const std::wstring& targetEndpointId)
    {
        {
            std::lock_guard<std::mutex> lk (configMutex_);
            requestedDeviceId_ = targetEndpointId;
        }
        reconnectRequested_.store (true, std::memory_order_release);
        notifyWake();
    }

    void triggerReconnect()
    {
        reconnectRequested_.store (true, std::memory_order_release);
        notifyWake();
    }

    void setHostSampleRate (double sr) noexcept
    {
        if (sr >= 8000.0 && sr <= 384000.0)
            hostSampleRate_.store (sr, std::memory_order_relaxed);
    }

    void setGainLinear (float g) noexcept   { gainLinear_.store (g, std::memory_order_relaxed); }
    float getGainLinear() const noexcept    { return gainLinear_.load (std::memory_order_relaxed); }

    void setMuted (bool m) noexcept         { muted_.store (m, std::memory_order_relaxed); }
    bool isMuted() const noexcept           { return muted_.load (std::memory_order_relaxed); }

    bool isConnected() const noexcept       { return streamConnected_.load (std::memory_order_relaxed); }
    double getHostSampleRate() const noexcept   { return hostSampleRate_.load (std::memory_order_relaxed); }
    double getDeviceSampleRate() const noexcept { return deviceSampleRate_.load (std::memory_order_relaxed); }
    uint32_t getUnderrunCount() const noexcept  { return underruns_.load (std::memory_order_relaxed); }

    float getPeakLeft() const noexcept      { return meterPeakL_.load (std::memory_order_relaxed); }
    float getPeakRight() const noexcept     { return meterPeakR_.load (std::memory_order_relaxed); }
    void decayMeterPeaks (float factor = 0.86f) noexcept
    {
        meterPeakL_.store (meterPeakL_.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);
        meterPeakR_.store (meterPeakR_.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);
    }

    std::wstring getActiveDeviceName() const
    {
        std::lock_guard<std::mutex> lk (statusMutex_);
        return activeDeviceName_;
    }

    std::wstring getStatusMessage() const
    {
        std::lock_guard<std::mutex> lk (statusMutex_);
        return statusMessage_;
    }

    // =========================================================================
    // オーディオスレッド用: 完全ロックフリー・ゼロアロケーション
    // =========================================================================
    void pushSamples (const float* inL, const float* inR, int numSamples) noexcept
    {
        if (numSamples <= 0 || inL == nullptr)
            return;

        const float* srcR = (inR != nullptr) ? inR : inL;
        const bool isMute = muted_.load (std::memory_order_relaxed);
        const float targetGain = isMute ? 0.0f : gainLinear_.load (std::memory_order_relaxed);
        float curGain = smoothGain_;
        const float gainStep = (targetGain - curGain) / static_cast<float> (std::max (numSamples, 32));

        uint32_t w = writePos_.load (std::memory_order_relaxed);
        const uint32_t r = readPos_.load (std::memory_order_acquire);
        const uint32_t used = w - r;
        const uint32_t freeSpace = (used < kRingFrames - 8u) ? (kRingFrames - 8u - used) : 0u;
        const uint32_t toWrite = std::min (static_cast<uint32_t> (numSamples), freeSpace);

        float peakL = 0.0f;
        float peakR = 0.0f;

        for (uint32_t i = 0; i < static_cast<uint32_t> (numSamples); ++i)
        {
            curGain += gainStep;
            float sL = inL[i] * curGain;
            float sR = srcR[i] * curGain;

            if (! (sL >= -4.0f && sL <= 4.0f)) sL = 0.0f;
            if (! (sR >= -4.0f && sR <= 4.0f)) sR = 0.0f;
            sL = softLimit (sL);
            sR = softLimit (sR);

            const float aL = std::fabs (sL);
            const float aR = std::fabs (sR);
            if (aL > peakL) peakL = aL;
            if (aR > peakR) peakR = aR;

            if (i < toWrite && streamConnected_.load (std::memory_order_relaxed))
            {
                const uint32_t idx = (w + i) & kRingMask;
                ringL_[idx] = sL;
                ringR_[idx] = sR;
            }
        }
        smoothGain_ = targetGain;

        if (toWrite > 0 && streamConnected_.load (std::memory_order_relaxed))
            writePos_.store (w + toWrite, std::memory_order_release);

        if (peakL > meterPeakL_.load (std::memory_order_relaxed))
            meterPeakL_.store (peakL, std::memory_order_relaxed);
        if (peakR > meterPeakR_.load (std::memory_order_relaxed))
            meterPeakR_.store (peakR, std::memory_order_relaxed);
    }

    // =========================================================================
    // 単体テスト用ヘッドレス検証API
    // =========================================================================
    void resetResamplerForTest (double hostSr, double devSr, uint32_t targetFillFrames = 1440u) noexcept
    {
        hostSampleRate_.store (hostSr, std::memory_order_relaxed);
        deviceSampleRate_.store (devSr, std::memory_order_relaxed);
        streamConnected_.store (true, std::memory_order_relaxed);
        smoothGain_ = gainLinear_.load (std::memory_order_relaxed);
        resetFifoWithSilenceCushion (targetFillFrames, hostSr / std::max (8000.0, devSr));
    }

    void renderResampledBlockForTest (float* outL, float* outR, uint32_t outFrames) noexcept
    {
        renderResampledStereo (outL, outR, outFrames,
                               hostSampleRate_.load (std::memory_order_relaxed),
                               deviceSampleRate_.load (std::memory_order_relaxed));
    }

private:
    void notifyWake() noexcept
    {
#ifdef _WIN32
        if (wakeEvent_ != nullptr)
            SetEvent (wakeEvent_);
#else
        wakeCv_.notify_all();
#endif
    }

    void sleepWakeableMs (uint32_t ms)
    {
#ifdef _WIN32
        if (wakeEvent_ != nullptr)
            WaitForSingleObject (wakeEvent_, ms);
#else
        std::unique_lock<std::mutex> lk (wakeMutex_);
        wakeCv_.wait_for (lk, std::chrono::milliseconds (ms), [this]
        {
            return stopRequested_.load (std::memory_order_acquire)
                || reconnectRequested_.load (std::memory_order_acquire);
        });
#endif
    }

    static float softLimit (float x) noexcept
    {
        if (x > 0.98f)
            return 0.98f + 0.02f * std::tanh ((x - 0.98f) * 20.0f);
        if (x < -0.98f)
            return -0.98f + 0.02f * std::tanh ((x + 0.98f) * 20.0f);
        return x;
    }

    // 4点3次 Hermite (Catmull-Rom) 補間
    static float hermite4 (float frac, float ym1, float y0, float y1, float y2) noexcept
    {
        const float c0 = y0;
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    void resetFifoWithSilenceCushion (uint32_t targetFrames, double nominalRatio) noexcept
    {
        std::fill (ringL_.begin(), ringL_.end(), 0.0f);
        std::fill (ringR_.begin(), ringR_.end(), 0.0f);
        targetFill_ = std::clamp (targetFrames, 512u, kRingFrames / 4u);
        readPos_.store (0u, std::memory_order_relaxed);
        writePos_.store (targetFill_, std::memory_order_release);
        fillSmooth_ = static_cast<double> (targetFill_);
        ratioSmooth_ = std::clamp (nominalRatio, 0.25, 4.0);
        fracPos_ = 0.0;
        std::memset (histL_, 0, sizeof (histL_));
        std::memset (histR_, 0, sizeof (histR_));
        histPrimed_ = false;
        refilling_ = false;
        needCrossfade_ = false;
        lastOutL_ = 0.0f;
        lastOutR_ = 0.0f;
        underruns_.store (0u, std::memory_order_relaxed);
    }

    void popOneInputFrame (uint32_t& r, float& outL, float& outR) noexcept
    {
        const uint32_t idx = r & kRingMask;
        outL = ringL_[idx];
        outR = ringR_[idx];
        ++r;
    }

    void advanceHistory (uint32_t& r) noexcept
    {
        float nL = 0.0f, nR = 0.0f;
        popOneInputFrame (r, nL, nR);
        histL_[0] = histL_[1]; histL_[1] = histL_[2]; histL_[2] = histL_[3]; histL_[3] = nL;
        histR_[0] = histR_[1]; histR_[1] = histR_[2]; histR_[2] = histR_[3]; histR_[3] = nR;
    }

    void renderResampledStereo (float* outL, float* outR, uint32_t outFrames, double hostSr, double devSr) noexcept
    {
        if (outFrames == 0)
            return;

        uint32_t r = readPos_.load (std::memory_order_relaxed);
        const uint32_t w = writePos_.load (std::memory_order_acquire);
        uint32_t avail = w - r;

        const uint32_t maxAllowed = std::max (targetFill_ * 3u, 4096u);
        if (avail > maxAllowed)
        {
            const uint32_t drop = avail - targetFill_;
            r += drop;
            readPos_.store (r, std::memory_order_release);
            avail = w - r;
            fillSmooth_ = static_cast<double> (targetFill_);
            histPrimed_ = false;
            refilling_ = false;
            needCrossfade_ = true;
        }

        if (refilling_)
        {
            if (avail < targetFill_)
            {
                for (uint32_t i = 0; i < outFrames; ++i)
                {
                    lastOutL_ = (std::fabs (lastOutL_) > 1.0e-6f) ? (lastOutL_ * 0.965f) : 0.0f;
                    lastOutR_ = (std::fabs (lastOutR_) > 1.0e-6f) ? (lastOutR_ * 0.965f) : 0.0f;
                    outL[i] = lastOutL_;
                    outR[i] = lastOutR_;
                }
                return;
            }
            refilling_ = false;
            fillSmooth_ = static_cast<double> (avail);
            ratioSmooth_ = std::clamp (hostSr / std::max (8000.0, devSr), 0.25, 4.0);
            histPrimed_ = false;
            needCrossfade_ = true;
        }

        fillSmooth_ += 0.02 * (static_cast<double> (avail) - fillSmooth_);
        const double diff = fillSmooth_ - static_cast<double> (targetFill_);
        const double deadband = 48.0;
        const double effDiff = (std::fabs (diff) <= deadband) ? 0.0
                             : (diff > 0.0 ? diff - deadband : diff + deadband);
        const double relErr = effDiff / static_cast<double> (std::max (1u, targetFill_));
        const double corr = std::clamp (relErr * 0.015, -0.003, 0.003);
        const double nominalRatio = hostSr / std::max (8000.0, devSr);
        const double desiredRatio = std::clamp (nominalRatio * (1.0 + corr), 0.25, 4.0);
        ratioSmooth_ += 0.02 * (desiredRatio - ratioSmooth_);

        const uint32_t neededInput = static_cast<uint32_t> (std::ceil (fracPos_ + static_cast<double> (outFrames) * ratioSmooth_)) + (histPrimed_ ? 2u : 6u);
        if (avail < neededInput)
        {
            underruns_.fetch_add (1u, std::memory_order_relaxed);
            for (uint32_t i = 0; i < outFrames; ++i)
            {
                lastOutL_ = (std::fabs (lastOutL_) > 1.0e-6f) ? (lastOutL_ * 0.965f) : 0.0f;
                lastOutR_ = (std::fabs (lastOutR_) > 1.0e-6f) ? (lastOutR_ * 0.965f) : 0.0f;
                outL[i] = lastOutL_;
                outR[i] = lastOutR_;
            }
            refilling_ = true;
            histPrimed_ = false;
            needCrossfade_ = true;
            return;
        }

        if (! histPrimed_)
        {
            float s0L = 0.0f, s0R = 0.0f;
            popOneInputFrame (r, s0L, s0R);
            histL_[0] = s0L; histL_[1] = s0L;
            histR_[0] = s0R; histR_[1] = s0R;
            popOneInputFrame (r, histL_[2], histR_[2]);
            popOneInputFrame (r, histL_[3], histR_[3]);
            fracPos_ = 0.0;
            histPrimed_ = true;
        }

        for (uint32_t i = 0; i < outFrames; ++i)
        {
            while (fracPos_ >= 1.0)
            {
                advanceHistory (r);
                fracPos_ -= 1.0;
            }
            const float f = static_cast<float> (fracPos_);
            outL[i] = hermite4 (f, histL_[0], histL_[1], histL_[2], histL_[3]);
            outR[i] = hermite4 (f, histR_[0], histR_[1], histR_[2], histR_[3]);
            fracPos_ += ratioSmooth_;
        }

        readPos_.store (r, std::memory_order_release);

        if (needCrossfade_)
        {
            const uint32_t xlen = std::min (outFrames, 64u);
            for (uint32_t i = 0; i < xlen; ++i)
            {
                const float g = static_cast<float> (i + 1u) / static_cast<float> (xlen + 1u);
                outL[i] = lastOutL_ * (1.0f - g) + outL[i] * g;
                outR[i] = lastOutR_ * (1.0f - g) + outR[i] * g;
            }
            needCrossfade_ = false;
        }

        lastOutL_ = outL[outFrames - 1u];
        lastOutR_ = outR[outFrames - 1u];
    }

    void setStatus (const std::wstring& devName, const std::wstring& msg)
    {
        std::lock_guard<std::mutex> lk (statusMutex_);
        activeDeviceName_ = devName;
        statusMessage_ = msg;
    }

#ifdef __APPLE__
    static OSStatus coreAudioRenderProc (void* inRefCon,
                                         AudioUnitRenderActionFlags* /*ioActionFlags*/,
                                         const AudioTimeStamp* /*inTimeStamp*/,
                                         UInt32 /*inBusNumber*/,
                                         UInt32 inNumberFrames,
                                         AudioBufferList* ioData) noexcept
    {
        auto* self = static_cast<WasapiLoopbackSender*> (inRefCon);
        if (self == nullptr || ioData == nullptr || ioData->mNumberBuffers == 0)
            return noErr;

        float* dstL = static_cast<float*> (ioData->mBuffers[0].mData);
        float* dstR = (ioData->mNumberBuffers > 1 && ioData->mBuffers[1].mData != nullptr)
                        ? static_cast<float*> (ioData->mBuffers[1].mData)
                        : nullptr;
        if (dstL == nullptr)
            return noErr;

        if (dstR != nullptr)
        {
            self->renderResampledStereo (dstL, dstR, inNumberFrames,
                                         self->hostSampleRate_.load (std::memory_order_relaxed),
                                         self->deviceSampleRate_.load (std::memory_order_relaxed));
            for (UInt32 b = 2; b < ioData->mNumberBuffers; ++b)
                if (ioData->mBuffers[b].mData != nullptr)
                    std::memset (ioData->mBuffers[b].mData, 0, inNumberFrames * sizeof (float));
        }
        else
        {
            const UInt32 frames = std::min<UInt32> (inNumberFrames, static_cast<UInt32> (self->macMonoScratchR_.size()));
            self->renderResampledStereo (dstL, self->macMonoScratchR_.data(), frames,
                                         self->hostSampleRate_.load (std::memory_order_relaxed),
                                         self->deviceSampleRate_.load (std::memory_order_relaxed));
            for (UInt32 i = 0; i < frames; ++i)
                dstL[i] = 0.5f * (dstL[i] + self->macMonoScratchR_[i]);
        }
        return noErr;
    }
#endif

    void workerLoop()
    {
#ifdef _WIN32
        const HRESULT hrCo = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        DWORD mmcssTaskIndex = 0;
        HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW (L"Pro Audio", &mmcssTaskIndex);

        std::vector<float> scratchL (8192, 0.0f);
        std::vector<float> scratchR (8192, 0.0f);

        while (! stopRequested_.load (std::memory_order_acquire))
        {
            reconnectRequested_.store (false, std::memory_order_release);
            streamConnected_.store (false, std::memory_order_release);

            std::wstring targetId;
            {
                std::lock_guard<std::mutex> lk (configMutex_);
                targetId = requestedDeviceId_;
            }

            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDevice* device = nullptr;
            IAudioClient* audioClient = nullptr;
            IAudioRenderClient* renderClient = nullptr;
            WAVEFORMATEX* mixFormat = nullptr;
            std::wstring chosenName;

            HRESULT hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                           __uuidof (IMMDeviceEnumerator), reinterpret_cast<void**> (&enumerator));
            if (SUCCEEDED (hr) && enumerator != nullptr)
            {
                if (! targetId.empty())
                {
                    hr = enumerator->GetDevice (targetId.c_str(), &device);
                    if (SUCCEEDED (hr) && device != nullptr)
                    {
                        DWORD state = 0;
                        if (FAILED (device->GetState (&state)) || state != DEVICE_STATE_ACTIVE)
                        {
                            device->Release();
                            device = nullptr;
                        }
                    }
                }
                else
                {
                    IMMDeviceCollection* col = nullptr;
                    if (SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &col)) && col != nullptr)
                    {
                        UINT count = 0;
                        col->GetCount (&count);
                        int bestPrio = 999;

                        for (UINT i = 0; i < count; ++i)
                        {
                            IMMDevice* cand = nullptr;
                            if (SUCCEEDED (col->Item (i, &cand)) && cand != nullptr)
                            {
                                std::wstring friendly;
                                IPropertyStore* ps = nullptr;
                                if (SUCCEEDED (cand->OpenPropertyStore (STGM_READ, &ps)) && ps != nullptr)
                                {
                                    PROPVARIANT pv;
                                    PropVariantInit (&pv);
                                    if (SUCCEEDED (ps->GetValue (PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr)
                                        friendly = pv.pwszVal;
                                    PropVariantClear (&pv);
                                    ps->Release();
                                }

                                const int prio = classifyVirtualDevicePriority (friendly);
                                if (prio > 0 && prio < bestPrio)
                                {
                                    if (device != nullptr)
                                        device->Release();
                                    device = cand;
                                    chosenName = friendly;
                                    bestPrio = prio;
                                    cand = nullptr;
                                    if (prio == 1)
                                        break;
                                }
                                if (cand != nullptr)
                                    cand->Release();
                            }
                        }
                        col->Release();
                    }
                }

                if (device != nullptr && chosenName.empty())
                {
                    IPropertyStore* ps = nullptr;
                    if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &ps)) && ps != nullptr)
                    {
                        PROPVARIANT pv;
                        PropVariantInit (&pv);
                        if (SUCCEEDED (ps->GetValue (PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr)
                            chosenName = pv.pwszVal;
                        PropVariantClear (&pv);
                        ps->Release();
                    }
                }
            }

            if (device == nullptr)
            {
                setStatus (L"", L"仮想オーディオドライバ未検出（送信先を選択してください）");
                if (enumerator != nullptr) enumerator->Release();
                WaitForSingleObject (wakeEvent_, 1500);
                continue;
            }

            hr = device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (&audioClient));
            if (SUCCEEDED (hr) && audioClient != nullptr)
                hr = audioClient->GetMixFormat (&mixFormat);

            bool isFloat32 = false;
            bool isPcm16 = false;
            UINT32 devChannels = 2;
            UINT32 devRate = 48000;

            if (SUCCEEDED (hr) && mixFormat != nullptr)
            {
                devChannels = std::max<UINT32> (1u, mixFormat->nChannels);
                devRate = std::max<UINT32> (8000u, mixFormat->nSamplesPerSec);

                if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mixFormat->wBitsPerSample == 32)
                {
                    isFloat32 = true;
                }
                else if (mixFormat->wFormatTag == WAVE_FORMAT_PCM && mixFormat->wBitsPerSample == 16)
                {
                    isPcm16 = true;
                }
                else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                {
                    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (mixFormat);
                    if (IsEqualGUID (ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && mixFormat->wBitsPerSample == 32)
                        isFloat32 = true;
                    else if (IsEqualGUID (ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && mixFormat->wBitsPerSample == 16)
                        isPcm16 = true;
                }

                const REFERENCE_TIME hnsBufferDuration = 200000; // 20ms
                hr = audioClient->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                              hnsBufferDuration,
                                              0,
                                              mixFormat,
                                              nullptr);
            }

            UINT32 bufferFrameCount = 0;
            if (SUCCEEDED (hr))
                hr = audioClient->SetEventHandle (audioEvent_);
            if (SUCCEEDED (hr))
                hr = audioClient->GetBufferSize (&bufferFrameCount);
            if (SUCCEEDED (hr))
                hr = audioClient->GetService (__uuidof (IAudioRenderClient), reinterpret_cast<void**> (&renderClient));

            if (FAILED (hr) || renderClient == nullptr || (! isFloat32 && ! isPcm16))
            {
                setStatus (chosenName, L"WASAPI 初期化失敗（再試行待機中）");
                if (renderClient != nullptr) renderClient->Release();
                if (mixFormat != nullptr)    CoTaskMemFree (mixFormat);
                if (audioClient != nullptr)  audioClient->Release();
                if (device != nullptr)       device->Release();
                if (enumerator != nullptr)   enumerator->Release();
                WaitForSingleObject (wakeEvent_, 1500);
                continue;
            }

            deviceSampleRate_.store (static_cast<double> (devRate), std::memory_order_relaxed);
            const double curHostSr = hostSampleRate_.load (std::memory_order_relaxed);
            const uint32_t targetFill = std::max (static_cast<uint32_t> (curHostSr * 0.030), bufferFrameCount * 2u);
            resetFifoWithSilenceCushion (targetFill, curHostSr / static_cast<double> (devRate));

            if (scratchL.size() < bufferFrameCount + 64u)
            {
                scratchL.resize (bufferFrameCount + 64u, 0.0f);
                scratchR.resize (bufferFrameCount + 64u, 0.0f);
            }

            BYTE* initData = nullptr;
            if (SUCCEEDED (renderClient->GetBuffer (bufferFrameCount, &initData)))
                renderClient->ReleaseBuffer (bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);

            ResetEvent (audioEvent_);
            hr = audioClient->Start();
            if (SUCCEEDED (hr))
            {
                streamConnected_.store (true, std::memory_order_release);
                setStatus (chosenName, L"OBS 送信中（正常稼働）");

                HANDLE waitHandles[2] = { wakeEvent_, audioEvent_ };
                while (! stopRequested_.load (std::memory_order_acquire)
                       && ! reconnectRequested_.load (std::memory_order_acquire))
                {
                    const DWORD wr = WaitForMultipleObjects (2, waitHandles, FALSE, 200);
                    if (stopRequested_.load (std::memory_order_acquire)
                        || reconnectRequested_.load (std::memory_order_acquire))
                        break;
                    if (wr == WAIT_FAILED)
                        break;

                    UINT32 padding = 0;
                    hr = audioClient->GetCurrentPadding (&padding);
                    if (FAILED (hr))
                        break;

                    const UINT32 availFrames = (bufferFrameCount > padding) ? (bufferFrameCount - padding) : 0u;
                    if (availFrames == 0u)
                        continue;

                    BYTE* pData = nullptr;
                    hr = renderClient->GetBuffer (availFrames, &pData);
                    if (FAILED (hr) || pData == nullptr)
                        break;

                    renderResampledStereo (scratchL.data(), scratchR.data(), availFrames,
                                           hostSampleRate_.load (std::memory_order_relaxed),
                                           static_cast<double> (devRate));

                    if (isFloat32)
                    {
                        float* dst = reinterpret_cast<float*> (pData);
                        if (devChannels == 2u)
                        {
                            for (UINT32 i = 0; i < availFrames; ++i)
                            {
                                dst[i * 2u]      = scratchL[i];
                                dst[i * 2u + 1u] = scratchR[i];
                            }
                        }
                        else
                        {
                            for (UINT32 i = 0; i < availFrames; ++i)
                            {
                                float* frame = dst + i * devChannels;
                                frame[0] = (devChannels == 1u) ? (0.5f * (scratchL[i] + scratchR[i])) : scratchL[i];
                                if (devChannels >= 2u) frame[1] = scratchR[i];
                                for (UINT32 ch = 2u; ch < devChannels; ++ch) frame[ch] = 0.0f;
                            }
                        }
                    }
                    else if (isPcm16)
                    {
                        int16_t* dst = reinterpret_cast<int16_t*> (pData);
                        for (UINT32 i = 0; i < availFrames; ++i)
                        {
                            int16_t* frame = dst + i * devChannels;
                            const float mL = std::clamp (scratchL[i], -1.0f, 1.0f);
                            const float mR = std::clamp (scratchR[i], -1.0f, 1.0f);
                            if (devChannels == 1u)
                            {
                                frame[0] = static_cast<int16_t> (std::lrintf (0.5f * (mL + mR) * 32767.0f));
                            }
                            else
                            {
                                frame[0] = static_cast<int16_t> (std::lrintf (mL * 32767.0f));
                                frame[1] = static_cast<int16_t> (std::lrintf (mR * 32767.0f));
                                for (UINT32 ch = 2u; ch < devChannels; ++ch) frame[ch] = 0;
                            }
                        }
                    }

                    hr = renderClient->ReleaseBuffer (availFrames, 0);
                    if (FAILED (hr))
                        break;
                }

                audioClient->Stop();
            }

            streamConnected_.store (false, std::memory_order_release);
            renderClient->Release();
            if (mixFormat != nullptr) CoTaskMemFree (mixFormat);
            audioClient->Release();
            device->Release();
            enumerator->Release();

            if (! stopRequested_.load (std::memory_order_acquire)
                && ! reconnectRequested_.load (std::memory_order_acquire))
            {
                setStatus (chosenName, L"デバイス再接続待機中...");
                WaitForSingleObject (wakeEvent_, 1000);
            }
        }

        if (mmcssHandle != nullptr)
            AvRevertMmThreadCharacteristics (mmcssHandle);
        if (SUCCEEDED (hrCo))
            CoUninitialize();

#elif defined(__APPLE__)
        macMonoScratchR_.assign (8192, 0.0f);

        while (! stopRequested_.load (std::memory_order_acquire))
        {
            reconnectRequested_.store (false, std::memory_order_release);
            streamConnected_.store (false, std::memory_order_release);

            std::wstring targetUid;
            {
                std::lock_guard<std::mutex> lk (configMutex_);
                targetUid = requestedDeviceId_;
            }

            const auto endpoints = enumerateRenderEndpoints();
            std::wstring chosenUid, chosenName;
            if (! targetUid.empty())
            {
                for (const auto& ep : endpoints)
                {
                    if (ep.id == targetUid)
                    {
                        chosenUid  = ep.id;
                        chosenName = ep.name;
                        break;
                    }
                }
            }
            else
            {
                for (const auto& ep : endpoints)
                {
                    if (ep.virtualPriority > 0)
                    {
                        chosenUid  = ep.id;
                        chosenName = ep.name;
                        break;
                    }
                }
            }

            if (chosenUid.empty())
            {
                setStatus (L"", L"仮想オーディオデバイス未検出（BlackHole等を導入または送信先を選択）");
                sleepWakeableMs (1500);
                continue;
            }

            // UID から AudioDeviceID を解決
            AudioDeviceID targetDevId = kAudioObjectUnknown;
            {
                AudioObjectPropertyAddress addr = {
                    kAudioHardwarePropertyDevices,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                UInt32 dataSize = 0;
                if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &dataSize) == noErr && dataSize > 0)
                {
                    std::vector<AudioDeviceID> ids (dataSize / sizeof (AudioDeviceID), 0);
                    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &dataSize, ids.data()) == noErr)
                    {
                        for (AudioDeviceID id : ids)
                        {
                            CFStringRef uidRef = nullptr;
                            UInt32 uSz = sizeof (uidRef);
                            AudioObjectPropertyAddress uidAddr = {
                                kAudioDevicePropertyDeviceUID,
                                kAudioObjectPropertyScopeGlobal,
                                kAudioObjectPropertyElementMain
                            };
                            if (AudioObjectGetPropertyData (id, &uidAddr, 0, nullptr, &uSz, &uidRef) == noErr && uidRef != nullptr)
                            {
                                const std::wstring ws = cfStringToWString (uidRef);
                                CFRelease (uidRef);
                                if (ws == chosenUid)
                                {
                                    targetDevId = id;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (targetDevId == kAudioObjectUnknown)
            {
                setStatus (chosenName, L"CoreAudio デバイス解決失敗（再試行待機中）");
                sleepWakeableMs (1500);
                continue;
            }

            Float64 nominalSr = 48000.0;
            {
                AudioObjectPropertyAddress srAddr = {
                    kAudioDevicePropertyNominalSampleRate,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                UInt32 srSz = sizeof (nominalSr);
                AudioObjectGetPropertyData (targetDevId, &srAddr, 0, nullptr, &srSz, &nominalSr);
                if (nominalSr < 8000.0) nominalSr = 48000.0;
            }

            AudioComponentDescription desc {};
            desc.componentType         = kAudioUnitType_Output;
            desc.componentSubType      = kAudioUnitSubType_HALOutput;
            desc.componentManufacturer = kAudioUnitManufacturer_Apple;

            AudioComponent comp = AudioComponentFindNext (nullptr, &desc);
            AudioComponentInstance au = nullptr;
            OSStatus st = (comp != nullptr) ? AudioComponentInstanceNew (comp, &au) : -1;

            if (st == noErr && au != nullptr)
            {
                UInt32 enableOut = 1, disableIn = 0;
                AudioUnitSetProperty (au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableOut, sizeof (enableOut));
                AudioUnitSetProperty (au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,  1, &disableIn, sizeof (disableIn));
                st = AudioUnitSetProperty (au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &targetDevId, sizeof (targetDevId));
            }

            if (st == noErr)
            {
                AudioStreamBasicDescription asbd {};
                asbd.mSampleRate       = nominalSr;
                asbd.mFormatID         = kAudioFormatLinearPCM;
                asbd.mFormatFlags      = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
                asbd.mBytesPerPacket   = sizeof (float);
                asbd.mFramesPerPacket  = 1;
                asbd.mBytesPerFrame    = sizeof (float);
                asbd.mChannelsPerFrame = 2;
                asbd.mBitsPerChannel   = 32;
                st = AudioUnitSetProperty (au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof (asbd));
            }

            if (st == noErr)
            {
                AURenderCallbackStruct cb {};
                cb.inputProc       = &WasapiLoopbackSender::coreAudioRenderProc;
                cb.inputProcRefCon = this;
                st = AudioUnitSetProperty (au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof (cb));
            }

            if (st == noErr)
                st = AudioUnitInitialize (au);

            if (st == noErr)
            {
                deviceSampleRate_.store (nominalSr, std::memory_order_relaxed);
                const double curHostSr = hostSampleRate_.load (std::memory_order_relaxed);
                const uint32_t targetFill = std::max (static_cast<uint32_t> (curHostSr * 0.030), 1536u);
                resetFifoWithSilenceCushion (targetFill, curHostSr / nominalSr);

                st = AudioOutputUnitStart (au);
                if (st == noErr)
                {
                    streamConnected_.store (true, std::memory_order_release);
                    setStatus (chosenName, L"OBS 送信中（CoreAudio 正常稼働）");

                    while (! stopRequested_.load (std::memory_order_acquire)
                           && ! reconnectRequested_.load (std::memory_order_acquire))
                    {
                        sleepWakeableMs (500);
                    }
                    AudioOutputUnitStop (au);
                }
                AudioUnitUninitialize (au);
            }

            streamConnected_.store (false, std::memory_order_release);
            if (au != nullptr)
                AudioComponentInstanceDispose (au);

            if (! stopRequested_.load (std::memory_order_acquire)
                && ! reconnectRequested_.load (std::memory_order_acquire))
            {
                setStatus (chosenName, L"CoreAudio 再接続待機中...");
                sleepWakeableMs (1500);
            }
        }
#endif
    }

    std::vector<float> ringL_;
    std::vector<float> ringR_;
    alignas (64) std::atomic<uint32_t> writePos_ { 0u };
    alignas (64) std::atomic<uint32_t> readPos_  { 0u };

    std::atomic<double>   hostSampleRate_   { 48000.0 };
    std::atomic<double>   deviceSampleRate_ { 44100.0 };
    std::atomic<float>    gainLinear_       { 1.0f };
    std::atomic<bool>     muted_            { false };
    std::atomic<bool>     streamConnected_  { false };
    std::atomic<bool>     stopRequested_    { false };
    std::atomic<bool>     reconnectRequested_ { false };
    std::atomic<bool>     threadRunning_    { false };
    std::atomic<uint32_t> underruns_        { 0u };
    std::atomic<float>    meterPeakL_       { 0.0f };
    std::atomic<float>    meterPeakR_       { 0.0f };

    float    smoothGain_    = 1.0f;
    uint32_t targetFill_    = 1440u;
    double   fillSmooth_    = 1440.0;
    double   ratioSmooth_   = 1.0;
    double   fracPos_       = 0.0;
    float    histL_[4]      { 0.0f, 0.0f, 0.0f, 0.0f };
    float    histR_[4]      { 0.0f, 0.0f, 0.0f, 0.0f };
    bool     histPrimed_    = false;
    bool     refilling_     = false;
    bool     needCrossfade_ = false;
    float    lastOutL_      = 0.0f;
    float    lastOutR_      = 0.0f;

    mutable std::mutex configMutex_;
    std::wstring requestedDeviceId_;

    mutable std::mutex statusMutex_;
    std::wstring activeDeviceName_;
    std::wstring statusMessage_ { L"起動準備中..." };

    std::thread worker_;
#ifdef _WIN32
    HANDLE wakeEvent_  = nullptr;
    HANDLE audioEvent_ = nullptr;
#else
    std::mutex              wakeMutex_;
    std::condition_variable wakeCv_;
#ifdef __APPLE__
    std::vector<float>      macMonoScratchR_;
#endif
#endif
};

} // namespace sobr
