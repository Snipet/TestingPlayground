#pragma once

#include "GraphPoint.h"

// frame.h transitively brings in canvas.h → gradient.h, which palette.h needs
// for Brush / GradientPosition (it doesn't include them itself).
#include "visage_ui/frame.h"
#include "visage_graphics/gradient.h"
#include "visage_graphics/palette.h"
#include "visage_widgets/graph_line.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace graph
{
    // The graph editor is composed of three regions:
    //   - Toolbar (top): File / Edit popup menus
    //   - Canvas  (left/centre): grid + GraphLine + draggable point handles
    //   - SidePanel (right): LFO/OneShot pill toggle + numeric settings
    //
    // Mouse interactions on the canvas:
    //   - left-click empty space     → add a new point at the cursor
    //   - left-click on a handle     → select + start drag (hold Alt to snap)
    //   - left-drag between handles  → bend the curve of that segment
    //   - right-click on a handle    → delete the point
    //   - keyboard Delete            → delete the selected point
    class GraphEditor : public visage::Frame
    {
    public:
        // Mode picker on the side panel.
        enum class Mode { LFO, OneShot };

        GraphEditor();
        ~GraphEditor() override;

        void resized() override;
        void draw(visage::Canvas& canvas) override;

        // External callbacks the host (PluginEditor) can install to talk to
        // the OS — file dialogs and the like. Each takes a continuation so
        // they can be async (juce::FileChooser::launchAsync).
        std::function<void(std::function<void(std::string)>)> requestSavePath;
        std::function<void(std::function<void(std::string)>)> requestLoadPath;

        // Public data access for hosts that want to read the curve out.
        const std::vector<Point>& points() const { return points_; }
        Mode mode() const { return mode_; }
        float latencyMs() const { return latency_ms_; }

    private:
        // -------- Sub-frames ------------------------------------------------
        class Toolbar;        // File / Edit popup menu host
        class Canvas;         // Grid + GraphLine + point handles
        class PointsOverlay;  // Drawn on top of GraphLine; renders handles
        class SidePanel;      // Mode toggle + latency + extra settings
        class SubdivControl;  // Tiny bottom-right grid subdivisions chooser
        class GraphStrip;     // Bottom-of-canvas multi-graph switcher

        // -------- Data model -----------------------------------------------
        // points_ is the *live* curve currently being edited; graphs_ owns the
        // saved curves for each tile in the bottom strip. The pair is kept in
        // sync by commitActive()/loadActive(): edits write through to
        // graphs_[active_graph_] on every rebuildLineSamples(), so the
        // preview tiles always reflect the latest state.
        std::vector<Point>              points_;
        std::vector<std::vector<Point>> graphs_;
        int  active_graph_     = 0;

        int  selected_         = -1;     // -1 = none
        int  hover_point_      = -1;     // updated by Canvas::mouseMove
        int  hover_mid_        = -1;     // midpoint indicator under the mouse
        float paste_target_x_  = 0.5f;   // normalised x where Paste should insert
        int  subdivisions_     = 8;
        Mode mode_             = Mode::LFO;

        // Side-panel settings, kept here so the panel UI is the view.
        float latency_ms_  = 0.0f;
        float rate_hz_     = 1.0f;
        float amplitude_   = 1.0f;
        float phase_       = 0.0f;

        // Clipboard for Copy/Cut/Paste of a point's y value. We also use the
        // OS clipboard via Frame::setClipboardText/readClipboardText so it
        // survives outside the plugin, but the live value here is canonical.
        bool  clipboard_has_value_ = false;
        float clipboard_value_     = 0.5f;

        // -------- Geometry helpers (used by sub-frames) --------------------
        void resetToDefault();
        void sortPointsByX();
        int  pointAtScreen(float local_x, float local_y, float radius_px) const;
        int  midpointAtScreen(float local_x, float local_y, float radius_px) const;

        // Called whenever points_ changes — pushes samples into the GraphLine.
        void rebuildLineSamples();

        // Trigger a repaint of the central graph + handle overlay.
        void redrawCanvas();

        // Multi-graph management.
        void switchToGraph(int idx);   // commit current, load graphs_[idx]
        void addGraph();                // append a default curve, switch to it
        void deleteGraph(int idx);      // remove graphs_[idx] (silently no-ops
                                        // if it's the only graph remaining)
        void commitActive();            // sync points_ → graphs_[active_graph_]

        // Toolbar actions.
        void actionNew();
        void actionSave();
        void actionSaveAs();
        void actionCopy();
        void actionCut();
        void actionPaste(float at_norm_x);
        void actionDelete();

        bool saveToFile(const std::string& path) const;
        bool loadFromFile(const std::string& path);

        // -------- Children --------------------------------------------------
        std::unique_ptr<Toolbar>      toolbar_;
        std::unique_ptr<Canvas>       canvas_;
        std::unique_ptr<SidePanel>    side_;
        std::unique_ptr<GraphStrip>   strip_;

        visage::Palette palette_;

        // Path that "Save" (without As) writes to. Set by Save As / Load.
        std::string current_path_;

        VISAGE_LEAK_CHECKER(GraphEditor)
    };
}
