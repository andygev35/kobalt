#include "kosync.h"
#include "storage/reader_storage.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUuid>

namespace kobalt::kosync {
namespace {
QString md5Hex(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex());
}
double number(const QJsonValue &value) {
    return value.isString() ? value.toString().toDouble() : value.toDouble();
}
QNetworkAccessManager *sharedManager() {
    static QPointer<QNetworkAccessManager> manager;
    if (!manager)
        manager = new QNetworkAccessManager(qApp);
    return manager;
}
Status statusFor(int http) {
    if (http == 401 || http == 403)
        return Status::Unauthorized;
    if (http >= 200 && http < 300)
        return Status::Ok;
    if (http == 404)
        return Status::NotFound;
    // 0 is no response at all; 5xx is a server or gateway that cannot answer now.
    return http == 0 || http >= 500 ? Status::Unavailable : Status::Invalid;
}
struct Answer {
    int http = 0;
    QByteArray body;
    QString error;
};
// Starts a reply with a deadline and reports once. The reply owns its timer and
// connection context, so nothing remains after it is deleted.
void watch(QNetworkReply *reply, int timeoutMilliseconds, std::function<void(Answer)> done) {
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done] {
        Answer answer;
        answer.http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // A progress record is a few hundred bytes. Never buffer more than this.
        if (reply->isOpen())
            answer.body = reply->read(64 * 1024);
        if (reply->error() != QNetworkReply::NoError)
            answer.error = reply->errorString();
        reply->deleteLater();
        done(std::move(answer));
    });
    timer->start(timeoutMilliseconds);
}
}

Config parseConfig(const QByteArray &json, QString deviceId) {
    Config config;
    config.deviceId = std::move(deviceId);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        config.error = QStringLiteral("kosync.json is not a JSON object");
        return config;
    }
    const auto object = document.object();
    config.enabled = object.value("enabled").toBool(true);
    auto server = object.value("server").toString().trimmed();
    if (!server.isEmpty() && !server.contains(QStringLiteral("://")))
        server.prepend(QStringLiteral("https://"));
    config.server = QUrl(server, QUrl::StrictMode);
    const auto scheme = config.server.scheme();
    if (!config.server.isValid() || config.server.host().isEmpty() || (scheme != "http" && scheme != "https")) {
        config.error = QStringLiteral("kosync.json needs an http or https server address");
        return config;
    }
    config.username = object.value("username").toString();
    const auto password = object.value("password").toString();
    const auto key = object.value("userkey").toString().trimmed().toLower();
    config.userKey = key.isEmpty() ? (password.isEmpty() ? QString() : md5Hex(password.toUtf8())) : key;
    if (config.username.isEmpty() || config.userKey.isEmpty()) {
        config.error = QStringLiteral("kosync.json needs a username and a password or userkey");
        return config;
    }
    config.deviceName = object.value("device_name").toString().trimmed();
    if (config.deviceName.isEmpty())
        config.deviceName = QStringLiteral("Kobalt");
    config.timeoutMilliseconds = qBound(1, object.value("timeout_seconds").toInt(8), 60) * 1000;
    config.replaceKoboSync = object.value("replace_kobo_sync").toBool(true);
    return config;
}

Config loadConfig(const QString &configPath, const QString &devicePath) {
    QFile file(configPath);
    if (configPath.isEmpty() || !file.exists())
        return {};
    const auto read = storage::readJsonFile(configPath);
    if (read.status != storage::ReadStatus::Ok) {
        Config config;
        config.error = QStringLiteral("kosync.json could not be read: ") + storage::readStatusName(read.status);
        return config;
    }
    auto deviceId = storage::readJson(devicePath).value("device_id").toString();
    if (deviceId.isEmpty()) {
        // KOReader shows the device by name; the ID tells this device's own
        // earlier positions from another device's. It must survive restarts.
        deviceId = QUuid::createUuid().toString().remove('{').remove('}').remove('-').toUpper();
        if (devicePath.isEmpty() || !storage::writeJson(devicePath, qt::object({{"device_id", deviceId}}))) {
            Config config;
            config.error = QStringLiteral("the kosync device ID could not be saved");
            return config;
        }
    }
    return parseConfig(QJsonDocument(read.object).toJson(QJsonDocument::Compact), deviceId);
}

QString partialMd5(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Md5);
    // KOReader computes `lshift(1024, 2 * i)` for i = -1..10 with 32-bit shifts.
    // A negative count wraps modulo 32, so i = -1 lands on offset 0.
    for (int i = -1; i <= 10; ++i) {
        const qint64 offset = i < 0 ? 0 : qint64(1024) << (2 * i);
        if (offset >= file.size() || !file.seek(offset))
            break;
        const auto sample = file.read(1024);
        if (sample.isEmpty())
            break;
        hash.addData(sample);
    }
    return file.error() == QFileDevice::NoError ? QString::fromLatin1(hash.result().toHex()) : QString();
}

QString filenameMd5(const QString &path) {
    return path.isEmpty() ? QString() : md5Hex(QFileInfo(path).fileName().toUtf8());
}

QByteArray encodeProgress(const Progress &progress) {
    return QJsonDocument(qt::object({{"document", progress.document},
                                     {"progress", progress.progress},
                                     {"percentage", progress.percentage},
                                     {"device", progress.device},
                                     {"device_id", progress.deviceId}}))
        .toJson(QJsonDocument::Compact);
}

Progress parseProgress(const QByteArray &body) {
    const auto document = QJsonDocument::fromJson(body);
    Progress result;
    if (!document.isObject())
        return result;
    const auto object = document.object();
    result.document = object.value("document").toString();
    result.progress = object.value("progress").toString();
    result.device = object.value("device").toString();
    result.deviceId = object.value("device_id").toString();
    result.percentage = qBound(0.0, number(object.value("percentage")), 1.0);
    result.timestamp = qint64(number(object.value("timestamp")));
    return result;
}

ReadingProgress toReadingProgress(const QString &book, const Progress &remote) {
    ReadingProgress result;
    result.book = book;
    result.location = makeLocation(remote.progress, remote.percentage);
    result.modified = remote.timestamp * 1000;
    result.status = 0;
    result.valid = remote.valid() && !book.isEmpty();
    return result;
}

Verdict classify(const Progress &remote, const QString &deviceId, const QString &localXpointer,
                 qint64 unsentAtMilliseconds) {
    if (!remote.valid())
        return Verdict::Ignore;
    if (remote.deviceId == deviceId || (!remote.progress.isEmpty() && remote.progress == localXpointer))
        return Verdict::Acknowledge;
    if (unsentAtMilliseconds > 0 && remote.timestamp * 1000 <= unsentAtMilliseconds)
        return Verdict::KeepLocal;
    return Verdict::Prompt;
}

Client::Client(Config config, QNetworkAccessManager *manager)
    : config_(std::move(config)), manager_(manager ? manager : sharedManager()) {}

QNetworkRequest Client::request(const QString &path) const {
    auto url = config_.server;
    auto base = url.path();
    while (base.endsWith('/'))
        base.chop(1);
    url.setPath(base + path);
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.koreader.v1+json");
    request.setRawHeader("x-auth-user", config_.username.toUtf8());
    request.setRawHeader("x-auth-key", config_.userKey.toUtf8());
    request.setRawHeader("User-Agent", "Kobalt kosync");
    return request;
}

void Client::pull(const QString &document, std::function<void(PullResult)> done) const {
    auto *reply = manager_->get(request(QStringLiteral("/syncs/progress/") + document));
    watch(reply, config_.timeoutMilliseconds, [done](Answer answer) {
        PullResult result;
        result.status = statusFor(answer.http);
        result.error = answer.error;
        if (result.status == Status::Ok) {
            result.progress = parseProgress(answer.body);
            // Some servers answer 200 with an empty object for an unknown book.
            if (!result.progress.valid())
                result.status = Status::NotFound;
        }
        done(std::move(result));
    });
}

void Client::push(const Progress &progress, std::function<void(PushResult)> done) const {
    auto message = request(QStringLiteral("/syncs/progress"));
    message.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply = manager_->put(message, encodeProgress(progress));
    watch(reply, config_.timeoutMilliseconds, [done](Answer answer) {
        PushResult result;
        result.status = statusFor(answer.http);
        result.error = answer.error;
        if (result.status == Status::Ok)
            result.timestamp = parseProgress(answer.body).timestamp;
        done(std::move(result));
    });
}
}
