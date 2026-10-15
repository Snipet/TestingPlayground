// Source/plugin/EditorPort.h: what the editor reaches besides the parameter ports (JUCE-free; message thread only).
// The processor implements it (Processor.h); the Panel and its graph view (Source/editor/ShapeView.h) take it as they
// take the ParamPorts, so the editor never includes the processor.
//
//   shape()          the LFO shape the graph edits, a copy (LfoShape.h; at most 32 points)
//   shapeRevision()  changes whenever the shape does, from the editor or from a state the host loaded, so a view
//                    re-reads it only then
//   setShape()       a new shape: sanitised (LfoShape.h's rules), kept in the plugin state and handed to the audio
//                    thread, which crossfades to it over 20 ms. A drag sets it on every move.
//   endShapeEdit()   an edit is complete (pointer up, a menu choice): the host is told the plugin's state changed, so
//                    it offers to save the session. Shapes are not parameters: hosts neither automate nor undo them.
//   sampleRate()     the rate the processor was prepared at (48 kHz before that), for the DELAYS readout
#pragma once

#include "dsp/LfoShape.h"

#include <cstdint>

namespace testingplayground
{
    class EditorPort
    {
    public:
        virtual ~EditorPort() = default;

        virtual LfoShape shape() const = 0;
        virtual uint64_t shapeRevision() const = 0;
        virtual void setShape(const LfoShape& shape) = 0;
        virtual void endShapeEdit() = 0;
        virtual float sampleRate() const = 0;
    };
}
