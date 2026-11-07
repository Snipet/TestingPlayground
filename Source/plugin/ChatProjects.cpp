// Source/plugin/ChatProjects.cpp: the chat's projects tool (ChatAgent.h): Claude lists the user's projects and presets,
// and saves, opens or applies one when the user asks, through ChatHost::chatProjects (the processor, which acts as the
// editor's PROJECTS does, on the message thread: ProcessorSaves.cpp). Safe by design (ChatAgent.h's ProjectCall): a save
// only adds, a preset is replaced only with replace (its old values said), unsaved changes are dropped only with
// discard, a factory project is never written (a save makes the user's own copy), and nothing is deleted.
#include "plugin/ChatAgent.h"

#include "Product.h"

namespace playground
{
    juce::var ChatAgent::projectsDefinition()
    {
        // Lean: tools.json sits in every one of Claude Desktop's conversations, so what each action takes is in the
        // description's second paragraph (the guide's projects part has it), the properties undescribed.
        const auto typed = [](const char* type) { return claude::object({ { "type", type } }); };
        juce::var props = claude::object(
            { { "action", claude::object({ { "enum", juce::Array<juce::var>{ "list", "save_version", "save_preset",
                                                                            "apply_preset", "open" } } }) },
              { "name", typed("string") },
              { "note", typed("string") },
              { "category", typed("string") },
              { "replace", typed("boolean") },
              { "discard", typed("boolean") } });
        return claude::object(
            { { "name", "projects" },
              { "description",
                "List the user's projects and presets; save, open or apply one only when the user asks.\n\n"
                "action: list (the default) gives the project open (its version, its preset, and whether it has unsaved "
                "changes), then every project, the user's and the factory's (read-only), each with its category, newest "
                "version and presets; with name, that project's versions (each with its note) and presets. save_version "
                "saves the editor's files and the dials as a new version of the project open, or of the project name "
                "names (a new one when none has it): note says in a few words what changed, which the user reads in the "
                "project's list, and category sets the project's; a factory project's save is the user's own copy, a new "
                "project. save_preset saves the dials as the preset name of the project open (category: its group in the "
                "list); one of that name is replaced only with replace: true, and the result gives its old values. "
                "apply_preset applies a preset of the project open (its dials glide there); open opens a project's newest "
                "version, or Name@N's (@N alone: the open one's): its files go into the editor (one step the user can "
                "undo there) and play, and its dials follow once it compiles. Both refuse while the editor holds unsaved "
                "changes (files, samples or dials changed since they were opened or saved) unless discard: true, only when "
                "the user said to drop them. A save only adds; nothing is deleted (that is the user's alone, in PROJECTS). Each "
                "save shows in the plugin and in the change notices." },
              { "eager_input_streaming", true },
              { "input_schema", claude::object({ { "type", "object" }, { "properties", props } }) } });
    }

    std::string ChatAgent::projectsText(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        using A = ProjectCall::Action;
        ProjectCall call;
        const juce::var act = input.getProperty("action", juce::var());
        const std::string action = act.isVoid() || act.isUndefined() ? std::string("list") : act.toString().toStdString();
        if (action == "list")
            call.action = A::list;
        else if (action == "save_version")
            call.action = A::saveVersion;
        else if (action == "save_preset")
            call.action = A::savePreset;
        else if (action == "apply_preset")
            call.action = A::applyPreset;
        else if (action == "open")
            call.action = A::open;
        else
            return fail("action is list, save_version, save_preset, apply_preset or open. Nothing was done.");
        for (const auto& [key, out] : { std::pair<const char*, std::string*>{ "name", &call.name }, { "note", &call.note },
                                        { "category", &call.category } })
        {
            const juce::var v = input.getProperty(key, juce::var());
            if (v.isVoid() || v.isUndefined())
                continue;
            if (!v.isString())
                return fail(std::string(key) + " is a text. Nothing was done.");
            *out = v.toString().toStdString();
        }
        for (const auto& [key, out] : { std::pair<const char*, bool*>{ "replace", &call.replace }, { "discard", &call.discard } })
        {
            const juce::var v = input.getProperty(key, juce::var());
            if (v.isVoid() || v.isUndefined())
                continue;
            if (v.isBool())
                *out = static_cast<bool>(v);
            else if (v.isString() && (v.toString() == "true" || v.toString() == "false"))
                *out = v.toString() == "true";
            else
                return fail(std::string(key) + " is true or false. Nothing was done.");
        }
        call.desktop = mcpWorking_.load();
        setActivity(call.action == A::list          ? "Reading the projects"
                    : call.action == A::open        ? "Opening the project"
                    : call.action == A::applyPreset ? "Applying the preset"
                                                    : "Saving");
        const ProjectAnswer r = host_.chatProjects(call);
        std::string text = r.text;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!r.entry.empty())
                addEntry(ChatEntry::Kind::tool, r.entry);
            // Another project opened: the checks were the program's before it, so they go, as at a fresh start
            // (Processor::startAfresh), and nothing measures the project the user asked for against them.
            if (const std::size_t n = checks_.size(); r.ok && r.another && n > 0)
            {
                checks_.clear();
                checksFailing_ = false;
                addEntry(ChatEntry::Kind::note, "The checks are cleared: they were for the program before the project opened.");
                touch();
                text += n == 1 ? "\nThe check you set was for the program before it, so the plugin cleared it (set_checks sets "
                                 "new ones, if this one needs them)."
                               : "\nThe " + std::to_string(n) + " checks you set were for the program before it, so the plugin "
                                 "cleared them (set_checks sets new ones, if this one needs them).";
            }
        }
        isError = !r.ok;
        return text;
    }
}
