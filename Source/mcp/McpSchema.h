// Source/mcp/McpSchema.h: what the plugin (ChatAgent::mcpTools, which writes tools.json) and the server (mcp/Main.cpp's
// toolList) both do to MCP's copy of the tools. The server's tools/list reply goes into every one of Claude Desktop's
// conversations (the tool budget, CLAUDE.md), so it says each thing once and leaves out what MCP's defaults already say.
// juce_core only: the server includes it as well as the plugin.
#pragma once

#include <juce_core/juce_core.h>

#include <set>
#include <utility>

namespace playground::mcp
{
    // The words of `values` in view_panel and in the server's gui_sandbox (a render's): the same in both, so the
    // server's list says them once (gui_sandbox's, listed first). No number in them, or both copies would stay.
    inline constexpr const char* kPanelValues = "Dials and fields (as panel.gui names them; in, out in dBFS; pointer.hover, "
                                                ".down, .x, .y the shaders' pointer) drawn at these values";

    // A property's description that an earlier property of the same name has word for word is left out; `seen` holds the
    // ones described so far. The plugin runs it over its tools (a goal's words in tune_dials are set_checks', a check's
    // render settings measure_output's), and the server again over the whole list once it has added `instance` to each
    // of the plugin's tools, which is then described on the first alone. A description with a number in it (a range, a
    // default, a cap) stays in every copy: Desktop may load or switch on one tool without the one that described it
    // first, and a missing range is a wrong value.
    inline void dropRepeated(const juce::var& node, std::set<std::pair<juce::String, juce::String>>& seen)
    {
        if (juce::DynamicObject* o = node.getDynamicObject())
        {
            if (juce::DynamicObject* props = o->getProperty("properties").getDynamicObject())
                for (const auto& kv : props->getProperties())
                    if (juce::DynamicObject* p = kv.value.getDynamicObject(); p != nullptr && p->hasProperty("description"))
                    {
                        const juce::String text = p->getProperty("description").toString();
                        if (!text.containsAnyOf("0123456789") && !seen.insert({ kv.name.toString(), text }).second)
                            p->removeProperty("description");
                    }
            for (const auto& kv : o->getProperties())
                dropRepeated(kv.value, seen);
        }
        else if (const juce::Array<juce::var>* a = node.getArray())
            for (const juce::var& v : *a)
                dropRepeated(v, seen);
    }

    // A tool's annotations, saying only what MCP's defaults do not: readOnlyHint when it is true (the default is false);
    // destructiveHint on a tool that writes alone (MCP reads it only when readOnlyHint is false, and its default there is
    // true, so a tool that takes nothing away says false); openWorldHint false (the default is true). No title: the
    // tool's own is MCP's since 2025-06-18, and the server copies it in here for a client from before (Main.cpp).
    inline juce::var annotations(bool readOnly, bool destructive)
    {
        auto* o = new juce::DynamicObject();
        if (readOnly)
            o->setProperty("readOnlyHint", true);
        else
            o->setProperty("destructiveHint", destructive);
        o->setProperty("openWorldHint", false);
        return juce::var(o);
    }
}
