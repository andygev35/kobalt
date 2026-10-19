#include "reader_inputs.h"
#include "interfaces/reader/settings/kosync_panel.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "support/strings.h"
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <vector>
#include "check.h"

namespace {
using kobalt::KosyncSettingsService;

class FakeKeyboard final : public kobalt::KeyboardController {
public:
    bool show() override {
        ++shown;
        return true;
    }
    void hide() override {
        ++hidden;
    }
    void place() override {}
    int shown = 0, hidden = 0;
};
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    require(kobalt::test::readerAssets().available(), "Component assets failed manifest verification");
    require(kobalt::ui::initializeInterfaceFonts(kobalt::test::readerAssets()), "Cannot load verified UI font bytes");

    FakeKeyboard *keyboard = nullptr;
    kobalt::KeyboardFactory factory = [&](QWidget *, QLineEdit *) {
        auto result = std::make_unique<FakeKeyboard>();
        keyboard = result.get();
        return result;
    };
    // The host service is faked: a stored login, and replies the test releases by hand.
    struct Call {
        QString server, username, password;
    };
    std::vector<Call> tests, saves;
    std::vector<std::function<void(KosyncSettingsService::TestResult)>> replies;
    bool saveSucceeds = true;
    KosyncSettingsService service;
    service.load = [] {
        return KosyncSettingsService::Stored{"https://sync.example.com", "reader", true};
    };
    service.test = [&](const QString &server, const QString &username, const QString &password,
                       std::function<void(KosyncSettingsService::TestResult)> done) {
        tests.push_back({server, username, password});
        replies.push_back(std::move(done));
    };
    service.save = [&](const QString &server, const QString &username, const QString &password, QString *error) {
        saves.push_back({server, username, password});
        if (!saveSucceeds && error)
            *error = "refused";
        return saveSucceeds;
    };
    require(service.complete(), "The fake service is incomplete");

    kobalt::KosyncPanel panel(nullptr, factory);
    require(keyboard, "The panel did not create its keyboard");
    panel.setService(service);
    panel.resize(1072, 1448);
    int dismissals = 0;
    panel.dismissed = [&] {
        ++dismissals;
    };
    panel.open();
    app.processEvents();

    auto button = [&](const char *name) {
        auto *found = panel.findChild<QPushButton *>(name);
        require(found != nullptr, "Panel button missing");
        return found;
    };
    auto message = [&] {
        return panel.findChild<QLabel *>("rrKosyncMessage")->text();
    };
    auto detail = [&](const char *name) {
        return static_cast<kobalt::ui::ListRow *>(button(name))->detailLabel()->text();
    };
    auto *query = panel.findChild<QLineEdit *>("rrKosyncQuery");
    require(query != nullptr, "The field is missing");
    auto type = [&](const char *row, const QString &text) {
        button(row)->click();
        require(query->isVisible() && keyboard->shown > 0, "Tapping a row did not open the field and keyboard");
        query->setText(text);
        emit query->returnPressed();
        require(!query->isVisible(), "Return did not close the field");
    };

    // The stored login fills the rows. The password is never read back.
    require(detail("rrKosyncServer") == "https://sync.example.com" && detail("rrKosyncUsername") == "reader",
            "The stored login is not shown");
    require(detail("rrKosyncPassword") != kobalt::strings::get("kosync_panel.not_set") &&
                !detail("rrKosyncPassword").contains('x'),
            "A stored password should show as dots");
    require(message() == kobalt::strings::get("kosync_panel.intro"), "The intro is missing");

    // Editing the password starts empty and shows dots.
    button("rrKosyncPassword")->click();
    require(query->text().isEmpty() && query->echoMode() == QLineEdit::Password, "The password field is not masked");
    // Back abandons the edit without leaving.
    button("rrKosyncBack")->click();
    require(!query->isVisible() && dismissals == 0, "Back should cancel the edit first");

    // A test with the stored password sends an empty one, so the host reuses it.
    button("rrKosyncTest")->click();
    require(tests.size() == 1 && tests[0].server == "https://sync.example.com" && tests[0].username == "reader" &&
                tests[0].password.isEmpty(),
            "The test did not use the stored login");
    require(message() == kobalt::strings::get("kosync_panel.testing"), "No progress message while testing");
    button("rrKosyncTest")->click();
    require(tests.size() == 1, "A second test started while one was running");
    replies[0]({KosyncSettingsService::Outcome::Success, {}});
    require(message() == kobalt::strings::get("kosync_panel.success") && saves.empty(),
            "A successful test should report success and save nothing");

    // Each failure says what went wrong.
    struct Case {
        KosyncSettingsService::Outcome outcome;
        const char *key;
    } cases[] = {{KosyncSettingsService::Outcome::BadLogin, "kosync_panel.bad_login"},
                 {KosyncSettingsService::Outcome::NotKosync, "kosync_panel.not_kosync"},
                 {KosyncSettingsService::Outcome::Rejected, "kosync_panel.rejected"},
                 {KosyncSettingsService::Outcome::Unreachable, "kosync_panel.unreachable"},
                 {KosyncSettingsService::Outcome::Invalid, "kosync_panel.invalid"}};
    for (const auto &item : cases) {
        const auto before = replies.size();
        button("rrKosyncSave")->click();
        require(replies.size() == before + 1, "Save should test the login first");
        replies.back()({item.outcome, {}});
        require(message() == kobalt::strings::get(item.key), "Wrong message for a failed login");
        require(saves.empty(), "A login that failed the test was saved");
    }
    button("rrKosyncSave")->click();
    replies.back()({KosyncSettingsService::Outcome::Unreachable, "SSL handshake failed"});
    require(message() == kobalt::strings::get("kosync_panel.unreachable_detail").arg("SSL handshake failed"),
            "The network error is not shown");

    // Edits change what is tested and saved.
    type("rrKosyncServer", "  https://other.example.com  ");
    type("rrKosyncUsername", "newname");
    type("rrKosyncPassword", "s3cret pass");
    require(detail("rrKosyncServer") == "https://other.example.com", "The server was not trimmed");
    button("rrKosyncSave")->click();
    require(tests.back().server == "https://other.example.com" && tests.back().username == "newname" &&
                tests.back().password == "s3cret pass",
            "The typed login was not tested");
    replies.back()({KosyncSettingsService::Outcome::Success, {}});
    require(saves.size() == 1 && saves[0].password == "s3cret pass" &&
                message() == kobalt::strings::get("kosync_panel.saved"),
            "A working login was not saved");
    // After saving the typed password is forgotten; the host keeps it.
    button("rrKosyncTest")->click();
    require(tests.back().password.isEmpty(), "The typed password was kept after saving");
    replies.back()({KosyncSettingsService::Outcome::Success, {}});

    // A reply for a login that has since been edited is ignored.
    button("rrKosyncTest")->click();
    const auto stale = replies.size() - 1;
    type("rrKosyncUsername", "someone-else");
    replies[stale]({KosyncSettingsService::Outcome::Success, {}});
    require(message() == kobalt::strings::get("kosync_panel.intro"), "A stale reply replaced the message");

    // A failed write after a good test says so.
    saveSucceeds = false;
    button("rrKosyncSave")->click();
    replies.back()({KosyncSettingsService::Outcome::Success, {}});
    require(message() == kobalt::strings::get("kosync_panel.save_failed"), "A failed save was not reported");

    // Missing fields never reach the host.
    const auto calls = tests.size();
    type("rrKosyncServer", "");
    button("rrKosyncTest")->click();
    require(tests.size() == calls && message() == kobalt::strings::get("kosync_panel.invalid"),
            "An empty server reached the host");

    button("rrKosyncBack")->click();
    require(dismissals == 1, "Back did not leave the panel");
    return 0;
}
