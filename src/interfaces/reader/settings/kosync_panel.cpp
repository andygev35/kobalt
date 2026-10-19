#include "kosync_panel.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/styles.h"
#include "support/strings.h"
#include <QGuiApplication>
#include <QInputMethod>
#include <QPainter>
#include <QPointer>

namespace kobalt {
namespace m = ui::metrics;
namespace c = ui::metrics::chrome;
namespace {
constexpr int fieldCount = 3;
}

KosyncPanel::KosyncPanel(QWidget *parent, KeyboardFactory makeKeyboard) : ui::Panel(parent) {
    setObjectName("rrKosync");
    back = new ui::CellButton(ui::ControlIcon::Back, this, c::backIcon);
    back->setObjectName("rrKosyncBack");
    back->setRule(Qt::RightEdge);
    back->setAccessibleName(strings::get("common.back"));
    connect(back, &QPushButton::clicked, this, [this] {
        // Back first abandons an edit in progress, then leaves.
        if (editing != Field::None) {
            endEdit();
            redraw();
        } else if (dismissed) {
            dismissed();
        }
    });
    section = new QLabel(strings::get("settings.advanced"), this);
    section->setObjectName("rrKosyncSection");
    ui::captionLabel(section);
    title = new QLabel(strings::get("kosync_panel.title"), this);
    title->setObjectName("rrKosyncTitle");
    title->setTextFormat(Qt::PlainText);
    ui::readerStyle(title, ui::styles::brand(c::titleText) + "background:transparent;");
    query = new QLineEdit(this);
    query->setObjectName("rrKosyncQuery");
    query->setMaxLength(256);
    ui::readerStyle(query, "QLineEdit{border:none;padding:0;background:palette(window);" +
                               ui::styles::font(c::queryText) + "}");
    query->setTextMargins(c::inset - m::control::border, 0, c::inset, 0);
    query->setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhSensitiveData);
    query->hide();
    keyboard = makeKeyboard(this, query);
    Q_ASSERT(keyboard);
    keyboard->submitted = [this] {
        commitEdit();
    };
    connect(query, &QLineEdit::returnPressed, this, [this] {
        commitEdit();
    });
    message = new QLabel(this);
    message->setObjectName("rrKosyncMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    ui::readerStyle(message,
                    ui::styles::font(c::descriptionText) + "background:transparent;color:palette(placeholder-text);");
    const Field fields[fieldCount] = {Field::Server, Field::Username, Field::Password};
    const char *names[fieldCount] = {"rrKosyncServer", "rrKosyncUsername", "rrKosyncPassword"};
    for (int i = 0; i < fieldCount; ++i) {
        auto *item = new ui::ListRow(this);
        item->setObjectName(names[i]);
        rows[i] = item;
        const auto field = fields[i];
        connect(item, &QPushButton::clicked, this, [this, field] {
            beginEdit(field);
        });
    }
    test = ui::makeCaptionButton(this, nullptr, "rrKosyncTest", strings::get("kosync_panel.test"), [this] {
        run(false);
    });
    save = ui::makeCaptionButton(this, nullptr, "rrKosyncSave", strings::get("kosync_panel.save"), [this] {
        run(true);
    });
    for (auto *action : {test, save}) {
        action->setRule(Qt::BottomEdge);
        ui::readerStyle(action, ui::styles::font(c::labelText));
    }
    hide();
}

void KosyncPanel::setService(KosyncSettingsService value) {
    service = std::move(value);
}

ui::ListRow *KosyncPanel::row(Field field) const {
    return rows[int(field) - 1];
}
QString KosyncPanel::label(Field field) const {
    return field == Field::Server     ? strings::get("kosync_panel.server")
           : field == Field::Username ? strings::get("kosync_panel.username")
                                      : strings::get("kosync_panel.password");
}
QString KosyncPanel::value(Field field) const {
    if (field == Field::Server)
        return server;
    if (field == Field::Username)
        return username;
    // The password is shown only as dots, and a stored one is never read back.
    return password.isEmpty() && !hasStoredPassword ? QString() : QString(8, QChar(0x2022));
}

void KosyncPanel::open() {
    const auto stored = service.load();
    server = stored.server;
    username = stored.username;
    password.clear();
    hasStoredPassword = stored.hasPassword;
    working = false;
    ++epoch;
    editing = Field::None;
    query->hide();
    title->show();
    setMessage(strings::get("kosync_panel.intro"));
    ui::Panel::show();
    raise();
    redraw();
}

void KosyncPanel::stop() {
    ++epoch;
    working = false;
    query->clearFocus();
    keyboard->hide();
    QGuiApplication::inputMethod()->hide();
}

void KosyncPanel::setMessage(const QString &text) {
    message->setText(text);
    if (isVisible())
        redraw();
}

void KosyncPanel::beginEdit(Field field) {
    editing = field;
    query->setEchoMode(field == Field::Password ? QLineEdit::Password : QLineEdit::Normal);
    query->setPlaceholderText(label(field));
    query->setAccessibleName(label(field));
    // A password is never shown again, so editing one starts empty.
    query->setText(field == Field::Server ? server : field == Field::Username ? username : QString());
    title->hide();
    query->show();
    redraw();
    query->setFocus();
    keyboard->show();
}

void KosyncPanel::commitEdit() {
    if (editing == Field::None)
        return;
    const auto text = query->text();
    if (editing == Field::Server)
        server = text.trimmed();
    else if (editing == Field::Username)
        username = text.trimmed();
    else
        password = text;
    // A test or save still running was for the old login. Drop its reply.
    ++epoch;
    working = false;
    endEdit();
    setMessage(strings::get("kosync_panel.intro"));
    redraw();
}

void KosyncPanel::endEdit() {
    editing = Field::None;
    query->clearFocus();
    keyboard->hide();
    QGuiApplication::inputMethod()->hide();
    query->hide();
    title->show();
}

void KosyncPanel::run(bool saveAfter) {
    if (working)
        return;
    if (editing != Field::None)
        commitEdit();
    if (server.isEmpty() || username.isEmpty() || (password.isEmpty() && !hasStoredPassword)) {
        setMessage(strings::get("kosync_panel.invalid"));
        return;
    }
    working = true;
    const int started = ++epoch;
    setMessage(strings::get("kosync_panel.testing"));
    using Outcome = KosyncSettingsService::Outcome;
    const QPointer<KosyncPanel> guard(this);
    service.test(server, username, password, [guard, started, saveAfter](KosyncSettingsService::TestResult result) {
        if (!guard || guard->epoch != started)
            return;
        guard->working = false;
        QString text;
        switch (result.outcome) {
        case Outcome::Success:
            if (!saveAfter) {
                text = strings::get("kosync_panel.success");
                break;
            }
            {
                QString error;
                if (guard->service.save(guard->server, guard->username, guard->password, &error)) {
                    guard->hasStoredPassword = true;
                    guard->password.clear();
                    text = strings::get("kosync_panel.saved");
                } else {
                    text = strings::get("kosync_panel.save_failed");
                }
            }
            break;
        case Outcome::Invalid:
            text = strings::get("kosync_panel.invalid");
            break;
        case Outcome::BadLogin:
            text = strings::get("kosync_panel.bad_login");
            break;
        case Outcome::NotKosync:
            text = strings::get("kosync_panel.not_kosync");
            break;
        case Outcome::Unreachable:
            text = result.detail.isEmpty() ? strings::get("kosync_panel.unreachable")
                                           : strings::get("kosync_panel.unreachable_detail").arg(result.detail);
            break;
        case Outcome::Rejected:
            text = strings::get("kosync_panel.rejected");
            break;
        }
        guard->setMessage(text);
    });
}

void KosyncPanel::resizeEvent(QResizeEvent *) {
    redraw();
    keyboard->place();
}

void KosyncPanel::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    ui::paintHeaderRules(painter, this);
}

void KosyncPanel::redraw() {
    const int textWidth = qMax(1, width() - 2 * c::inset), cellHeight = c::actionHeight - c::heavyRule;
    section->setGeometry(c::inset, 0, textWidth, c::statusHeight - m::control::border);
    back->setGeometry(0, c::statusHeight, c::cellWidth, cellHeight);
    title->setGeometry(c::cellWidth + c::inset, c::statusHeight, qMax(1, width() - c::cellWidth - 2 * c::inset),
                       cellHeight);
    query->setGeometry(c::cellWidth, c::statusHeight, qMax(1, width() - c::cellWidth), cellHeight);
    // The message sits above the rows so a result stays visible with the keyboard up.
    int y = c::headerHeight + c::textGap;
    message->setGeometry(c::inset, y, textWidth, message->heightForWidth(textWidth));
    y = message->geometry().bottom() + 1 + c::textGap;
    const Field fields[fieldCount] = {Field::Server, Field::Username, Field::Password};
    for (auto field : fields) {
        auto *item = row(field);
        item->setTitle(label(field));
        const auto text = value(field);
        item->setDetail(text.isEmpty() ? strings::get("kosync_panel.not_set") : text);
        item->setGeometry(0, y, width(), m::list::twoLines);
        y += m::list::twoLines;
    }
    for (auto *action : {test, save}) {
        action->setGeometry(0, y, width(), m::list::twoLines);
        action->setEnabled(!working);
        y += m::list::twoLines;
    }
}
}
