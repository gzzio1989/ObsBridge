#pragma once
// =============================================================================
// PluginEditor.h
// Windows / macOS 両対応の日本語ステータス・音量調整画面
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class SyncroomObsBridgeEditor final : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit SyncroomObsBridgeEditor (SyncroomObsBridgeProcessor&);
    ~SyncroomObsBridgeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static const char* defaultJapaneseFontName() noexcept
    {
#if JUCE_MAC
        return "Hiragino Sans";
#else
        return "Yu Gothic UI";
#endif
    }

    void timerCallback() override;
    void rebuildDeviceCombo();

    SyncroomObsBridgeProcessor& proc_;
    juce::Font jpFontBold_  { juce::FontOptions (defaultJapaneseFontName(), 16.0f, juce::Font::bold) };
    juce::Font jpFont_      { juce::FontOptions (defaultJapaneseFontName(), 13.5f, juce::Font::plain) };
    juce::Font jpFontSmall_ { juce::FontOptions (defaultJapaneseFontName(), 12.0f, juce::Font::plain) };

    juce::ComboBox   deviceCombo_;
    juce::TextButton reconnectBtn_ { juce::String::fromUTF8 ("再接続") };
    juce::Slider     gainSlider_;
    juce::ToggleButton muteToggle_ { juce::String::fromUTF8 ("OBS送信のみミュート（ヘッドホン音は維持）") };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttach_;

    juce::StringArray endpointIds_;
    float dispPeakL_ = 0.0f;
    float dispPeakR_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncroomObsBridgeEditor)
};
