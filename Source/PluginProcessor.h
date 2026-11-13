#pragma once
// =============================================================================
// PluginProcessor.h
// REAPER の Master トラック（syncroom_vst_bridge2 の下）に挿すだけで、
// 本線の ASIO 出力は完全パススルーで守りつつ、裏で Yamaha SYNCROOM Driver (WDM) へ
// 自分＋SYNCROOM他メンバー全員のミックス音を流し込む VST3 プロセッサ。
// =============================================================================

#include <juce_audio_processors/juce_audio_processors.h>
#include "WasapiLoopbackSender.h"

class SyncroomObsBridgeProcessor final : public juce::AudioProcessor
{
public:
    SyncroomObsBridgeProcessor();
    ~SyncroomObsBridgeProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override    { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // UI操作用API
    void setTargetEndpointId (const juce::String& endpointId);
    juce::String getTargetEndpointId() const { return selectedEndpointId_; }
    void reconnectNow() { sender_.triggerReconnect(); }

    sobr::WasapiLoopbackSender& getSender() noexcept             { return sender_; }
    const sobr::WasapiLoopbackSender& getSender() const noexcept { return sender_; }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    sobr::WasapiLoopbackSender sender_;
    juce::String selectedEndpointId_; // 空文字 = 自動（Yamaha SYNCROOM Driver 優先）
    std::atomic<float>* gainDbParam_ = nullptr;
    std::atomic<float>* muteParam_   = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncroomObsBridgeProcessor)
};
