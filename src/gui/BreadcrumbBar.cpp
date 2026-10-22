#include "BreadcrumbBar.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

#include "Theme.h"

using namespace storage;

BreadcrumbBar::BreadcrumbBar(QWidget* parent) : QWidget(parent) {
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
}

void BreadcrumbBar::setNode(Node* node) {
    while (QLayoutItem* item = layout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    std::vector<Node*> chain;  // root first
    for (Node* n = node; n; n = n->parent) chain.insert(chain.begin(), n);

    auto separator = [this] {
        auto* sep = new QLabel("›", this);
        sep->setStyleSheet(QString("color:%1; padding:0 2px;").arg(theme::mutedText().name()));
        layout_->addWidget(sep);
    };

    // Keep the bar compact: root › … › last few levels.
    constexpr size_t kTail = 4;
    const bool collapse = chain.size() > kTail + 2;
    for (size_t i = 0; i < chain.size(); ++i) {
        if (collapse && i > 0 && i < chain.size() - kTail) {
            if (i == 1) {
                layout_->addWidget(new QLabel("…", this));
                separator();
            }
            continue;
        }
        Node* n = chain[i];
        QString label = QString::fromStdString(n->name);
        if (!n->parent) {
            const QString base = QFileInfo(label).fileName();
            if (!base.isEmpty()) label = base;
        }
        const bool last = i + 1 == chain.size();
        auto* b = new QToolButton(this);
        b->setObjectName("Crumb");
        b->setText(label);
        b->setToolTip(QString::fromStdString(n->path()));
        b->setAutoRaise(true);
        b->setEnabled(!last);
        connect(b, &QToolButton::clicked, this, [this, n] { emit navigate(n); });
        layout_->addWidget(b);
        if (!last) separator();
    }
    layout_->addStretch(1);
}
