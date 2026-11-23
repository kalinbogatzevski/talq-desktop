#include "core/PhoneAppCode.h"

#include "qrcodegen.hpp"

#include <QByteArray>
#include <QPainter>

#include <exception>

namespace talq::phoneapp {

QUrl phoneAppLink(const char *raw)
{
    if (!raw || !*raw)
        return {};
    const QUrl url(QString::fromUtf8(raw), QUrl::StrictMode);
    // QUrl lower-cases the scheme, so "HTTPS://" passes as "https".
    const QString scheme = url.scheme();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QLatin1String("https") && scheme != QLatin1String("http")))
        return {};
    return url;
}

Symbol encode(const QString &text)
{
    if (text.isEmpty())
        return {};
    const QByteArray utf8 = text.toUtf8();
    // The encoder takes a C string; a NUL would silently cut the link short.
    if (utf8.contains('\0'))
        return {};

    using qrcodegen::QrCode;
    using qrcodegen::QrSegment;
    try {
        const std::vector<QrSegment> segs = QrSegment::makeSegments(utf8.constData());
        const QrCode qr = QrCode::encodeSegments(segs, QrCode::Ecc::MEDIUM,
                                                 QrCode::MIN_VERSION, QrCode::MAX_VERSION,
                                                 /*mask=*/-1, /*boostEcl=*/false);
        Symbol s;
        s.size = qr.getSize();
        s.version = qr.getVersion();
        switch (qr.getErrorCorrectionLevel()) {
        case QrCode::Ecc::LOW:      s.ecc = 'L'; break;
        case QrCode::Ecc::MEDIUM:   s.ecc = 'M'; break;
        case QrCode::Ecc::QUARTILE: s.ecc = 'Q'; break;
        case QrCode::Ecc::HIGH:     s.ecc = 'H'; break;
        }
        s.dark.resize(static_cast<size_t>(s.size) * static_cast<size_t>(s.size));
        for (int y = 0; y < s.size; ++y)
            for (int x = 0; x < s.size; ++x)
                s.dark[static_cast<size_t>(y) * static_cast<size_t>(s.size)
                       + static_cast<size_t>(x)] = qr.getModule(x, y);
        return s;
    } catch (const std::exception &) {
        // data_too_long (a std::length_error) is the only one a caller can cause.
        return {};
    }
}

int modulePixelsFor(int symbolSize, int maxSidePx)
{
    if (symbolSize <= 0)
        return 0;
    const int modules = symbolSize + 2 * kQuietModules;
    const int fit = maxSidePx > 0 ? maxSidePx / modules : 0;
    return fit < kMinModulePx ? kMinModulePx : fit;
}

int renderedSide(int symbolSize, int modulePx)
{
    return (symbolSize + 2 * kQuietModules) * modulePx;
}

QImage render(const Symbol &symbol, int modulePx)
{
    if (symbol.isNull() || modulePx < 1)
        return {};
    const int side = renderedSide(symbol.size, modulePx);
    QImage img(side, side, QImage::Format_RGB32);
    img.fill(Qt::white);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    const int origin = kQuietModules * modulePx;
    for (int y = 0; y < symbol.size; ++y) {
        // One rectangle per horizontal run of dark modules: fewer calls, same pixels.
        int x = 0;
        while (x < symbol.size) {
            if (!symbol.isDark(x, y)) { ++x; continue; }
            const int start = x;
            while (x < symbol.size && symbol.isDark(x, y))
                ++x;
            p.fillRect(origin + start * modulePx, origin + y * modulePx,
                       (x - start) * modulePx, modulePx, Qt::black);
        }
    }
    p.end();
    return img;
}

} // namespace talq::phoneapp
