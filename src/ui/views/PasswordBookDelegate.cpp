// coding: utf-8
// =============================================================================
// PasswordBookDelegate.cpp
// =============================================================================
#include "PasswordBookDelegate.h"
#include "IconKit.h"
#include "Theme.h"
#include "VaultItemUi.h"

#include <QBrush>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyle>
#include <QVariant>

#include <algorithm>
#include <string>

namespace yuli::vault::ui {

namespace {

/// 列表项统一高度（含Tags与不含Tags均使用此高度，便于 setUniformItemSizes）。
constexpr int kItemHeight = 72;
/// 列表项左右内边距。
constexpr int kPaddingH = 12;
/// 列表项上下内边距。
constexpr int kPaddingV = 8;
/// 头像尺寸。
constexpr int kAvatarSize = 40;
/// 头像与文本列间距。
constexpr int kAvatarGap = 10;
/// chevron 尺寸。
constexpr int kChevronSize = 16;
/// 名称与Tags行垂直间距。
constexpr int kLineGap = 4;
/// Tags chip 之间水平间距。
constexpr int kChipSpacing = 4;
/// Tags chip 左右内边距。
constexpr int kChipPadH = 6;
/// Tags chip 上下内边距。
constexpr int kChipPadV = 1;
/// Tags chip 圆角。
constexpr int kChipRadius = 9;
/// 头像圆角（直径的一半 = 圆形）。
constexpr int kAvatarRadius = 20;

/// 取字符串首字母（大写）作为头像字符，回退 "?"。
QString avatar_letter(const std::string& text) {
    if (text.empty()) return QStringLiteral("?");
    QChar ch = QChar::fromLatin1(text[0]).toUpper();
    if (!ch.isLetterOrNumber()) return QStringLiteral("?");
    return ch;
}

/// Theme相关颜色组：dark / light 各一组。
struct ThemePalette {
    QColor avatar_bg;       ///< 头像圆形背景
    QColor avatar_fg;       ///< 头像字符颜色
    QColor name_color;      ///< Title文字色
    QColor chip_bg;         ///< Tags chip 背景
    QColor chip_fg;         ///< Tags chip 文字
    QColor chip_border;     ///< Tags chip 边框
    QColor chip_more_fg;    ///< "+N" 文字色
    QColor selected_bg;     ///< 选Medium态背景
    QColor hover_bg;        ///< 悬停态背景
};

ThemePalette palette_for_theme() {
    const bool dark = Theme::is_dark();
    if (dark) {
        return ThemePalette{
            QColor(59, 107, 255, 36),    // avatar_bg  rgba(59,107,255,0.14)
            QColor(0x3b, 0x6b, 0xff),     // avatar_fg  #3b6bff
            QColor(0xe8, 0xec, 0xf4),     // name       #e8ecf4
            QColor(59, 107, 255, 46),     // chip_bg    rgba(59,107,255,0.18)
            QColor(0x8f, 0xb4, 0xff),     // chip_fg    #8fb4ff
            QColor(59, 107, 255, 71),     // chip_border rgba(59,107,255,0.28)
            QColor(0x8b, 0x95, 0xa8),     // chip_more  #8b95a8
            QColor(59, 107, 255, 36),     // selected   rgba(59,107,255,0.14)
            QColor(59, 107, 255, 10),     // hover      rgba(59,107,255,0.04)
        };
    }
    return ThemePalette{
        QColor(47, 95, 255, 26),       // avatar_bg  rgba(47,95,255,0.10)
        QColor(0x2f, 0x5f, 0xff),       // avatar_fg  #2f5fff
        QColor(0x0f, 0x16, 0x26),       // name       #0f1626
        QColor(47, 95, 255, 26),        // chip_bg    rgba(47,95,255,0.10)
        QColor(0x2a, 0x55, 0xc8),       // chip_fg    #2a55c8
        QColor(47, 95, 255, 51),        // chip_border rgba(47,95,255,0.20)
        QColor(0x5c, 0x66, 0x78),       // chip_more  #5c6678
        QColor(47, 95, 255, 26),        // selected   rgba(47,95,255,0.10)
        QColor(15, 22, 38, 10),         // hover      rgba(15,22,38,0.04)
    };
}

}  // namespace

PasswordBookDelegate::PasswordBookDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
    // Theme切换时清空 chevron pixmap 缓存，下次 paint 自动按新Theme重新渲染。
    if (auto* theme = Theme::instance()) {
        QObject::connect(theme, &Theme::theme_changed,
                         parent ? parent : this, [this]() { clear_pixmap_cache(); });
    }
}

PasswordBookDelegate::~PasswordBookDelegate() = default;

QSize PasswordBookDelegate::sizeHint(const QStyleOptionViewItem& option,
                                     const QModelIndex& /*index*/) const {
    // 宽度自适应：若父组件已布局则用 option.rect.width()，否则用默认 320。
    // 高度统一 72px，便于 QListView::setUniformItemSizes(true) 优化。
    const int width = option.rect.width() > 0 ? option.rect.width() : 320;
    return QSize(width, kItemHeight);
}

void PasswordBookDelegate::paint(QPainter* painter,
                                 const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    if (!index.isValid()) return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const auto pal = palette_for_theme();

    // ── 1. 背景（选Medium / 悬停 / 默认） ──
    // 选Medium态优先于悬停态。QPainter 无 fillRoundedRect，用 path + fillPath。
    QRect bg_rect = option.rect.adjusted(kPaddingH / 2, 2, -kPaddingH / 2, -2);
    if (option.state & QStyle::State_Selected) {
        QPainterPath bg_path;
        bg_path.addRoundedRect(QRectF(bg_rect), 8, 8);
        painter->fillPath(bg_path, pal.selected_bg);
    } else if (option.state & QStyle::State_MouseOver) {
        QPainterPath bg_path;
        bg_path.addRoundedRect(QRectF(bg_rect), 8, 8);
        painter->fillPath(bg_path, pal.hover_bg);
    }

    // ── 2. 取 entry 数据 ──
    const auto entry = index.data(Qt::UserRole).value<core::PasswordEntry>();

    // 头像字符优先级：entry_name → account → website
    std::string avatar_src = entry.title;
    if (avatar_src.empty()) avatar_src = entry.account;
    if (avatar_src.empty()) avatar_src = entry.website;
    const QString avatar_text = avatar_letter(avatar_src);

    // 名称（用于显示）同样回退
    std::string title_src = entry.title;
    if (title_src.empty()) title_src = entry.account;
    if (title_src.empty()) title_src = entry.website;
    const QString name_text = QString::fromStdString(title_src);

    // ── 3. 头像（左侧 12,8 偏移，40x40 圆形） ──
    const QRect avatar_rect(
        option.rect.left() + kPaddingH,
        option.rect.top() + (option.rect.height() - kAvatarSize) / 2,
        kAvatarSize, kAvatarSize);
    {
        QPainterPath circle;
        circle.addRoundedRect(QRectF(avatar_rect), kAvatarRadius, kAvatarRadius);
        painter->fillPath(circle, pal.avatar_bg);
        QFont avatar_font = option.font;
        avatar_font.setPixelSize(14);
        avatar_font.setBold(true);
        painter->setFont(avatar_font);
        painter->setPen(pal.avatar_fg);
        painter->drawText(avatar_rect, Qt::AlignCenter, avatar_text);
    }

    // ── 4. 名称（elide） ──
    // 名称可用宽度 = 总宽 - 左右内边距 - 头像 - 头像间距 - chevron - chevron 右间距
    const int name_width = option.rect.width()
                           - kPaddingH * 2
                           - kAvatarSize
                           - kAvatarGap
                           - kChevronSize
                           - kPaddingH;
    QFont name_font = option.font;
    name_font.setPixelSize(13);
    name_font.setBold(true);
    const QFontMetrics name_fm(name_font);
    const QString elided_name = name_fm.elidedText(name_text, Qt::ElideRight,
                                                   qMax(0, name_width));
    const QRect name_rect(
        avatar_rect.right() + kAvatarGap,
        option.rect.top() + kPaddingV,
        name_width,
        name_fm.height());
    painter->setFont(name_font);
    painter->setPen(pal.name_color);
    painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter, elided_name);

    // Type badge + optional tags (type is always shown so later item kinds can coexist).
    {
        QFont chip_font = option.font;
        chip_font.setPixelSize(10);
        const QFontMetrics chip_fm(chip_font);
        const int chip_y = name_rect.bottom() + kLineGap;
        const int chip_height = chip_fm.height() + kChipPadV * 2;
        int chip_x = avatar_rect.right() + kAvatarGap;
        const int chip_right = option.rect.right() - kPaddingH - kChevronSize - kPaddingH;

        auto draw_chip = [&](const QString& chip_text, bool more_style) {
            const int text_w = chip_fm.horizontalAdvance(chip_text);
            const int chip_w = text_w + kChipPadH * 2;
            if (chip_x + chip_w > chip_right) return false;
            const QRect chip_rect(chip_x, chip_y, chip_w, chip_height);
            QPainterPath chip_path;
            chip_path.addRoundedRect(QRectF(chip_rect), kChipRadius, kChipRadius);
            painter->fillPath(chip_path, pal.chip_bg);
            painter->setPen(QPen(pal.chip_border, 1));
            painter->drawPath(chip_path);
            painter->setPen(more_style ? pal.chip_more_fg : pal.chip_fg);
            painter->drawText(chip_rect, Qt::AlignCenter, chip_text);
            chip_x += chip_w + kChipSpacing;
            return true;
        };

        painter->setFont(chip_font);
        draw_chip(vault_item_type_label(entry.type), false);

        const size_t max_show = 3;
        const size_t show_count = std::min(entry.tags.size(), max_show);
        for (size_t i = 0; i < show_count; ++i) {
            if (!draw_chip(QString::fromStdString(entry.tags[i].name), false)) break;
        }
        if (entry.tags.size() > max_show) {
            const int more = static_cast<int>(entry.tags.size() - max_show);
            draw_chip(QStringLiteral("+%1").arg(more), true);
        }
    }

    // ── 6. chevron（右侧 12px 内边距，16x16） ──
    const QPixmap chev = chevron_pixmap();
    if (!chev.isNull()) {
        const QRect chev_rect(
            option.rect.right() - kPaddingH - kChevronSize,
            option.rect.top() + (option.rect.height() - kChevronSize) / 2,
            kChevronSize, kChevronSize);
        painter->drawPixmap(chev_rect.topLeft(), chev);
    }

    painter->restore();
}

QPixmap PasswordBookDelegate::chevron_pixmap() const {
    const QString key = Theme::is_dark() ? QStringLiteral("chevron|dark")
                                         : QStringLiteral("chevron|light");
    const auto it = pixmap_cache_.constFind(key);
    if (it != pixmap_cache_.constEnd()) return it.value();
    QPixmap pm = tinted_pixmap(QStringLiteral(":/icons/chevron-right.svg"),
                               IconRole::Normal, QSize(16, 16));
    if (!pm.isNull()) pixmap_cache_.insert(key, pm);
    return pm;
}

void PasswordBookDelegate::clear_pixmap_cache() {
    pixmap_cache_.clear();
}

}  // namespace yuli::vault::ui
