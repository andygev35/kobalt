#include "check.h"
#include "http_server.h"
#include "reading/kosync.h"
#include "reading/kosync_session.h"
#include "storage/reader_storage.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

using namespace kobalt;
using test::http;
using test::Request;
using test::Server;
namespace {
void waitUntil(const std::function<bool()> &ready, int milliseconds = 5000) {
    QEventLoop loop;
    QTimer poll, timeout;
    bool timedOut = false;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    poll.start(1);
    timeout.start(milliseconds);
    if (!ready())
        loop.exec();
    require(!timedOut, "Timed out waiting for the kosync check");
}
void pause(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
QByteArray pattern(int size) {
    QByteArray bytes(size, '\0');
    for (int i = 0; i < size; ++i)
        bytes[i] = char((i * 31 + 7) & 0xff);
    return bytes;
}
QString writeBook(const QTemporaryDir &dir, const QString &name, int size) {
    const auto path = dir.filePath(name);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(pattern(size)) == size, "Could not write a test book");
    return path;
}

kosync::Config configFor(const Server &server) {
    auto config = kosync::parseConfig("{\"server\":\"" + server.url().toString().toUtf8() +
                                          "/\",\"username\":\"andy\",\"password\":\"secret\","
                                          "\"device_name\":\"Test Kobo\",\"timeout_seconds\":1}",
                                      "DEVICE1");
    require(config.usable(), "Test config is not usable");
    return config;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    require(dir.isValid(), "No temporary directory");

    // Document IDs, checked against an independent implementation of KOReader's algorithm.
    struct {
        int size;
        const char *hash;
    } books[] = {{0, "d41d8cd98f00b204e9800998ecf8427e"},      {100, "d2fc8ccfe9948facc7c6d47098357135"},
                 {1024, "63b2177a7af739b5cc52ab1d1c714702"},   {1025, "31d56d8e673d2856b7a4eee144ee8275"},
                 {5000, "5ed57c7e8b80bdccb94703bbbbe792b7"},   {70000, "7c495b34b24f561520622156e96f834d"},
                 {300000, "3840df5dc77bb7f3d6d4b67e0979ed0b"}, {1100000, "9176994efdf840ffd6a4fb8d1cd31fb1"}};
    for (const auto &book : books)
        require(kosync::partialMd5(writeBook(dir, QStringLiteral("b%1.epub").arg(book.size), book.size)) ==
                    QString::fromLatin1(book.hash),
                "Partial MD5 differs from KOReader's document ID");
    require(kosync::partialMd5(dir.filePath("missing.epub")).isEmpty(), "A missing file produced an ID");
    require(kosync::filenameMd5("/mnt/onboard/My Book.epub") == "9a671b9d07b5404d84ce60af27058b04",
            "Filename ID is not the MD5 of the file name");

    // Configuration.
    auto config = kosync::parseConfig(
        "{\"server\":\"sync.example.org:8081/kosync/\",\"username\":\"u\",\"password\":\"secret\"}", "ID");
    require(config.usable() && config.server.toString() == "https://sync.example.org:8081/kosync/",
            "A server without a scheme did not default to https");
    require(config.userKey == "5ebe2294ecd0e0f08eab7690d2a6ee69" && config.deviceName == "Kobalt" &&
                config.timeoutMilliseconds == 8000 && config.replaceKoboSync,
            "Config defaults or password hash are wrong");
    config = kosync::parseConfig(
        "{\"server\":\"http://h\",\"username\":\"u\",\"userkey\":\"ABCDEF\",\"password\":\"x\"}", "ID");
    require(config.usable() && config.userKey == "abcdef", "userkey did not take precedence");
    require(!kosync::parseConfig("{\"server\":\"ftp://h\",\"username\":\"u\",\"password\":\"p\"}", "ID").usable(),
            "A non-http server was accepted");
    require(!kosync::parseConfig("{\"server\":\"http://h\",\"password\":\"p\"}", "ID").usable(),
            "A config without a username was accepted");
    require(
        !kosync::parseConfig("{\"enabled\":false,\"server\":\"http://h\",\"username\":\"u\",\"password\":\"p\"}", "ID")
             .usable(),
        "A disabled config was usable");
    require(!kosync::parseConfig("[1]", "ID").usable() && !kosync::parseConfig("nonsense", "ID").usable(),
            "Invalid JSON was accepted");
    require(kosync::parseConfig(
                "{\"server\":\"http://h\",\"username\":\"u\",\"password\":\"p\",\"timeout_seconds\":999}", "ID")
                    .timeoutMilliseconds == 60000,
            "The timeout was not limited");
    const auto configPath = dir.filePath("kosync.json"), devicePath = dir.filePath("kosync-device.json");
    require(!kosync::loadConfig(configPath, devicePath).usable() &&
                kosync::loadConfig(configPath, devicePath).error.isEmpty(),
            "A missing config should mean not configured, without an error");
    require(
        storage::writeJson(configPath, qt::object({{"server", "http://h:1"}, {"username", "u"}, {"password", "p"}})),
        "Could not write a config");
    const auto loaded = kosync::loadConfig(configPath, devicePath);
    require(loaded.usable() && loaded.deviceId.size() == 32, "A configured device did not get an ID");
    require(kosync::loadConfig(configPath, devicePath).deviceId == loaded.deviceId,
            "The device ID changed between loads");
    require(!kosync::loadConfig(configPath, dir.filePath("no/such/dir/device.json")).usable(),
            "A device ID that cannot be saved must not be used");

    // Progress records and locations.
    kosync::Progress progress{"abc", "/body/DocFragment[3]/body/p[2]/text().5", "Kobo", "ID", 0.4321, 0};
    const auto sent = QJsonDocument::fromJson(kosync::encodeProgress(progress)).object();
    require(sent.value("document") == "abc" && sent.value("progress") == progress.progress &&
                sent.value("percentage").toDouble() == 0.4321 && sent.value("device") == "Kobo" &&
                sent.value("device_id") == "ID" && sent.size() == 5,
            "The progress body does not match KOReader's");
    const auto parsed = kosync::parseProgress(
        "{\"document\":\"abc\",\"progress\":\"p\",\"percentage\":\"0.5\",\"device\":\"d\",\"device_id\":\"i\","
        "\"timestamp\":1700000000}");
    require(parsed.valid() && parsed.percentage == 0.5 && parsed.timestamp == 1700000000 && parsed.deviceId == "i",
            "A server record was not parsed");
    require(!kosync::parseProgress("{}").valid() && !kosync::parseProgress("<html>").valid() &&
                kosync::parseProgress("{\"document\":\"a\",\"percentage\":7}").percentage == 1.0,
            "Empty, invalid or out of range records were not handled");
    QString xpointer;
    double fraction = -1;
    const auto location = kosync::makeLocation("/body/DocFragment[1]/body/p|x/text().0", 0.25);
    require(kosync::isLocation(location) && kosync::splitLocation(location, &xpointer, &fraction) &&
                xpointer == "/body/DocFragment[1]/body/p|x/text().0" && fraction == 0.25,
            "A location did not survive its round trip");
    require(!kosync::splitLocation("chapter.xhtml#id:3", &xpointer, &fraction) &&
                !kosync::splitLocation("kosync:nonsense", &xpointer, &fraction),
            "A foreign or malformed location was accepted");
    progress.timestamp = 100;
    const auto asProgress = kosync::toReadingProgress("book", progress);
    require(asProgress.valid && asProgress.modified == 100000 && kosync::isLocation(asProgress.location),
            "Remote progress did not convert to reading progress");

    // Which remote records are offered, ignored or overridden.
    using kosync::Verdict;
    auto other = progress;
    other.deviceId = "OTHER";
    other.timestamp = 1000;
    require(kosync::classify({}, "ID", "x", 0) == Verdict::Ignore, "An empty record was not ignored");
    require(kosync::classify(progress, "ID", "elsewhere", 0) == Verdict::Acknowledge,
            "This device's own record was offered back to it");
    require(kosync::classify(other, "ID", other.progress, 0) == Verdict::Acknowledge,
            "A record at the current position was offered");
    require(kosync::classify(other, "ID", "elsewhere", 0) == Verdict::Prompt,
            "Another device's position was not offered");
    require(kosync::classify(other, "ID", "elsewhere", 2000 * 1000) == Verdict::KeepLocal,
            "An older server record beat an undelivered local position");
    require(kosync::classify(other, "ID", "elsewhere", 500 * 1000) == Verdict::Prompt,
            "A newer server record lost to an older undelivered position");

    // Requests. `server` is reached over the loopback interface only.
    Server server;
    const auto cfg = configFor(server);
    kosync::Client client(cfg);
    const auto request = client.request("/syncs/progress/abc");
    require(request.url().path() == "/syncs/progress/abc", "A trailing slash in the server address doubled");
    require(request.rawHeader("x-auth-user") == "andy" &&
                request.rawHeader("x-auth-key") == "5ebe2294ecd0e0f08eab7690d2a6ee69" &&
                request.rawHeader("Accept") == "application/vnd.koreader.v1+json",
            "Request headers do not match KOReader's");
    require(kosync::Client(
                kosync::parseConfig("{\"server\":\"http://h/base/\",\"username\":\"u\",\"password\":\"p\"}", "I"))
                    .request("/syncs/progress")
                    .url()
                    .path() == "/base/syncs/progress",
            "A server path prefix was lost");

    const auto body =
        QByteArray("{\"document\":\"abc\",\"progress\":\"/body/x\",\"percentage\":0.5,\"device\":\"Phone\","
                   "\"device_id\":\"P1\",\"timestamp\":1700000000}");
    server.respond = [&](const Request &r) {
        if (r.path == "/syncs/progress/known")
            return http(200, body);
        if (r.path == "/syncs/progress/empty")
            return http(200, "{}");
        if (r.path == "/syncs/progress/denied")
            return http(401, "{\"message\":\"Unauthorized\"}");
        if (r.path == "/syncs/progress/down")
            return http(502, "{\"message\":\"Unknown server error.\"}");
        if (r.path == "/syncs/progress/silent")
            return QByteArray();
        if (r.path == "/syncs/progress" && r.method == "PUT")
            return http(200, "{\"document\":\"abc\",\"timestamp\":1700000001}");
        return http(404);
    };
    auto pull = [&](const QString &document) {
        kosync::PullResult result;
        bool done = false;
        client.pull(document, [&](kosync::PullResult r) {
            result = r;
            done = true;
        });
        waitUntil([&] {
            return done;
        });
        return result;
    };
    auto result = pull("known");
    require(result.status == kosync::Status::Ok && result.progress.deviceId == "P1" &&
                result.progress.percentage == 0.5,
            "A record was not fetched");
    require(server.requests.last().method == "GET" && server.requests.last().headers.value("x-auth-user") == "andy",
            "The fetch was not an authenticated GET");
    require(pull("empty").status == kosync::Status::NotFound && pull("unknown").status == kosync::Status::NotFound,
            "An unknown book was not reported as such");
    require(pull("denied").status == kosync::Status::Unauthorized, "A refused login was not reported");
    require(pull("down").status == kosync::Status::Unavailable, "A gateway error was not reported as unavailable");
    require(pull("silent").status == kosync::Status::Unavailable, "A server that never answers did not time out");
    {
        kosync::PushResult pushed;
        bool done = false;
        client.push(progress, [&](kosync::PushResult r) {
            pushed = r;
            done = true;
        });
        waitUntil([&] {
            return done;
        });
        require(pushed.status == kosync::Status::Ok && pushed.timestamp == 1700000001, "A position was not sent");
        const auto &put = server.requests.last();
        require(put.method == "PUT" && put.path == "/syncs/progress" &&
                    put.headers.value("content-type") == "application/json" &&
                    QJsonDocument::fromJson(put.body).object().value("device_id") == "ID",
                "The position was not sent as KOReader sends it");
    }
    {
        // Nothing listens here, as when Wi-Fi is off.
        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost, 0);
        const auto port = closed.serverPort();
        closed.close();
        auto offline = cfg;
        offline.server = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port));
        bool done = false;
        kosync::PullResult refused;
        kosync::Client(offline).pull("x", [&](kosync::PullResult r) {
            refused = r;
            done = true;
        });
        waitUntil([&] {
            return done;
        });
        require(refused.status == kosync::Status::Unavailable && !refused.error.isEmpty(),
                "A refused connection was not reported as unavailable");
    }

    // Sessions.
    const auto unsentPath = dir.filePath("kosync-unsent.json");
    server.requests.clear();
    {
        kosync::Session session(cfg, "known", unsentPath);
        require(session.push("/body/a", 0.1) == kosync::Session::Push::Waiting && server.requests.isEmpty(),
                "A position was sent before the server's record was seen");
        session.pull();
        session.pull();
        waitUntil([&] {
            return session.settled();
        });
        require(server.requests.size() == 1, "A second fetch started while one was running");
        kosync::Progress remote;
        require(session.takeRemote(&remote) && remote.deviceId == "P1" && !session.takeRemote(&remote),
                "The fetched record was not handed over exactly once");
        require(session.push("/body/x", 0.5) == kosync::Session::Push::Unchanged,
                "The position the server already holds was sent again");
        require(session.push("/body/y", 0.6) == kosync::Session::Push::Started, "A new position was not sent");
        require(session.push("/body/y", 0.6) == kosync::Session::Push::Unchanged,
                "A position already on its way was queued again");
        waitUntil([&] {
            return server.requests.size() == 2;
        });
        waitUntil([&] {
            return storage::readJson(unsentPath).isEmpty();
        });
        require(session.unsentAt() == 0, "A delivered position stayed unsent");
        require(session.push("/body/y", 0.6) == kosync::Session::Push::Unchanged,
                "A delivered position was sent twice");
    }
    {
        // A position sent while the book closes still completes and clears its record.
        server.requests.clear();
        auto *session = new kosync::Session(cfg, "known", unsentPath);
        session->pull();
        waitUntil([&] {
            return session->settled();
        });
        require(session->push("/body/closing", 0.9) == kosync::Session::Push::Started,
                "The closing position was not sent");
        require(!storage::readJson(unsentPath).isEmpty(), "The position was not recorded before it was sent");
        delete session;
        waitUntil([&] {
            return storage::readJson(unsentPath).isEmpty();
        });
        require(QJsonDocument::fromJson(server.requests.last().body).object().value("progress") == "/body/closing",
                "The position sent while closing is wrong");
    }
    {
        // Unreachable server: the position stays recorded for the next opening.
        auto offline = cfg;
        offline.server = QUrl("http://127.0.0.1:1");
        kosync::Session session(offline, "offline-book", unsentPath);
        session.pull();
        waitUntil([&] {
            return session.settled();
        });
        require(session.push("/body/z", 0.3) == kosync::Session::Push::Started, "An offline push was refused");
        pause(300);
        const auto record = storage::readJson(unsentPath).value("offline-book").toObject();
        require(record.value("progress") == "/body/z" && session.unsentAt() > 0,
                "An undelivered position was not kept");
        kosync::Session later(cfg, "offline-book", unsentPath);
        require(later.unsentAt() == session.unsentAt(), "The undelivered position was not found at the next opening");
    }
    {
        // A refused login stops further requests.
        kosync::Session session(cfg, "denied", unsentPath);
        session.pull();
        waitUntil([&] {
            return session.settled();
        });
        const auto count = server.requests.size();
        require(session.push("/body/q", 0.2) == kosync::Session::Push::Blocked && session.pulling() == false,
                "A refused login still sent a position");
        session.pull();
        pause(50);
        require(server.requests.size() == count, "A refused login still fetched");
    }
    return 0;
}
