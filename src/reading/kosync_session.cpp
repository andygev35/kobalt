#include "kosync_session.h"
#include "storage/reader_storage.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QDateTime>

namespace kobalt::kosync {
namespace {
constexpr int maxUnsentBooks = 32;

struct Unsent {
    QString progress;
    double percentage = 0;
    qint64 at = 0;
};
QJsonObject readStore(const QString &path) {
    return path.isEmpty() ? QJsonObject() : storage::readJson(path);
}
qint64 unsentTime(const QString &path, const QString &document) {
    return qint64(readStore(path).value(document).toObject().value("at").toString().toLongLong());
}
void writeUnsent(const QString &path, const QString &document, const Unsent &unsent) {
    if (path.isEmpty())
        return;
    auto store = readStore(path);
    store.insert(document, qt::object({{"progress", unsent.progress},
                                       {"percentage", unsent.percentage},
                                       {"at", QString::number(unsent.at)}}));
    // Drop the oldest entries so the file stays small however many books were read.
    while (store.size() > maxUnsentBooks) {
        QString oldest;
        qint64 oldestAt = 0;
        for (auto it = store.begin(); it != store.end(); ++it) {
            const auto at = it.value().toObject().value("at").toString().toLongLong();
            if (oldest.isEmpty() || at < oldestAt) {
                oldest = it.key();
                oldestAt = at;
            }
        }
        store.remove(oldest);
    }
    storage::writeJson(path, store);
}
// A later failure for the same book stays recorded; only the delivered one is removed.
void clearUnsent(const QString &path, const QString &document, qint64 at) {
    if (path.isEmpty())
        return;
    auto store = readStore(path);
    if (!store.contains(document) || store.value(document).toObject().value("at").toString().toLongLong() > at)
        return;
    store.remove(document);
    storage::writeJson(path, store);
}
}

struct Session::State {
    Client client;
    QString document, unsentPath;
    bool pulling = false, settled = false, sending = false, blocked = false, hasQueued = false, fresh = false;
    Progress remote;
    // What the server is known to hold for this book, and what is on its way.
    QString known, inflight;
    Unsent queued;
    qint64 unsentAt = 0;
    State(Config config, QNetworkAccessManager *manager) : client(std::move(config), manager) {}
};

Session::Session(Config config, QString document, QString unsentPath, QNetworkAccessManager *manager)
    : state(std::make_shared<State>(std::move(config), manager)) {
    state->document = std::move(document);
    state->unsentPath = std::move(unsentPath);
    state->unsentAt = unsentTime(state->unsentPath, state->document);
}
// Requests in flight keep their own reference to the state, so a position sent
// while the book closes still completes and updates the unsent record.
Session::~Session() = default;

const Config &Session::config() const {
    return state->client.config();
}
const QString &Session::document() const {
    return state->document;
}
bool Session::pulling() const {
    return state->pulling;
}
bool Session::settled() const {
    return state->settled;
}
qint64 Session::unsentAt() const {
    return state->unsentAt;
}

void Session::pull() {
    if (state->pulling || state->blocked)
        return;
    state->pulling = true;
    // The callback may run after the book has closed. It then only updates state
    // that nothing reads.
    const std::weak_ptr<State> weak = state;
    state->client.pull(state->document, [weak](PullResult result) {
        const auto self = weak.lock();
        if (!self)
            return;
        self->pulling = false;
        self->settled = true;
        diagnostic("kosync-pull", {{"status", int(result.status)}});
        if (result.status == Status::Unauthorized)
            self->blocked = true;
        if (result.status != Status::Ok)
            return;
        self->remote = result.progress;
        self->known = result.progress.progress;
        self->fresh = true;
    });
}

bool Session::takeRemote(Progress *remote) {
    if (!state->fresh)
        return false;
    state->fresh = false;
    *remote = state->remote;
    return true;
}

Session::Push Session::push(const QString &xpointer, double percentage) {
    if (state->blocked)
        return Push::Blocked;
    if (!state->settled)
        return Push::Waiting;
    if (xpointer.isEmpty() || xpointer == state->known || (xpointer == state->inflight && !state->hasQueued))
        return Push::Unchanged;
    Unsent unsent{xpointer, qBound(0.0, percentage, 1.0), QDateTime::currentMSecsSinceEpoch()};
    // Persist before sending. The device may sleep or close before the reply.
    writeUnsent(state->unsentPath, state->document, unsent);
    state->unsentAt = unsent.at;
    state->queued = unsent;
    state->hasQueued = true;
    send(state);
    return Push::Started;
}

void Session::send(const std::shared_ptr<State> &state) {
    if (state->sending || !state->hasQueued)
        return;
    const auto unsent = state->queued;
    state->hasQueued = false;
    state->sending = true;
    state->inflight = unsent.progress;
    Progress progress;
    progress.document = state->document;
    progress.progress = unsent.progress;
    progress.percentage = unsent.percentage;
    progress.device = state->client.config().deviceName;
    progress.deviceId = state->client.config().deviceId;
    const std::weak_ptr<State> weak = state;
    const auto path = state->unsentPath;
    const auto document = state->document;
    state->client.push(progress, [weak, path, document, unsent](PushResult result) {
        diagnostic("kosync-push", {{"status", int(result.status)}});
        if (result.status == Status::Ok)
            clearUnsent(path, document, unsent.at);
        const auto self = weak.lock();
        if (!self)
            return;
        self->sending = false;
        self->inflight.clear();
        if (result.status == Status::Ok) {
            if (self->unsentAt <= unsent.at)
                self->unsentAt = 0;
            self->known = unsent.progress;
        } else if (result.status == Status::Unauthorized) {
            self->blocked = true;
        }
        send(self);
    });
}
}
