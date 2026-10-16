#pragma once
#include "PluginProcessor.h"
#include <deque>

class AetherLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float, float,
                          const juce::Slider::SliderStyle, juce::Slider&) override
    {
        auto b = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(width), static_cast<float>(height)).reduced(2.0f, 6.0f);
        float trackH = 6.0f;
        float trackY = b.getCentreY() - trackH * 0.5f;

        g.setColour(juce::Colour(0xff161f30));
        g.fillRoundedRectangle(b.getX(), trackY, b.getWidth(), trackH, 3.0f);

        float fillW = juce::jlimit(0.0f, b.getWidth(), sliderPos - b.getX());
        juce::ColourGradient grad(juce::Colour(0xff06b6d4), b.getX(), trackY,
                                  juce::Colour(0xff6366f1), sliderPos, trackY, false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(b.getX(), trackY, fillW, trackH, 3.0f);

        g.setColour(juce::Colours::white);
        g.fillEllipse(sliderPos - 6.0f, b.getCentreY() - 6.0f, 12.0f, 12.0f);
    }
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

    std::deque<juce::Point<float>> cursorTrail;
    std::array<float, 256> scopeData {};

    juce::ComboBox rootKeySelector, scaleSelector;
    juce::TextButton loadOnnxButton { "LOAD .ONNX" };
    juce::TextButton generateButton { "GENERATE 4-BAR MASTER (WAV + MIDI)" };
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::Slider densitySlider, tensionSlider, contourSlider, spaceSlider;
    juce::Label  densityLabel, tensionLabel, contourLabel, spaceLabel;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<ComboAttachment>  rootKeyAttachment, scaleAttachment;
    std::unique_ptr<SliderAttachment> densityAttachment, tensionAttachment, contourAttachment, spaceAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AetherMatrixEditor)
};
