#include "PluginEditor.h"

AetherMatrixEditor::AetherMatrixEditor(AetherMatrixProcessor& p)
    : AudioProcessorEditor(&p), engineProcessor(p)
{
    setLookAndFeel(&customLookAndFeel);
    setSize(620, 740);

    auto styleCombo = [this](juce::ComboBox& box, const juce::StringArray& items)
    {
        box.addItemList(items, 1);
        box.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff111827));
        box.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff1f2937));
        box.setColour(juce::ComboBox::textColourId, juce::Colour(0xffe2e8f0));
        addAndMakeVisible(box);
    };

    styleCombo(rootKeySelector, { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    styleCombo(scaleSelector,   { "Natural Minor", "Dorian", "Phrygian Dom", "Major Lush" });

    rootKeyAttachment = std::make_unique<ComboAttachment>(engineProcessor.apvts, "rootKey", rootKeySelector);
    scaleAttachment   = std::make_unique<ComboAttachment>(engineProcessor.apvts, "scaleType", scaleSelector);

    loadOnnxButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff161f30));
    loadOnnxButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff38bdf8));
    loadOnnxButton.onClick = [this] { promptForOnnxFile(); };
    addAndMakeVisible(loadOnnxButton);

    generateButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff06b6d4));
    generateButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff05070c));
    generateButton.onClick = [this] { engineProcessor.triggerMasterGeneration(); };
    addAndMakeVisible(generateButton);

    auto setupSliderRow = [this](juce::Slider& s, juce::Label& l, const juce::String& title)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(s);

        l.setText(title, juce::dontSendNotification);
        l.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        l.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));
        addAndMakeVisible(l);
    };

    setupSliderRow(densitySlider, densityLabel, "RHYTHM DENSITY");
    setupSliderRow(tensionSlider, tensionLabel, "HARMONIC TENSION");
    setupSliderRow(contourSlider, contourLabel, "ARTICULATION");
    setupSliderRow(spaceSlider,   spaceLabel,   "ACOUSTIC SPACE");

    densityAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "density", densitySlider);
    tensionAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "tension", tensionSlider);
    contourAttachment = std::make_unique<SliderAttachment>(engineProcessor.apvts, "contour", contourSlider);
    spaceAttachment   = std::make_unique<SliderAttachment>(engineProcessor.apvts, "space",   spaceSlider);

    startTimerHz(30);
}

AetherMatrixEditor::~AetherMatrixEditor()
{
    setLookAndFeel(nullptr);
}

void AetherMatrixEditor::promptForOnnxFile()
{
    fileChooser = std::make_unique<juce::FileChooser>("Select RAVE ONNX Model", juce::File{}, "*.onnx");
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
    float x = engineProcessor.apvts.getRawParameterValue("matrixX")->load(std::memory_order_relaxed);
    float y = engineProcessor.apvts.getRawParameterValue("matrixY")->load(std::memory_order_relaxed);

    juce::Point<float> pt(matrixBounds.getX() + x * matrixBounds.getWidth(),
                          matrixBounds.getBottom() - y * matrixBounds.getHeight());
    cursorTrail.push_back(pt);
    if (cursorTrail.size() > 24)
        cursorTrail.pop_front();

    engineProcessor.copyVisualScopeData(scopeData);
    repaint();
}

void AetherMatrixEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff060911));

    juce::ColourGradient padGrad(juce::Colour(0xff0f172a), matrixBounds.getCentreX(), matrixBounds.getY(),
                                 juce::Colour(0xff070a12), matrixBounds.getCentreX(), matrixBounds.getBottom(), false);
    g.setGradientFill(padGrad);
    g.fillRoundedRectangle(matrixBounds, 16.0f);

    g.setColour(engineProcessor.onnxModelLoaded.load() ? juce::Colour(0xff10b981) : juce::Colour(0xff1e293b));
    g.drawRoundedRectangle(matrixBounds, 16.0f, 1.5f);

    struct StyleNode { const char* name; float nx; float ny; };
    static constexpr std::array<StyleNode, 5> nodes {{
        { "HIP-HOP",    0.15f, 0.85f },
        { "CLASSICAL",  0.85f, 0.85f },
        { "POP",        0.50f, 0.50f },
        { "ELECTRONIC", 0.15f, 0.15f },
        { "CINEMATIC",  0.85f, 0.15f }
    }};

    auto weights = engineProcessor.getStyleWeights();
    float x = engineProcessor.apvts.getRawParameterValue("matrixX")->load(std::memory_order_relaxed);
    float y = engineProcessor.apvts.getRawParameterValue("matrixY")->load(std::memory_order_relaxed);
    float cx = matrixBounds.getX() + x * matrixBounds.getWidth();
    float cy = matrixBounds.getBottom() - y * matrixBounds.getHeight();

    for (size_t i = 0; i < nodes.size(); ++i)
    {
        float nx = matrixBounds.getX() + nodes[i].nx * matrixBounds.getWidth();
        float ny = matrixBounds.getBottom() - nodes[i].ny * matrixBounds.getHeight();
        float w = weights[i];

        g.setColour(juce::Colour(0xff38bdf8).withAlpha(0.10f + w * 0.65f));
        g.drawLine(cx, cy, nx, ny, 1.0f + w * 3.5f);

        g.setColour(juce::Colour(0xff6366f1).withAlpha(0.25f + w * 0.75f));
        float r = 6.0f + w * 14.0f;
        g.fillEllipse(nx - r, ny - r, r * 2.0f, r * 2.0f);

        g.setColour(juce::Colours::white.withAlpha(0.6f + w * 0.4f));
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        juce::String label = juce::String(nodes[i].name) + " (" + juce::String(w * 100.0f, 0) + "%)";
        g.drawText(label, static_cast<int>(nx - 60.0f), static_cast<int>(ny - r - 20.0f),
                   120, 18, juce::Justification::centred, false);
    }

    juce::Path wavePath;
    float midY = matrixBounds.getCentreY();
    for (size_t i = 0; i < scopeData.size(); ++i)
    {
        float px = matrixBounds.getX() + (static_cast<float>(i) / 255.0f) * matrixBounds.getWidth();
        float py = midY - juce::jlimit(-1.0f, 1.0f, scopeData[i]) * (matrixBounds.getHeight() * 0.22f);
        if (i == 0) wavePath.startNewSubPath(px, py);
        else        wavePath.lineTo(px, py);
    }
    g.setColour(juce::Colour(0x4038bdf8));
    g.strokePath(wavePath, juce::PathStrokeType(1.8f));

    for (size_t i = 1; i < cursorTrail.size(); ++i)
    {
        float a = static_cast<float>(i) / static_cast<float>(cursorTrail.size());
        g.setColour(juce::Colour(0xff06b6d4).withAlpha(a * 0.45f));
        g.drawLine(cursorTrail[i - 1].x, cursorTrail[i - 1].y, cursorTrail[i].x, cursorTrail[i].y, a * 4.5f);
    }

    float rms = engineProcessor.currentRmsLevel.load(std::memory_order_relaxed);
    float halo = 15.0f + rms * 95.0f;
    g.setColour(juce::Colour(0x3306b6d4));
    g.fillEllipse(cx - halo, cy - halo, halo * 2.0f, halo * 2.0f);
    g.setColour(juce::Colours::white);
    g.fillEllipse(cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);

    if (engineProcessor.isGenerating())
    {
        const auto overlay = matrixBounds.withHeight(66.0f).withY(matrixBounds.getBottom() - 66.0f).reduced(12.0f);
        g.setColour(juce::Colour(0xee060911));
        g.fillRoundedRectangle(overlay, 8.0f);
        const float progress = juce::jlimit(0.0f, 1.0f, engineProcessor.getGenerationProgress());
        const auto track = overlay.reduced(12.0f).removeFromBottom(5.0f);
        g.setColour(juce::Colour(0xff1e293b));
        g.fillRoundedRectangle(track, 2.5f);
        g.setColour(juce::Colour(0xff38bdf8));
        g.fillRoundedRectangle(track.withWidth(track.getWidth() * progress), 2.5f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Rendering WAV + MIDI  " + juce::String(progress * 100.0f, 0) + "%",
                   overlay.withTrimmedBottom(10.0f), juce::Justification::centred, false);
    }
    if (engineProcessor.hasGeneratedFiles())
    {
        g.setColour(juce::Colour(0xff10b981));
        g.fillRoundedRectangle(dragWavPillBounds, 8.0f);
        g.fillRoundedRectangle(dragMidiPillBounds, 8.0f);

        g.setColour(juce::Colour(0xff05070c));
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText("DRAG .WAV TO DAW", dragWavPillBounds, juce::Justification::centred, false);
        g.drawText("DRAG .MID TO DAW", dragMidiPillBounds, juce::Justification::centred, false);
    }
    else
    {
        g.setColour(juce::Colour(0xff64748b));
        g.setFont(juce::FontOptions(11.5f));
        g.drawText(engineProcessor.getLoadedModelNameSafe(),
                   dragWavPillBounds.getUnion(dragMidiPillBounds),
                   juce::Justification::centred, false);
    }
}

void AetherMatrixEditor::resized()
{
    auto area = getLocalBounds().reduced(22);

    auto topBar = area.removeFromTop(30);
    rootKeySelector.setBounds(topBar.removeFromLeft(75));
    topBar.removeFromLeft(8);
    scaleSelector.setBounds(topBar.removeFromLeft(160));
    loadOnnxButton.setBounds(topBar.removeFromRight(110));

    area.removeFromTop(12);

    auto bottomBar = area.removeFromBottom(86);
    generateButton.setBounds(bottomBar.removeFromTop(42));
    bottomBar.removeFromTop(8);

    int halfW = (bottomBar.getWidth() - 12) / 2;
    dragWavPillBounds  = bottomBar.removeFromLeft(halfW).toFloat();
    bottomBar.removeFromLeft(12);
    dragMidiPillBounds = bottomBar.toFloat();

    area.removeFromBottom(12);
    auto sliderArea = area.removeFromBottom(136);
    matrixBounds = area.reduced(0, 6).toFloat();

    int rowH = sliderArea.getHeight() / 4;
    auto layoutRow = [rowH](juce::Rectangle<int>& b, juce::Label& lbl, juce::Slider& sld)
    {
        auto row = b.removeFromTop(rowH);
        lbl.setBounds(row.removeFromLeft(130));
        sld.setBounds(row);
    };

    layoutRow(sliderArea, densityLabel, densitySlider);
    layoutRow(sliderArea, tensionLabel, tensionSlider);
    layoutRow(sliderArea, contourLabel, contourSlider);
    layoutRow(sliderArea, spaceLabel,   spaceSlider);
}
