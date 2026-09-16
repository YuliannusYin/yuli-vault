// coding: utf-8
// =============================================================================
// IconKit.cpp — SVG 图标着色工具实现
// =============================================================================
#include "IconKit.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPushButton>
#include <QSvgRenderer>
#include <QTimer>

namespace yuli::vault::ui {

QColor icon_color(IconRole role) {
    const bool dark = Theme::is_dark();
    switch (role) {
        case IconRole::Normal:
            // muted：dark #8b95a8 / light #5c6678
            return dark ? QColor(0x8b, 0x95, 0xa8) : QColor(0x5c, 0x66, 0x78);
        case IconRole::Active:
            // 主文字色：dark #e8ecf4 / light #0f1626
            return dark ? QColor(0xe8, 0xec, 0xf4) : QColor(0x0f, 0x16, 0x26);
        case IconRole::OnPrimary:
            // primary 按钮上的图标统一白色
            return QColor(0xff, 0xff, 0xff);
        case IconRole::Danger:
            // 危险红：dark #f56363 / light #dc2626
            return dark ? QColor(0xf5, 0x63, 0x63) : QColor(0xdc, 0x26, 0x26);
        case IconRole::Success:
            // Success绿：dark #2bd576 / light #1f9d57
            return dark ? QColor(0x2b, 0xd5, 0x76) : QColor(0x1f, 0x9d, 0x57);
        case IconRole::Info:
            // 信息蓝：dark #3b6bff / light #2f5fff
            return dark ? QColor(0x3b, 0x6b, 0xff) : QColor(0x2f, 0x5f, 0xff);
    }
    return dark ? QColor(0x8b, 0x95, 0xa8) : QColor(0x5c, 0x66, 0x78);
}

namespace {

/// 着色渲染核心：SVG → 透明 pixmap → SourceIn 着色。
/// \param size  目标尺寸（由调用方决定语义：逻辑尺寸或物理尺寸）
///              engine 的 pixmap()/paint() 收到的 size 由 Qt 框架传入，
///              高 DPI 下框架会传入物理尺寸，故此处不自行乘 dpr。
QPixmap render_tinted(const QString& svg_path, const QColor& color, const QSize& size) {
    if (svg_path.isEmpty() || !color.isValid() || size.isEmpty()) return QPixmap();

    QSvgRenderer renderer(svg_path);
    if (!renderer.isValid()) return QPixmap();

    // 1. 渲染 SVG 到透明 pixmap。QSvgRenderer 不支持 currentColor，会 fallback
    //    到黑色；但后续 SourceIn 着色会整体替换颜色，故渲染出的颜色无所谓，
    //    只需要 alpha 形状正确。明确指定 bounds 确保 SVG 缩放填满整个 pixmap。
    QPixmap src(size);
    src.fill(Qt::transparent);
    {
        QPainter p(&src);
        renderer.render(&p, QRectF(QPointF(0, 0), size));
    }

    // 2. 着色：先画原图到目标，再用 CompositionMode_SourceIn 覆盖颜色矩形。
    //    SourceIn 语义：result = source.rgb * destination.alpha
    //    即用颜色填入原图的 alpha 形状。
    QPixmap tinted(size);
    tinted.fill(Qt::transparent);
    {
        QPainter p(&tinted);
        p.drawPixmap(0, 0, src);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(tinted.rect(), color);
    }
    return tinted;
}

/// 自定义 QIconEngine：按需着色渲染 SVG。
///
/// 实现遵循 Qt 官方 QSvgIconEngine 的模式：
///   - pixmap(size) 用传入 size 直接渲染，不自行乘 dpr / 不 setDevicePixelRatio。
///     Qt 框架在高 DPI 下会传入物理尺寸，清晰度由框架保证。
///   - paint(painter, rect) 调用 pixmap(rect.size()) 后用 drawPixmap(topLeft, pm)。
///
/// 避免在 engine 内部手动处理 dpr —— 那会与 Qt 框架自身的 dpr 处理叠加，
/// 导致 pixmap 物理尺寸翻倍，绘制时只显示左上角 1/4。
class TintedIconEngine : public QIconEngine {
public:
    TintedIconEngine(QString svg_path, QColor color)
        : svg_path_(std::move(svg_path)), color_(std::move(color)) {}

    QIconEngine* clone() const override {
        return new TintedIconEngine(*this);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(mode); Q_UNUSED(state);
        return render_tinted(svg_path_, color_, size);
    }

    void paint(QPainter* painter, const QRect& rect,
               QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(mode); Q_UNUSED(state);
        const QPixmap pm = pixmap(rect.size(), mode, state);
        painter->drawPixmap(rect.topLeft(), pm);
    }

private:
    QString svg_path_;
    QColor color_;
};

// ---------------------------------------------------------------------------
// 缓存：避免Theme切换时重复 new QSvgRenderer + parse SVG
// ---------------------------------------------------------------------------
//
// 设计要点：
//   - icons_:    key = "svg_path|HexArgb"，value = QIcon
//     公开接口 tinted_icon 返回的 QIcon 会被业务方缓存（如 btn->setIcon），
//     在Theme未变时多次调用应返回同一 QIcon 实例。
//   - pixmaps_:  key = "svg_path|HexArgb|WxH"，value = QPixmap
//     pixmap 与 size Strong相关，size 进入 key。
//   - Theme切换检测：用 last_dark 记录上次渲染时的Theme，每次 tinted_* 入口
//     检查 Theme::is_dark() != last_dark，是则清空两个缓存。这是无 connect 的
//     轻量方案，避免 IconKit 自由函数依赖某个 QObject 实例。
//   - TintedIconEngine::pixmap 不走缓存（Qt 框架按需调用，难以拦截），但调用
//     频率不高，且 tinted_icon 公开接口层已缓存，整体收益足够。
//   - 缓存 key 用字符串拼接，避免为 QPair/std::tuple 注册 qHash 函数。
//
struct IconKitCache {
    QHash<QString, QIcon> icons;      ///< key: "svg_path|HexArgb"
    QHash<QString, QPixmap> pixmaps;  ///< key: "svg_path|HexArgb|WxH"
    /// 上次缓存写入时的Theme（dark/light）。任一 tinted_* 入口检测变化时清空缓存。
    /// 初始化为与 Theme::is_dark() 相反的值，确保首次调用走清空分支完成初始化。
    bool last_dark = !Theme::is_dark();
};

/// 单例缓存（函数局部 static，线程Security初始化，UI 单线程使用）。
IconKitCache& icon_cache_state() {
    static IconKitCache state;
    return state;
}

/// Theme变化时清空两个缓存。每次 tinted_* 入口调用，开销仅一次 bool 比较。
void maybe_invalidate_cache() {
    const bool dark = Theme::is_dark();
    if (dark != icon_cache_state().last_dark) {
        icon_cache_state().icons.clear();
        icon_cache_state().pixmaps.clear();
        icon_cache_state().last_dark = dark;
    }
}

/// 拼接 icon cache key：svg_path + "|" + HexArgb 颜色。
QString icon_cache_key(const QString& svg_path, const QColor& color) {
    return svg_path + QLatin1Char('|') + color.name(QColor::HexArgb);
}

/// 拼接 pixmap cache key：svg_path + "|" + HexArgb + "|" + WxH。
/// 注意：用物理尺寸（已含 dpr）作 key，dpr 变化时 key 不同，自动渲染新 pixmap。
QString pixmap_cache_key(const QString& svg_path, const QColor& color, const QSize& size) {
    return svg_path + QLatin1Char('|') + color.name(QColor::HexArgb)
         + QLatin1Char('|') + QString::number(size.width())
         + QLatin1Char('x') + QString::number(size.height());
}

}  // namespace

QPixmap tinted_pixmap(const QString& svg_path, const QColor& color, const QSize& size) {
    maybe_invalidate_cache();
    // QLabel::setPixmap 场景：size 是逻辑尺寸，按 qApp dpr 渲染高清并Settings dpr，
    // 让 QLabel 在高 DPI 屏幕上清晰显示。
    const qreal dpr = qMax(qreal(1.0), qApp ? qApp->devicePixelRatio() : qreal(1.0));
    const QSize phys(qRound(size.width() * dpr), qRound(size.height() * dpr));
    // 缓存命Medium直接返回，避免重复 new QSvgRenderer + parse SVG
    const QString key = pixmap_cache_key(svg_path, color, phys);
    const auto it = icon_cache_state().pixmaps.constFind(key);
    if (it != icon_cache_state().pixmaps.constEnd()) {
        return it.value();
    }
    QPixmap pm = render_tinted(svg_path, color, phys);
    pm.setDevicePixelRatio(dpr);
    // 仅缓存有效 pixmap；null 不缓存以便下次重试（便于开发期发现 SVG 路径错误）
    if (!pm.isNull()) {
        icon_cache_state().pixmaps.insert(key, pm);
    }
    return pm;
}

QPixmap tinted_pixmap(const QString& svg_path, IconRole role, const QSize& size) {
    return tinted_pixmap(svg_path, icon_color(role), size);
}

QIcon tinted_icon(const QString& svg_path, const QColor& color) {
    maybe_invalidate_cache();
    // 缓存命Medium直接返回，避免每次都 new TintedIconEngine
    const QString key = icon_cache_key(svg_path, color);
    const auto it = icon_cache_state().icons.constFind(key);
    if (it != icon_cache_state().icons.constEnd()) {
        return it.value();
    }
    QIcon icon(new TintedIconEngine(svg_path, color));
    icon_cache_state().icons.insert(key, icon);
    return icon;
}

QIcon tinted_icon(const QString& svg_path, IconRole role) {
    return tinted_icon(svg_path, icon_color(role));
}

void copy_secure_to_clipboard(const QString& text) {
    QApplication::clipboard()->setText(text);
    // 30 秒后自动清空剪贴板，避免Password明文长期留存被其他程序读取。
    // 使用静态 QTimer 保证全生命周止单例，多次Copy会重置计时器，
    // 不会提前清空后续Copy的内容。
    static QTimer clear_timer;
    clear_timer.setSingleShot(true);
    clear_timer.disconnect();
    QObject::connect(&clear_timer, &QTimer::timeout, []() {
        QApplication::clipboard()->clear();
    });
    clear_timer.start(30000);
}

}  // namespace yuli::vault::ui
