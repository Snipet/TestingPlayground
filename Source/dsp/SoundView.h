// Source/dsp/SoundView.h: what the chat's view_sound shows Claude (JUCE-free; plugin/SoundPicture.cpp draws it): the
// program rendered offline (measure::Session, Measure.h) and analysed for a picture, since Claude cannot hear but can
// see. An instrument plays the notes held and released; an effect plays one input (a sweep from 20 Hz to 20 kHz, noise,
// an impulse, a sine or silence) for holdSeconds, then silence for tailSeconds.
//
//   the waveform   each column's least and greatest sample, left and right (the right drawn too where it differs), and
//                  the level (RMS over 10 ms, dBFS) at each column
//   spectrogram    `rows` rows on a log frequency axis from 20 Hz to 20 kHz (or the Nyquist frequency) by `columns`
//                  columns in time (an 85 ms Blackman-Harris window about each: 4096 points at 48 kHz), in dB under
//                  the loudest cell, down to -120
//   the spectrum   the held part's average (Welch: 171 ms Blackman-Harris windows, 8192 points at 48 kHz, half
//                  overlapped; a part shorter than one, one window over it alone, nothing from outside it), on the same
//                  axis, in dB under its loudest; with one note, its harmonics' frequencies, to mark
// The windows are fixed in seconds as nearly as a power of two allows (the 48 kHz sizes scaled by the rate, so 44.1 kHz
// keeps them): a picture at 96 kHz shows the detail of one at 48 kHz.
//   a close-up     a few cycles of the held sound (four of the lowest note; an effect's: of its sine, or 10 ms), or the
//                  stretch from..to that the request asks for
//
// A request's window (Request::window, Measure.h: seconds of the render, the tail included, for an effect too, and the
// render then lasts at least until its end) replaces the held part: the average spectrum and the RMS are over it, the
// default close-up is at its middle, and the picture shades it (View::windowFrom, windowTo).
//
// view() is renderSound() then analyse(): a sound recorded instead of rendered (a capture's) is analysed and
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
        bool keepInput = false;                   // an effect's Sound keeps its input (trace's export)
    };

    struct View
    {
        bool compiled = false;
        std::string errors;                       // the compiler's messages when it did not
        double rate = 48000.0;
        std::size_t length = 0;                   // samples rendered
        double heldUntil = -1.0;                  // seconds: the note-off (an effect's input's end; -1 for none)
        double windowFrom = -1.0, windowTo = -1.0;   // seconds: the window the spectrum and the RMS are over (-1: none)
        std::string title;                        // what played ("Note 48 (C3) at velocity 0.80, held 1.00 s, ...")
        std::string dials;                        // "cutoff 1200, resonance 0.3"
        uint32_t alerts = 0;
        std::string stopped;                      // a render that stopped (Patch.h, "The loop budget", "Bounds
                                                  // checks"), in words; "" none
        std::string hash;                         // view(): the render's hash (Export.h, 64 hex digits; "" from analyse()),
        std::string hashed;                       // over its own length (Sound::plain), and what it is of, in words
        double peakDb = -240.0, rmsDb = -240.0;   // the whole render's peak, and the held part's RMS (the window's)

        std::vector<float> minL, maxL, minR, maxR;   // the waveform: per column
        bool stereo = false;                      // left and right differ
        std::vector<float> levelDb;               // per column, dBFS

        int rows = 0, columns = 0;
        double loHz = 20.0, hiHz = 20000.0;       // the frequency axis (log)
        std::vector<float> cells;                 // rows x columns, row 0 the lowest: dB under the loudest cell, >= -120

        std::vector<float> spectrum;              // the held part's average (the window's), per spectrum column: dB
                                                  // under its loudest
        float spectrumTop = -300.0f;              // that loudest column, in dB of power on the 48 kHz frame's scale at
                                                  // every rate (two views' spectra drawn on one scale: compare's picture,
                                                  // at two rates too)
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
        Patch::Method stopCall = Patch::Method::none; // a call that ran past its budget (Session::Render's), and when
        std::size_t stopAt = 0;
        Patch::Bounds stopBounds;                 // or the bounds check it failed, if one did
        double cycleHz = 0.0;                     // the close-up shows four cycles of it (0: 10 ms)
        bool harmonics = false;                   // mark cycleHz's harmonics on the spectrum
        bool fromStart = false;                   // the close-up at the start (an impulse's response)
        bool cancelled = false;                   // its render was cancelled (Session::setCancel: Session::cancelWords)
        std::vector<float> inL, inR;              // an effect's input as it went in, as long as l (ViewRequest::keepInput;
                                                  // inR empty: inL on both)
        std::size_t plain = 0;                    // renderSound's: the samples it has without the window's end (a window
                                                  // past it renders on: Timeline::plain); the render hash's (0: all)
    };
    Sound renderSound(const Sources& files, const ViewRequest& request);
    Sound renderSound(Session& session, const ViewRequest& request);   // one compile, rendered again (at its rate)
    Sound renderSound(Session& session, const ViewRequest& request, RenderHooks* hooks);   // its calls watched (Measure.h)
    // A sound's analysis for the picture (columns, rows, spectrum columns and the close-up from `shape`): a render's or
    // a recording's. heldUntil in seconds, -1 for none (then the whole is "held").
    View analyse(const std::vector<float>& l, const std::vector<float>& r, double rate, double heldUntil,
                 const ViewRequest& shape = {}, double cycleHz = 0.0, bool harmonics = false, bool fromStart = false);

    View view(const Sources& files, const ViewRequest& request);
    // The same in a Session of the caller's (one it may cancel: Session::setCancel; a cancelled render's View is analysed
    // all the same, so the caller asks session.cancelled() first).
    View view(Session& session, const ViewRequest& request);
}
