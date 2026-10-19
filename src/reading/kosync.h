#pragma once
#include "kosync_location.h"
#include "reading_progress.h"
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkRequest;

// KOReader's kosync protocol: a book is identified by a hash of its file, and
// the server keeps one progress record per user and book. Progress is a
// CREngine XPointer plus a fraction, the same values KOReader sends, so any
// kosync-compatible reader sharing the same EPUB bytes sees the same position.
namespace kobalt::kosync {
// kosync.json in the Kobalt directory. Only `server`, `username` and either
// `password` or `userkey` (the password's MD5, as KOReader stores it) are needed.
struct Config {
    QUrl server;
    QString username, userKey, deviceName, deviceId;
    int timeoutMilliseconds = 8000;
    // Kosync is the only source of cross-device position. Changes that Nickel's
    // own sync brings are not offered, and Kobalt does not queue Kobo Sync sends.
    bool replaceKoboSync = true;
    bool enabled = false;
    // Why a present file is unusable. Empty when the file is absent or usable.
    QString error;
    bool usable() const {
        return enabled && error.isEmpty() && server.isValid() && !username.isEmpty() && !userKey.isEmpty() &&
               !deviceId.isEmpty();
    }
};
// A missing kosync.json means kosync is not configured. The device file keeps a
// generated device ID, which is created on first use and never edited by hand.
Config loadConfig(const QString &configPath, const QString &devicePath);
Config parseConfig(const QByteArray &json, QString deviceId);

// The login as the settings screen edits it. The password is never read back:
// an empty one on save keeps whatever kosync.json already holds.
struct Login {
    QString server, username, password;
};
// What kosync.json holds, read leniently for the settings screen.
struct StoredLogin {
    QString server, username;
    bool hasPassword = false, enabled = true;
};
StoredLogin readLogin(const QString &configPath);
// Writes the login and turns kosync on, keeping every other key in the file.
// Returns false with `error` set when the login is unusable or cannot be saved.
bool writeLogin(const QString &configPath, const Login &login, QString *error);
// A Config for a login that is not yet saved. An empty password falls back to
// the one in the file at `configPath`, if there is one.
Config configFor(const Login &login, const QString &configPath = QString());

// KOReader's default "Binary" document ID: the MD5 of up to 1024 bytes at each of
// the offsets 0, 1 KiB, 4 KiB, 16 KiB ... 1 GiB, stopping at the end of the file.
// Empty when the file cannot be read.
QString partialMd5(const QString &path);
// KOReader's "Filename" method, selected there instead of Binary.
QString filenameMd5(const QString &path);

struct Progress {
    QString document, progress, device, deviceId;
    // 0 to 1, as KOReader sends it.
    double percentage = 0;
    // Server clock in seconds; zero for a value that has not been to the server.
    qint64 timestamp = 0;
    bool valid() const {
        return !document.isEmpty() && (!progress.isEmpty() || percentage > 0);
    }
};
QByteArray encodeProgress(const Progress &progress);
// Tolerates servers that send numbers as strings. Invalid when nothing usable arrived.
Progress parseProgress(const QByteArray &body);

// See kosync_location.h for the location format that carries it through the reader.
ReadingProgress toReadingProgress(const QString &book, const Progress &remote);

enum class Verdict {
    Ignore,      // nothing usable
    Acknowledge, // already here, or written by this device
    KeepLocal,   // a local position the server never received is newer
    Prompt       // another device is somewhere else; ask before moving
};
// `unsentAtMilliseconds` is when this device last failed to deliver a position,
// or zero. The server's timestamp is its own clock, so only a coarse order is used.
Verdict classify(const Progress &remote, const QString &deviceId, const QString &localXpointer,
                 qint64 unsentAtMilliseconds);

enum class Status { Ok, NotFound, Unauthorized, Unavailable, Invalid };
struct PullResult {
    Status status = Status::Unavailable;
    Progress progress;
    QString error;
    int http = 0;
};
struct AuthResult {
    Status status = Status::Unavailable;
    int http = 0;
    QString error;
};
struct PushResult {
    Status status = Status::Unavailable;
    qint64 timestamp = 0;
    QString error;
    int http = 0;
};

// Requests belong to a manager that outlives any book, so a position sent while
// closing still completes. Construct and call on the GUI thread. Each callback
// runs once, on that thread, including after a timeout.
class Client {
public:
    // A null manager uses one shared by the whole process.
    explicit Client(Config config, QNetworkAccessManager *manager = nullptr);
    const Config &config() const {
        return config_;
    }
    void pull(const QString &document, std::function<void(PullResult)> done) const;
    void push(const Progress &progress, std::function<void(PushResult)> done) const;
    // Asks the server whether the username and key are accepted.
    void authenticate(std::function<void(AuthResult)> done) const;
    // The request without a body, for inspection.
    QNetworkRequest request(const QString &path) const;

private:
    Config config_;
    QNetworkAccessManager *manager_;
};
}
