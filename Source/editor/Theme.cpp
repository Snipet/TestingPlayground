// Source/editor/Theme.cpp: see Theme.h. The product's colours, its kind's: the one place the theme meets Product.h.
#include "editor/Theme.h"

#include "Product.h"

namespace playground::ui
{
    funkgui::Theme productTheme(const funkgui::Theme& host)
    {
        return kindTheme(host, product::kIsInstrument);
    }

    funkgui::Col codeSecond(const funkgui::Theme& th)
    {
        return kindCodeSecond(th, product::kIsInstrument);
    }
}
