// =============================================================================
// verify_obs_bridge.cpp
// 1. クロスプラットフォーム共通: 48000 Hz -> 44100 Hz 可変レート Hermite 補間＆ドリフト吸収テスト
// 2. 仮想デバイス優先度分類ロジックテスト（Windows / macOS 両対応）
// 3. Windows 実機環境: Yamaha SYNCROOM Driver が存在する場合はライブ送受信テストを実施
// =============================================================================

#include "WasapiLoopbackSender.h"
#include <chrono>
#include <cstdio>
#include <cmath>
#include <thread>
#include <vector>

int main()
{
    // -------------------------------------------------------------------------
    // Test 1: 48000 Hz (128 samples) -> 44100 Hz リサンプリング無クリック検証
    // -------------------------------------------------------------------------
    {
        sobr::WasapiLoopbackSender sender;
        sender.resetResamplerForTest (48000.0, 44100.0, 1440u);

        double phase = 0.0;
        const double twoPi = 6.28318530717958647692;
        std::vector<float> inL (128), inR (128);
        std::vector<float> outL (441), outR (441);

        double prodTime = 0.0;
        double consTime = 0.0;
        float prev1 = 0.0f, prev2 = 0.0f;
        int histCount = 0;
        float maxSecondDiff = 0.0f;
        float maxOutPeak = 0.0f;

        while (consTime < 2.0)
        {
            while (prodTime <= consTime + 0.012)
            {
                for (int i = 0; i < 128; ++i)
                {
                    const float s = static_cast<float> (0.5 * std::sin (phase));
                    phase += twoPi * 440.0 / 48000.0;
                    if (phase >= twoPi) phase -= twoPi;
                    inL[i] = s;
                    inR[i] = s;
                }
                sender.pushSamples (inL.data(), inR.data(), 128);
                prodTime += 128.0 / 48000.0;
            }

            sender.renderResampledBlockForTest (outL.data(), outR.data(), 441u);
            consTime += 441.0 / 44100.0;

            if (consTime > 0.15)
            {
                for (size_t i = 0; i < 441u; ++i)
                {
                    const float y = outL[i];
                    if (std::fabs (y) > maxOutPeak) maxOutPeak = std::fabs (y);
                    if (histCount >= 2)
                    {
                        const float d2 = std::fabs (y - 2.0f * prev1 + prev2);
                        if (d2 > maxSecondDiff) maxSecondDiff = d2;
                    }
                    prev2 = prev1;
                    prev1 = y;
                    ++histCount;
                }
            }
        }

        std::printf ("[Test 1: 48k->44.1k Resampler] maxPeak=%.4f maxSecondDiff=%.5f underruns=%u\n",
                     maxOutPeak, maxSecondDiff, sender.getUnderrunCount());
        if (maxOutPeak < 0.48f || maxOutPeak > 0.52f || maxSecondDiff > 0.02f || sender.getUnderrunCount() != 0u)
        {
            std::fprintf (stderr, "FAIL: Resampler continuity test failed!\n");
            return 1;
        }
    }

    // -------------------------------------------------------------------------
    // Test 2: 仮想デバイス優先度判定検証（Windows / macOS 両対応）
    // -------------------------------------------------------------------------
    {
        const int pSync = sobr::WasapiLoopbackSender::classifyVirtualDevicePriority (L"ライン (Yamaha SYNCROOM Driver (WDM))");
        const int pBh   = sobr::WasapiLoopbackSender::classifyVirtualDevicePriority (L"BlackHole 2ch");
        const int pVb   = sobr::WasapiLoopbackSender::classifyVirtualDevicePriority (L"CABLE Input (VB-Audio Virtual Cable)");
        const int pSpk  = sobr::WasapiLoopbackSender::classifyVirtualDevicePriority (L"スピーカー (Roland Rubix 44)");
        const int pGpu  = sobr::WasapiLoopbackSender::classifyVirtualDevicePriority (L"NVIDIA Virtual Audio Device");
        if (pSync != 1 || pBh != 3 || pVb != 4 || pSpk != 0 || pGpu != 0)
        {
            std::fprintf (stderr, "FAIL: Virtual device priority classification mismatch!\n");
            return 2;
        }
        std::printf ("[Test 2: Device Classification] SYNCROOM=%d BlackHole=%d VB-CABLE=%d Speaker=%d GPU=%d\n",
                     pSync, pBh, pVb, pSpk, pGpu);
    }

#ifdef _WIN32
    // -------------------------------------------------------------------------
    // Test 3: Windows 実機に Yamaha SYNCROOM Driver が入っている場合はライブ疎通検査
    //         （CI環境等でドライバ未導入の場合は自動スキップ）
    // -------------------------------------------------------------------------
    {
        HRESULT hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof (IMMDeviceEnumerator), reinterpret_cast<void**> (&enumerator));

        IMMDevice* capDevice = nullptr;
        IMMDeviceCollection* col = nullptr;
        if (enumerator != nullptr && SUCCEEDED (enumerator->EnumAudioEndpoints (eCapture, DEVICE_STATE_ACTIVE, &col)))
        {
            UINT count = 0;
            col->GetCount (&count);
            for (UINT i = 0; i < count; ++i)
            {
                IMMDevice* cand = nullptr;
                if (SUCCEEDED (col->Item (i, &cand)) && cand != nullptr)
                {
                    IPropertyStore* ps = nullptr;
                    if (SUCCEEDED (cand->OpenPropertyStore (STGM_READ, &ps)) && ps != nullptr)
                    {
                        PROPVARIANT pv;
                        PropVariantInit (&pv);
                        if (SUCCEEDED (ps->GetValue (PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr)
                        {
                            std::wstring name = pv.pwszVal;
                            if (name.find (L"Yamaha SYNCROOM") != std::wstring::npos)
                            {
                                capDevice = cand;
                                cand = nullptr;
                            }
                        }
                        PropVariantClear (&pv);
                        ps->Release();
                    }
                    if (cand != nullptr) cand->Release();
                    if (capDevice != nullptr) break;
                }
            }
            col->Release();
        }

        if (capDevice == nullptr)
        {
            std::printf ("[Test 3: Live Driver Loopback] SKIPPED (Yamaha SYNCROOM Driver not installed on this host)\n");
            if (enumerator != nullptr) enumerator->Release();
            if (SUCCEEDED (hr)) CoUninitialize();
        }
        else
        {
            IAudioClient* capClient = nullptr;
            WAVEFORMATEX* capFmt = nullptr;
            IAudioCaptureClient* capture = nullptr;
            capDevice->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (&capClient));
            capClient->GetMixFormat (&capFmt);
            capClient->Initialize (AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, capFmt, nullptr);
            capClient->GetService (__uuidof (IAudioCaptureClient), reinterpret_cast<void**> (&capture));
            capClient->Start();

            sobr::WasapiLoopbackSender liveSender;
            liveSender.start (48000.0, L"");

            for (int wait = 0; wait < 40 && ! liveSender.isConnected(); ++wait)
                std::this_thread::sleep_for (std::chrono::milliseconds (25));

            if (! liveSender.isConnected())
            {
                std::fprintf (stderr, "FAIL: liveSender failed to connect to Yamaha SYNCROOM Driver!\n");
                return 3;
            }

            double ph = 0.0;
            float bufL[128], bufR[128];
            float capturedPeak = 0.0f;
            uint32_t capturedFrames = 0;

            const auto tStart = std::chrono::steady_clock::now();
            int blocksPushed = 0;
            while (blocksPushed < 135)
            {
                const double targetSec = (blocksPushed * 128.0) / 48000.0;
                const double elapsedSec = std::chrono::duration<double> (std::chrono::steady_clock::now() - tStart).count();
                if (elapsedSec < targetSec)
                {
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
                    continue;
                }

                for (int i = 0; i < 128; ++i)
                {
                    const float env = (blocksPushed < 10) ? (blocksPushed / 10.0f)
                                    : (blocksPushed > 120 ? ((135 - blocksPushed) / 15.0f) : 1.0f);
                    const float s = static_cast<float> (0.025 * env * std::sin (ph));
                    ph += 6.28318530717958647692 * 440.0 / 48000.0;
                    bufL[i] = s;
                    bufR[i] = s;
                }
                liveSender.pushSamples (bufL, bufR, 128);
                ++blocksPushed;

                UINT32 pktLen = 0;
                while (SUCCEEDED (capture->GetNextPacketSize (&pktLen)) && pktLen > 0)
                {
                    BYTE* pData = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (SUCCEEDED (capture->GetBuffer (&pData, &frames, &flags, nullptr, nullptr)))
                    {
                        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && pData != nullptr)
                        {
                            const float* f = reinterpret_cast<const float*> (pData);
                            for (UINT32 k = 0; k < frames * capFmt->nChannels; ++k)
                            {
                                const float a = std::fabs (f[k]);
                                if (a > capturedPeak) capturedPeak = a;
                            }
                        }
                        capturedFrames += frames;
                        capture->ReleaseBuffer (frames);
                    }
                }
            }

            liveSender.stop();
            capClient->Stop();
            capture->Release();
            CoTaskMemFree (capFmt);
            capClient->Release();
            capDevice->Release();
            enumerator->Release();
            if (SUCCEEDED (hr)) CoUninitialize();

            std::printf ("[Test 3: Live Yamaha SYNCROOM Driver Loopback] hostSr=%.0f -> devSr=%.0f | capturedFrames=%u | capturedPeak=%.5f\n",
                         liveSender.getHostSampleRate(), liveSender.getDeviceSampleRate(), capturedFrames, capturedPeak);

            if (capturedPeak < 0.015f)
            {
                std::fprintf (stderr, "FAIL: Live loopback peak too low (%.5f)!\n", capturedPeak);
                return 4;
            }
        }
    }
#endif

    std::printf ("PASS: All SyncroomObsBridge verification tests succeeded!\n");
    return 0;
}
