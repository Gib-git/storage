#include "RuleDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Theme.h"

using namespace storage;

namespace {
const Category kTypes[] = {Category::Image,       Category::Video,  Category::Audio, Category::Document,
                           Category::Archive,     Category::Code,   Category::Application, Category::System,
                           Category::Other};

QString tokenHelp() {
    return QObject::tr("<b>{type}</b> Images, Videos… · <b>{ext}</b> pdf · <b>{EXT}</b> PDF · <b>{name}</b> file name "
                       "without extension · <b>{filename}</b> · <b>{year}</b> <b>{month}</b> <b>{day}</b> "
                       "<b>{monthname}</b> (photos use the date taken) · <b>{size}</b> Small/Medium/Large/Huge · "
                       "<b>{first}</b> first letter · <b>{parent}</b> current folder name. Use <b>/</b> for subfolders.");
}
}  // namespace

RuleDialog::RuleDialog(const OrganizeRule& r, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Organize Rule"));
    setMinimumWidth(560);
    auto* v = new QVBoxLayout(this);

    auto* top = new QFormLayout;
    name_ = new QLineEdit(QString::fromStdString(r.name), this);
    name_->setPlaceholderText(tr("e.g. Invoices"));
    top->addRow(tr("Rule name:"), name_);
    v->addLayout(top);

    auto* matchBox = new QGroupBox(tr("Files that match"), this);
    auto* mv = new QVBoxLayout(matchBox);
    auto* grid = new QGridLayout;
    int i = 0;
    for (Category c : kTypes) {
        auto* cb = new QCheckBox(QString::fromUtf8(categoryName(c)), matchBox);
        cb->setProperty("category", static_cast<int>(c));
        cb->setChecked(std::find(r.match.categories.begin(), r.match.categories.end(), c) != r.match.categories.end());
        types_.push_back(cb);
        grid->addWidget(cb, i / 5, i % 5);
        ++i;
    }
    mv->addWidget(new QLabel(tr("Kinds of file (none ticked = any):"), matchBox));
    mv->addLayout(grid);

    auto* form = new QFormLayout;
    QStringList exts;
    for (const auto& e : r.match.extensions) exts << QString::fromStdString(e);
    extensions_ = new QLineEdit(exts.join(", "), matchBox);
    extensions_->setPlaceholderText(tr("e.g. pdf, docx (any if empty)"));
    form->addRow(tr("Extensions:"), extensions_);
    contains_ = new QLineEdit(QString::fromStdString(r.match.nameContains), matchBox);
    contains_->setPlaceholderText(tr("text in the file name, e.g. invoice"));
    form->addRow(tr("Name contains:"), contains_);
    regex_ = new QLineEdit(QString::fromStdString(r.match.nameRegex), matchBox);
    regex_->setPlaceholderText(tr("advanced: regular expression, e.g. ^IMG_\\d+"));
    form->addRow(tr("Name pattern:"), regex_);

    auto spin = [matchBox](int max, int value, const QString& suffix, const QString& zero) {
        auto* s = new QSpinBox(matchBox);
        s->setRange(0, max);
        s->setValue(value);
        s->setSuffix(suffix);
        s->setSpecialValueText(zero);
        return s;
    };
    auto* sizes = new QHBoxLayout;
    minMb_ = spin(10000000, static_cast<int>(r.match.minSize / (1024 * 1024)), tr(" MB"), tr("no minimum"));
    maxMb_ = spin(10000000, static_cast<int>(r.match.maxSize / (1024 * 1024)), tr(" MB"), tr("no maximum"));
    sizes->addWidget(minMb_);
    sizes->addWidget(new QLabel(tr("to"), matchBox));
    sizes->addWidget(maxMb_);
    form->addRow(tr("Size:"), sizes);
    auto* ages = new QHBoxLayout;
    older_ = spin(36500, r.match.olderThanDays, tr(" days"), tr("any age"));
    newer_ = spin(36500, r.match.newerThanDays, tr(" days"), tr("any age"));
    ages->addWidget(new QLabel(tr("older than"), matchBox));
    ages->addWidget(older_);
    ages->addWidget(new QLabel(tr("newer than"), matchBox));
    ages->addWidget(newer_);
    form->addRow(tr("Modified:"), ages);
    mv->addLayout(form);
    v->addWidget(matchBox);

    auto* goBox = new QGroupBox(tr("Go to"), this);
    auto* gv = new QVBoxLayout(goBox);
    moveRadio_ = new QRadioButton(tr("Move into folder:"), goBox);
    leaveRadio_ = new QRadioButton(tr("Leave them where they are (and stop checking other rules)"), goBox);
    folder_ = new QLineEdit(QString::fromStdString(r.folder), goBox);
    folder_->setPlaceholderText(tr("e.g. Finance/Invoices/{year}"));
    rename_ = new QLineEdit(QString::fromStdString(r.rename), goBox);
    rename_->setPlaceholderText(tr("optional, e.g. {year}-{month}-{day} {name}  (extension is kept)"));
    gv->addWidget(moveRadio_);
    auto* f2 = new QFormLayout;
    f2->addRow(tr("Folder:"), folder_);
    f2->addRow(tr("Rename to:"), rename_);
    gv->addLayout(f2);
    gv->addWidget(leaveRadio_);
    example_ = new QLabel(goBox);
    example_->setStyleSheet(QString("color:%1").arg(theme::mutedText().name()));
    gv->addWidget(example_);
    auto* help = new QLabel(tokenHelp(), goBox);
    help->setWordWrap(true);
    help->setStyleSheet(QString("color:%1; font-size:11px").arg(theme::mutedText().name()));
    gv->addWidget(help);
    v->addWidget(goBox);
    (r.leave ? leaveRadio_ : moveRadio_)->setChecked(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &RuleDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    for (auto* le : {folder_, rename_}) connect(le, &QLineEdit::textChanged, this, &RuleDialog::updateExample);
    connect(moveRadio_, &QRadioButton::toggled, this, [this](bool on) {
        folder_->setEnabled(on);
        rename_->setEnabled(on);
        updateExample();
    });
    folder_->setEnabled(!r.leave);
    rename_->setEnabled(!r.leave);
    updateExample();
}

void RuleDialog::updateExample() {
    if (leaveRadio_->isChecked()) {
        example_->setText(tr("Matching files stay where they are."));
        return;
    }
    // Rough preview with a sample file; the real preview uses your files.
    QString f = folder_->text(), n = rename_->text();
    const QMap<QString, QString> sample = {{"{type}", "Images"}, {"{ext}", "jpg"},        {"{EXT}", "JPG"},
                                           {"{name}", "IMG_0042"}, {"{filename}", "IMG_0042.jpg"}, {"{year}", "2024"},
                                           {"{month}", "07"},      {"{day}", "14"},         {"{monthname}", "July"},
                                           {"{size}", "Medium (1-100 MB)"}, {"{first}", "I"}, {"{parent}", "Camera"}};
    for (auto it = sample.begin(); it != sample.end(); ++it) {
        f.replace(it.key(), it.value());
        n.replace(it.key(), it.value());
    }
    if (n.isEmpty()) n = "IMG_0042.jpg";
    else if (!n.contains('.')) n += ".jpg";
    example_->setText(tr("Example: IMG_0042.jpg → %1/%2").arg(f.isEmpty() ? QString(".") : f, n));
}

OrganizeRule RuleDialog::rule() const {
    OrganizeRule r;
    r.name = name_->text().trimmed().toStdString();
    for (auto* cb : types_)
        if (cb->isChecked()) r.match.categories.push_back(static_cast<Category>(cb->property("category").toInt()));
    for (QString e : extensions_->text().split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts)) {
        e = e.toLower();
        if (e.startsWith('.')) e.remove(0, 1);
        r.match.extensions.push_back(e.toStdString());
    }
    r.match.nameContains = contains_->text().trimmed().toStdString();
    r.match.nameRegex = regex_->text().trimmed().toStdString();
    r.match.minSize = static_cast<uint64_t>(minMb_->value()) * 1024 * 1024;
    r.match.maxSize = static_cast<uint64_t>(maxMb_->value()) * 1024 * 1024;
    r.match.olderThanDays = older_->value();
    r.match.newerThanDays = newer_->value();
    r.leave = leaveRadio_->isChecked();
    if (!r.leave) {
        r.folder = folder_->text().trimmed().toStdString();
        r.rename = rename_->text().trimmed().toStdString();
    }
    return r;
}

void RuleDialog::accept() {
    if (name_->text().trimmed().isEmpty()) name_->setText(tr("Rule"));
    if (moveRadio_->isChecked() && folder_->text().trimmed().isEmpty() && rename_->text().trimmed().isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Enter a folder (or a new name) for matching files."));
        return;
    }
    if (!regex_->text().trimmed().isEmpty() && !QRegularExpression(regex_->text().trimmed()).isValid()) {
        QMessageBox::information(this, windowTitle(), tr("The name pattern is not a valid regular expression."));
        return;
    }
    QDialog::accept();
}
