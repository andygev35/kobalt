#include "reading_bridge.h"
#include "native_progress.h"
#include "native_reading_session.h"
#include "native_lifecycle.h"

namespace kobalt::nickel {
namespace {
class Bridge final : public ReadingBridge {
public:
    Bridge(QString identity, std::unique_ptr<NativeProgress> progress)
        : progress_(std::move(progress)), reading_(std::move(identity)) {}
    bool deviceTransition() const override {
        return progress_->deviceTransition();
    }
    bool writable() const override {
        return (!NativeLifecycle::suspended() || NativeLifecycle::usbSaveAllowed() ||
                NativeLifecycle::sleepSaveAllowed()) &&
               !progress_->deviceTransition();
    }
    ReadingProgress read(QString *error) const override {
        return progress_->read(error);
    }
    bool takeChange() override {
        return progress_->takeChange();
    }
    bool write(const ReadingProgress &expected, const QString &location, double percent, bool finished,
               ReadingProgress *verified, QString *error) override {
        return progress_->write(expected, location, percent, finished, verified, error);
    }
    void observeReading(double percent, const QString &position) override {
        reading_.start(percent, position);
        reading_.page(percent, position);
    }
    void stopReading() override {
        reading_.stop();
    }
    void setCloudSync(bool enabled) override {
        progress_->setCloudSync(enabled);
    }

private:
    std::unique_ptr<NativeProgress> progress_;
    NativeReadingSession reading_;
};
}
std::unique_ptr<ReadingBridge> makeReadingBridge(QString identity, std::unique_ptr<NativeProgress> progress) {
    if (!progress)
        return {};
    return std::make_unique<Bridge>(std::move(identity), std::move(progress));
}
}
