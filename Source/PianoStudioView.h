#pragma once
#include "PianoStudio.h"
#include <juce_gui_basics/juce_gui_basics.h>
namespace aether
{
class PianoStudioView : public juce::Component,public juce::DragAndDropContainer,private juce::Timer
{
public:
    PianoStudioView(PianoStudio&,AudioPreview&);
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;
    PianoStudio::Settings settings() const;
    void chooseResources();void chooseMidi();
    PianoStudio& studio;AudioPreview& preview;
    juce::File resources;
    juce::TextButton resourceButton{"Local piano resources"},generateButton{"Generate piano"},importButton{"Render edited MIDI"},playButton{"Audition"},stopButton{"Stop"},cancelButton{"Cancel"},folderButton{"Open job files"};
    juce::ComboBox key,bars;juce::Slider tempo,seed;
    juce::Label title,information,statusLabel,tempoLabel,keyLabel,barsLabel,seedLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    PianoStudio::Status last;int refreshHz=0;bool wasPlaying=false;
    struct ExportButton : juce::TextButton
    {
        juce::File file;void mouseDrag(const juce::MouseEvent& e) override
        {if (file.existsAsFile() && e.getDistanceFromDragStart()>4) juce::DragAndDropContainer::performExternalDragDropOfFiles({file.getFullPathName()},false,this);}
    } wav,midi;
};
}
