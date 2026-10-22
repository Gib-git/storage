#include "NasDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "Theme.h"

using namespace storage;

NasDialog::NasDialog(const NasConfig& c, QWidget* parent) : QDialog(parent), transport_(c.transport) {
    setWindowTitle(c.name.empty() ? tr("Add NAS") : tr("Edit NAS"));
    setMinimumWidth(520);
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Storage connects with your computer's own <b>ssh</b> and your SSH keys — no passwords "
                                "are stored. Indexing, hashing and moving run on the NAS itself."),
                             this);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto* form = new QFormLayout;
    name_ = new QLineEdit(QString::fromStdString(c.name), this);
    name_->setPlaceholderText(tr("e.g. Home NAS"));
    host_ = new QLineEdit(QString::fromStdString(c.host), this);
    host_->setPlaceholderText(tr("192.168.1.20, nas.local or an alias from ~/.ssh/config"));
    user_ = new QLineEdit(QString::fromStdString(c.user), this);
    user_->setPlaceholderText(tr("optional"));
    port_ = new QSpinBox(this);
    port_->setRange(0, 65535);
    port_->setSpecialValueText(tr("default (22)"));
    port_->setValue(c.port);
    auto* keyRow = new QHBoxLayout;
    key_ = new QLineEdit(QString::fromStdString(c.identityFile), this);
    key_->setPlaceholderText(tr("optional — uses your ssh agent and default keys"));
    auto* browse = new QToolButton(this);
    browse->setText(tr("…"));
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Private key"), QDir::home().filePath(".ssh"));
        if (!f.isEmpty()) key_->setText(f);
    });
    keyRow->addWidget(key_, 1);
    keyRow->addWidget(browse);
    root_ = new QLineEdit(QString::fromStdString(c.root), this);
    root_->setPlaceholderText(tr("folder to scan, e.g. /volume1 or /mnt/user"));
    form->addRow(tr("Name:"), name_);
    form->addRow(tr("Host:"), host_);
    form->addRow(tr("User:"), user_);
    form->addRow(tr("Port:"), port_);
    form->addRow(tr("Key file:"), keyRow);
    form->addRow(tr("Folder:"), root_);
    v->addLayout(form);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    testButton_ = buttons->addButton(tr("Test Connection"), QDialogButtonBox::ActionRole);
    connect(testButton_, &QPushButton::clicked, this, &NasDialog::test);
    connect(buttons, &QDialogButtonBox::accepted, this, &NasDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

NasConfig NasDialog::config() const {
    NasConfig c;
    c.host = host_->text().trimmed().toStdString();
    c.name = name_->text().trimmed().isEmpty() ? c.host : name_->text().trimmed().toStdString();
    c.user = user_->text().trimmed().toStdString();
    c.port = port_->value();
    c.identityFile = key_->text().trimmed().toStdString();
    c.root = root_->text().trimmed().isEmpty() ? "/" : root_->text().trimmed().toStdString();
    c.transport = transport_;
    return c;
}

void NasDialog::test() {
    if (task_.running() || host_->text().trimmed().isEmpty()) return;
    testButton_->setEnabled(false);
    status_->setStyleSheet({});
    status_->setText(tr("Connecting…"));
    const NasConfig cfg = config();
    task_.start<QString>(
        this,
        [cfg] {
            SshFileSystem fs(cfg, 1);
            std::string message;
            if (auto ec = fs.connect(message))
                return QString("ERR") + QString::fromStdString(message.empty() ? ec.message() : message);
            DirEntry e;
            const bool rootOk = !fs.stat(cfg.root, e) && e.kind == NodeKind::Directory;
            const auto& c = fs.caps();
            QString s = QObject::tr("Connected to %1. Listing: %2 · hashing: %3 · clones: %4.")
                            .arg(QString::fromStdString(c.system), c.gnuFind ? QObject::tr("fast") : QObject::tr("compatible"),
                                 c.sha.empty() ? QObject::tr("unavailable") : QString::fromStdString(c.sha),
                                 c.reflink ? QObject::tr("yes") : QObject::tr("no"));
            if (!rootOk) return QString("ERR") + s + QObject::tr("\nBut the folder %1 was not found.").arg(QString::fromStdString(cfg.root));
            return QString("OK") + s;
        },
        [this, cfg](QString result) {
            testButton_->setEnabled(true);
            const bool ok = result.startsWith("OK");
            QString text = result.mid(ok ? 2 : 3);
            if (!ok && text.contains("ermission denied"))
                text += tr("\n\nSet up key login once in a terminal:\n  ssh-copy-id %1%2")
                            .arg(cfg.user.empty() ? QString() : QString::fromStdString(cfg.user) + "@", QString::fromStdString(cfg.host));
            status_->setStyleSheet(QString("color:%1").arg(ok ? "#22A06B" : "#EF4444"));
            status_->setText(text);
        });
}

void NasDialog::accept() {
    if (host_->text().trimmed().isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Enter the NAS host name or IP address."));
        return;
    }
    if (!root_->text().trimmed().isEmpty() && !root_->text().trimmed().startsWith('/')) {
        QMessageBox::information(this, windowTitle(), tr("The folder must be an absolute path, like /volume1."));
        return;
    }
    task_.join();
    QDialog::accept();
}
