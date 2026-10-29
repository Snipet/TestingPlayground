// Source/plugin/ChatCaptured.cpp: scenario::captured (ChatScenario.h), the one seam the sources "capture" go
// through: a scenario's phrase or input, compare's a or b, trace's source. The capture lane fills it in with the
// processor's recorder (per host block: the events, the dials' values, the transport, the input and the output);
// until then there is never a recording.
#include "plugin/ChatScenario.h"

namespace playground::scenario
{
    bool captured(ChatHost& /*host*/, Captured& /*out*/, std::string& why)
    {
        why = "nothing recorded yet (the plugin keeps no recording of the user's playing)";
        return false;
    }
}
