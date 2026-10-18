#pragma once
#include "kosync.h"
#include <memory>

namespace kobalt::kosync {
// One open book's conversation with the sync server, on the GUI thread. It
// never moves the reader: it fetches the server's record and hands it over, and
// it sends positions the reader gives it. Nothing here blocks on the network.
//
// Sending waits for the first fetch to finish, because a position sent before
// the server's newer one has been seen would overwrite it. A position that
// could not be delivered is remembered in `unsentPath` and wins over an older
// server record at the next opening.
class Session {
public:
    // `manager` is for tests; null uses the process-wide manager.
    Session(Config config, QString document, QString unsentPath, QNetworkAccessManager *manager = nullptr);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    const Config &config() const;
    const QString &document() const;

    // Starts a fetch unless one is running. The result arrives through takeRemote().
    void pull();
    // True while a fetch is running.
    bool pulling() const;
    // True once the first fetch has finished, with or without a record.
    bool settled() const;
    // The newest fetched record, once. False when nothing new arrived.
    bool takeRemote(Progress *remote);

    // When this device last failed to deliver a position, in milliseconds; zero if none.
    qint64 unsentAt() const;

    enum class Push {
        Started,   // sent, or queued behind a send in progress
        Unchanged, // the server already has this position
        Waiting,   // the first fetch has not finished
        Blocked    // the server refused this login
    };
    // `xpointer` is the engine position; `percentage` runs from 0 to 1.
    Push push(const QString &xpointer, double percentage);

private:
    struct State;
    std::shared_ptr<State> state;
    static void send(const std::shared_ptr<State> &state);
};
}
