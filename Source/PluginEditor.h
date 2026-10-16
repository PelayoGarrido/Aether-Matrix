#pragma once
#include "PluginProcessor.h"


class AetherLookAndFeel : public juce::LookAndFeel_V4
{
public:
    juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;
    void drawRotarySlider(juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int, int, int, int, float, float, float,
                          const juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox(juce::Graphics&, int, int, bool, int, int, int, int, juce::ComboBox&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    juce::Font getLabelFont(juce::Label& label) override
    {
        if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr) return juce::FontOptions(29.0f, juce::Font::bold);
        return label.getFont();
    }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return juce::FontOptions(18.0f, juce::Font::bold); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::FontOptions(18.0f); }

};

class AetherMatrixEditor : public juce::AudioProcessorEditor,
                           public juce::DragAndDropContainer,
                           public juce::FileDragAndDropTarget,
                           private juce::Timer
{
public:
    explicit AetherMatrixEditor(AetherMatrixProcessor&);
    ~AetherMatrixEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void updateMatrixFromMouse(juce::Point<float> pos);
    void promptForOnnxFile();

    AetherMatrixProcessor& engineProcessor;
    AetherLookAndFeel customLookAndFeel;

    juce::Rectangle<float> matrixBounds;
    juce::Rectangle<float> dragWavPillBounds;
    juce::Rectangle<float> dragMidiPillBounds;
    bool isDraggingMatrix = false;
    juce::File pendingDragFile;

    std::array<juce::Rectangle<float>, 4> moduleBounds {};
    std::array<float, 256> scopeData {};

    juce::ComboBox rootKeySelector, scaleSelector;
    juce::TextButton loadOnnxButton { "Load model" };
    juce::TextButton generateButton { "Generate WAV + MIDI" };
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::Slider densitySlider, tensionSlider, contourSlider, spaceSlider, matrixXSlider, matrixYSlider;
    juce::Label  densityLabel, tensionLabel, contourLabel, spaceLabel;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<ComboAttachment>  rootKeyAttachment, scaleAttachment;
    std::unique_ptr<SliderAttachment> densityAttachment, tensionAttachment, contourAttachment, spaceAttachment, matrixXAttachment, matrixYAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AetherMatrixEditor)
};
