#pragma once
// ---------------------------------------------------------------------------
// The skill library.
//
// Lists the installed procedures, shows what each is for, and lets the user
// enable or disable them. A skill the agent wrote for itself appears here
// marked as awaiting review, and stays inert until a person approves it.
// ---------------------------------------------------------------------------
#include <QListWidget>
#include <QTextBrowser>
#include <QWidget>

#include "skills/SkillRegistry.h"

class QPushButton;

namespace localcode::gui {

class SkillsPanel : public QWidget {
    Q_OBJECT

public:
    explicit SkillsPanel(SkillRegistry& registry, QWidget* parent = nullptr);

    void refresh();

signals:
    // Emitted when the user changes a skill's enabled or reviewed state, so the
    // owner can persist it.
    void skillStateChanged(const QString& id, bool enabled, bool validated);

private slots:
    void showSelected();
    void toggleEnabled();
    void markReviewed();

private:
    [[nodiscard]] const Skill* selectedSkill() const;

    SkillRegistry& registry_;
    QListWidget*   list_ = nullptr;
    QTextBrowser*  detail_ = nullptr;
    QPushButton*   toggle_ = nullptr;
    QPushButton*   review_ = nullptr;
};

}  // namespace localcode::gui
