// Source/dsp/SoundView.h: what the chat's view_sound shows Claude (JUCE-free; plugin/SoundPicture.cpp draws it): the
// program rendered offline (measure::Session, Measure.h) and analysed for a picture, since Claude cannot hear but can
// see. An instrument plays the notes held and released; an effect plays one input (a sweep from 20 Hz to 20 kHz, noise,
// an impulse, a sine or silence) for holdSeconds, then silence for tailSeconds.
//
//   the waveform   each column's least and greatest sample, left and right (the right drawn too where it differs), and
//                  the level (RMS over 10 ms, dBFS) at each column
//   spectrogram    `rows` rows on a log frequency axis from 20 Hz to 20 kHz (or the Nyquist frequency) by `columns`
//                  columns in time (a 4096-point Blackman-Harris window about each), in dB under the loudest cell,
//                  down to -120
//   the spectrum   the held part's average (Welch: 8192-point Blackman-Harris windows, half overlapped), on the same
//                  axis, in dB under its loudest; with one note, its harmonics' frequencies, to mark
//   a close-up     a few cycles of the held sound (four of the lowest note; an effect's: of its sine, or 10 ms), or the
//                  stretch from..to that the request asks for
//
// view() is renderSound() then analyse(): a sound recorded instead of rendered (the capture lane's) is analysed and
// drawn the same way. A request's scenario (Measure.h) plays as measure_output plays it: an instrument's phrase (the
// held part its first note-on to its last note-off), an effect's input chosen by Request::signal or its own
// (Request::input). The render lasts until the scenario's last move or edit and the tail after it, if that is later.
//
// Off the audio thread only. Deterministic.
#pragma once

#include "dsp/Measure.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace playground::measure
{
    enum class ViewInput : uint8_t { sweep, noise, impulse, sine, silence };   // an effect's

    struct ViewRequest
    {
        Request render;                           // the notes, the velocity, the times, the dials, the host
        ViewInput input = ViewInput::sweep;
        float frequency = 220.0f;                 // the sine's, Hz
        float from = -1.0f, to = -1.0f;           // the close-up, in seconds from the start (-1: a few cycles)
        int columns = 1000;                       // the waveform's and the spectrogram's
        int rows = 300;                           // the spectrogram's
        int spectrumColumns = 520;                // the average spectrum's
        std::size_t length = 0;                   // samples rendered (0: as the request plays; compare: a recording's)
    };

    struct View
    {
        bool compiled = false;
        std::string errors;                       // the compiler's messages when it did not
        double rate = 48000.0;
        std::size_t length = 0;                   // samples rendered
        double heldUntil = -1.0;                  // seconds: the note-off (an effect's input's end; -1 for none)
        std::string title;                        // what played ("Note 48 (C3) at velocity 0.80, held 1.00 s, ...")
        std::string dials;                        // "cutoff 1200, resonance 0.3"
        uint32_t alerts = 0;
        double peakDb = -240.0, rmsDb = -240.0;   // the whole render's peak, and the held part's RMS

        std::vector<float> minL, maxL, minR, maxR;   // the waveform: per column
        bool stereo = false;                      // left and right differ
        std::vector<float> levelDb;               // per column, dBFS

        int rows = 0, columns = 0;
        double loHz = 20.0, hiHz = 20000.0;       // the frequency axis (log)
        std::vector<float> cells;                 // rows x columns, row 0 the lowest: dB under the loudest cell, >= -120

        std::vector<float> spectrum;              // the held part's average, per spectrum column: dB under its loudest
        float spectrumTop = -300.0f;              // that loudest column, in dB of power (two views' spectra drawn on one
                                                  // scale: compare's picture)
        std::vector<double> harmonics;            // one note: its harmonics' Hz below hiHz (at most 40)

        std::size_t zoomFrom = 0, zoomTo = 0;     // the close-up's samples
        std::vector<float> zoomL, zoomR;
    };

    // The row (0 at loHz) of a frequency on a View's axis of `rows`, and back.
    double rowOf(const View& v, double hz);
    double hzOfRow(const View& v, double row);

    // A sound rendered for a view: what played, and what the close-up and the spectrum's marks follow.
    struct Sound
    {
        bool compiled = false;
        std::string errors;
        double rate = 48000.0;
        std::vector<float> l, r;
        double heldUntil = -1.0;                  // seconds (-1: none)
        std::string title, dials;
        std::vector<std::pair<std::string, float>> values;   // the dials, plain, by name
        uint32_t alerts = 0;
        double cycleHz = 0.0;                     // the close-up shows four cycles of it (0: 10 ms)
        bool harmonics = false;                   // mark cycleHz's harmonics on the spectrum
        bool fromStart = false;                   // the close-up at the start (an impulse's response)
    };
    Sound renderSound(const Sources& files, const ViewRequest& request);
    Sound renderSound(Session& session, const ViewRequest& request);   // one compile, rendered again (at its rate)
    // A sound's analysis for the picture (columns, rows, spectrum columns and the close-up from `shape`): a render's or
    // a recording's. heldUntil in seconds, -1 for none (then the whole is "held").
    View analyse(const std::vector<float>& l, const std::vector<float>& r, double rate, double heldUntil,
                 const ViewRequest& shape = {}, double cycleHz = 0.0, bool harmonics = false, bool fromStart = false);

    View view(const Sources& files, const ViewRequest& request);
}
