#include "reader_storage.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QCryptographicHash>

namespace kobalt::storage {
namespace {
QString child(const QString &directory, const QString &name) {
    return directory.isEmpty() ? QString() : directory + '/' + name;
}
}
QString Paths::preferences() const {
    return child(directory, "reader-settings.json");
}
QString Paths::selection() const {
    return child(directory, "config.json");
}
QString Paths::annotations() const {
    return child(directory, "annotations");
}
QString Paths::layoutCache() const {
    return child(directory, "reopen-layout.json");
}
QString Paths::documentCache() const {
    return child(directory, ".cache");
}
QString Paths::kosyncConfig() const {
    return child(directory, "kosync.json");
}
QString Paths::kosyncDevice() const {
    return child(directory, "kosync-device.json");
}
QString Paths::kosyncUnsent() const {
    return child(directory, "kosync-unsent.json");
}
QString Paths::bookState(const QString &identity) const {
    return child(directory, bookPrefix +
                                QString::fromLatin1(
                                    QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex()) +
                                ".json");
}
bool writeJson(const QString &path, const QJsonObject &object) {
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
JsonRead readJsonFile(const QString &path, qint64 maxBytes) {
    QFile file(path);
    if (!file.exists())
        return {{}, ReadStatus::Missing};
    if (!file.open(QIODevice::ReadOnly))
        return {{}, ReadStatus::Unreadable};
    maxBytes = qMax<qint64>(0, maxBytes);
    if (file.size() > maxBytes)
        return {{}, ReadStatus::TooLarge};
    // The file can grow after size() is checked. Read in chunks and stop one
    // byte past the limit, so a large limit never reserves a large buffer.
    QByteArray bytes;
    for (;;) {
        const qint64 left = maxBytes - bytes.size();
        const auto chunk = file.read(qMin<qint64>(left, 64 * 1024 - 1) + 1);
        if (chunk.isEmpty())
            break;
        bytes += chunk;
        if (bytes.size() > maxBytes)
            return {{}, ReadStatus::TooLarge};
    }
    if (file.error() != QFileDevice::NoError)
        return {{}, ReadStatus::Unreadable};
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return {{}, ReadStatus::Invalid};
    return {document.object(), ReadStatus::Ok};
}
const char *readStatusName(ReadStatus status) {
    switch (status) {
    case ReadStatus::Ok:
        return "ok";
    case ReadStatus::Missing:
        return "missing";
    case ReadStatus::Unreadable:
        return "unreadable";
    case ReadStatus::TooLarge:
        return "too-large";
    case ReadStatus::Invalid:
        return "invalid";
    }
    return "unknown";
}
QJsonObject readJson(const QString &path) {
    return readJsonFile(path).object;
}
}
