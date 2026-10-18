#pragma once
#include "reading_progress.h"

namespace kobalt {
// GUI-thread services for a platform's existing reading database and activity
// recorder. A standalone reader has no bridge; local state still belongs to
// ReadingBook. The bridge never owns the engine session or publication access.
class ReadingBridge {
public:
    virtual ~ReadingBridge() = default;
    virtual bool deviceTransition() const = 0;
    virtual bool writable() const = 0;
    virtual ReadingProgress read(QString *error = nullptr) const = 0;
    virtual bool takeChange() = 0;
    virtual bool write(const ReadingProgress &expected, const QString &location, double percent, bool finished,
                       ReadingProgress *verified, QString *error) = 0;
    virtual void observeReading(double percent, const QString &position) = 0;
    virtual void stopReading() = 0;
    // Whether the platform may queue a saved position for its own cloud sync.
    // A bridge without cloud sync ignores this.
    virtual void setCloudSync(bool) {}
};
}
