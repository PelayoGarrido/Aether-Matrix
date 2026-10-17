#include "PianoStudioView.h"
namespace aether
{
PianoStudioView::PianoStudioView(PianoStudio& s,AudioPreview& p):studio(s),preview(p)
{
    resources=studio.status().resources;
    if (resources.isDirectory()) resourceButton.setButtonText("Piano resources selected");
    title.setText("PIANO STUDIO",juce::dontSendNotification);title.setFont(juce::FontOptions(30.0f,juce::Font::bold));
    information.setText("Piano, performed locally. Editable MIDI and 24-bit audio. Development preview.",juce::dontSendNotification);information.setFont(juce::FontOptions(18.0f));
    statusLabel.setFont(juce::FontOptions(19.0f));statusLabel.setJustificationType(juce::Justification::centredLeft);
    for (auto* label:{&title,&information,&statusLabel,&tempoLabel,&keyLabel,&barsLabel,&seedLabel})
    {label->setMinimumHorizontalScale(1.0f);label->setColour(juce::Label::textColourId,juce::Colour(0xffedf0f8));addAndMakeVisible(*label);}
    tempoLabel.setText("TEMPO",juce::dontSendNotification);keyLabel.setText("KEY",juce::dontSendNotification);barsLabel.setText("BARS",juce::dontSendNotification);seedLabel.setText("VARIATION",juce::dontSendNotification);
    for (auto* label:{&tempoLabel,&keyLabel,&barsLabel,&seedLabel}) label->setFont(juce::FontOptions(18.0f,juce::Font::bold));
    tempo.setRange(40,300,1);tempo.setValue(120);tempo.setTextValueSuffix(" BPM");tempo.setSliderStyle(juce::Slider::IncDecButtons);tempo.setTextBoxStyle(juce::Slider::TextBoxLeft,false,120,40);tempo.setTitle("Piano tempo");
    seed.setRange(0,2147483647,1);seed.setValue(42);seed.setSliderStyle(juce::Slider::IncDecButtons);seed.setTextBoxStyle(juce::Slider::TextBoxLeft,false,210,40);seed.setTitle("Piano generation seed");
    key.addItemList({"C major","G major","D major","A major","E major","B major","F# major","C# major","F major","Bb major","Eb major","Ab major","Db major","Gb major","Cb major",
        "A minor","E minor","B minor","F# minor","C# minor","G# minor","D# minor","A# minor","D minor","G minor","C minor","F minor","Bb minor","Eb minor","Ab minor"},1);
    key.setSelectedId(1);key.setTitle("Piano key signature");bars.addItem("4 bars",4);bars.addItem("8 bars",8);bars.addItem("16 bars",16);bars.setSelectedId(8);bars.setTitle("Piano phrase length");
    for (auto* component:std::array<juce::Component*,4>{&tempo,&seed,&key,&bars}) addAndMakeVisible(*component);
    for (auto* button:{&resourceButton,&generateButton,&importButton,&playButton,&stopButton,&cancelButton,&folderButton,static_cast<juce::TextButton*>(&wav),static_cast<juce::TextButton*>(&midi)})
    {button->setColour(juce::TextButton::buttonColourId,juce::Colour(0xff484c5d));button->setColour(juce::TextButton::textColourOffId,juce::Colour(0xffedf0f8));addAndMakeVisible(*button);}
    wav.setButtonText("Drag WAV");midi.setButtonText("Drag MIDI");
    resourceButton.onClick=[this]{chooseResources();};generateButton.onClick=[this]{studio.generate(resources,settings());};importButton.onClick=[this]{chooseMidi();};
    playButton.onClick=[this]{preview.play();};stopButton.onClick=[this]{preview.stop();};cancelButton.onClick=[this]{studio.cancel();};
    folderButton.onClick=[this]{if (last.job.isDirectory()) last.job.revealToUser();};
    startTimerHz(12);timerCallback();
}
PianoStudio::Settings PianoStudioView::settings() const
{
    PianoStudio::Settings s;
    const auto bpm=tempo.getValue(),variation=seed.getValue();
    s.bpm=std::isfinite(bpm) ? static_cast<int>(std::clamp(bpm,40.0,300.0)) : 120;
    s.seed=std::isfinite(variation) ? static_cast<int>(std::clamp(variation,0.0,2147483647.0)) : 42;
    s.bars=std::clamp(bars.getSelectedId(),1,32);
    const int index=std::clamp(key.getSelectedId(),1,30)-1;s.minor=index>=15 ? 1 : 0;const int signature=index%15;s.sharps=signature<8 ? signature : 7-signature;return s;
}
void PianoStudioView::chooseResources()
{
    chooser=std::make_unique<juce::FileChooser>("Choose the folder containing midi-tv2o-medium, pianist-transformer and salamander-v3",resources);
    const juce::Component::SafePointer<PianoStudioView> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectDirectories,[safe](const juce::FileChooser& c){if (safe && c.getResult().isDirectory()) {safe->resources=c.getResult();safe->studio.selectResources(safe->resources);safe->resourceButton.setButtonText("Piano resources selected");}});
}
void PianoStudioView::chooseMidi()
{
    chooser=std::make_unique<juce::FileChooser>("Render edited single-instrument MIDI",juce::File{},"*.mid;*.midi");
    const juce::Component::SafePointer<PianoStudioView> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe](const juce::FileChooser& c){if (safe && c.getResult().existsAsFile()) safe->studio.importMidi(safe->resources,c.getResult(),safe->settings());});
}
void PianoStudioView::timerCallback()
{
    const auto next=studio.status();
    const bool playing=preview.isPlaying();
    const bool changed=playing!=wasPlaying || playing || next.busy || next.message!=last.message || next.performance!=last.performance;
    const int hz=playing ? 30 : next.busy ? 12 : 4;
    if (hz!=refreshHz) {refreshHz=hz;startTimerHz(hz);}
    if (next.performance && next.performance!=last.performance)
    {
        const auto& p=*next.performance;tempo.setValue(p.bpm,juce::dontSendNotification);
        const int signature=p.sharps>=0 ? p.sharps : 7-p.sharps;key.setSelectedId(1+signature+15*p.minor,juce::dontSendNotification);
        if (bars.indexOfItemId(p.bars)<0) bars.addItem(juce::String(p.bars)+" bars",p.bars);
        bars.setSelectedId(p.bars,juce::dontSendNotification);
    }
    wasPlaying=playing;last=next;statusLabel.setText(last.message,juce::dontSendNotification);
    const bool ready=last.wav.existsAsFile() && last.midi.existsAsFile();
    for (auto* button:{&generateButton,&importButton,&resourceButton}) button->setEnabled(!last.busy);
    generateButton.setEnabled(!last.busy && resources.isDirectory());importButton.setEnabled(!last.busy && resources.isDirectory());
    for (auto* component:std::array<juce::Component*,4>{&tempo,&seed,&key,&bars}) component->setEnabled(!last.busy);
    cancelButton.setEnabled(last.busy);playButton.setEnabled(ready && !last.busy);stopButton.setEnabled(preview.isPlaying());
    wav.setEnabled(ready);midi.setEnabled(ready);wav.file=last.wav;midi.file=last.midi;folderButton.setEnabled(last.job.isDirectory());
    if (isShowing() && changed) repaint();
}
void PianoStudioView::resized()
{
    title.setBounds(25,18,500,40);information.setBounds(25,65,getWidth()-50,48);
    tempoLabel.setBounds(25,124,180,26);tempo.setBounds(25,155,210,42);
    keyLabel.setBounds(260,124,200,26);key.setBounds(260,155,240,42);
    barsLabel.setBounds(525,124,160,26);bars.setBounds(525,155,175,42);
    seedLabel.setBounds(725,124,230,26);seed.setBounds(725,155,300,42);
    resourceButton.setBounds(25,218,300,42);generateButton.setBounds(340,218,240,42);importButton.setBounds(595,218,280,42);cancelButton.setBounds(890,218,140,42);
    statusLabel.setBounds(25,277,getWidth()-50,66);
    playButton.setBounds(25,getHeight()-66,150,42);stopButton.setBounds(190,getHeight()-66,110,42);wav.setBounds(325,getHeight()-66,200,42);midi.setBounds(540,getHeight()-66,200,42);folderButton.setBounds(765,getHeight()-66,265,42);
}
void PianoStudioView::paint(juce::Graphics& g)
{
    auto b=getLocalBounds().toFloat().reduced(2);g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4a5060),b.getTopLeft(),juce::Colour(0xff252a34),b.getBottomLeft(),false));g.fillRoundedRectangle(b,26);
    g.setColour(juce::Colour(0xffa0a6b5));g.drawRoundedRectangle(b,26,1);g.setColour(juce::Colour(0xff111620));
    const auto roll=juce::Rectangle<float>(25,361,static_cast<float>(getWidth()-50),static_cast<float>(getHeight()-445));g.fillRoundedRectangle(roll,16);
    if (last.performance)
    {
        juce::Graphics::ScopedSaveState clipped(g);g.reduceClipRegion(roll.toNearestInt());
        const auto& p=*last.performance;const double length=p.bars*4*p.ppq;
        const double durationUs=p.microsAt(static_cast<int64_t>(length));
        for (int bar=0;bar<=p.bars;++bar) {g.setColour(juce::Colours::white.withAlpha(.12f));const float x=roll.getX()+roll.getWidth()*static_cast<float>(bar)/static_cast<float>(p.bars);g.drawVerticalLine(static_cast<int>(x),roll.getY()+8,roll.getBottom()-8);}
        int low=108,high=21;for (const auto& n:p.notes) {low=std::min(low,n.pitch);high=std::max(high,n.pitch);}
        low=std::max(21,low-6);high=std::min(108,high+6);
        const float pitchSpan=static_cast<float>(std::max(12,high-low));
        const float noteHeight=std::clamp((roll.getHeight()-24)/(pitchSpan+1)*.7f,4.0f,10.0f);
        for (const auto& n:p.notes)
        {
            const double start=p.microsAt(n.tick,n.onsetOffsetUs),end=p.microsAt(n.tick+n.duration,n.releaseOffsetUs);
            const float x=roll.getX()+static_cast<float>(start/durationUs)*roll.getWidth();const float y=roll.getBottom()-12-static_cast<float>(n.pitch-low)/pitchSpan*(roll.getHeight()-24);
            g.setColour(juce::Colour(0xffb5a0fa).withAlpha(.35f+.65f*static_cast<float>(n.velocity)/127));g.fillRoundedRectangle(x,y-noteHeight/2,std::max(2.0f,static_cast<float>((end-start)/durationUs)*roll.getWidth()),noteHeight,2);
        }
        if (preview.isPlaying()) {g.setColour(juce::Colour(0xffffb15b));const float x=roll.getX()+static_cast<float>(std::min(1.0,preview.seconds()*1e6/durationUs))*roll.getWidth();g.drawVerticalLine(static_cast<int>(x),roll.getY()+8,roll.getBottom()-8);}
    }
    else
    {g.setColour(juce::Colour(0xffc6cbd8));g.setFont(juce::FontOptions(21.0f));g.drawFittedText("Your piano take appears here.\nDrag MIDI into your DAW to edit it, then render the edited file.",roll.reduced(20).toNearestInt(),juce::Justification::centred,3);}
    if (last.busy) {g.setColour(juce::Colour(0xffffb15b));g.fillRoundedRectangle(25,346,(getWidth()-50)*std::clamp(last.progress,0.0f,1.0f),4,2);}
}
}
