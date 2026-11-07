// Source/plugin/SecretStore.h: a place for secrets outside the plugin's own files, for the chat's API keys
// (plugin/ChatStore.h). On Linux it is the desktop's Secret Service (org.freedesktop.secrets over the session's D-Bus:
// GNOME Keyring, KDE's KWallet, KeePassXC), reached through libsecret, which is loaded when it is first needed
// (dlopen("libsecret-1.so.0")), so the plugin neither links it nor needs it: where it is missing, or no Secret Service
// runs, ChatStore keeps the key in its own file, encrypted with a key of the machine's (ChatStore.h). macOS and Windows
// have none here (desktop() is null): the key's file is theirs.
//
// The items: libsecret's schema "funk.playground.ApiKey", whose attributes are "kind" (the provider's: "anthropic",
// "openai", ...) and "origin" (its server's, "https://api.anthropic.com"), labelled for the keyring's own window
// ("Funk Playground: API key for https://api.anthropic.com"), in the default collection (the login keyring).
//
// Never a bus started: the desktop's store is available() only when the session's bus is there to be reached
// ($DBUS_SESSION_BUS_ADDRESS set, or $XDG_RUNTIME_DIR/bus a socket), so that GLib never autolaunches one (dbus-launch,
// under X11) for a process that has none. has() looks without unlocking anything (no prompt); lookup() and store() may
// have the desktop ask the user to unlock the keyring, and wait for the answer. Any thread; each call blocks until the
// service answers (or the bus's own timeout, 25 s).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace playground::secrets
{
    struct Item
    {
        std::string kind, origin;
    };

    class Store
    {
    public:
        virtual ~Store() = default;
        // What the store is, for a reason's words ("the desktop's keyring").
        virtual std::string name() const = 0;
        // Whether there is a store to ask now; false with why when there is none (no bus, no libsecret).
        virtual bool available(std::string* why) = 0;
        // Whether `item` is kept: 1 yes, 0 no, -1 the store could not be asked (why). Never unlocks (no prompt).
        virtual int has(const Item& item, std::string* why) = 0;
        // The secret kept for `item`; nothing when there is none (why left empty) or the store failed (why said).
        virtual std::optional<std::string> lookup(const Item& item, std::string* why) = 0;
        // `secret` kept for `item` under `label`, replacing what was; false with why.
        virtual bool store(const Item& item, const std::string& label, std::string_view secret, std::string* why) = 0;
        // `item` removed (true when it was not there either); false with why.
        virtual bool clear(const Item& item, std::string* why) = 0;
    };

    // The desktop's Secret Service through libsecret (Linux); null elsewhere. One for the process.
    Store* desktop();

    // Linux: whether the session's D-Bus can be reached without starting one ($DBUS_SESSION_BUS_ADDRESS set, or
    // $XDG_RUNTIME_DIR/bus a socket); false with why when not.
    bool sessionBus(std::string* why);
    // Linux: whether libsecret could be loaded (once for the process); false with why when not.
    bool libsecretLoaded(std::string* why);
}
