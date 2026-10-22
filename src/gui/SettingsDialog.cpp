#include "SettingsDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Theme.h"
#include "storage/core/api.hpp"

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    setMinimumWidth(560);
    QSettings s;
    auto* v = new QVBoxLayout(this);

    auto* general = new QGroupBox(tr("General"), this);
    auto* gf = new QFormLayout(general);
    theme_ = new QComboBox(general);
    theme_->addItems({tr("Match System"), tr("Light"), tr("Dark")});
    theme_->setCurrentIndex(static_cast<int>(theme::current()));
    gf->addRow(tr("Appearance:"), theme_);
    rescanDays_ = new QSpinBox(general);
    rescanDays_->setRange(0, 365);
    rescanDays_->setSpecialValueText(tr("never"));
    rescanDays_->setSuffix(tr(" days old"));
    rescanDays_->setValue(s.value("scans/autoRescanDays", 0).toInt());
    gf->addRow(tr("Rescan saved scans when:"), rescanDays_);
    minSize_ = new QComboBox(general);
    minSize_->addItems({tr("Any size"), tr("≥ 100 KB"), tr("≥ 1 MB"), tr("≥ 10 MB"), tr("≥ 100 MB")});
    minSize_->setCurrentIndex(s.value("dupes/minSizeIndex", 2).toInt());
    gf->addRow(tr("Duplicates: ignore files"), minSize_);
    v->addWidget(general);

    auto* ai = new QGroupBox(tr("AI suggestions (Ollama)"), this);
    auto* af = new QFormLayout(ai);
    ollamaHost_ = new QLineEdit(s.value("ai/host", "http://localhost:11434").toString(), ai);
    ollamaModel_ = new QLineEdit(s.value("ai/model", "llama3.2").toString(), ai);
    af->addRow(tr("Ollama address:"), ollamaHost_);
    af->addRow(tr("Model:"), ollamaModel_);
    auto* aiNote = new QLabel(tr("Only file names, kinds, sizes and dates are sent — never file contents."), ai);
    aiNote->setStyleSheet(QString("color:%1").arg(theme::mutedText().name()));
    af->addRow(aiNote);
    v->addWidget(ai);

    auto* api = new QGroupBox(tr("Automation"), this);
    auto* apf = new QFormLayout(api);
    apiEnabled_ = new QCheckBox(tr("Run the local API while Storage is open"), api);
    apiEnabled_->setChecked(s.value("api/enabled", false).toBool());
    apiPort_ = new QSpinBox(api);
    apiPort_->setRange(1024, 65535);
    apiPort_->setValue(s.value("api/port", 8765).toInt());
    apf->addRow(apiEnabled_);
    apf->addRow(tr("Port (127.0.0.1 only):"), apiPort_);
    const QString token = QString::fromStdString(storage::apiToken());
    auto* tokenRow = new QHBoxLayout;
    auto* tokenEdit = new QLineEdit(token, api);
    tokenEdit->setReadOnly(true);
    tokenEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    auto* copyToken = new QPushButton(tr("Copy"), api);
    connect(copyToken, &QPushButton::clicked, this, [token] { QApplication::clipboard()->setText(token); });
    tokenRow->addWidget(tokenEdit, 1);
    tokenRow->addWidget(copyToken);
    apf->addRow(tr("API token:"), tokenRow);

    // The CLI sits next to the app binary in every release package.
    const QString cli = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "storagectl.exe"
#else
        "storagectl"
#endif
    );
    const QString mcp = QString("claude mcp add storage -- \"%1\" mcp").arg(QDir::toNativeSeparators(cli));
    auto* mcpRow = new QHBoxLayout;
    auto* mcpEdit = new QLineEdit(mcp, api);
    mcpEdit->setReadOnly(true);
    auto* copyMcp = new QPushButton(tr("Copy"), api);
    connect(copyMcp, &QPushButton::clicked, this, [mcp] { QApplication::clipboard()->setText(mcp); });
    mcpRow->addWidget(mcpEdit, 1);
    mcpRow->addWidget(copyMcp);
    apf->addRow(tr("Use with Claude Code:"), mcpRow);
    v->addWidget(api);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

void SettingsDialog::save() {
    QSettings s;
    const bool apiChanged = s.value("api/enabled", false).toBool() != apiEnabled_->isChecked() ||
                            s.value("api/port", 8765).toInt() != apiPort_->value();
    s.setValue("scans/autoRescanDays", rescanDays_->value());
    s.setValue("dupes/minSizeIndex", minSize_->currentIndex());
    s.setValue("ai/host", ollamaHost_->text().trimmed());
    s.setValue("ai/model", ollamaModel_->text().trimmed());
    s.setValue("api/enabled", apiEnabled_->isChecked());
    s.setValue("api/port", apiPort_->value());
    if (theme_->currentIndex() != static_cast<int>(theme::current())) emit themeChanged(theme_->currentIndex());
    if (apiChanged) emit apiSettingsChanged();
}
