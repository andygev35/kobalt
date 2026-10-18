#pragma once
#include <QString>

// Remote progress travels through the reader as a ReadingProgress, so the
// existing "Keep reading here / Go to synced position" choice applies to it.
// Its location is `kosync:<fraction>|<xpointer>`. Header-only, because both the
// reading library and the Kobo position codec read it.
namespace kobalt::kosync {
inline constexpr char locationPrefix[] = "kosync:";
inline bool isLocation(const QString &location) {
    return location.startsWith(QLatin1String(locationPrefix));
}
inline QString makeLocation(const QString &xpointer, double percentage) {
    return QLatin1String(locationPrefix) + QString::number(qBound(0.0, percentage, 1.0), 'f', 6) + QLatin1Char('|') +
           xpointer;
}
// False for a foreign location or one without a fraction.
inline bool splitLocation(const QString &location, QString *xpointer, double *percentage) {
    if (!isLocation(location))
        return false;
    const auto body = location.mid(int(sizeof(locationPrefix)) - 1);
    const int bar = body.indexOf(QLatin1Char('|'));
    bool ok = false;
    const double fraction = bar < 0 ? 0.0 : body.left(bar).toDouble(&ok);
    if (bar < 0 || !ok)
        return false;
    if (xpointer)
        *xpointer = body.mid(bar + 1);
    if (percentage)
        *percentage = fraction;
    return true;
}
}
