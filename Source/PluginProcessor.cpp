#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout SyncroomObsBridgeProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "obs_gain_db", 1 },
        juce::String::fromUTF8 ("OBS送信音量 (dB)"),
        juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f),
        0.0f));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "obs_mute", 1 },
        juce::String::fromUTF8 ("OBS送信ミュート"),
        false));

    return { params.begin(), params.end() };
}

SyncroomObsBridgeProcessor::SyncroomObsBridgeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SyncroomObsBridgeState", createParameterLayout())
{
    gainDbParam_ = apvts.getRawParameterValue ("obs_gain_db");
    muteParam_   = apvts.getRawParameterValue ("obs_mute");

    // プラグイン挿入直後からすぐに OBS 側へ接続を開始する
    sender_.start (48000.0, L"");
}

SyncroomObsBridgeProcessor::~SyncroomObsBridgeProcessor()
{
    sender_.stop();
}

void SyncroomObsBridgeProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    sender_.setHostSampleRate (sampleRate);
    if (! sender_.isConnected())
        sender_.start (sampleRate, selectedEndpointId_.toWideCharPointer());
}

void SyncroomObsBridgeProcessor::releaseResources()
{
    // REAPER は再生/停止時にも releaseResources() を呼ぶことがあるため、
    // デストラクタまで WASAPI セッションは維持して OBS 側の切断・再初期化を防ぐ。
}

bool SyncroomObsBridgeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto inSet  = layouts.getMainInputChannelSet();
    const auto outSet = layouts.getMainOutputChannelSet();
    if (inSet != outSet)
        return false;
    return (inSet == juce::AudioChannelSet::stereo() || inSet == juce::AudioChannelSet::mono());
}

void SyncroomObsBridgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // 1. 本線のオーディオ（Rubix44 → ヘッドホン / M-Track Duo / ColorSing）は
    //    一切加工・遅延させず完全パススルーでそのまま通す。
    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || totalIn <= 0)
        return;

    // 2. パラメータ読み取り（atomic load のみ）
    const float gainDb = (gainDbParam_ != nullptr) ? gainDbParam_->load (std::memory_order_relaxed) : 0.0f;
    const bool  muted  = (muteParam_   != nullptr) ? (muteParam_->load (std::memory_order_relaxed) >= 0.5f) : false;
    sender_.setGainLinear (juce::Decibels::decibelsToGain (gainDb, -96.0f));
    sender_.setMuted (muted);

    // 3. ロックフリーFIFOへコピーしてバックグラウンドWASAPI送信スレッドへ渡す
    const float* inL = buffer.getReadPointer (0);
    const float* inR = (totalIn > 1) ? buffer.getReadPointer (1) : inL;
    sender_.pushSamples (inL, inR, numSamples);
}

void SyncroomObsBridgeProcessor::processBlockBypassed (juce::AudioBuffer<float>& /*buffer*/, juce::MidiBuffer&)
{
    // REAPER 上でプラグイン自体をバイパスした場合は OBS への送信だけ止める（本線はそのまま通過）
}

void SyncroomObsBridgeProcessor::setTargetEndpointId (const juce::String& endpointId)
{
    selectedEndpointId_ = endpointId;
    sender_.requestDeviceChange (selectedEndpointId_.toWideCharPointer());
}

juce::AudioProcessorEditor* SyncroomObsBridgeProcessor::createEditor()
{
    return new SyncroomObsBridgeEditor (*this);
}

void SyncroomObsBridgeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("targetEndpointId", selectedEndpointId_, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void SyncroomObsBridgeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto vt = juce::ValueTree::fromXml (*xml);
            selectedEndpointId_ = vt.getProperty ("targetEndpointId", "").toString();
            apvts.replaceState (vt);
            sender_.requestDeviceChange (selectedEndpointId_.toWideCharPointer());
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SyncroomObsBridgeProcessor();
}
