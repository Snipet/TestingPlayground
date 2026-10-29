// Source/plugin/ChatCapture.h: the capture tool (ChatCapture.cpp), which ChatAgent's tools list and runTool reach: its
// definition (toolDefinitions) and its handler (ChatAgent::captureTool, which draws its picture), over
// ChatHost::chatCapture, the recorder (Capture.h) and its replay (CaptureReplay.h).
#pragma once

#include "plugin/ChatAgent.h"

#include <functional>
#include <string>

namespace playground
{
    // The tool as the API takes it ({ name, description, eager_input_streaming, input_schema }).
    juce::var captureToolDefinition();

    // The tool run: its result's text (isError for a call that did nothing); `note` adds a row to the chat's card
    // (only arm and forget do: stop and status are live, and touch nothing of the chat); `taken`, when given, the take
    // that stop and status report (ChatAgent::captureTool draws it for `picture`).
    std::string captureToolText(ChatHost& host, const juce::var& input, bool& isError,
                                const std::function<void(std::string)>& note,
                                std::shared_ptr<const capture::Take>* taken = nullptr);

    // The take in words, at most 25 lines (stop's and status's report): `state` what the recorder does now, `controls`
    // the dials' names and units.
    std::string captureReport(const capture::Reply& reply, const Controls& controls);
}
