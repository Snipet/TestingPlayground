// Source/plugin/SoundPicture.h: view_sound's picture (ChatAgent.h): a measure::View (dsp/SoundView.h) drawn as a PNG
// with JUCE's software renderer (any thread: the agent's worker draws it), and a thumbnail of it for the chat, so the
// user sees what Claude saw. 1120 x 800 pixels (about 1,200 input tokens):
//
//   top      what played and the dials; the waveform (left blue, right orange where it differs), on a scale that fits
//            its peak, and the level (green, RMS over 10 ms, 0 to -60 dBFS on the right); the note-off a pink line
//   middle   the spectrogram, 20 Hz to 20 kHz upward on a log scale, time as above, its colours dB under the loudest
//            cell (the scale at the right: yellow 0, orange -30, purple -60, black -120)
//   bottom   the held part's average spectrum (a note's harmonics marked in yellow) and a close-up of a few cycles
#pragma once

#include "plugin/ChatPort.h"

#include "dsp/SoundView.h"

#include <cstdint>
#include <string>
#include <vector>

namespace playground::soundpicture
{
    inline constexpr int kWidth = 1120, kHeight = 800;

    struct Drawn
    {
        std::vector<uint8_t> png;                 // the picture as Claude gets it
        ChatPicturePtr thumb;                     // for the chat (a tool entry's picture)
        std::string about;                        // what the picture shows, in words (the tool result's text)
    };

    // The view's ViewRequest columns, rows and spectrum columns must be those it was made with (soundpicture::request
    // gives them).
    Drawn draw(const measure::View& view);

    // A ViewRequest sized for this picture.
    measure::ViewRequest request();

    // compare's picture (ChatCompare.cpp), 1120 x 560: two sounds' levels over time (RMS over 10 ms, A blue, B orange)
    // with their difference's level (pink, `difference` one value a column, dBFS), and their average spectra on one
    // scale; the octave bands' differences (B against A, dB, NaN for none) along its foot.
    inline constexpr int kPairHeight = 560;
    Drawn drawPair(const measure::View& a, const measure::View& b, const std::vector<float>& difference,
                   const std::vector<double>& bandsDb, const std::string& title, const std::string& about);
}
