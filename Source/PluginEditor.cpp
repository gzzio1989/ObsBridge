#include "PluginEditor.h"

SyncroomObsBridgeEditor::SyncroomObsBridgeEditor (SyncroomObsBridgeProcessor& p)
    : AudioProcessorEditor (&p), proc_ (p)
{
    setSize (530, 252);

    getLookAndFeel().setDefaultSansSerifTypefaceName (defaultJapaneseFontName());

    addAndMakeVisible (deviceCombo_);
    deviceCombo_.onChange = [this]
    {
        const int idx = deviceCombo_.getSelectedItemIndex();
        if (idx >= 0 && idx < endpointIds_.size())
            proc_.setTargetEndpointId (endpointIds_[idx]);
    };

    addAndMakeVisible (reconnectBtn_);
    reconnectBtn_.onClick = [this]
    {
        rebuildDeviceCombo();
        proc_.reconnectNow();
    };

    addAndMakeVisible (gainSlider_);
    gainSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider_.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 24);
    gainSlider_.setTextValueSuffix (" dB");
    gainSlider_.setDoubleClickReturnValue (true, 0.0);

    addAndMakeVisible (muteToggle_);

    gainAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc_.apvts, "obs_gain_db", gainSlider_);
    muteAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc_.apvts, "obs_mute", muteToggle_);

    rebuildDeviceCombo();
    startTimerHz (30);
}

SyncroomObsBridgeEditor::~SyncroomObsBridgeEditor()
{
    stopTimer();
}

void SyncroomObsBridgeEditor::rebuildDeviceCombo()
{
    const auto currentTarget = proc_.getTargetEndpointId();
    deviceCombo_.clear (juce::dontSendNotification);
    endpointIds_.clear();

    endpointIds_.add ("");
#if JUCE_MAC
    deviceCombo_.addItem (juce::String::fromUTF8 ("自動選択（BlackHole / SYNCROOM / 仮想デバイス優先）"), 1);
#else
    deviceCombo_.addItem (juce::String::fromUTF8 ("自動選択（Yamaha SYNCROOM / VB-CABLE 優先）"), 1);
#endif

    const auto endpoints = sobr::WasapiLoopbackSender::enumerateRenderEndpoints();
    int selectedIndex = 0;
    int itemId = 2;
    for (const auto& ep : endpoints)
    {
        const juce::String id (ep.id.c_str());
        const juce::String name (ep.name.c_str());
        endpointIds_.add (id);
        deviceCombo_.addItem (name, itemId);
        if (currentTarget.isNotEmpty() && currentTarget == id)
            selectedIndex = itemId - 1;
        ++itemId;
    }

    deviceCombo_.setSelectedItemIndex (selectedIndex, juce::dontSendNotification);
}

void SyncroomObsBridgeEditor::timerCallback()
{
    auto& s = proc_.getSender();
    dispPeakL_ = juce::jlimit (0.0f, 1.0f, s.getPeakLeft());
    dispPeakR_ = juce::jlimit (0.0f, 1.0f, s.getPeakRight());
    s.decayMeterPeaks (0.84f);
    repaint();
}

void SyncroomObsBridgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff161922));

    // ヘッダー背景
    g.setColour (juce::Colour (0xff202534));
    g.fillRoundedRectangle (12.0f, 10.0f, (float) getWidth() - 24.0f, 54.0f, 8.0f);

    g.setColour (juce::Colours::white);
    g.setFont (jpFontBold_);
    g.drawText (juce::String::fromUTF8 ("Syncroom OBS Bridge  v1.0.0  —  OBS音声送信ブリッジ"),
                24, 15, getWidth() - 48, 22, juce::Justification::centredLeft);

    g.setColour (juce::Colour (0xff9aa6c0));
    g.setFont (jpFontSmall_);
    g.drawText (juce::String::fromUTF8 ("配置場所: Masterトラックの syncroom_vst_bridge2 の「下」に挿入（自分＋他メンバー全員の音を送信）"),
                24, 38, getWidth() - 48, 18, juce::Justification::centredLeft);

    // ステータスカード
    const auto& sender = proc_.getSender();
    const bool connected = sender.isConnected();
    const juce::String devName (sender.getActiveDeviceName().c_str());
    const juce::String statusMsg (sender.getStatusMessage().c_str());

    g.setColour (juce::Colour (0xff1d2230));
    g.fillRoundedRectangle (12.0f, 72.0f, (float) getWidth() - 24.0f, 44.0f, 8.0f);

    g.setColour (connected ? juce::Colour (0xff2ee59d) : juce::Colour (0xffff6b6b));
    g.fillEllipse (24.0f, 87.0f, 12.0f, 12.0f);

    g.setColour (juce::Colours::white);
    g.setFont (jpFont_);
    juce::String line1 = connected
        ? (juce::String::fromUTF8 ("送信中: ") + (devName.isNotEmpty() ? devName : juce::String ("Virtual Audio Device")))
        : statusMsg;
    g.drawText (line1, 44, 76, getWidth() - 64, 20, juce::Justification::centredLeft);

    g.setColour (juce::Colour (0xff9aa6c0));
    g.setFont (jpFontSmall_);
    juce::String rateInfo = juce::String::formatted (
        "DAW %.0f Hz  ->  Driver %.0f Hz  |  本線モニター追加遅延: 0.0 ms",
        sender.getHostSampleRate(),
        sender.getDeviceSampleRate());
    g.drawText (rateInfo, 44, 95, getWidth() - 64, 16, juce::Justification::centredLeft);

    // ラベル類
    g.setColour (juce::Colour (0xffcbd5e1));
    g.setFont (jpFont_);
    g.drawText (juce::String::fromUTF8 ("送信先:"), 16, 126, 70, 26, juce::Justification::centredLeft);
    g.drawText (juce::String::fromUTF8 ("送信音量:"), 16, 160, 70, 26, juce::Justification::centredLeft);

    // L/R レベルメーター
    g.setFont (jpFontSmall_);
    g.setColour (juce::Colour (0xff9aa6c0));
    g.drawText ("OBS L/R", 16, 220, 70, 20, juce::Justification::centredLeft);

    const float meterX = 88.0f;
    const float meterW = (float) getWidth() - meterX - 20.0f;
    g.setColour (juce::Colour (0xff0f1219));
    g.fillRoundedRectangle (meterX, 221.0f, meterW, 8.0f, 3.0f);
    g.fillRoundedRectangle (meterX, 232.0f, meterW, 8.0f, 3.0f);

    g.setColour (juce::Colour (0xff2ee59d));
    g.fillRoundedRectangle (meterX, 221.0f, meterW * dispPeakL_, 8.0f, 3.0f);
    g.fillRoundedRectangle (meterX, 232.0f, meterW * dispPeakR_, 8.0f, 3.0f);
}

void SyncroomObsBridgeEditor::resized()
{
    deviceCombo_.setBounds (88, 126, getWidth() - 88 - 92, 26);
    reconnectBtn_.setBounds (getWidth() - 84, 126, 68, 26);
    gainSlider_.setBounds (88, 160, getWidth() - 104, 26);
    muteToggle_.setBounds (88, 190, getWidth() - 104, 24);
}
