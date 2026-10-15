// Source/plugin/CreateEditorGeneric.cpp: the editor factory of the probe executable (CMakeLists.txt compiles it there
// only). The probes run the FunkGui Panel headless through funkgui::HeadlessHost; a JUCE editor is never opened there.
// The plugin's factory is Source/editor/gpu/CreateEditorGpu.cpp.
#include "plugin/Processor.h"

namespace testingplaygroundsynth
{
    juce::AudioProcessorEditor* createEditor(Processor& processor)
    {
        return new juce::GenericAudioProcessorEditor(processor);
    }
}
