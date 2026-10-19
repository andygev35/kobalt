#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "platform/api/keyboard_controller.h"
#include "platform/api/kosync_settings.h"
#include <QLabel>
#include <QLineEdit>
#include <functional>
#include <memory>

namespace kobalt {
// Edits and tests the KOReader sync login. One field and one keyboard serve all
// three values, as the search panel's do: tapping a row edits that value.
class KosyncPanel : public ui::Panel {
public:
    KosyncPanel(QWidget *parent, KeyboardFactory makeKeyboard);
    std::function<void()> dismissed;
    // The host's login service. The caller opens the panel only when it is complete.
    void setService(KosyncSettingsService service);
    void open();
    void stop();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    enum class Field { None, Server, Username, Password };
    KosyncSettingsService service;
    std::unique_ptr<KeyboardController> keyboard;
    ui::CellButton *back;
    QLabel *section, *title, *message;
    QLineEdit *query;
    ui::ListRow *rows[3];
    ui::CaptionButton *test, *save;
    QString server, username, password;
    bool hasStoredPassword = false, working = false;
    Field editing = Field::None;
    // Bumped by every change, so a reply for an older login is ignored.
    int epoch = 0;
    ui::ListRow *row(Field field) const;
    QString label(Field field) const;
    QString value(Field field) const;
    void beginEdit(Field field);
    void commitEdit();
    void endEdit();
    void run(bool saveAfter);
    void setMessage(const QString &text);
    void redraw();
};
}
