// Source/plugin/Live.h: what the live tools read of the plugin (JUCE-free types; ChatHost's chatLive and chatChanges,
// ChatAgent.h).
//
//   LiveRead  what plays now, for listen: the Engine's meters (the levels, the displays' fields, the host, the notes
//             and the voices), its scopes and its blocks' costs (Engine.h), the processor's bypass and host blocks, the
//             latched alerts and the running program's displays. Read about 25 times a second while listen listens.
//   Changes   what changed since a token, for get_status `since` (Journal.h): a line a change, oldest first, and the
//             token for next time.
//
// Both are filled from any thread, beside a running call: lock-free reads (the seqlock, the rings, atomics) and getters
// under their own mutexes (CLAUDE.md, "A live tool").
#pragma once

#include "dsp/Engine.h"
#include "fminus/PanelSpec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace playground
{
    struct LiveRead
    {
        uint64_t blocksFrom = 0;                  // in: where the last read's blocks ended (0 at first)

        bool metered = false;                     // `meters` was read (false while the audio thread publishes)
        Meters meters{};
        std::vector<float> in, out;               // the scopes' last Engine::kScopeLength samples, mono, oldest first
        std::vector<BlockStat> blocks;            // the blocks since blocksFrom (the ring's last kBlockRing at most)
        uint64_t blocksTo = 0;                    // the next read's blocksFrom
        bool bypassed = false;                    // the host's bypass, as of its last block
        int largestBlock = 0;                     // the largest host block since prepareToPlay
        int announcedBlock = 0;                   // the block prepareToPlay announced (0: not prepared)
        uint64_t hostBlocks = 0;                  // the host's blocks so far (processBlock calls)
        float rate = 48000.0f;
        float load = 0.0f;                        // Engine::load: the busiest recent block's share
        uint32_t alerts = 0;                      // the output guard's alerts of the last two seconds (Engine::Alert)
        uint64_t program = 0;                     // the program that plays (its compile, as Meters::program names it)
        std::vector<PanelDisplay> displays;       // its displays
    };

    struct Changes
    {
        std::string token;                        // where they end: the next call's since
        bool known = false;                       // `since` was this journal's token (else no lines: the caller has
                                                  // never looked, or looked at another instance, or before a reload)
        bool complete = true;                     // nothing after `since` was dropped (the journal keeps the newest)
        std::vector<std::string> lines;           // a change a line, oldest first ("14:02:31-33 you moved cutoff ...")
        int changes = 0;                          // of them, what the user, the host or the plugin's chat changed
        int alerts = 0;                           // the output guard's
    };
}
