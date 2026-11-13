// PROBE name=gui.preview timeout=300
//
// gui.preview: the Projects card's preview of a version's panel (editor/Preview.h), made without compiling anything,
// against view_panel's picture of the same program playing in a real Processor. PNGs to probe-out/<key>/:
//   gui.preview.example.png        the prompt's example, previewed (whole: 1280 x 918)
//   gui.preview.example.view.png   view_panel's picture of it
//   gui.preview.example.diff.png   where they differ (red), the displays masked (blue)
//   gui.preview.pointer*.png       the same for the first folder example (its panel.gui's pointer and shader dial)
//   gui.preview.values.png         VALUES' program at the version's values
//   gui.preview.rows.*.png         ROWS' programs, previewed and (.view.png) as view_panel draws them
// and checks:
//   MATCH    the prompt's example (ChatAgent::promptExample): the preview at its defaults is view_panel's panel picture
//            of the same files at the same size, pixel for pixel but the displays and live= marks (the sandbox's demo
//            values against the processor's silence) and the shaders' placeholders (SoftRaster's stand-in: the full
//            Panel paints over a placeholder's last row, the sandbox's panel does not), within plugin.sandbox MATCH's
//            bound; both drawn by SoftRaster here (picture::forceSoftRasterForTests), so the row is judged on every
//            machine; the card's picture is the same scaled to 640 px across. A GPU picture where this platform has
//            them (else skipped, saying why)
//   POINTER  MATCH's rows for the first example that is a folder (effect: 7-light-filter, synth: 6-light-saw): a .gui
//            with pointer properties (a box's x= and y=, a shader's inputs pointer.hover and pointer.down) and a shader
//            whose width= and height= are its size (96 x 96); its pointer items inert in a preview, as in a picture
//   VALUES   a version's saved values set its dials by name (case aside; an option's name for an int), the others at
//            their defaults, a name the program lacks ignored; the level moved changes the picture in one cell, the
//            wave moved too in another
//   SHADER   a .gui with a shader: built through the process's cache with the tools given (CTest's shaderc), or, with
//            no shaderc, a note that it shows as a placeholder, the picture drawn all the same
//   ROWS     a program with no .gui, in plain rows and in #pragma panel sections (every dial style, a colour, a meter
//            and a led): the preview's cells are the playing panel's in the PANEL view (the same names, sizes and
//            places, one offset for all), and each dial's pixels view_panel's within a small bound (the displays
//            aside: demo values against silence); gui.preview.rows.plain.png, .rows.sections.png and their .view.png
//   FAILS    a .gui of which nothing reads and a program of no dials (its first note; with dials, they are drawn as the
//            PANEL view shows them), no files: no preview, saying why; draw() of one draws nothing
#include "Pictures.h"
#include "Probe.h"

#include "editor/Panel.h"
#include "editor/Picture.h"
#include "editor/Preview.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "sandbox/SandboxProtocol.h"

#include "Examples.h"
#include "Product.h"

#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    pg::ShaderTools tools(bool withShaderc)
    {
        pg::ShaderTools t;
        const char* env = std::getenv((std::string(pg::product::kEnvPrefix) + "SHADERC").c_str());
        t.shaderc = withShaderc && env != nullptr ? env : "";
        t.prelude = funkgui::shaders::fragmentPrelude();
        t.varyingDef = funkgui::shaders::varyingDef();
        t.bgfxShaderSh = funkgui::shaders::bgfxShaderSh();
#if defined(__APPLE__)
        t.platform = "osx";
        t.profile = "metal";
#elif defined(_WIN32)
        t.platform = "windows";
        t.profile = "s_5_0";
#else
        t.platform = "linux";
        t.profile = "spirv";
#endif
        return t;
    }

    juce::Image decode(const std::string& png)
    {
        return png.empty() ? juce::Image() : juce::ImageFileFormat::loadFrom(png.data(), png.size());
    }

    juce::Image imageOf(const funkgui::Image& img)
    {
        if (img.w <= 0 || img.h <= 0)
            return {};
        juce::Image out(juce::Image::ARGB, img.w, img.h, true);
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
            {
                const uint8_t* q = img.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.w) + static_cast<std::size_t>(x)) * 4;
                out.setPixelAt(x, y, juce::Colour(q[0], q[1], q[2], q[3]));
            }
        return out;
    }

    void writeImage(const juce::Image& img, const char* name)
    {
        juce::File out(juce::String(probe::outPath(name)));
        out.deleteFile();
        juce::FileOutputStream stream(out);
        juce::PNGImageFormat().writeImageToStream(img, stream);
        std::printf("wrote %s\n", out.getFullPathName().toRawUTF8());
    }

    // Pixels where two pictures of one size differ by more than `tolerance` in a channel, inside and outside the masks.
    struct Diff
    {
        int inside = 0, outside = 0, largestOutside = 0, total = 0;
        bool sameSize = false;
    };

    Diff compare(const juce::Image& a, const juce::Image& b, const std::vector<juce::Rectangle<int>>& masks, int tolerance,
                 juce::Image* where = nullptr)
    {
        Diff d;
        d.sameSize = a.isValid() && b.isValid() && a.getWidth() == b.getWidth() && a.getHeight() == b.getHeight();
        if (!d.sameSize)
            return d;
        if (where != nullptr)
            *where = juce::Image(juce::Image::RGB, a.getWidth(), a.getHeight(), true);
        for (int y = 0; y < a.getHeight(); ++y)
            for (int x = 0; x < a.getWidth(); ++x)
            {
                const juce::Colour ca = a.getPixelAt(x, y), cb = b.getPixelAt(x, y);
                const int diff = std::max({ std::abs(ca.getRed() - cb.getRed()), std::abs(ca.getGreen() - cb.getGreen()),
                                            std::abs(ca.getBlue() - cb.getBlue()) });
                ++d.total;
                const bool masked = std::any_of(masks.begin(), masks.end(), [&](const juce::Rectangle<int>& m) { return m.contains(x, y); });
                if (masked)
                    d.inside += diff > tolerance ? 1 : 0;
                else
                {
                    d.outside += diff > tolerance ? 1 : 0;
                    d.largestOutside = std::max(d.largestOutside, diff);
                }
                if (where != nullptr)
                    where->setPixelAt(x, y, diff > tolerance && !masked ? juce::Colours::red
                                            : masked                    ? juce::Colours::darkblue
                                                                        : ca.withMultipliedBrightness(0.4f));
            }
        return d;
    }

    juce::Rectangle<int> pixels(float x, float y, float w, float h, int grow)
    {
        const float k = pg::sandbox::kDpi;
        return juce::Rectangle<int>(static_cast<int>(std::floor(x * k)) - grow, static_cast<int>(std::floor(y * k)) - grow,
                                    static_cast<int>(std::ceil(w * k)) + 2 * grow, static_cast<int>(std::ceil(h * k)) + 2 * grow);
    }

    std::string valuesProgram()
    {
        const std::string params = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float tone [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param int wave [[fminus::range(0, 2)]] [[fminus::default(0)]];\n";
        return kInstrument
            ? "// Values: a quiet voice\nmodule Voice\n{\n" + params
                  + "    bool held;\n    void start(float note, float v) { held = true; }\n    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n) { return held; }\n};\n"
            : "// Values: a level\nmodule Effect\n{\n" + params
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] *= level[i]; right[i] *= level[i]; }\n    }\n};\n";
    }

    const char* const kValuesGui = "panel {\n    row {\n        section \"LEVEL\" { knob level }\n        section \"TONE\" { knob tone }\n"
                                   "        section \"WAVE\" { buttons wave \"SAW\" \"SQUARE\" \"TRI\" }\n    }\n}\n";

    const char* const kStill = "// still.sc: it reads no u_time, so it changes only with its input\n"
                               "void main()\n"
                               "{\n"
                               "    vec3 c = mix(vec3(0.1, 0.1, 0.2), vec3(1.0, 0.6, 0.2), in_level * v_uv.x);\n"
                               "    gl_FragColor = vec4(c, 1.0);\n"
                               "}\n";
    const char* const kShaderGui = "panel {\n    row {\n        section \"GLOW\" { shader \"still.sc\" inputs=\"level\" width=160 height=90 }\n"
                                   "        section \"LEVEL\" { knob level }\n    }\n}\n";

    // Two programs with no .gui: dials in plain rows, and #pragma panel lines' sections of every style and displays.
    std::string rowsProgram(bool sections)
    {
        const std::string params = "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                                   "    param float resonance [[fminus::range(0, 1)]] [[fminus::default(0.3)]];\n"
                                   "    param float drive [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.7)]];\n"
                                   "    param int wave [[fminus::range(0, 2)]] [[fminus::default(1)]];\n"
                                   "    param bool sync [[fminus::default(0)]];\n"
                                   "    float env;\n";
        const std::string pragmas = sections ? "#pragma panel section \"FILTER\" cutoff resonance env\n"
                                               "#pragma panel section \"OSC\" wave sync drive\n"
                                               "#pragma panel section \"OUT\" level out\n"
                                               "#pragma panel style wave buttons\n#pragma panel style sync switch\n"
                                               "#pragma panel style drive vintage\n#pragma panel style level fader\n"
                                               "#pragma panel options wave \"SAW\" \"SQUARE\" \"TRI\"\n"
                                               "#pragma panel color \"FILTER\" orange\n"
                                               "#pragma panel meter out \"OUTPUT\"\n#pragma panel led env \"ENV\"\n"
                                             : "";
        return kInstrument
            ? pragmas + "module Voice\n{\n" + params
                  + "    bool held;\n    void start(float note, float v) { held = true; env = 1; }\n    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n) { return held; }\n};\n"
            : pragmas + "module Effect\n{\n" + params
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] *= level[i]; right[i] *= level[i]; }\n        env = level[0];\n    }\n};\n";
    }

    int slotOf(const pg::preview::Prepared& p, const std::string& name)
    {
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (p.controls[static_cast<std::size_t>(c)].used && p.controls[static_cast<std::size_t>(c)].name == name)
                return c;
        return -1;
    }
}

PROBE(gui, preview)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);

    // ---- MATCH: the prompt's example, previewed and as view_panel draws it playing --------------------------------------
    // ---- POINTER: a panel with an area, pointer properties and a 96 x 96 shader dial (the folder example) -------------
    const auto match = [&](const std::string& key, const pg::Sources& example, const std::string& png) {
        pg::Processor processor;
        processor.compiler().setFreezing(false);
        pg::EditorPort& port = processor.editorPort();
        port.setDraft({ example, 0 });
        port.run(example);
        processor.settle();
        P.eq(key + ".compiled", port.status().ok && !port.panel().gui.empty() ? 1 : 0, 1);
        pg::picture::forceSoftRasterForTests(true);       // both SoftRaster: the same rasteriser on every machine
        const pg::PanelPicture view = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        // Where the displays and live= marks are (a Panel of the probe's own, in the PANEL view).
        std::vector<juce::Rectangle<int>> masks;
        {
            pg::ui::Panel panel(processor.ports(), port);
            funkgui::HeadlessHost host(panel, 0, pg::sandbox::kDpi);
            host.settle(120);
            panel.setPanelView(true);
            host.settle(120);
            const pg::ui::GuiLayout& gl = panel.guiLayout();
            const std::vector<pg::GuiNode>& nodes = panel.panelSpec().gui.nodes;
            for (std::size_t i = 0; i < nodes.size() && i < gl.rects.size(); ++i)
                // the displays and live= marks (demo values against silence), and a shader's placeholder (SoftRaster's
                // stand-in, which only a picture without the GPU draws: the full Panel paints over its last row, the
                // sandbox's panel does not; the GPU draws the shader itself)
                if (nodes[i].kind == pg::GuiKind::display || nodes[i].live >= 0 || nodes[i].kind == pg::GuiKind::shader)
                {
                    const funkgui::Rect& r = gl.rects[i];
                    masks.push_back(pixels(r.x - gl.area.x, r.y - gl.area.y, r.w, r.h, 3));
                }
            port.setPanelView(false);
        }
        pg::preview::Request request;
        request.files = example;
        request.instrument = kInstrument;
        const pg::Controls controls = port.controls();
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (const pg::Control& k = controls[static_cast<std::size_t>(c)]; k.used)
                request.dials.push_back({ k.name, k.def, "" });
        const pg::preview::Prepared prepared = pg::preview::prepare(request, tools(true));
        P.eq(key + ".prepared", prepared.ok ? 1 : 0, 1);
        int pointered = 0;                               // the elements a pointer drives (area, shader, box, ...)
        for (const pg::GuiNode& n : prepared.spec.gui.nodes)
            pointered += n.pointer.any() ? 1 : 0;
        std::printf("%s: %d element(s) a pointer drives\n", key.c_str(), pointered);
        if (key == "pointer")
            P.ge(key + ".pointer_elements", pointered, 1);
        const pg::preview::Picture whole = pg::preview::draw(prepared, 0, 0);
        const juce::Image mine = imageOf(whole.image);
        writeImage(mine, ("gui.preview." + png + ".png").c_str());
        {
            juce::File out(juce::String(probe::outPath("gui.preview." + png + ".view.png")));
            out.replaceWithData(view.png.data(), view.png.size());
        }
        juce::Image where;
        const Diff d = compare(decode(view.png), mine, masks, 4, &where);
        if (where.isValid())
            writeImage(where, ("gui.preview." + png + ".diff.png").c_str());
        std::printf("against view_panel (%s; %zu displays and live marks masked): %d of %d pixels differ by more than 4 "
                    "outside them (largest %d), %d inside\n", view.about.c_str(), masks.size(), d.outside, d.total, d.largestOutside, d.inside);
        P.eq(key + ".soft", !whole.gpu && !view.gpu ? 1 : 0, 1);
        P.eq(key + ".same_size", d.sameSize ? 1 : 0, 1);
        P.le(key + ".pixels_off", static_cast<double>(d.outside), 0.002 * d.total);
        P.le(key + ".largest", d.largestOutside, 40);
        const pg::preview::Picture card = pg::preview::draw(prepared, 0, pg::preview::kMaxWidth);
        std::printf("the card's picture: %d x %d (whole: %d x %d)\n", card.image.w, card.image.h, whole.image.w, whole.image.h);
        P.eq(key + ".card_width", card.image.w, pg::preview::kMaxWidth);
        P.near(key + ".card_shape", static_cast<double>(card.image.h) / card.image.w, static_cast<double>(whole.image.h) / whole.image.w, 0.01);
        pg::picture::forceSoftRasterForTests(false);
        if (const std::string none = probe::noGpuPictures(); none.empty())
        {
            const pg::preview::Picture gpu = pg::preview::draw(prepared, 0, pg::preview::kMaxWidth);
            P.eq(key + ".gpu", gpu.gpu && gpu.image.w == pg::preview::kMaxWidth ? 1 : 0, 1);
        }
        else
        {
            probe::skip(P, key + ".gpu", probe::noGpuPicturesSkip());
            const pg::preview::Picture soft = pg::preview::draw(prepared, 0, pg::preview::kMaxWidth);
            P.eq(key + ".soft_said", !soft.gpu && has(soft.why, none) ? 1 : 0, 1);
        }
    };
    match("match", pg::ChatAgent::promptExample(), "example");
    {
        // The first example that is a folder (its own panel.gui and shader): pointer properties, an area or a box, and
        // a shader whose width= and height= are its size. Its pointer items in a preview are inert: nothing hovers or
        // presses in a picture, so it draws as view_panel's does with no pointer.
        const pg::Example* folder = nullptr;
        for (const pg::Example& e : pg::kExamples)
            if (folder == nullptr && e.count > 0)
                folder = &e;
        P.eq("pointer.example", folder != nullptr ? 1 : 0, 1);
        if (folder != nullptr)
            match("pointer", pg::exampleSources(*folder), "pointer");
    }

    pg::picture::forceSoftRasterForTests(true);
    // ---- ROWS: a program with no .gui, previewed and as view_panel draws it playing ------------------------------------
    for (const bool sections : { false, true })
    {
        const std::string key = sections ? "rows.sections" : "rows.plain";
        pg::Processor processor;
        processor.compiler().setFreezing(false);
        pg::EditorPort& port = processor.editorPort();
        const pg::Sources files = pg::singleFile(rowsProgram(sections));
        port.setDraft({ files, 0 });
        port.run(files);
        processor.settle();
        if (!port.status().ok)
            std::printf("%s: %s\n", key.c_str(), port.status().errors.c_str());
        P.eq(key + ".compiled", port.status().ok && port.panel().gui.empty() ? 1 : 0, 1);
        // Every dial at its default as the version saved it (a slot the program before used may hold that one's value,
        // which a bool's knob draws off its steps).
        {
            const pg::Controls cs = port.controls();
            for (int c = 0; c < pg::kMaxControls && c < static_cast<int>(processor.getParameters().size()); ++c)
                if (const pg::Control& k = cs[static_cast<std::size_t>(c)]; k.used)
                    processor.getParameters()[c]->setValueNotifyingHost(pg::normOf(k, k.def));
            processor.poll();
        }
        const pg::PanelPicture view = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        // The playing panel's cells (a Panel of the probe's own, in the PANEL view), from its card's corner as the
        // picture is cropped.
        std::vector<std::pair<std::string, funkgui::Rect>> played;   // a dial's name (or "display N") and its cell
        funkgui::Rect card;
        {
            pg::ui::Panel panel(processor.ports(), port);
            funkgui::HeadlessHost host(panel, 0, pg::sandbox::kDpi);
            host.settle(120);
            panel.setPanelView(true);
            host.settle(120);
            card = panel.programCard();
            const pg::ui::DialsLayout& l = panel.dialsLayout();
            const pg::Controls cs = port.controls();
            for (int c = 0; c < pg::kMaxControls; ++c)
                if (cs[static_cast<std::size_t>(c)].used)
                {
                    const pg::ui::ItemPlace& at = l.dials[static_cast<std::size_t>(c)];
                    played.push_back({ cs[static_cast<std::size_t>(c)].name, { at.x - card.x, at.top - card.y, at.w, at.h } });
                }
            for (std::size_t d = 0; d < port.panel().displays.size(); ++d)
                played.push_back({ "display " + std::to_string(d),
                                   { l.displays[d].x - card.x, l.displays[d].top - card.y, l.displays[d].w, l.displays[d].h } });
            port.setPanelView(false);
        }
        pg::preview::Request request;
        request.files = files;
        request.instrument = kInstrument;
        const pg::preview::Prepared prepared = pg::preview::prepare(request, tools(true));
        P.eq(key + ".prepared", prepared.ok && prepared.rows ? 1 : 0, 1);
        const pg::preview::RowsPlaces places = pg::preview::rowsPlaces(prepared);
        std::vector<std::pair<std::string, funkgui::Rect>> mine;
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (prepared.controls[static_cast<std::size_t>(c)].used)
                mine.push_back({ prepared.controls[static_cast<std::size_t>(c)].name, places.dials[static_cast<std::size_t>(c)] });
        for (std::size_t d = 0; d < places.displays.size(); ++d)
            mine.push_back({ "display " + std::to_string(d), places.displays[d] });
        // The same cells, the same sizes, one offset between the two pictures for all (the area's corner in the card).
        bool same = played.size() == mine.size() && !mine.empty();
        const float dx = same ? played[0].second.x - mine[0].second.x : 0.0f, dy = same ? played[0].second.y - mine[0].second.y : 0.0f;
        for (std::size_t i = 0; same && i < mine.size(); ++i)
        {
            const funkgui::Rect& a = played[i].second;
            const funkgui::Rect& b = mine[i].second;
            same = played[i].first == mine[i].first && std::fabs(a.w - b.w) < 0.01f && std::fabs(a.h - b.h) < 0.01f
                && std::fabs(a.x - b.x - dx) < 0.01f && std::fabs(a.y - b.y - dy) < 0.01f;
        }
        std::printf("%s: %zu cells, the area at %.1f, %.1f in the card\n", key.c_str(), mine.size(), dx, dy);
        P.eq(key + ".same_cells", same ? 1 : 0, 1);
        // Each dial's pixels against the playing panel's (the displays aside: demo values against silence).
        const pg::preview::Picture pic = pg::preview::draw(prepared, 0, 0);
        const juce::Image a = decode(view.png), b = imageOf(pic.image);
        writeImage(b, ("gui.preview." + key + ".png").c_str());
        {
            juce::File out(juce::String(probe::outPath("gui.preview." + key + ".view.png")));
            out.replaceWithData(view.png.data(), view.png.size());
        }
        int off = 0, total = 0, largest = 0;
        const float k = pg::sandbox::kDpi;
        for (std::size_t i = 0; same && i < mine.size(); ++i)
        {
            if (mine[i].first.rfind("display ", 0) == 0)
                continue;
            const funkgui::Rect& r = mine[i].second;
            for (int y = static_cast<int>(std::ceil(r.y * k)); y < static_cast<int>(std::floor((r.y + r.h) * k)); ++y)
                for (int x = static_cast<int>(std::ceil(r.x * k)); x < static_cast<int>(std::floor((r.x + r.w) * k)); ++x)
                {
                    const int ax = x + static_cast<int>(std::lround(dx * k)), ay = y + static_cast<int>(std::lround(dy * k));
                    if (x < 0 || y < 0 || x >= b.getWidth() || y >= b.getHeight() || ax < 0 || ay < 0 || ax >= a.getWidth() || ay >= a.getHeight())
                        continue;
                    const juce::Colour ca = a.getPixelAt(ax, ay), cb = b.getPixelAt(x, y);
                    const int d = std::max({ std::abs(ca.getRed() - cb.getRed()), std::abs(ca.getGreen() - cb.getGreen()),
                                             std::abs(ca.getBlue() - cb.getBlue()) });
                    ++total;
                    off += d > 4 ? 1 : 0;
                    largest = std::max(largest, d);
                }
        }
        std::printf("%s: the dials' cells, %d of %d pixels differ by more than 4 (largest %d)\n", key.c_str(), off, total, largest);
        P.ge(key + ".pixels_compared", total, 1000);
        P.le(key + ".pixels_off", static_cast<double>(off), 0.01 * total);
        P.le(key + ".largest", largest, 40);
    }

    // ---- VALUES ------------------------------------------------------------------------------------------------------------
    {
        pg::preview::Request request;
        request.files = { { pg::kProgramName, valuesProgram() }, { "panel.gui", kValuesGui } };
        request.instrument = kInstrument;
        const pg::preview::Prepared defaults = pg::preview::prepare(request, tools(true));
        request.dials = { { "LEVEL", 0.9f, "" }, { "wave", 0.0f, "TRI" }, { "missing", 0.3f, "" } };
        const pg::preview::Prepared saved = pg::preview::prepare(request, tools(true));
        const int level = slotOf(saved, "level"), tone = slotOf(saved, "tone"), wave = slotOf(saved, "wave");
        P.eq("values.prepared", defaults.ok && saved.ok && level >= 0 && tone >= 0 && wave >= 0 ? 1 : 0, 1);
        if (level >= 0 && tone >= 0 && wave >= 0)
        {
            const auto& k = saved.controls;
            P.near("values.level", saved.values01[static_cast<std::size_t>(level)], pg::normOf(k[static_cast<std::size_t>(level)], 0.9f), 1e-6);
            P.near("values.tone_default", saved.values01[static_cast<std::size_t>(tone)], pg::normOf(k[static_cast<std::size_t>(tone)], 0.5f), 1e-6);
            P.near("values.wave_by_name", saved.values01[static_cast<std::size_t>(wave)], pg::normOf(k[static_cast<std::size_t>(wave)], 2.0f), 1e-6);
            P.near("values.level_default", defaults.values01[static_cast<std::size_t>(level)], pg::normOf(k[static_cast<std::size_t>(level)], 0.5f), 1e-6);
        }
        // Level alone moved, then the wave too: each change in a cell of its own (the bounds of what changed apart).
        request.dials = { { "level", 0.9f, "" } };
        const pg::preview::Prepared levelOnly = pg::preview::prepare(request, tools(true));
        const pg::preview::Picture a = pg::preview::draw(defaults, 0, 0), b = pg::preview::draw(levelOnly, 0, 0),
                                   c = pg::preview::draw(saved, 0, 0);
        writeImage(imageOf(c.image), "gui.preview.values.png");
        struct Changed
        {
            int n = 0, x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
        };
        const auto changed = [](const funkgui::Image& p, const funkgui::Image& q) {
            Changed ch;
            if (p.w != q.w || p.h != q.h)
                return ch;
            for (int y = 0; y < p.h; ++y)
                for (int x = 0; x < p.w; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(p.w) + static_cast<std::size_t>(x)) * 4;
                    if (std::equal(p.rgba.begin() + static_cast<std::ptrdiff_t>(i), p.rgba.begin() + static_cast<std::ptrdiff_t>(i + 4),
                                   q.rgba.begin() + static_cast<std::ptrdiff_t>(i)))
                        continue;
                    ++ch.n;
                    ch.x0 = std::min(ch.x0, x);
                    ch.x1 = std::max(ch.x1, x);
                    ch.y0 = std::min(ch.y0, y);
                    ch.y1 = std::max(ch.y1, y);
                }
            return ch;
        };
        const Changed byLevel = changed(a.image, b.image), byWave = changed(b.image, c.image);
        std::printf("pixels changed: %d by the level (x %d to %d), %d by the wave (x %d to %d)\n", byLevel.n, byLevel.x0, byLevel.x1,
                    byWave.n, byWave.x0, byWave.x1);
        P.eq("values.same_size", a.image.w == c.image.w && a.image.h == c.image.h && a.image.w > 0 ? 1 : 0, 1);
        P.ge("values.level_drawn", byLevel.n, 50);
        P.ge("values.wave_drawn", byWave.n, 50);
        P.eq("values.cells_apart", byLevel.x1 < byWave.x0 || byWave.x1 < byLevel.x0 || byLevel.y1 < byWave.y0 || byWave.y1 < byLevel.y0 ? 1 : 0, 1);
    }

    // ---- SHADER ------------------------------------------------------------------------------------------------------------
    {
        pg::preview::Request request;
        request.files = { { pg::kProgramName, valuesProgram() }, { "panel.gui", kShaderGui }, { "still.sc", kStill } };
        request.instrument = kInstrument;
        const char* env = std::getenv((std::string(pg::product::kEnvPrefix) + "SHADERC").c_str());
        if (env != nullptr && *env != '\0')
        {
            const pg::preview::Prepared built = pg::preview::prepare(request, tools(true));
            std::printf("with shaderc: %s", built.notes.c_str());
            P.eq("shader.built", built.ok && built.spec.shaders.size() == 1 && !built.spec.shaders[0].binary.empty() ? 1 : 0, 1);
            P.eq("shader.no_note", has(built.notes, "still.sc") ? 0 : 1, 1);
            P.eq("shader.drawn", pg::preview::draw(built, 0, pg::preview::kMaxWidth).image.w == pg::preview::kMaxWidth ? 1 : 0, 1);
        }
        else
            probe::skip(P, "shader.built", "no shaderc given (<prefix>SHADERC): CTest gives it");
        const pg::preview::Prepared none = pg::preview::prepare(request, tools(false));
        std::printf("without shaderc: %s", none.notes.c_str());
        P.eq("shader.placeholder_said", none.ok && none.spec.shaders.size() == 1 && none.spec.shaders[0].binary.empty()
                                            && has(none.notes, "still.sc: note: not built") && has(none.notes, "placeholder") ? 1 : 0, 1);
        P.eq("shader.placeholder_drawn", pg::preview::draw(none, 0, pg::preview::kMaxWidth).image.w == pg::preview::kMaxWidth ? 1 : 0, 1);
    }

    // ---- FAILS -------------------------------------------------------------------------------------------------------------
    {
        pg::preview::Request request;
        request.instrument = kInstrument;
        request.files = { { pg::kProgramName, valuesProgram() } };
        // A .gui of which nothing reads: with dials, they come in a section of their own as the PANEL view shows them;
        // with none, nothing to draw, its first note the why.
        request.files.push_back({ "panel.gui", "not a panel\n" });
        const pg::preview::Prepared dialsOnly = pg::preview::prepare(request, tools(true));
        P.eq("fails.unreadable_dials_shown", dialsOnly.ok && has(dialsOnly.notes, "unknown element 'not'") ? 1 : 0, 1);
        request.files[0].text = kInstrument ? "module Voice\n{\n    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                                            : "module Effect\n{\n    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const pg::preview::Prepared broken = pg::preview::prepare(request, tools(true));
        std::printf("a .gui that does not read: %s\n", broken.why.c_str());
        P.eq("fails.unreadable", !broken.ok && broken.why.rfind("panel.gui does not read: unknown element 'not'", 0) == 0 ? 1 : 0, 1);
        const pg::preview::Picture nothing = pg::preview::draw(broken, 0, pg::preview::kMaxWidth);
        P.eq("fails.draws_nothing", nothing.image.w == 0 && nothing.why == broken.why ? 1 : 0, 1);
        request.files.clear();
        const pg::preview::Prepared empty = pg::preview::prepare(request, tools(true));
        P.eq("fails.no_files", !empty.ok && empty.why == "the version has no files" ? 1 : 0, 1);
    }
    pg::picture::forceSoftRasterForTests(false);
    return P.finish();
}
