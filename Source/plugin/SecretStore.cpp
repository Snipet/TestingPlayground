// Source/plugin/SecretStore.cpp: see SecretStore.h.
#include "plugin/SecretStore.h"

#if defined(__linux__)
#include "fminus/Xdg.h"

#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#endif

namespace playground::secrets
{
#if defined(__linux__)
    namespace
    {
        // ---- libsecret's and GLib's C API, as their headers declare it (libsecret 0.19 and later) ------------------
        // Declared here rather than included, so that the build needs neither libsecret's headers nor GLib's include
        // path, and the plugin links neither: the library is loaded at run time (SecretStore.h).
        using gboolean = int;
        using gchar = char;
        using gpointer = void*;

        struct GError
        {
            uint32_t domain;                                 // GQuark
            int code;
            gchar* message;
        };

        struct GList
        {
            gpointer data;
            GList* next;
            GList* prev;
        };

        struct SchemaAttribute
        {
            const gchar* name;
            int type;                                        // SecretSchemaAttributeType: 0 a string
        };

        struct Schema
        {
            const gchar* name;
            int flags;                                       // SecretSchemaFlags: 0, the schema's name must match
            SchemaAttribute attributes[32];
            int reserved;
            gpointer reserved1, reserved2, reserved3, reserved4, reserved5, reserved6, reserved7;
        };

        constexpr int kSearchNone = 0;                       // SECRET_SEARCH_NONE: no unlocking, no secrets loaded
        constexpr int kSearchAll = 2;                        // SECRET_SEARCH_ALL: every match, not the first alone
        constexpr int kSearchUnlock = 4;                     // SECRET_SEARCH_UNLOCK: locked matches unlocked (a prompt)

        using StoreFn = gboolean (*)(const Schema*, const gchar* collection, const gchar* label, const gchar* password,
                                     void* cancellable, GError** error, ...);
        using LookupFn = gchar* (*)(const Schema*, void* cancellable, GError** error, ...);
        using ClearFn = gboolean (*)(const Schema*, void* cancellable, GError** error, ...);
        using SearchFn = GList* (*)(const Schema*, int flags, void* cancellable, GError** error, ...);
        using PasswordFreeFn = void (*)(gchar*);
        using ErrorFreeFn = void (*)(GError*);
        using ListFreeFullFn = void (*)(GList*, void (*)(gpointer));
        using UnrefFn = void (*)(gpointer);

        // The items' schema (SecretStore.h): its name and two string attributes; the rest of the 32 are empty.
        const Schema kSchema = { "funk.playground.ApiKey", 0, { { "kind", 0 }, { "origin", 0 } }, 0,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

        struct Lib
        {
            bool ok = false;
            std::string why;
            StoreFn store = nullptr;
            LookupFn lookup = nullptr;
            ClearFn clear = nullptr;
            SearchFn search = nullptr;                       // libsecret 0.19 and later; none: has() cannot ask
            PasswordFreeFn passwordFree = nullptr;
            ErrorFreeFn errorFree = nullptr;
            ListFreeFullFn listFreeFull = nullptr;
            UnrefFn unref = nullptr;
        };

        template <typename F>
        F symbol(void* handle, const char* name)
        {
            return reinterpret_cast<F>(dlsym(handle, name));
        }

        // libsecret, loaded once and kept (never unloaded: GLib's types cannot be), with GLib's few functions found
        // through it (its dependencies).
        Lib load()
        {
            Lib l;
            void* h = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
            if (h == nullptr)
            {
                const char* e = dlerror();
                l.why = std::string("libsecret is not installed (") + (e != nullptr ? e : "libsecret-1.so.0") + ")";
                return l;
            }
            l.store = symbol<StoreFn>(h, "secret_password_store_sync");
            l.lookup = symbol<LookupFn>(h, "secret_password_lookup_sync");
            l.clear = symbol<ClearFn>(h, "secret_password_clear_sync");
            l.search = symbol<SearchFn>(h, "secret_password_search_sync");
            l.passwordFree = symbol<PasswordFreeFn>(h, "secret_password_free");
            l.errorFree = symbol<ErrorFreeFn>(h, "g_error_free");
            l.listFreeFull = symbol<ListFreeFullFn>(h, "g_list_free_full");
            l.unref = symbol<UnrefFn>(h, "g_object_unref");
            l.ok = l.store != nullptr && l.lookup != nullptr && l.clear != nullptr && l.passwordFree != nullptr
                && l.errorFree != nullptr && l.listFreeFull != nullptr && l.unref != nullptr;
            if (!l.ok)
                l.why = "this libsecret lacks the functions it needs";
            return l;
        }

        const Lib& lib()
        {
            static const Lib l = load();
            return l;
        }

        // An error's words (and the error freed); "" for none.
        std::string taken(GError* e)
        {
            if (e == nullptr)
                return {};
            std::string s = e->message != nullptr && *e->message != '\0' ? e->message : "the Secret Service failed";
            lib().errorFree(e);
            return s;
        }

        const char* const kNull = nullptr;                   // the attributes' end, as a pointer in the varargs

        class Desktop final : public Store
        {
        public:
            std::string name() const override { return "the desktop's keyring"; }

            bool available(std::string* why) override
            {
                std::string w;
                const bool ok = sessionBus(&w) && libsecretLoaded(&w);
                if (!ok && why != nullptr)
                    *why = w;
                return ok;
            }

            int has(const Item& item, std::string* why) override
            {
                const Lib& l = lib();
                if (!available(why))
                    return -1;
                if (l.search == nullptr)
                {
                    if (why != nullptr)
                        *why = "this libsecret cannot search without unlocking (0.19 or later can)";
                    return -1;
                }
                GError* e = nullptr;
                GList* found = l.search(&kSchema, kSearchNone, nullptr, &e, "kind", item.kind.c_str(), "origin",
                                        item.origin.c_str(), kNull);
                if (e != nullptr)
                {
                    const std::string w = taken(e);
                    if (found != nullptr)
                        l.listFreeFull(found, l.unref);
                    if (why != nullptr)
                        *why = w;
                    return -1;
                }
                const bool any = found != nullptr;
                if (found != nullptr)
                    l.listFreeFull(found, l.unref);
                return any ? 1 : 0;
            }

            std::optional<std::string> lookup(const Item& item, std::string* why) override
            {
                const Lib& l = lib();
                if (!available(why))
                    return std::nullopt;
                GError* e = nullptr;
                gchar* got = l.lookup(&kSchema, nullptr, &e, "kind", item.kind.c_str(), "origin", item.origin.c_str(),
                                      kNull);
                if (e != nullptr)
                {
                    if (got != nullptr)
                        l.passwordFree(got);
                    const std::string w = taken(e);
                    if (why != nullptr)
                        *why = w;
                    return std::nullopt;
                }
                if (got == nullptr)
                {
                    if (why != nullptr)
                        why->clear();
                    return std::nullopt;
                }
                std::string out = got;
                l.passwordFree(got);                         // wiped, then freed
                return out;
            }

            bool store(const Item& item, const std::string& label, std::string_view secret, std::string* why) override
            {
                const Lib& l = lib();
                if (!available(why))
                    return false;
                std::string text(secret);                   // NUL-terminated for the C API
                GError* e = nullptr;
                const gboolean ok = l.store(&kSchema, nullptr, label.c_str(), text.c_str(), nullptr, &e, "kind",
                                            item.kind.c_str(), "origin", item.origin.c_str(), kNull);
                std::fill(text.begin(), text.end(), '\0');
                const std::string w = taken(e);
                if (ok == 0 || !w.empty())
                {
                    if (why != nullptr)
                        *why = w.empty() ? "the keyring did not keep it" : w;
                    return false;
                }
                return true;
            }

            bool clear(const Item& item, std::string* why) override
            {
                const Lib& l = lib();
                if (!available(why))
                    return false;
                // libsecret's clear deletes only the unlocked items that match: a locked keyring's stays, and it says
                // nothing (false, no error). So the matches are unlocked first, by a search that unlocks them (the
                // desktop asks the user; libsecret before 0.19, which cannot, by a lookup, which unlocks the first).
                // One the user leaves locked stays, which has() still finds (SecretStore.h).
                GError* e = nullptr;
                if (l.search != nullptr)
                {
                    GList* found = l.search(&kSchema, kSearchAll | kSearchUnlock, nullptr, &e, "kind", item.kind.c_str(),
                                            "origin", item.origin.c_str(), kNull);
                    if (found != nullptr)
                        l.listFreeFull(found, l.unref);
                }
                else if (gchar* got = l.lookup(&kSchema, nullptr, &e, "kind", item.kind.c_str(), "origin",
                                               item.origin.c_str(), kNull);
                         got != nullptr)
                    l.passwordFree(got);
                if (std::string w = taken(e); !w.empty())
                {
                    if (why != nullptr)
                        *why = w;
                    return false;
                }
                l.clear(&kSchema, nullptr, &e, "kind", item.kind.c_str(), "origin", item.origin.c_str(), kNull);
                const std::string w = taken(e);              // false and no error: nothing unlocked to remove
                if (!w.empty())
                {
                    if (why != nullptr)
                        *why = w;
                    return false;
                }
                return true;
            }
        };
    }

    Store* desktop()
    {
        static Desktop d;
        return &d;
    }

    bool sessionBus(std::string* why)
    {
        if (const char* a = std::getenv("DBUS_SESSION_BUS_ADDRESS"); a != nullptr && *a != '\0')
            return true;
        if (const std::string run = xdg::runtimeDir(); !run.empty())
        {
            struct stat st{};
            if (::stat((run + "/bus").c_str(), &st) == 0 && S_ISSOCK(st.st_mode))
                return true;
        }
        if (why != nullptr)
            *why = "no session bus to reach (DBUS_SESSION_BUS_ADDRESS unset, no $XDG_RUNTIME_DIR/bus)";
        return false;
    }

    bool libsecretLoaded(std::string* why)
    {
        const Lib& l = lib();
        if (!l.ok && why != nullptr)
            *why = l.why;
        return l.ok;
    }
#else
    Store* desktop() { return nullptr; }

    bool sessionBus(std::string* why)
    {
        if (why != nullptr)
            *why = "no Secret Service on this platform";
        return false;
    }

    bool libsecretLoaded(std::string* why)
    {
        if (why != nullptr)
            *why = "no libsecret on this platform";
        return false;
    }
#endif
}
