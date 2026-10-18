#include "reader_inputs.h"
#include "check.h"
#include "http_server.h"
#include "reading/kosync.h"
#include "reading/reading_book.h"
#include "reading/appearance_settings.h"
#include "interfaces/opening_message.h"
#include "reading/reader_session.h"
#include "storage/reader_storage.h"
#include "publication/native_resources.h"
#include "support/qt_compat.h"
#include "publication/resource_server.h"
#include "publication/native_archive.h"
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <utility>

using namespace kobalt;
namespace {
void waitUntil(const std::function<bool()> &ready) {
    if (ready())
        return;
    QEventLoop loop;
    QTimer poll, timeout;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [] {
        require(false, "ReadingBook check timed out");
    });
    poll.start(1);
    timeout.start(60000);
    loop.exec();
}
void tick() {
    QEventLoop loop;
    qt::later(10, &loop, [&] {
        loop.quit();
    });
    loop.exec();
}
struct Calls {
    int reads = 0, writes = 0, observed = 0, stopped = 0, destroyed = 0;
    bool writable = true, transition = false, changed = true, failWrite = false;
    bool finished = false, cloudSync = true;
    QString location;
    ReadingProgress current{"store-id", "chapter.xhtml#one", 1, 1, true};
};
class Bridge final : public ReadingBridge {
public:
    explicit Bridge(Calls &calls) : calls(calls) {}
    ~Bridge() override {
        ++calls.destroyed;
    }
    bool deviceTransition() const override {
        return calls.transition;
    }
    bool writable() const override {
        return calls.writable && !calls.transition;
    }
    ReadingProgress read(QString *) const override {
        ++calls.reads;
        return calls.current;
    }
    bool takeChange() override {
        return std::exchange(calls.changed, false);
    }
    bool write(const ReadingProgress &expected, const QString &location, double, bool finished,
               ReadingProgress *verified, QString *error) override {
        ++calls.writes;
        calls.finished = finished;
        calls.location = location;
        require(expected == calls.current, "Bridge received a stale expected baseline");
        if (calls.failWrite) {
            *error = "synthetic write failure";
            return false;
        }
        calls.current.location = location;
        ++calls.current.modified;
        calls.current.status = finished ? 2 : 1;
        *verified = calls.current;
        return true;
    }
    void observeReading(double, const QString &) override {
        ++calls.observed;
    }
    void stopReading() override {
        ++calls.stopped;
    }
    void setCloudSync(bool enabled) override {
        calls.cloudSync = enabled;
    }

private:
    Calls &calls;
};
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    require(argc == 2, "Expected a synthetic EPUB");
    const auto path = QString::fromLocal8Bit(argv[1]);
    native::ResourceServer server;
    require(server.open(path,
                        [path] {
                            return native::openPlainArchive(path);
                        }),
            "Cannot open synthetic publication");
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create state directory");
    storage::Paths paths{temporary.path()};
    const auto open = [&] {
        auto session = std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        bool ready = false;
        auto lease = std::make_shared<SourceLease>();
        session->open(
            {"test-book", path, QSize(758, 900), {}, {}, {}, lease, nativeResourceFactory(path, server.port(), lease)},
            [&](const OpeningResult &result) {
                require(result.ready(), qPrintable(openingMessage(result)));
                ready = true;
            });
        waitUntil([&] {
            return ready;
        });
        return session;
    };
    for (const auto &original : {qt::object({{"line_spacing", 122}}), QJsonObject(),
                                 qt::object({{"font_pixels", 31},
                                             {"font_face", "Libron"},
                                             {"line_spacing", QJsonValue(QJsonValue::Null)},
                                             {"dark_mode", true}})}) {
        auto migratedOriginal = original;
        appearance::migrateLineSpacing(migratedOriginal);
        const QString identity = "file:///mnt/onboard/kobalt-benchmark.kepub.epub";
        auto session = std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        bool ready = false;
        auto lease = std::make_shared<SourceLease>();
        OpenRequest request{identity, path, QSize(758, 900), original,
                            {},       {},   lease,           nativeResourceFactory(path, server.port(), lease)};
        request.temporaryAppearance = appearance::benchmarkTypography(identity);
        request.originalAppearance = original;
        session->open(std::move(request), [&](const OpeningResult &result) {
            require(result.ready(), qPrintable(openingMessage(result)));
            ready = true;
        });
        waitUntil([&] {
            return ready;
        });
        require(session->temporaryTypography() && session->fontFace().isEmpty() &&
                    session->fontSize() == defaultFontPixels() && session->margin() == readingPixels(60) &&
                    session->topMargin() == readingPixels(60) && !session->state().value("dark_mode").toBool(),
                "Temporary appearance was lost during cold opening");
        require(!session->style(30, 100, "Libron", 0, "left") && !session->setWeight(2) &&
                    !session->setHyphenation("on") && !session->setVerticalMargins(0),
                "Temporary appearance accepted a typography edit");
        session->setDarkMode(true);
        session->apply(original);
        waitUntil([&] {
            return !session->busy();
        });
        const auto hash = session->sourceHash();
        const auto statePath = paths.bookState(identity);
        std::unique_ptr<ReaderSession> retained;
        {
            ReadingBook benchmark(std::move(session), statePath, paths, identity, hash, nullptr, {}, {});
            QJsonObject snapshot;
            require(benchmark.checkpoint(&snapshot), "Benchmark checkpoint failed");
            require(snapshot.value("font_pixels").toInt() == defaultFontPixels(),
                    "Live snapshot lost benchmark appearance");
            const auto persisted = storage::readJson(statePath);
            require(appearance::same(persisted, migratedOriginal),
                    "Checkpoint persisted temporary appearance or added absent keys");
            require(benchmark.session()->turn(1), "Benchmark cannot turn");
            waitUntil([&] {
                return !benchmark.session()->busy();
            });
            require(benchmark.checkpoint(&snapshot), "Benchmark turn checkpoint failed");
            require(appearance::same(storage::readJson(statePath), migratedOriginal),
                    "Turn checkpoint changed original appearance");
            retained = benchmark.session()->retained();
            require(bool(retained), "Benchmark could not retain its parsed document");
        }
        require(appearance::same(storage::readJson(statePath), migratedOriginal),
                "Closing changed original appearance");
        ready = false;
        lease = std::make_shared<SourceLease>();
        retained->open({identity,
                        path,
                        QSize(758, 900),
                        storage::readJson(statePath),
                        {},
                        {},
                        lease,
                        nativeResourceFactory(path, server.port(), lease)},
                       [&](const OpeningResult &result) {
                           require(result.ready(), qPrintable(openingMessage(result)));
                           ready = true;
                       });
        waitUntil([&] {
            return ready;
        });
        require(!retained->temporaryTypography() &&
                    retained->fontSize() == original.value("font_pixels").toInt(defaultFontPixels()) &&
                    retained->spacing() == migratedOriginal.value("line_spacing").toInt(defaultLineSpacing),
                "Normal reopening retained temporary typography");
        auto ordinary = open();
        require(!ordinary->temporaryTypography(), "Unmatched opening inherited benchmark typography");
    }
    {
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook local(std::move(session), paths.bookState("local-id"), paths, "local-id", hash, nullptr, {}, {});
        require(local.saveLayoutPreferences(
                    qt::object({{"show_clock", false}, {"header_text_size", "large"}, {"orientation", 2}})),
                "Cannot save book layout");
        require(local.saveDictionaryChoice("test-dictionary"), "Cannot save dictionary choice");
        QJsonObject layoutSnapshot;
        require(local.checkpoint(&layoutSnapshot), "Cannot checkpoint book layout");
        const auto savedLayout = storage::readJson(paths.bookState("local-id"));
        require(!savedLayout["show_clock"].toBool() && savedLayout["header_text_size"].toString() == "large" &&
                    savedLayout["orientation"].toInt() == 2,
                "Checkpoint lost book layout");
        require(savedLayout["dictionary_id"].toString() == "test-dictionary", "Checkpoint lost dictionary choice");
        require(!storage::readJson(paths.preferences()).contains("show_clock"),
                "Book layout changed global preferences");
        require(!local.canWriteNative() && !local.deviceTransition(), "Local book acquired platform services");
        local.pollProgress();
        local.observeProgress();
        require(!local.readProgress().valid && !local.pendingProgress(), "Local book invented external progress");
        local.observeReading();
        tick();
        local.stopReading();
        const auto active = local.readingMilliseconds();
        require(active > 0, "Local reading time did not run without a bridge");
        tick();
        require(local.readingMilliseconds() == active, "Stopped local reading time kept running");
        require(local.session()->go(local.session()->pages() - 1), "Cannot reach the final page");
        waitUntil([&] {
            return !local.session()->busy();
        });
        require(local.offerFinish(), "Local book cannot be marked finished without a bridge");
        local.markFinished();
        require(!local.offerFinish(), "Local book repeatedly offered the same finish position");
        require(local.writeNative({}, "no native mapping"), "Local-only save required a platform position");
        QJsonObject snapshot;
        require(local.checkpoint(&snapshot), "Local checkpoint failed");
        require(snapshot == storage::readJson(paths.bookState("local-id")), "Local checkpoint fields changed on disk");
        require(snapshot.value("book") == QStringLiteral("local-id") && snapshot.value("schema") == 1 &&
                    snapshot.value("fingerprint").toString() == QString::fromLatin1(hash) &&
                    !snapshot.value("finished_position").toString().isEmpty() &&
                    snapshot.value("native_writeback") == QStringLiteral("local-only") &&
                    snapshot.value("native_writeback_error").toString().isEmpty(),
                "Local checkpoint lost identity, finish or writeback state");
        local.releaseNative();
        local.observeReading();
        tick();
        require(local.readingMilliseconds() == active && !local.offerFinish(),
                "Released local book became active again");
    }
    {
        Calls calls;
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("store-id"), paths, "store-id", hash,
                         std::make_unique<Bridge>(calls), calls.current, calls.current);
        book.observeReading();
        require(calls.observed == 1, "Platform activity was not recorded");
        calls.transition = true;
        book.observeReading();
        require(book.deviceTransition() && !book.canWriteNative() && calls.observed == 1 && calls.stopped == 1,
                "Device transition did not stop platform activity");
        require(book.writeNative("position", {}), "Deferred platform write blocked local checkpoint");
        require(calls.writes == 0, "Platform write ran during transition");
        calls.transition = false;
        ++calls.current.modified;
        calls.current.location = "chapter.xhtml#remote";
        book.pollProgress();
        require(book.pendingProgress(), "External progress change did not create a conflict");
        require(!book.writeNative("position", {}) && calls.writes == 0, "Conflict was overwritten without a choice");
        book.acknowledge(book.candidate(), false);
        require(book.writeNative("position", {}) && calls.writes == 1 && calls.location == "position",
                "Acknowledged progress was not written");
        QJsonObject snapshot;
        require(book.checkpoint(&snapshot), "Platform checkpoint failed");
        require(snapshot.value("nickel_progress").toObject() == calls.current.json() &&
                    snapshot.value("native_writeback") == QStringLiteral("verified"),
                "Verified native baseline changed");
        const auto committed = calls.current;
        calls.failWrite = true;
        require(book.writeNative("position-two", {}), "Ordinary platform failure blocked local persistence");
        require(book.checkpoint(&snapshot), "Failed-write local checkpoint failed");
        require(snapshot.value("nickel_progress").toObject() == committed.json() &&
                    snapshot.value("native_writeback") == QStringLiteral("local-only") &&
                    snapshot.value("native_writeback_error") == QStringLiteral("synthetic write failure"),
                "Failed native write changed the confirmed baseline");
        const int writes = calls.writes;
        require(book.writeNative({}, "unmappable store position") && calls.writes == writes,
                "Store book accepted a time-only write");
        const auto statePath = paths.bookState("store-id");
        require(QFile::remove(statePath) && QDir().mkpath(statePath), "Cannot obstruct checkpoint destination");
        require(!book.checkpoint(&snapshot), "Checkpoint reported success when storage refused the write");
        require(QDir().rmdir(statePath) && book.checkpoint(&snapshot),
                "Failed checkpoint was cached instead of retried");
        require(storage::readJson(statePath) == snapshot, "Retried checkpoint lost the current state");
        book.releaseNative();
        require(calls.destroyed == 1 && !book.canWriteNative(), "Release retained platform services");
        book.pollProgress();
        book.observeProgress();
        book.observeReading();
        require(calls.destroyed == 1 && !book.offerFinish(), "Released book accessed its bridge again");
    }
    {
        // A sync server: another device's position is offered, never applied silently.
        auto first = open();
        require(first->go(first->pages() - 1), "Cannot reach the final page");
        waitUntil([&] {
            return !first->busy();
        });
        const auto farAnchor = first->state().value("position").toString();
        require(first->go(0), "Cannot return to the first page");
        waitUntil([&] {
            return !first->busy();
        });
        const auto startAnchor = first->state().value("position").toString();
        require(!farAnchor.isEmpty() && farAnchor != startAnchor, "The test book has no two positions to sync");
        test::Server server;
        QString remoteDevice = "OTHER";
        server.respond = [&](const test::Request &request) {
            if (request.method == "PUT")
                return test::http(200, "{\"timestamp\":1700000100}");
            if (request.path.endsWith("doc2"))
                return test::http(404);
            return test::http(200, QJsonDocument(qt::object({{"document", "doc"},
                                                             {"progress", farAnchor},
                                                             {"percentage", 0.9},
                                                             {"device", "Phone"},
                                                             {"device_id", remoteDevice},
                                                             {"timestamp", qint64(1700000000)}}))
                                       .toJson());
        };
        const auto puts = [&] {
            int count = 0;
            for (const auto &request : server.requests)
                count += request.method == "PUT";
            return count;
        };
        const auto configure = [&](bool replaceKoboSync) {
            return kosync::parseConfig("{\"server\":\"" + server.url().toString().toUtf8() +
                                           "\",\"username\":\"u\",\"password\":\"p\",\"timeout_seconds\":2,"
                                           "\"replace_kobo_sync\":" +
                                           (replaceKoboSync ? "true" : "false") + "}",
                                       "THISDEVICE");
        };
        const auto hash = first->sourceHash();
        {
            auto sync = std::make_unique<kosync::Session>(configure(true), "doc", paths.kosyncUnsent());
            auto *watched = sync.get();
            ReadingBook book(std::move(first), paths.bookState("sync-id"), paths, "sync-id", hash, nullptr, {}, {});
            book.attachKosync(std::move(sync));
            waitUntil([&] {
                return watched->settled();
            });
            book.pollProgress();
            require(book.pendingProgress(), "Another device's position was not offered");
            const auto offered = book.candidate();
            QString xpointer;
            double fraction = 0;
            require(kosync::splitLocation(offered.location, &xpointer, &fraction) && xpointer == farAnchor &&
                        fraction == 0.9,
                    "The offered position is not the server's");
            require(!book.writeNative({}, "no native mapping"), "A pending server position was saved over");
            book.pushKosync();
            tick();
            require(puts() == 0, "A position was sent while another device's was waiting for a choice");
            book.acknowledge(offered, false);
            require(!book.pendingProgress(), "Keeping the local position left the choice pending");
            book.pushKosync();
            waitUntil([&] {
                return puts() == 1;
            });
            const auto sent = QJsonDocument::fromJson(server.requests.last().body).object();
            require(sent.value("progress").toString() == startAnchor && sent.value("document") == "doc" &&
                        sent.value("device_id") == "THISDEVICE" && sent.value("percentage").toDouble() > 0 &&
                        sent.value("percentage").toDouble() <= 1,
                    "The position sent to the server is wrong");
            QJsonObject snapshot;
            require(book.checkpoint(&snapshot) && snapshot.contains("kosync_progress"),
                    "The decided server record was not saved with the book");
        }
        {
            // The same record is not offered again at the next opening.
            auto second = open();
            const auto secondHash = second->sourceHash();
            auto sync = std::make_unique<kosync::Session>(configure(true), "doc", paths.kosyncUnsent());
            auto *watched = sync.get();
            ReadingBook book(std::move(second), paths.bookState("sync-id"), paths, "sync-id", secondHash, nullptr, {},
                             {});
            book.attachKosync(std::move(sync));
            waitUntil([&] {
                return watched->settled();
            });
            book.pollProgress();
            require(!book.pendingProgress(), "A decided server record was offered again");
        }
        {
            // A record this device wrote is not another device's position.
            remoteDevice = "THISDEVICE";
            auto third = open();
            const auto thirdHash = third->sourceHash();
            auto sync = std::make_unique<kosync::Session>(configure(true), "doc", paths.kosyncUnsent());
            auto *watched = sync.get();
            ReadingBook book(std::move(third), paths.bookState("own-id"), paths, "own-id", thirdHash, nullptr, {}, {});
            book.attachKosync(std::move(sync));
            waitUntil([&] {
                return watched->settled();
            });
            book.pollProgress();
            require(!book.pendingProgress(), "This device's own record was offered to it");
        }
        for (const bool replace : {true, false}) {
            // With a sync server in charge, Nickel's own sync is not offered.
            Calls calls;
            auto fourth = open();
            const auto fourthHash = fourth->sourceHash();
            ReadingBook book(std::move(fourth), paths.bookState(replace ? "replace-id" : "keep-id"), paths, "store-id",
                             fourthHash, std::make_unique<Bridge>(calls), calls.current, calls.current);
            book.attachKosync(std::make_unique<kosync::Session>(configure(replace), "doc2", paths.kosyncUnsent()));
            require(calls.cloudSync == !replace, "The platform's cloud sync was not switched with the server");
            ++calls.current.modified;
            calls.current.location = "chapter.xhtml#remote";
            calls.changed = true;
            book.pollProgress();
            require(book.pendingProgress() == !replace,
                    replace ? "Nickel's sync was offered although the server replaces it"
                            : "Nickel's sync was lost without a sync server in charge");
        }
    }
    bool drained = false;
    ReaderSession::drain([&] {
        drained = true;
    });
    waitUntil([&] {
        return drained;
    });
    bool disconnected = false;
    releaseResourceConnections([&] {
        disconnected = true;
    });
    waitUntil([&] {
        return disconnected;
    });
    std::puts("PASS: local reading state and optional platform activity, conflict, writeback and release");
    // ReaderSession stops its worker on application shutdown. Nested waits
    // above do not emit aboutToQuit; leave through the application event loop.
    qt::later(0, &app, [&] {
        app.quit();
    });
    return app.exec();
}
