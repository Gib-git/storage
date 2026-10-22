#include "ShareDelegate.h"

#include <QApplication>
#include <QPainter>

#include "ScanModel.h"
#include "Theme.h"

void ShareDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    opt.text.clear();
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);

    const double share = std::clamp(index.data(ScanModel::ShareRole).toDouble(), 0.0, 1.0);
    const QRect r = option.rect.adjusted(6, 0, -6, 0);
    const int labelW = option.fontMetrics.horizontalAdvance("100.0%") + 6;
    const QRect bar(r.left(), r.center().y() - 3, std::max(10, r.width() - labelW), 6);

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    QColor track = option.palette.color(QPalette::Mid);
    track.setAlpha(110);
    p->setBrush(track);
    p->drawRoundedRect(bar, 3, 3);
    if (share > 0) {
        QRect fill = bar;
        fill.setWidth(std::max(2, static_cast<int>(bar.width() * share)));
        p->setBrush(theme::accent());
        p->drawRoundedRect(fill, 3, 3);
    }
    const bool selected = option.state & QStyle::State_Selected;
    p->setPen(selected ? option.palette.color(QPalette::HighlightedText) : theme::mutedText());
    p->drawText(QRect(bar.right() + 6, r.top(), labelW, r.height()), Qt::AlignVCenter | Qt::AlignRight,
                QString::number(share * 100.0, 'f', 1) + "%");
    p->restore();
}
