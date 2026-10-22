#pragma once
// ---------------------------------------------------------------------------
// The mission, as the engine understands it.
//
// Shows the objective, the plan and its progress, the files touched, and - most
// importantly - whether the result has actually been verified. That last line
// is derived from evidence, not from anything the model asserted.
// ---------------------------------------------------------------------------
#include <QLabel>
#include <QString>
#include <QStringList>
#include <QTextBrowser>
#include <QWidget>

namespace localcode::gui {

class TaskPanel : public QWidget {
    Q_OBJECT

public:
    explicit TaskPanel(QWidget* parent = nullptr);

    // The rendered task-state block the agent produces.
    void setTaskBlock(const QString& block);
    void setChangedFiles(const QStringList& changed, const QStringList& conflicting);
    void setVerification(const QString& state, const QStringList& unsupportedClaims,
                         bool objectiveVerified, bool hadPlan);
    void clearVerification();
    void setGitSummary(const QString& branch, int changedFiles, bool clean);
    void clear();

signals:
    void fileActivated(const QString& relativePath);

private:
    void rebuild();

    QTextBrowser* body_ = nullptr;
    QLabel*       verification_ = nullptr;

    QString     taskBlock_;
    QStringList changed_;
    QStringList conflicting_;
    QString     gitBranch_;
    int         gitChanged_ = 0;
    bool        gitClean_ = true;
};

}  // namespace localcode::gui
