#include "PluginEditor.h"

namespace
{
const juce::Colour ink(0xff0d1118), metal(0xff404451), light(0xffedf0f8);
const std::array<juce::Colour, 4> accents { juce::Colour(0xff9caeff), juce::Colour(0xffffb15b),
    juce::Colour(0xffea8cff), juce::Colour(0xffbb9bff) };
void panel(juce::Graphics& g, juce::Rectangle<float> b, float radius)
{
    for (int i = 8; i > 0; --i)
    {
        g.setColour(juce::Colours::black.withAlpha(0.035f * static_cast<float>(9 - i)));
        g.fillRoundedRectangle(b.expanded(static_cast<float>(i) * 0.45f).translated(0, static_cast<float>(i)), radius);
    }
    g.setColour(juce::Colour(0xff10131a)); g.fillRoundedRectangle(b, radius);
    juce::ColourGradient bevel(juce::Colour(0xffa0a6b5), b.getTopLeft(), juce::Colour(0xff090c11), b.getBottomLeft(), false);
    bevel.addColour(0.04, juce::Colour(0xff454c5a));
    bevel.addColour(0.50, juce::Colour(0xff333946));
    bevel.addColour(0.95, juce::Colour(0xff1d222b));
    g.setGradientFill(bevel); g.fillRoundedRectangle(b.reduced(1), radius - 1);
    juce::ColourGradient face(juce::Colour(0xff454b59), b.getTopLeft(), juce::Colour(0xff252a34), b.getBottomLeft(), false);
    face.addColour(0.35, juce::Colour(0xff3d4350));
    g.setGradientFill(face); g.fillRoundedRectangle(b.reduced(3.5f), radius - 3);
    g.setColour(juce::Colour(0xff181c24)); g.drawRoundedRectangle(b.reduced(3.5f), radius - 3, 0.8f);
    g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.22f), b.getTopLeft(),
        juce::Colours::transparentWhite, b.getCentre(), false));
    g.drawRoundedRectangle(b.reduced(1.8f), radius - 2, 0.9f);
}
}

juce::Slider::SliderLayout AetherLookAndFeel::getSliderLayout(juce::Slider& slider)
{
    if (!slider.isRotary()) return juce::LookAndFeel_V4::getSliderLayout(slider);
    juce::Slider::SliderLayout layout;
    const bool compact = static_cast<bool>(slider.getProperties()["compact"]);
    layout.sliderBounds = compact ? juce::Rectangle<int>(0, 0, 80, 80) : juce::Rectangle<int>(0, 0, 150, 150);
    layout.textBoxBounds = compact ? juce::Rectangle<int>(127, 12, 110, 44) : juce::Rectangle<int>(152, 47, 95, 44);
    return layout;
}

juce::Label* AetherLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(juce::FontOptions(29.0f, juce::Font::bold));
    label->setJustificationType(juce::Justification::centred);
    return label;
}

void AetherLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                        float pos, float start, float end, juce::Slider& slider)
{
    const auto b = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(width), static_cast<float>(height)).reduced(12);
    const float radius = juce::jmin(b.getWidth(), b.getHeight()) / 2;
    const auto c = b.getCentre();
    auto knob = juce::Rectangle<float>(radius * 2, radius * 2).withCentre(c);
    // Concentric machining rings, a soft cast shadow and a directional face highlight.
    for (int i = 10; i > 0; --i)
    {
        g.setColour(juce::Colours::black.withAlpha(0.04f));
        g.fillEllipse(knob.expanded(static_cast<float>(i) * 0.55f).translated(1, static_cast<float>(i) * 0.8f));
    }
    g.setColour(juce::Colour(0xff090c12)); g.fillEllipse(knob.expanded(4));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffb0b7c6), knob.getTopLeft(),
        juce::Colour(0xff171d27), knob.getBottomRight(), false));
    g.fillEllipse(knob.expanded(1.5f));
    g.setColour(juce::Colour(0xff11151d)); g.fillEllipse(knob.reduced(0.8f));
    juce::ColourGradient face(juce::Colour(0xff4d5462), c.x - radius, c.y - radius,
        juce::Colour(0xff252a33), c.x + radius, c.y + radius, false);
    face.addColour(0.55, juce::Colour(0xff393f4b));
    g.setGradientFill(face); g.fillEllipse(knob.reduced(2.3f));
    g.setColour(juce::Colour(0xff626a79)); g.drawEllipse(knob.reduced(3.1f), 0.6f);
    const auto accent = slider.findColour(juce::Slider::rotarySliderFillColourId);
    for (int i = 0; i <= 6; ++i)
    {
        const float tickAngle = start + static_cast<float>(i) / 6 * (end - start);
        const juce::Point<float> direction(std::sin(tickAngle), -std::cos(tickAngle));
        g.setColour(juce::Colour(0xffb1b6c4).withAlpha(i == 3 ? 0.8f : 0.35f));
        g.drawLine({c + direction * (radius + 9), c + direction * (radius + 11)}, 1.5f);
    }
    const float angle = start + pos * (end - start);
    juce::Path needle; needle.addRoundedRectangle(-2.1f, -radius * 0.78f, 4.2f, radius * 0.40f, 2.1f);
    const auto transform = juce::AffineTransform::rotation(angle).translated(c.x, c.y);
    g.setColour(accent.withAlpha(0.15f)); g.strokePath(needle, juce::PathStrokeType(5.5f), transform);
    g.setColour(accent); g.fillPath(needle, transform);

}

void AetherLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
    float pos, float, float, const juce::Slider::SliderStyle, juce::Slider& slider)
{
    const float cx = static_cast<float>(x) + static_cast<float>(width) / 2;
    auto b = juce::Rectangle<float>(cx - 10, static_cast<float>(y), 20, static_cast<float>(height));
    g.setColour(ink); g.fillRoundedRectangle(b, 10);
    g.setColour(juce::Colour(0xff72798b)); g.drawRoundedRectangle(b, 10, 1);
    g.setColour(slider.findColour(juce::Slider::trackColourId).withAlpha(0.65f));
    g.fillRoundedRectangle(cx - 2, pos, 4, b.getBottom() - pos, 2);
    panel(g, {cx - 18, pos - 8, 36, 16}, 7);
    g.setColour(light); g.drawLine(cx - 9, pos, cx + 9, pos, 2);
}

void AetherLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
    int, int, int, int, juce::ComboBox&)
{
    const auto b = juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height)).reduced(0.8f);
    g.setColour(juce::Colour(0xff171d27)); g.fillRoundedRectangle(b, 18);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff343b49), b.getTopLeft(),
        juce::Colour(0xff424957), b.getBottomLeft(), false));
    g.fillRoundedRectangle(b.reduced(2), 16);
    g.setColour(juce::Colour(0xff7e8699)); g.drawRoundedRectangle(b, 18, 0.8f);
    const float right = static_cast<float>(width) - 18;
    const float mid = static_cast<float>(height) * 0.5f;
    juce::Path arrow; arrow.startNewSubPath(right - 5, mid - 2); arrow.lineTo(right, mid + 3); arrow.lineTo(right + 5, mid - 2);
    g.setColour(light.withAlpha(0.75f)); g.strokePath(arrow, juce::PathStrokeType(1.5f));
}

void AetherLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
    const juce::Colour& colour, bool hover, bool down)
{
    auto b = button.getLocalBounds().toFloat().reduced(2);
    g.setGradientFill(juce::ColourGradient(colour.brighter(hover ? 0.25f : 0.1f), b.getTopLeft(),
        colour.darker(down ? 0.5f : 0.3f), b.getBottomLeft(), false));
    g.fillRoundedRectangle(b, 12);
    g.setColour(colour.brighter(0.5f)); g.drawRoundedRectangle(b, 12, 1);
}

AetherMatrixEditor::AetherMatrixEditor(AetherMatrixProcessor& p)
    : AudioProcessorEditor(&p), engineProcessor(p)
{
    setLookAndFeel(&customLookAndFeel);
    setSize(1160, 700);
    auto styleCombo = [this](juce::ComboBox& box, const juce::StringArray& items)
    {
        box.addItemList(items, 1);
        box.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff30343f));
        box.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff777d8e));
        box.setColour(juce::ComboBox::textColourId, light);
        addAndMakeVisible(box);
    };
    styleCombo(rootKeySelector, { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    styleCombo(scaleSelector, { "Natural Minor", "Dorian", "Phrygian Dom", "Major Lush" });
    rootKeySelector.setTitle("Root key"); scaleSelector.setTitle("Scale");
    rootKeyAttachment = std::make_unique<ComboAttachment>(engineProcessor.apvts, "rootKey", rootKeySelector);
    scaleAttachment = std::make_unique<ComboAttachment>(engineProcessor.apvts, "scaleType", scaleSelector);
    loadOnnxButton.setColour(juce::TextButton::buttonColourId, metal);
    loadOnnxButton.setColour(juce::TextButton::textColourOffId, light);
    loadOnnxButton.onClick = [this] { promptForOnnxFile(); };
    addAndMakeVisible(loadOnnxButton);
    generateButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff555069));
    generateButton.setColour(juce::TextButton::textColourOffId, light);
    generateButton.onClick = [this] { engineProcessor.triggerMasterGeneration(); };
    addAndMakeVisible(generateButton);
    auto setup = [this](juce::Slider& s, juce::Label& l, const juce::String& title, int i)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        s.getProperties().set("compact", i >= 2);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 100, 44);
        s.setColour(juce::Slider::rotarySliderFillColourId, accents[static_cast<size_t>(i)]);
        s.setColour(juce::Slider::textBoxTextColourId, accents[static_cast<size_t>(i)]);
        s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        s.textFromValueFunction = [](double value) { return juce::String(juce::roundToInt(value * 100)) + "%"; };
        s.valueFromTextFunction = [](const juce::String& value) { return value.getDoubleValue() / 100.0; };
        s.setTitle(title); addAndMakeVisible(s);
        l.setText(title, juce::dontSendNotification);
        l.setFont(juce::FontOptions(23.0f, juce::Font::bold));
        l.setColour(juce::Label::textColourId, light); addAndMakeVisible(l);
    };
    setup(densitySlider, densityLabel, "DENSITY", 0);
    setup(tensionSlider, tensionLabel, "TENSION", 1);
    setup(contourSlider, contourLabel, "CONTOUR", 2);
    setup(spaceSlider, spaceLabel, "SPACE", 3);
    densityAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "density", densitySlider);
    tensionAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "tension", tensionSlider);
    contourAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "contour", contourSlider);
    spaceAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "space", spaceSlider);
    // Attachments install the parameter formatter; show the normalised controls as percentages afterward.
    for (auto* slider : { &densitySlider, &tensionSlider, &contourSlider, &spaceSlider })
    {
        slider->textFromValueFunction = [](double value) { return juce::String(juce::roundToInt(value * 100)) + "%"; };
        slider->valueFromTextFunction = [](const juce::String& value) { return juce::jlimit(0.0, 1.0, value.getDoubleValue() / 100.0); };
        slider->updateText();
    }
    for (auto* slider : { &matrixXSlider, &matrixYSlider })
    {
        slider->setSliderStyle(juce::Slider::LinearVertical);
        slider->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider->setColour(juce::Slider::trackColourId, accents[0]);
        addAndMakeVisible(*slider);
    }
    matrixXSlider.setTitle("Style matrix X"); matrixYSlider.setTitle("Style matrix Y");
    matrixXAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "matrixX", matrixXSlider);
    matrixYAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "matrixY", matrixYSlider);
    startTimerHz(30);
}

AetherMatrixEditor::~AetherMatrixEditor()
{
    if (isDraggingMatrix)
    {
        if (auto* px = engineProcessor.apvts.getParameter("matrixX")) px->endChangeGesture();
        if (auto* py = engineProcessor.apvts.getParameter("matrixY")) py->endChangeGesture();
    }
    setLookAndFeel(nullptr);
}

void AetherMatrixEditor::promptForOnnxFile()
{
    fileChooser = std::make_unique<juce::FileChooser>("Select compatible ONNX decoder", juce::File{}, "*.onnx");
    const juce::Component::SafePointer<AetherMatrixEditor> safeThis(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safeThis](const juce::FileChooser& fc)
                             {
                                 auto file = fc.getResult();
                                 if (safeThis != nullptr && file.existsAsFile())
                                     safeThis->engineProcessor.loadOnnxModel(file);
                             });
}

bool AetherMatrixEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".onnx"))
            return true;
    return false;
}

void AetherMatrixEditor::filesDropped(const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
    {
        if (f.endsWithIgnoreCase(".onnx"))
        {
            engineProcessor.loadOnnxModel(juce::File(f));
            break;
        }
    }
}

void AetherMatrixEditor::updateMatrixFromMouse(juce::Point<float> pos)
{
    float nx = juce::jlimit(0.0f, 1.0f, (pos.x - matrixBounds.getX()) / matrixBounds.getWidth());
    float ny = juce::jlimit(0.0f, 1.0f, 1.0f - (pos.y - matrixBounds.getY()) / matrixBounds.getHeight());

    if (auto* px = engineProcessor.apvts.getParameter("matrixX")) px->setValueNotifyingHost(nx);
    if (auto* py = engineProcessor.apvts.getParameter("matrixY")) py->setValueNotifyingHost(ny);
}

void AetherMatrixEditor::mouseDown(const juce::MouseEvent& e)
{
    if (engineProcessor.hasGeneratedFiles())
    {
        if (dragWavPillBounds.contains(e.position))
        {
            auto wav = engineProcessor.getGeneratedWavFile();
            if (wav.existsAsFile())
                pendingDragFile = wav;
            return;
        }
        if (dragMidiPillBounds.contains(e.position))
        {
            auto mid = engineProcessor.getGeneratedMidiFile();
            if (mid.existsAsFile())
                pendingDragFile = mid;
            return;
        }
    }

    if (matrixBounds.contains(e.position))
    {
        isDraggingMatrix = true;
        if (auto* px = engineProcessor.apvts.getParameter("matrixX")) px->beginChangeGesture();
        if (auto* py = engineProcessor.apvts.getParameter("matrixY")) py->beginChangeGesture();
        updateMatrixFromMouse(e.position);
    }
}

void AetherMatrixEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (pendingDragFile.existsAsFile() && e.getDistanceFromDragStart() >= 4)
    {
        const auto file = pendingDragFile;
        pendingDragFile = juce::File{};
        performExternalDragDropOfFiles({ file.getFullPathName() }, false, this);
        return;
    }
    if (isDraggingMatrix)
        updateMatrixFromMouse(e.position);
}

void AetherMatrixEditor::mouseUp(const juce::MouseEvent&)
{
    pendingDragFile = juce::File{};
    if (isDraggingMatrix)
    {
        isDraggingMatrix = false;
        if (auto* px = engineProcessor.apvts.getParameter("matrixX")) px->endChangeGesture();
        if (auto* py = engineProcessor.apvts.getParameter("matrixY")) py->endChangeGesture();
    }
}

void AetherMatrixEditor::timerCallback()
{
    generateButton.setEnabled(!engineProcessor.isGenerating());
    if (!isShowing())
        return;
    engineProcessor.copyVisualScopeData(scopeData);
    repaint();
}

void AetherMatrixEditor::paint(juce::Graphics& g)
{
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff414657), 0, 0, juce::Colour(0xff282d39), 0, static_cast<float>(getHeight()), false));
    g.fillAll();
    panel(g, getLocalBounds().toFloat().reduced(8, 80).withBottom(static_cast<float>(getHeight() - 10)), 38);
    g.setColour(light); g.setFont(juce::FontOptions(28.0f, juce::Font::bold));
    g.drawText("AETHER MATRIX", 32, 22, 310, 40, juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(18.0f)); g.setColour(juce::Colour(0xffc0c5d4));
    g.drawText(juce::String(juce::roundToInt(engineProcessor.currentHostBpm.load())) + " BPM", 1000, 24, 125, 36, juce::Justification::centredRight);
    g.setGradientFill(juce::ColourGradient(ink, matrixBounds.getTopLeft(), juce::Colour(0xff171824), matrixBounds.getBottomLeft(), false));
    g.fillRoundedRectangle(matrixBounds, 28);
    g.setColour(juce::Colour(0xff9297a7)); g.drawRoundedRectangle(matrixBounds.expanded(1), 28, 1.5f);
    g.setColour(juce::Colour(0xff080a0f)); g.drawRoundedRectangle(matrixBounds.reduced(2), 27, 2);
    const auto w = engineProcessor.getStyleWeights();
    static const std::array<const char*, 5> names { "HIP-HOP", "CLASSICAL", "POP", "ELECTRONIC", "CINEMATIC" };
    static const std::array<juce::Colour, 5> colours { juce::Colour(0xff808aff), juce::Colour(0xffb793fa),
        juce::Colour(0xffe58eea), juce::Colour(0xffffaa66), juce::Colour(0xffd778a7) };
    const float bottom = matrixBounds.getBottom() - 24;
    for (size_t i = 0; i < w.size(); ++i)
    {
        const float centre = matrixBounds.getX() + matrixBounds.getWidth() * (0.12f + static_cast<float>(i) * 0.19f);
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        g.drawVerticalLine(juce::roundToInt(centre), matrixBounds.getY() + 66, bottom);
        juce::Path lobe; lobe.startNewSubPath(matrixBounds.getX() + 3, bottom);
        for (int n = 0; n <= 160; ++n)
        {
            const float px = matrixBounds.getX() + 3 + (matrixBounds.getWidth() - 6) * static_cast<float>(n) / 160;
            const float d = (px - centre) / (matrixBounds.getWidth() * 0.13f);
            const float height = (12 + w[i] * 172) * std::exp(-d * d * 2);
            lobe.lineTo(px, bottom - height);
        }
        lobe.closeSubPath();
        g.setGradientFill(juce::ColourGradient(colours[i].withAlpha(0.95f), centre, bottom,
            colours[i].withAlpha(0.50f), centre, bottom - 210, false));
        g.fillPath(lobe); g.setColour(colours[i]); g.strokePath(lobe, juce::PathStrokeType(1.2f));
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold)); g.setColour(light);
        g.drawText(names[i], juce::Rectangle<float>(centre - 82, matrixBounds.getY() + 20, 164, 24), juce::Justification::centred, false);
        g.setFont(juce::FontOptions(18.0f)); g.setColour(colours[i]);
        g.drawText(juce::String(juce::roundToInt(w[i] * 100)) + "%", juce::Rectangle<float>(centre - 60, matrixBounds.getY() + 46, 120, 25), juce::Justification::centred, false);
    }
    juce::Path wave;
    for (size_t i = 0; i < scopeData.size(); ++i)
    {
        const float px = matrixBounds.getX() + 4 + static_cast<float>(i) / 255 * (matrixBounds.getWidth() - 8);
        const float py = bottom - 28 - juce::jlimit(-1.0f, 1.0f, scopeData[i]) * 65;
        if (i == 0) wave.startNewSubPath(px, py); else wave.lineTo(px, py);
    }
    g.setColour(light.withAlpha(0.45f)); g.strokePath(wave, juce::PathStrokeType(1.3f));
    const float x = engineProcessor.apvts.getRawParameterValue("matrixX")->load();
    const float y = engineProcessor.apvts.getRawParameterValue("matrixY")->load();
    const auto cursor = juce::Point<float>(matrixBounds.getX() + x * matrixBounds.getWidth(), matrixBounds.getBottom() - y * matrixBounds.getHeight());
    g.setColour(light.withAlpha(0.7f));
    g.drawLine(cursor.x - 10, cursor.y, cursor.x + 10, cursor.y, 2);
    g.drawLine(cursor.x, cursor.y - 10, cursor.x, cursor.y + 10, 2);
    g.setFont(juce::FontOptions(16.0f)); g.setColour(juce::Colour(0xffc0c5d4));
    g.drawText("Drag to blend styles", matrixBounds.withHeight(26).withY(bottom), juce::Justification::centred, false);
    // Recessed readouts sit beside the knobs, as on a physical desktop instrument.
    for (size_t i = 0; i < moduleBounds.size(); ++i)
    {
        const auto b = moduleBounds[i]; panel(g, b, 27);
        const bool compact = i >= 2;
        const auto display = compact ? juce::Rectangle<float>(b.getX() + 142, b.getY() + 39, 130, 66)
                                     : juce::Rectangle<float>(b.getX() + 166, b.getY() + 83, 101, 82);
        g.setColour(juce::Colour(0xff0a0e15)); g.fillRoundedRectangle(display, 18);
        g.setGradientFill(juce::ColourGradient(accents[i].withAlpha(0.24f), display.getBottomLeft(),
            juce::Colours::transparentBlack, display.getTopLeft(), false));
        g.fillRoundedRectangle(display.reduced(3), 16);
        g.setColour(juce::Colour(0xff5d6574)); g.drawRoundedRectangle(display, 18, 0.9f);
        g.setColour(accents[i].withAlpha(0.45f));
        g.drawLine(display.getX() + 20, display.getBottom() - 5, display.getRight() - 20, display.getBottom() - 5, 2);
        g.setFont(juce::FontOptions(12.5f, juce::Font::bold)); g.setColour(accents[i]);
        g.drawText("AMOUNT", display.withHeight(19).translated(0, 4), juce::Justification::centred, false);
        const std::array<const char*, 4> descriptions { "Rhythmic movement", "Harmonic colour", "Note articulation", "Acoustic depth" };
        g.setFont(juce::FontOptions(16.0f)); g.setColour(juce::Colour(0xffbbc0cc));
        const auto caption = compact ? juce::Rectangle<float>(b.getX() + 287, b.getY() + 43, 113, 56)
                                     : juce::Rectangle<float>(b.getX() + 18, b.getBottom() - 36, b.getWidth() - 36, 27);
        g.drawFittedText(descriptions[i], caption.toNearestInt(), juce::Justification::centred, 2);
    }
    g.setFont(juce::FontOptions(16.0f, juce::Font::bold)); g.setColour(light);
    g.drawText("X", 27, 612, 40, 28, juce::Justification::centred);
    g.drawText("Y", 1093, 612, 40, 28, juce::Justification::centred);
    if (engineProcessor.isGenerating())
    {
        auto overlay = matrixBounds.reduced(150, 100);
        g.setColour(ink.withAlpha(0.95f)); g.fillRoundedRectangle(overlay, 18);
        const float progress = juce::jlimit(0.0f, 1.0f, engineProcessor.getGenerationProgress());
        g.setColour(light); g.setFont(juce::FontOptions(22.0f));
        g.drawText("Rendering WAV + MIDI  " + juce::String(juce::roundToInt(progress * 100)) + "%", overlay, juce::Justification::centred, false);
        const auto track = overlay.reduced(25).removeFromBottom(5);
        g.setColour(accents[3]); g.fillRoundedRectangle(track.withWidth(track.getWidth() * progress), 2);
    }
    if (engineProcessor.hasGeneratedFiles())
    {
        for (const auto& b : {dragWavPillBounds, dragMidiPillBounds}) panel(g, b, 12);
        g.setColour(light); g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        g.drawText("Drag WAV", dragWavPillBounds, juce::Justification::centred, false);
        g.drawText("Drag MIDI", dragMidiPillBounds, juce::Justification::centred, false);
    }
    else
    {
        g.setColour(light); g.setFont(juce::FontOptions(17.0f));
        g.drawText(engineProcessor.getLoadedModelNameSafe(), dragWavPillBounds.getUnion(dragMidiPillBounds), juce::Justification::centred, false);
    }
}

void AetherMatrixEditor::resized()
{
    rootKeySelector.setBounds(420, 25, 90, 36);
    scaleSelector.setBounds(520, 25, 230, 36);
    loadOnnxButton.setBounds(770, 23, 160, 40);
    matrixBounds = {32, 102, 1096, 270};
    matrixXSlider.setBounds(27, 407, 40, 200);
    matrixYSlider.setBounds(1093, 407, 40, 200);
    moduleBounds = { juce::Rectangle<float>(82, 392, 277, 242), juce::Rectangle<float>(369, 392, 277, 242),
                     juce::Rectangle<float>(658, 392, 416, 116), juce::Rectangle<float>(658, 518, 416, 116) };
    const std::array<juce::Slider*, 4> sliders { &densitySlider, &tensionSlider, &contourSlider, &spaceSlider };
    const std::array<juce::Label*, 4> labels { &densityLabel, &tensionLabel, &contourLabel, &spaceLabel };
    for (size_t i = 0; i < sliders.size(); ++i)
    {
        auto b = moduleBounds[i].toNearestInt();
        labels[i]->setBounds(b.getX() + 20, b.getY() + 13, 240, 32);
        sliders[i]->setBounds(b.getX() + 18, b.getY() + (i >= 2 ? 36 : 55), 253, i >= 2 ? 91 : 150);
    }
    generateButton.setBounds(82, 651, 330, 36);
    dragWavPillBounds = {430, 652, 325, 34};
    dragMidiPillBounds = {770, 652, 304, 34};
}
