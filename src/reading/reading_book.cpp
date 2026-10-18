#include "support/strings.h"
#include "reading_book.h"
#include "appearance_settings.h"
#include "reader_session.h"
#include "storage/reader_storage.h"
#include "support/diagnostics.h"
#include <QDateTime>

namespace kobalt {
ReadingBook::ReadingBook(std::unique_ptr<ReaderSession> session, QString statePath, storage::Paths paths,
                         QString identity, QByteArray digest, std::unique_ptr<ReadingBridge> bridge,
                         ReadingProgress baseline, ReadingProgress diskBaseline)
    : model(std::move(session)), statePath(std::move(statePath)), identity_(std::move(identity)),
      paths_(std::move(paths)), digest(std::move(digest)), bridge(std::move(bridge)), baseline(std::move(baseline)),
      reconciliation(std::move(diskBaseline)), kosyncReconciliation(ReadingProgress{identity_, {}, 0, 0, true}) {
    const auto saved = storage::readJson(this->statePath);
    const auto preferences = appearance::readerPreferences(
        storage::readJson(paths_.preferences()), saved.value("book").toString() == identity_ ? saved : QJsonObject());
    for (const auto *key : appearance::layoutKeys)
        if (preferences.contains(key))
            layoutPreferences_[key] = preferences.value(key);
    if (saved.value("book").toString() == identity_) {
        dictionaryChoice_ = saved.value("dictionary_id").toString();
        // The server record already decided on. Without it, an unchanged record
        // would be offered again at every opening.
        const auto seen = ReadingProgress::fromJson(saved.value("kosync_progress").toObject());
        if (seen.valid && seen.book == identity_)
            kosyncReconciliation = ProgressReconciler(seen);
    }
    // Qt 5.2 leaves a default-constructed QElapsedTimer uninitialized, so
    // isValid() is not reliable until the timer is invalidated or started.
    progressPoll.invalidate();
    activeReading.invalidate();
}
ReadingBook::~ReadingBook() = default;
bool ReadingBook::deviceTransition() const {
    return bridge && bridge->deviceTransition();
}
bool ReadingBook::canWriteNative() const {
    return bridge && bridge->writable();
}
void ReadingBook::releaseNative() {
    stopReading();
    bridge.reset();
    released = true;
}
void ReadingBook::observeReading() {
    if (released || (bridge && !bridge->writable())) {
        stopReading();
        return;
    }
    if (model->busy())
        return;
    if (model->image().isNull() || !model->error().isEmpty()) {
        stopReading();
        return;
    }
    const auto anchor = model->state().value("position").toString();
    const double percent = 100.0 * model->page() / qMax(1, model->pages());
    if (!activeReading.isValid())
        activeReading.start();
    if (bridge)
        bridge->observeReading(percent, anchor);
}
void ReadingBook::stopReading() {
    if (activeReading.isValid()) {
        completedReadingMilliseconds += activeReading.elapsed();
        activeReading.invalidate();
    }
    if (bridge)
        bridge->stopReading();
}
qint64 ReadingBook::readingMilliseconds() const {
    return completedReadingMilliseconds + (activeReading.isValid() ? activeReading.elapsed() : 0);
}
bool ReadingBook::offerFinish() const {
    if (released || (bridge && !bridge->writable()) || model->busy() || model->pages() <= 0 ||
        !model->error().isEmpty())
        return false;
    const auto anchor = model->state().value("position").toString();
    if (anchor.isEmpty() || anchor == acceptedFinishedAnchor)
        return false;
    // Nickel asks on exit at 90%. Include the final page of very short books,
    // and the last page of the text before trailing non-linear sections.
    return model->page() * 10 >= model->pages() * 9 || model->nextPage() == model->page();
}
void ReadingBook::markFinished() {
    if (offerFinish())
        acceptedFinishedAnchor = model->state().value("position").toString();
}
ReadingProgress ReadingBook::readProgress() const {
    return bridge ? bridge->read() : ReadingProgress{};
}
void ReadingBook::observeNative(const ReadingProgress &incoming) {
    reconciliation.observe(incoming);
    // With a sync server in charge, a position Nickel's own sync brought is not
    // offered. Accepting it as seen lets the next save replace it.
    if (replacesKoboSync() && reconciliation.pending())
        reconciliation.acknowledge(reconciliation.candidate());
}
void ReadingBook::observeProgress() {
    pollKosync();
    if (bridge)
        observeNative(bridge->read());
}
void ReadingBook::attachKosync(std::unique_ptr<kosync::Session> session) {
    syncServer = std::move(session);
    if (!syncServer)
        return;
    if (bridge)
        bridge->setCloudSync(!syncServer->config().replaceKoboSync);
    syncServer->pull();
}
void ReadingBook::pollKosync() {
    if (!syncServer)
        return;
    // A wake can mean another device moved on. Ask again, at most once at a time.
    if (kosyncRefresh && !syncServer->pulling()) {
        kosyncRefresh = false;
        syncServer->pull();
    }
    kosync::Progress remote;
    if (syncServer->takeRemote(&remote))
        observeKosync(std::move(remote));
}
void ReadingBook::observeKosync(kosync::Progress remote) {
    // A server without timestamps still has to order its answers.
    if (remote.timestamp <= 0)
        remote.timestamp = QDateTime::currentMSecsSinceEpoch() / 1000;
    const auto shown = kosync::toReadingProgress(identity_, remote);
    const auto anchor = model->state().value("position").toString();
    switch (kosync::classify(remote, syncServer->config().deviceId, anchor, syncServer->unsentAt())) {
    case kosync::Verdict::Ignore:
        return;
    case kosync::Verdict::Prompt:
        kosyncReconciliation.observe(shown);
        return;
    case kosync::Verdict::KeepLocal:
        kosyncReconciliation.observe(shown);
        kosyncReconciliation.acknowledge(shown);
        // This device read further than the server knows. Say so now, not at exit.
        pushKosync();
        return;
    case kosync::Verdict::Acknowledge:
        kosyncReconciliation.observe(shown);
        kosyncReconciliation.acknowledge(shown);
        return;
    }
}
void ReadingBook::pushKosync() {
    if (!syncServer)
        return;
    pollKosync();
    if (kosyncReconciliation.pending() || model->image().isNull() || !model->error().isEmpty())
        return;
    const auto anchor = model->state().value("position").toString();
    // The same measure KOReader sends: the fraction of the book through the current page.
    const double percentage = qBound(0.0, double(model->page() + 1) / qMax(1, model->pages()), 1.0);
    syncServer->push(anchor, percentage);
}
void ReadingBook::pollProgress() {
    pollKosync();
    if (!bridge)
        return;
    // Nickel's saved position changes after sync, account updates, USB and
    // wake. Check then, and otherwise once a minute.
    if (!bridge->takeChange() && progressPoll.isValid() && progressPoll.elapsed() < 60000)
        return;
    observeNative(bridge->read());
    progressPoll.restart();
    if (!reconciliation.pending() && reconciliation.baseline().valid)
        baseline = reconciliation.baseline();
}
void ReadingBook::acknowledge(const ReadingProgress &shown, bool moved) {
    if (kosync::isLocation(shown.location)) {
        kosyncReconciliation.acknowledge(shown);
        acceptedFinishedAnchor.clear();
        return;
    }
    reconciliation.acknowledge(shown);
    baseline = reconciliation.baseline();
    acceptedFinishedAnchor = moved && shown.status == 2 ? model->state().value("position").toString() : QString();
}
bool ReadingBook::writeNative(const QString &location, const QString &mappingError) {
    // Another device's position waits for a choice. Saving over it first would
    // lose the one place the reader has not yet been asked about.
    pollKosync();
    if (kosyncReconciliation.pending())
        return false;
    if (!bridge && !released) {
        nativeWriteback = "local-only";
        nativeWritebackError.clear();
        return true;
    }
    // Some plain EPUB files have positions Nickel cannot store. Record the
    // time, status and percentage; Nickel keeps its own position. Kobo's cloud
    // does not sync sideloaded EPUB files, so no other device sees the old
    // position with a new time. Other books save only locally when their
    // position cannot be mapped.
    const bool timeOnly = !mappingError.isEmpty();
    QString error = timeOnly && !plainEpub() ? mappingError : QString();
    nativeWriteback = "local-only";
    if (error.isEmpty() && canWriteNative()) {
        const auto current = bridge->read(&error);
        observeNative(current);
        if (reconciliation.pending())
            return false;
        if (reconciliation.mayWrite(current)) {
            ReadingProgress verified;
            const double percent = 100.0 * model->page() / qMax(1, model->pages());
            const bool preserveFinished = !acceptedFinishedAnchor.isEmpty() &&
                                          acceptedFinishedAnchor == model->state().value("position").toString();
            diagnostic("native-save-start");
            if (bridge->write(current, timeOnly ? QString() : location, percent, preserveFinished, &verified, &error)) {
                baseline = verified;
                reconciliation = ProgressReconciler(verified);
                nativeWriteback = timeOnly ? "time-only" : "verified";
            } else {
                // A commit can lose a race after the earlier read. Ask about
                // that update before letting the window finish its exit.
                observeProgress();
                if (reconciliation.pending() && canWriteNative())
                    return false;
            }
        } else if (error.isEmpty())
            error = strings::get("error.nickel_progress_changed_before_exit");
    } else if (error.isEmpty())
        error = strings::get("error.nickel_save_deferred_while_progress_or_device_state_is_unresolved");
    nativeWritebackError = error;
    diagnostic("native-save",
               {{"result", nativeWriteback}, {"mapping_failed", timeOnly}, {"failed", !error.isEmpty()}});
    return true;
}
bool ReadingBook::saveDictionaryChoice(const QString &id) {
    const auto previous = dictionaryChoice_;
    dictionaryChoice_ = id;
    QJsonObject snapshot;
    if (checkpoint(&snapshot))
        return true;
    dictionaryChoice_ = previous;
    return false;
}
bool ReadingBook::saveLayoutPreferences(const QJsonObject &preferences) {
    QJsonObject next;
    for (const auto *key : appearance::layoutKeys)
        if (preferences.contains(key))
            next[key] = preferences.value(key);
    const auto previous = layoutPreferences_;
    layoutPreferences_ = next;
    QJsonObject snapshot;
    if (checkpoint(&snapshot))
        return true;
    layoutPreferences_ = previous;
    return false;
}
bool ReadingBook::checkpoint(QJsonObject *snapshot) {
    auto state = model->state();
    state["book"] = identity_;
    state["fingerprint"] = QString::fromLatin1(digest);
    for (auto it = layoutPreferences_.begin(); it != layoutPreferences_.end(); ++it)
        state[it.key()] = it.value();
    if (!dictionaryChoice_.isEmpty())
        state["dictionary_id"] = dictionaryChoice_;
    state["schema"] = 1;
    state["nickel_progress"] = baseline.json();
    state["finished_position"] = acceptedFinishedAnchor;
    if (syncServer && kosyncReconciliation.baseline().modified > 0)
        state["kosync_progress"] = kosyncReconciliation.baseline().json();
    state["native_writeback"] = nativeWriteback;
    state["sync_pending"] = reconciliation.pending();
    state["native_writeback_error"] = nativeWritebackError;
    *snapshot = state;
    auto persisted = state;
    if (model->temporaryTypography())
        appearance::copy(persisted, model->persistentState());
    if (persisted == savedCheckpoint)
        return true;
    const bool saved = storage::writeJson(statePath, persisted);
    if (saved)
        savedCheckpoint = persisted;
    else
        diagnostic("local-save-failed");
    return saved;
}
}
