#pragma once

#include <QImage>
#include <QString>
#include <QUrl>

#include <vector>

// "TalQ for your phone" (Settings > Account): one link, shown as a QR code that a
// phone camera opens. This header is the whole decision and drawing half, so the
// dialog (src/ui/PhoneAppDialog) and phone-app-code-test use the same code.
// The encoder is the vendored, unmodified third_party/qrcodegen.
namespace talq::phoneapp {

// The white margin round the symbol, in modules: the four the QR standard asks for.
// A camera finds the code by this margin; without it, a dark window behind the
// dialog can merge with the outer modules.
inline constexpr int kQuietModules = 4;

// Below this many device pixels per module, a phone held at arm's length starts to
// miss modules on an ordinary screen. The renderer never goes under it, even when
// the space asked for is smaller.
inline constexpr int kMinModulePx = 4;

// One encoded symbol: a square of dark and light modules.
struct Symbol {
    int size = 0;            // modules per side, 21..177; 0 = no symbol
    int version = 0;         // 1..40
    char ecc = 0;            // 'L', 'M', 'Q' or 'H'
    std::vector<bool> dark;  // row-major, size * size

    bool isNull() const { return size <= 0; }
    bool isDark(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < size && y < size
            && dark[static_cast<size_t>(y) * static_cast<size_t>(size)
                    + static_cast<size_t>(x)];
    }
};

// The link the entry opens, or an empty QUrl when `raw` is empty or is not a valid
// http/https address with a host. An empty result hides the entry: a QR code that
// leads nowhere is worse than none, and only http/https is ever opened.
QUrl phoneAppLink(const char *raw);

// Encodes `text` as UTF-8 at error correction level MEDIUM, exactly: the encoder
// is told not to raise it, so the symbol is the smallest version that holds the
// text at M. Returns a null Symbol for empty text, text with a NUL in it, or text
// too long for any version; never throws.
Symbol encode(const QString &text);

// The largest whole number of pixels per module at which the symbol and its quiet
// zone fit in `maxSidePx`, but never under kMinModulePx. 0 for a null symbol.
int modulePixelsFor(int symbolSize, int maxSidePx);

// The side of the rendered image: (size + 2 * kQuietModules) * modulePx.
int renderedSide(int symbolSize, int modulePx);

// Paints the symbol with QPainter: black modules on white, `modulePx` pixels per
// module, the quiet zone included in the image. Black on white whatever the app's
// theme, on purpose: a camera needs the contrast, and a light-on-dark code is not
// read by every scanner. Null image for a null symbol or modulePx < 1.
QImage render(const Symbol &symbol, int modulePx);

} // namespace talq::phoneapp
