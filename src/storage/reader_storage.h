#pragma once
#include <QJsonObject>
#include <QString>

namespace kobalt::storage {
// Hosts choose the directories. Filenames and identity hashing are shared so
// changing a host or edition does not move an existing book's saved state.
struct Paths {
    QString directory{};
    QString startup{}, active{}, stateReport{}, sessionReport{};
    QString bookPrefix{};
    QString preferences() const;
    QString selection() const;
    QString annotations() const;
    QString layoutCache() const;
    // Parsed books, owned by CREngine. It deletes any other file it finds there.
    QString documentCache() const;
    QString bookState(const QString &identity) const;
    // Sync server login, this device's ID there, and positions it has not delivered.
    QString kosyncConfig() const;
    QString kosyncDevice() const;
    QString kosyncUnsent() const;
};
bool writeJson(const QString &path, const QJsonObject &object);

enum class ReadStatus { Ok, Missing, Unreadable, TooLarge, Invalid };
struct JsonRead {
    QJsonObject object;
    ReadStatus status = ReadStatus::Missing;
};
// Ordinary state files are small. A larger file is refused, not parsed.
inline constexpr qint64 maxJsonBytes = 16384;
// Invalid covers bad JSON and a top level that is not an object.
JsonRead readJsonFile(const QString &path, qint64 maxBytes = maxJsonBytes);
const char *readStatusName(ReadStatus status);
// The object, or an empty one after any failure. Use readJsonFile when a
// damaged file must be told apart from a missing one.
QJsonObject readJson(const QString &path);
}
