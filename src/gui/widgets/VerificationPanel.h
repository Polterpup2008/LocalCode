#pragma once
// ---------------------------------------------------------------------------
// What has actually been established, and against which version of the tree.
//
// The claim this panel exists to make checkable is "it works". A run can show
// a green build, a clean exit and a confident summary while the program prints
// the wrong answer, so the stages are drawn separately and never merged into a
// single reassuring word.
//
// It shows exactly what the model is shown. A friendlier version for the user
// would mean the two could disagree, and the user would be the one not told.
// ---------------------------------------------------------------------------
#include <QLabel>
#include <QTextBrowser>
#include <QWidget>

#include "../AgentController.h"

namespace localcode::gui {

class VerificationPanel : public QWidget {
    Q_OBJECT

public:
    explicit VerificationPanel(QWidget* parent = nullptr);

public slots:
    void setVerification(const UiVerification& verification);
    void clear();

    void refreshTheme() { rebuild(); }

private:
    void rebuild();

    QTextBrowser* body_ = nullptr;
    QLabel* headline_ = nullptr;
    UiVerification state_;
    bool hasState_ = false;
};

}   // namespace localcode::gui
