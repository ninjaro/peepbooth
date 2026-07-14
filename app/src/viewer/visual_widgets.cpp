#include "viewer/visual_widgets.hpp"

#include <QAccessible>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <limits>

namespace visual_widgets_support {

constexpr int chart_y_axis_width = 56;
constexpr int chart_right_padding = 18;
constexpr int chart_title_height = 20;
constexpr int chart_legend_swatch_size = 10;
constexpr int chart_tooltip_radius_px = 8;

bool is_finite(double value) { return std::isfinite(value); }

double safe_span(double min_value, double max_value) {
    const double span = max_value - min_value;
    return span > 1e-9 ? span : 1.0;
}

void draw_axis_and_footer(
    QPainter* painter, const QWidget* widget, const QRect& plot,
    const QString& x_axis_text, const QStringList& footer_text_lines,
    int x_axis_label_height
) {
    if (painter == nullptr || widget == nullptr) {
        return;
    }

    const QString axis_label = x_axis_text.isEmpty()
        ? QStringLiteral("sample index (old -> new)")
        : x_axis_text;
    painter->setPen(widget->palette().color(QPalette::Text));
    painter->drawText(
        QRect(
            plot.left(), plot.bottom() + 4, plot.width(), x_axis_label_height
        ),
        Qt::AlignHCenter | Qt::AlignVCenter, axis_label
    );

    int footer_y = plot.bottom() + x_axis_label_height + 6;
    for (const QString& footer_line : footer_text_lines) {
        painter->setPen(widget->palette().color(QPalette::Text));
        painter->drawText(
            QRect(12, footer_y, widget->width() - 24, 14),
            Qt::AlignLeft | Qt::AlignVCenter, footer_line
        );
        footer_y += 14;
    }
}

void draw_focus_indicator(QPainter* painter, const QWidget* widget) {
    if (painter == nullptr || widget == nullptr || !widget->hasFocus()) {
        return;
    }
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(widget->palette().color(QPalette::Highlight), 2));
    painter->drawRect(widget->rect().adjusted(1, 1, -2, -2));
}

struct overlay_geometry {
    QString label;
    QColor color;
    QSize source_size;
    QRect projected_rect;
};

overlay_geometry build_overlay(
    const QString& label, const QColor& color, const QSize& source_size,
    const QSize& max_source_size, const QSize& available_plot_size,
    const QPoint& origin
) {
    overlay_geometry overlay {
        .label = label,
        .color = color,
        .source_size = source_size,
        .projected_rect = QRect(),
    };
    const QSize projected_size
        = monitor_visual_geometry::project_size_preserving_aspect(
            source_size, max_source_size, available_plot_size
        );
    if (projected_size.width() <= 0 || projected_size.height() <= 0) {
        return overlay;
    }

    overlay.projected_rect = QRect(
        origin.x() + 1, origin.y() - projected_size.height(),
        projected_size.width(), projected_size.height()
    );
    return overlay;
}

} // namespace visual_widgets_support

using visual_widgets_support::build_overlay;
using visual_widgets_support::chart_legend_swatch_size;
using visual_widgets_support::chart_right_padding;
using visual_widgets_support::chart_title_height;
using visual_widgets_support::chart_tooltip_radius_px;
using visual_widgets_support::chart_y_axis_width;
using visual_widgets_support::draw_axis_and_footer;
using visual_widgets_support::draw_focus_indicator;
using visual_widgets_support::is_finite;
using visual_widgets_support::overlay_geometry;
using visual_widgets_support::safe_span;

namespace monitor_visual_geometry {

bool projected_spread_region::is_valid() const {
    return !outer_rect.isEmpty() && !inner_rect.isEmpty()
        && outer_rect.contains(inner_rect);
}

QSize project_size_preserving_aspect(
    const QSize& source_size, const QSize& max_source_size,
    const QSize& available_plot_size
) {
    if (source_size.width() <= 0 || source_size.height() <= 0
        || max_source_size.width() <= 0 || max_source_size.height() <= 0
        || available_plot_size.width() <= 0
        || available_plot_size.height() <= 0) {
        return {};
    }

    const double scale_x = static_cast<double>(available_plot_size.width())
        / static_cast<double>(max_source_size.width());
    const double scale_y = static_cast<double>(available_plot_size.height())
        / static_cast<double>(max_source_size.height());
    const double scale = std::min(scale_x, scale_y);
    if (!std::isfinite(scale) || scale <= 0.0) {
        return {};
    }

    const int draw_width = std::max(
        1,
        static_cast<int>(
            std::lround(static_cast<double>(source_size.width()) * scale)
        )
    );
    const int draw_height = std::max(
        1,
        static_cast<int>(
            std::lround(static_cast<double>(source_size.height()) * scale)
        )
    );
    return { draw_width, draw_height };
}

projected_spread_region resolve_projected_spread_region(
    const QRect& first_rect, const QRect& second_rect
) {
    if (first_rect.isEmpty() || second_rect.isEmpty()) {
        return {};
    }

    const qint64 first_area = qint64(first_rect.width()) * first_rect.height();
    const qint64 second_area
        = qint64(second_rect.width()) * second_rect.height();
    const QRect outer = first_area >= second_area ? first_rect : second_rect;
    const QRect inner = first_area >= second_area ? second_rect : first_rect;
    if (!outer.contains(inner)) {
        return {};
    }
    return projected_spread_region { outer, inner };
}

} // namespace monitor_visual_geometry

monitor_line_chart_widget::monitor_line_chart_widget(QWidget* parent)
    : QWidget(parent)
    , title_text()
    , unit_text()
    , x_axis_text(QStringLiteral("sample index (old -> new)"))
    , chart_series()
    , footer_text_lines()
    , tooltip_points()
    , active_tooltip_text()
    , accessible_summary_text()
    , accessible_point_texts()
    , keyboard_point_index(-1) {
    setAutoFillBackground(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void monitor_line_chart_widget::set_title(const QString& title) {
    title_text = title;
    rebuild_accessible_data();
    update();
}

void monitor_line_chart_widget::set_unit_label(const QString& unit_label) {
    unit_text = unit_label;
    rebuild_accessible_data();
    update();
}

void monitor_line_chart_widget::set_x_axis_label(const QString& x_axis_label) {
    x_axis_text = x_axis_label;
    update();
}

void monitor_line_chart_widget::set_series(const QVector<series>& series_list) {
    chart_series = series_list;
    keyboard_point_index = -1;
    rebuild_accessible_data();
    update();
}

void monitor_line_chart_widget::set_footer_lines(
    const QStringList& footer_lines
) {
    footer_text_lines = footer_lines;
    update();
}

QSize monitor_line_chart_widget::minimumSizeHint() const {
    return { 360, 180 };
}

void monitor_line_chart_widget::rebuild_accessible_data() {
    QStringList summaries;
    accessible_point_texts.clear();
    for (const series& line : chart_series) {
        double minimum = std::numeric_limits<double>::infinity();
        double maximum = -std::numeric_limits<double>::infinity();
        double latest = 0.0;
        int finite_count = 0;
        for (int index = 0; index < line.values.size(); ++index) {
            const double value = line.values.at(index);
            if (!is_finite(value)) {
                continue;
            }
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            latest = value;
            ++finite_count;
            const QString value_text = unit_text.isEmpty()
                ? QString::number(value, 'g', 10)
                : QStringLiteral("%1 %2").arg(
                      QString::number(value, 'g', 10), unit_text
                  );
            accessible_point_texts.push_back(
                QStringLiteral("%1, sample %2 of %3, value %4")
                    .arg(line.label)
                    .arg(index + 1)
                    .arg(line.values.size())
                    .arg(value_text)
            );
        }
        if (finite_count > 0) {
            const QString suffix = unit_text.isEmpty()
                ? QString()
                : QLatin1Char(' ') + unit_text;
            summaries.push_back(
                QStringLiteral(
                    "%1: %2 samples, latest %3%4, minimum %5%4, maximum %6%4"
                )
                    .arg(line.label)
                    .arg(finite_count)
                    .arg(QString::number(latest, 'g', 10))
                    .arg(suffix)
                    .arg(QString::number(minimum, 'g', 10))
                    .arg(QString::number(maximum, 'g', 10))
            );
        }
    }
    accessible_summary_text = title_text;
    if (!summaries.isEmpty()) {
        accessible_summary_text
            += QStringLiteral(". ") + summaries.join(QStringLiteral(". "));
    } else {
        accessible_summary_text += QStringLiteral(". No chart data.");
    }
    if (!accessible_point_texts.isEmpty()) {
        accessible_summary_text += QStringLiteral(
            " Use Left and Right arrow keys to inspect individual points."
        );
    }
    update_accessible_description();
}

void monitor_line_chart_widget::update_accessible_description() {
    QString description = accessible_summary_text;
    if (keyboard_point_index >= 0
        && keyboard_point_index < accessible_point_texts.size()) {
        description += QStringLiteral(" Selected point: ")
            + accessible_point_texts.at(keyboard_point_index)
            + QLatin1Char('.');
    }
    setAccessibleDescription(description);
    QAccessibleEvent accessibility_event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&accessibility_event);
}

void monitor_line_chart_widget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    draw_focus_indicator(&painter, this);

    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(
        QRect(12, 8, width() - 24, chart_title_height),
        Qt::AlignLeft | Qt::AlignVCenter, title_text
    );

    struct legend_entry_layout {
        QColor color;
        QString label;
        QRect swatch_rect;
        QRect text_rect;
    };

    const QFontMetrics metrics = painter.fontMetrics();
    const int legend_line_height = std::max(14, metrics.height());
    int legend_x = 12;
    int legend_y = 8 + chart_title_height + 4;
    QVector<legend_entry_layout> legend_entries;
    legend_entries.reserve(chart_series.size());

    for (const series& line : chart_series) {
        if (line.values.isEmpty()) {
            continue;
        }

        const int available_text_width = std::max(80, width() - 120);
        const QString legend_text = metrics.elidedText(
            line.label, Qt::ElideRight, available_text_width
        );
        const int text_width
            = std::max(20, metrics.horizontalAdvance(legend_text));
        const int item_width = chart_legend_swatch_size + 4 + text_width + 12;
        if (legend_x + item_width > width() - 12 && legend_x > 12) {
            legend_x = 12;
            legend_y += legend_line_height + 4;
        }

        const QRect swatch_rect(
            legend_x,
            legend_y + ((legend_line_height - chart_legend_swatch_size) / 2),
            chart_legend_swatch_size, chart_legend_swatch_size
        );
        const QRect text_rect(
            swatch_rect.right() + 4, legend_y, text_width, legend_line_height
        );
        legend_entries.push_back(
            legend_entry_layout { line.color, legend_text, swatch_rect,
                                  text_rect }
        );
        legend_x += item_width;
    }

    for (const legend_entry_layout& entry : legend_entries) {
        painter.fillRect(entry.swatch_rect, entry.color);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(
            entry.text_rect, Qt::AlignLeft | Qt::AlignVCenter, entry.label
        );
    }

    const int legend_bottom
        = legend_entries.isEmpty() ? legend_y : legend_y + legend_line_height;
    const int footer_height = static_cast<int>(footer_text_lines.size()) * 14;
    const int x_axis_label_height = 16;
    const int bottom_padding = footer_height + x_axis_label_height + 10;
    const QRect plot(
        chart_y_axis_width, legend_bottom + 8,
        width() - chart_y_axis_width - chart_right_padding,
        height() - (legend_bottom + 8) - bottom_padding
    );

    tooltip_points.clear();
    active_tooltip_text.clear();

    if (plot.width() <= 8 || plot.height() <= 8) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(
            rect().adjusted(8, 8, -8, -8), Qt::AlignCenter,
            QStringLiteral("Chart area is too small")
        );
        return;
    }

    painter.setPen(QPen(palette().color(QPalette::Mid), 1));
    painter.drawRect(plot);

    double min_value = std::numeric_limits<double>::infinity();
    double max_value = -std::numeric_limits<double>::infinity();
    int max_count = 0;
    int visible_series_count = 0;
    for (const series& line : chart_series) {
        if (line.values.isEmpty()) {
            continue;
        }
        ++visible_series_count;
        max_count = std::max(max_count, static_cast<int>(line.values.size()));
        for (double value : line.values) {
            if (!is_finite(value)) {
                continue;
            }
            min_value = std::min(min_value, value);
            max_value = std::max(max_value, value);
        }
    }

    if (visible_series_count <= 0 || !is_finite(min_value)
        || !is_finite(max_value) || max_count <= 0) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(
            plot.adjusted(8, 8, -8, -8), Qt::AlignCenter,
            QStringLiteral("No chart data")
        );
        draw_axis_and_footer(
            &painter, this, plot, x_axis_text, footer_text_lines,
            x_axis_label_height
        );
        return;
    }

    const double value_span = safe_span(min_value, max_value);
    const int grid_lines = 4;
    for (int i = 0; i <= grid_lines; ++i) {
        const double t
            = static_cast<double>(i) / static_cast<double>(grid_lines);
        const int y = plot.bottom()
            - static_cast<int>(
                          std::lround(t * static_cast<double>(plot.height()))
            );
        painter.setPen(QPen(palette().color(QPalette::Midlight), 1));
        painter.drawLine(plot.left(), y, plot.right(), y);

        const double axis_value = min_value + (value_span * t);
        painter.setPen(palette().color(QPalette::Text));
        const QString axis_text_value = unit_text.isEmpty()
            ? QString::number(axis_value, 'f', 1)
            : QStringLiteral("%1 %2").arg(
                  QString::number(axis_value, 'f', 1), unit_text
              );
        painter.drawText(
            QRect(4, y - 10, chart_y_axis_width - 8, 20),
            Qt::AlignRight | Qt::AlignVCenter, axis_text_value
        );
    }

    tooltip_points.reserve(max_count * visible_series_count);
    for (const series& line : chart_series) {
        if (line.values.isEmpty()) {
            continue;
        }

        QPainterPath path;
        bool has_segment = false;
        for (int index = 0; index < line.values.size(); ++index) {
            const double value = line.values.at(index);
            if (!is_finite(value)) {
                has_segment = false;
                continue;
            }

            const double x_t = line.values.size() <= 1
                ? 0.0
                : static_cast<double>(index)
                    / static_cast<double>(line.values.size() - 1);
            const double y_t = (value - min_value) / value_span;
            const int x
                = plot.left()
                + static_cast<int>(
                      std::lround(x_t * static_cast<double>(plot.width()))
                );
            const int y
                = plot.bottom()
                - static_cast<int>(
                      std::lround(y_t * static_cast<double>(plot.height()))
                );

            if (!has_segment) {
                path.moveTo(static_cast<qreal>(x), static_cast<qreal>(y));
                has_segment = true;
            } else {
                path.lineTo(static_cast<qreal>(x), static_cast<qreal>(y));
            }

            const QString value_text = unit_text.isEmpty()
                ? QString::number(value, 'f', 2)
                : QStringLiteral("%1 %2").arg(
                      QString::number(value, 'f', 2), unit_text
                  );
            const QString tooltip_text
                = QStringLiteral("%1\nsample index: %2 of %3\nvalue: %4")
                      .arg(line.label)
                      .arg(index + 1)
                      .arg(line.values.size())
                      .arg(value_text);
            tooltip_points.push_back(
                tooltip_point { QPoint(x, y), tooltip_text }
            );
        }

        painter.setPen(QPen(line.color, 2));
        painter.drawPath(path);
    }

    draw_axis_and_footer(
        &painter, this, plot, x_axis_text, footer_text_lines,
        x_axis_label_height
    );
}

void monitor_line_chart_widget::mouseMoveEvent(QMouseEvent* event) {
    if (event == nullptr || tooltip_points.isEmpty()) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint cursor = event->position().toPoint();
    const int radius_squared
        = chart_tooltip_radius_px * chart_tooltip_radius_px;
    const tooltip_point* nearest = nullptr;
    int nearest_distance_squared = radius_squared + 1;
    for (const tooltip_point& point : tooltip_points) {
        const int dx = cursor.x() - point.pixel_pos.x();
        const int dy = cursor.y() - point.pixel_pos.y();
        const int distance_squared = (dx * dx) + (dy * dy);
        if (distance_squared <= radius_squared
            && distance_squared < nearest_distance_squared) {
            nearest = &point;
            nearest_distance_squared = distance_squared;
        }
    }

    if (nearest != nullptr) {
        if (active_tooltip_text != nearest->text) {
            active_tooltip_text = nearest->text;
            QToolTip::showText(
                event->globalPosition().toPoint(), active_tooltip_text, this
            );
        }
    } else if (!active_tooltip_text.isEmpty()) {
        active_tooltip_text.clear();
        QToolTip::hideText();
    }

    QWidget::mouseMoveEvent(event);
}

void monitor_line_chart_widget::leaveEvent(QEvent* event) {
    active_tooltip_text.clear();
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

void monitor_line_chart_widget::keyPressEvent(QKeyEvent* event) {
    if (event == nullptr || accessible_point_texts.isEmpty()) {
        QWidget::keyPressEvent(event);
        return;
    }

    const int point_count = static_cast<int>(accessible_point_texts.size());
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Up:
        keyboard_point_index = keyboard_point_index <= 0
            ? point_count - 1
            : keyboard_point_index - 1;
        break;
    case Qt::Key_Right:
    case Qt::Key_Down:
        keyboard_point_index = (keyboard_point_index + 1) % point_count;
        break;
    case Qt::Key_Home:
        keyboard_point_index = 0;
        break;
    case Qt::Key_End:
        keyboard_point_index = point_count - 1;
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    active_tooltip_text = accessible_point_texts.at(keyboard_point_index);
    QToolTip::showText(mapToGlobal(rect().center()), active_tooltip_text, this);
    update_accessible_description();
    event->accept();
}

void monitor_line_chart_widget::focusOutEvent(QFocusEvent* event) {
    QToolTip::hideText();
    QWidget::focusOutEvent(event);
}

monitor_pie_chart_widget::monitor_pie_chart_widget(QWidget* parent)
    : QWidget(parent)
    , title_text()
    , chart_slices()
    , footer() {
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
}

void monitor_pie_chart_widget::set_title(const QString& title) {
    title_text = title;
    update();
}

void monitor_pie_chart_widget::set_slices(const QVector<slice>& slice_list) {
    chart_slices = slice_list;
    update();
}

void monitor_pie_chart_widget::set_footer_text(const QString& footer_text) {
    footer = footer_text;
    update();
}

QSize monitor_pie_chart_widget::minimumSizeHint() const { return { 360, 190 }; }

void monitor_pie_chart_widget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    draw_focus_indicator(&painter, this);

    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(
        QRect(12, 8, width() - 24, 20), Qt::AlignLeft | Qt::AlignVCenter,
        title_text
    );

    double total = 0.0;
    for (const slice& part : chart_slices) {
        if (part.value > 0.0 && is_finite(part.value)) {
            total += part.value;
        }
    }

    if (total <= 0.0) {
        painter.drawText(
            QRect(12, 32, width() - 24, height() - 64), Qt::AlignCenter,
            QStringLiteral("No composition data")
        );
        return;
    }

    const int pie_diameter = std::max(80, std::min(width() / 2, height() - 64));
    const QRect pie_rect(16, 30, pie_diameter, pie_diameter);

    int start_angle = 90 * 16;
    for (const slice& part : chart_slices) {
        if (part.value <= 0.0 || !is_finite(part.value)) {
            continue;
        }
        const double fraction = part.value / total;
        const int span_angle
            = static_cast<int>(std::lround(fraction * 360.0 * 16.0));
        painter.setBrush(part.color);
        painter.setPen(QPen(palette().color(QPalette::Base), 1));
        painter.drawPie(pie_rect, start_angle, -span_angle);
        start_angle -= span_angle;
    }

    int legend_y = 36;
    for (const slice& part : chart_slices) {
        if (part.value <= 0.0 || !is_finite(part.value)) {
            continue;
        }

        painter.fillRect(QRect(width() / 2 + 12, legend_y, 10, 10), part.color);
        painter.setPen(palette().color(QPalette::Text));
        const QString legend_text
            = QStringLiteral("%1: %2")
                  .arg(part.label)
                  .arg(QString::number(part.value, 'f', 2));
        painter.drawText(
            QRect(width() / 2 + 28, legend_y - 3, width() / 2 - 40, 16),
            Qt::AlignLeft | Qt::AlignVCenter, legend_text
        );
        legend_y += 16;
    }

    if (!footer.isEmpty()) {
        painter.drawText(
            QRect(12, height() - 20, width() - 24, 16),
            Qt::AlignLeft | Qt::AlignVCenter, footer
        );
    }
}

monitor_geometry_schematic_widget::monitor_geometry_schematic_widget(
    QWidget* parent
)
    : QWidget(parent)
    , has_snapshot(false)
    , current_snapshot() {
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(QStringLiteral(
        "Overlay legend: window, layout, display-card target, cache raster, "
        "preloaded raster.\nShaded area indicates preload spread."
    ));
}

void monitor_geometry_schematic_widget::set_snapshot(
    const geometry_debug_snapshot& snapshot
) {
    has_snapshot = true;
    current_snapshot = snapshot;
    update();
}

void monitor_geometry_schematic_widget::clear_snapshot() {
    has_snapshot = false;
    current_snapshot = geometry_debug_snapshot();
    update();
}

QSize monitor_geometry_schematic_widget::minimumSizeHint() const {
    return { 360, 220 };
}

void monitor_geometry_schematic_widget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    draw_focus_indicator(&painter, this);

    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(
        QRect(12, 8, width() - 24, 20), Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("Geometry schematic (lower-left anchored)")
    );

    if (!has_snapshot) {
        painter.drawText(
            QRect(12, 32, width() - 24, height() - 44), Qt::AlignCenter,
            QStringLiteral("No geometry snapshot")
        );
        return;
    }

    const int overlay_count = 5;
    const int legend_columns = width() >= 520 ? 2 : 1;
    const int legend_line_height = std::max(14, painter.fontMetrics().height());
    const int legend_rows
        = (overlay_count + legend_columns - 1) / legend_columns;
    const int legend_area_height
        = (legend_rows * (legend_line_height + 4)) + 12;
    const QRect plot(
        44, 36, width() - 76, height() - 36 - legend_area_height - 20
    );
    if (plot.width() <= 8 || plot.height() <= 8) {
        painter.drawText(
            QRect(12, 32, width() - 24, height() - 44), Qt::AlignCenter,
            QStringLiteral("Geometry view is too small")
        );
        return;
    }

    const QPoint origin(plot.left(), plot.bottom());
    painter.setPen(QPen(palette().color(QPalette::Text), 1));
    painter.drawLine(origin, QPoint(plot.right(), origin.y()));
    painter.drawLine(origin, QPoint(origin.x(), plot.top()));

    const int max_width = std::max(
        1,
        std::max(
            {
                current_snapshot.window_size.width(),
                current_snapshot.layout_size.width(),
                current_snapshot.display_card_size.width(),
                current_snapshot.cache_raster_size.width(),
                current_snapshot.preloaded_raster_size.width(),
            }
        )
    );
    const int max_height = std::max(
        1,
        std::max(
            {
                current_snapshot.window_size.height(),
                current_snapshot.layout_size.height(),
                current_snapshot.display_card_size.height(),
                current_snapshot.cache_raster_size.height(),
                current_snapshot.preloaded_raster_size.height(),
            }
        )
    );

    const QSize max_source_size(max_width, max_height);
    const QSize available_plot_size(
        std::max(1, plot.width() - 6), std::max(1, plot.height() - 6)
    );

    QVector<overlay_geometry> overlays;
    overlays.reserve(5);
    overlays.push_back(build_overlay(
        QStringLiteral("Window bounds"), QColor(0, 114, 178),
        current_snapshot.window_size, max_source_size, available_plot_size,
        origin
    ));
    overlays.push_back(build_overlay(
        QStringLiteral("Layout bounds"), QColor(0, 158, 115),
        current_snapshot.layout_size, max_source_size, available_plot_size,
        origin
    ));
    overlays.push_back(build_overlay(
        QStringLiteral("Display-card target"), QColor(230, 159, 0),
        current_snapshot.display_card_size, max_source_size,
        available_plot_size, origin
    ));
    overlays.push_back(build_overlay(
        QStringLiteral("Cache raster"), QColor(213, 94, 0),
        current_snapshot.cache_raster_size, max_source_size,
        available_plot_size, origin
    ));
    overlays.push_back(build_overlay(
        QStringLiteral("Preloaded raster"), QColor(204, 121, 167),
        current_snapshot.preloaded_raster_size, max_source_size,
        available_plot_size, origin
    ));

    if (overlays.size() >= 5) {
        const QRect cache_rect = overlays.at(3).projected_rect;
        const QRect preload_rect = overlays.at(4).projected_rect;
        const monitor_visual_geometry::projected_spread_region spread_region
            = monitor_visual_geometry::resolve_projected_spread_region(
                cache_rect, preload_rect
            );
        if (spread_region.is_valid()) {
            QPainterPath outer_path;
            outer_path.addRect(spread_region.outer_rect);
            QPainterPath inner_path;
            inner_path.addRect(spread_region.inner_rect);
            const QPainterPath spread = outer_path.subtracted(inner_path);
            painter.fillPath(spread, QColor(204, 121, 167, 72));
        }
    }

    for (const overlay_geometry& overlay : overlays) {
        if (overlay.projected_rect.isEmpty()) {
            continue;
        }
        painter.setPen(QPen(overlay.color, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(overlay.projected_rect);
    }

    const QFontMetrics legend_metrics = painter.fontMetrics();
    const int legend_top = plot.bottom() + 8;
    const int legend_left = 12;
    const int legend_column_spacing = 16;
    const int legend_available_width = std::max(80, width() - 24);
    const int legend_column_width = std::max(
        80,
        (legend_available_width
         - ((legend_columns - 1) * legend_column_spacing))
            / legend_columns
    );

    for (int index = 0; index < overlays.size(); ++index) {
        const overlay_geometry& overlay = overlays.at(index);
        const int column = index % legend_columns;
        const int row = index / legend_columns;
        const int legend_x = legend_left
            + (column * (legend_column_width + legend_column_spacing));
        const int legend_y = legend_top + (row * (legend_line_height + 4));

        painter.fillRect(QRect(legend_x, legend_y + 2, 10, 10), overlay.color);
        painter.setPen(palette().color(QPalette::Text));
        const QString legend_text_full = QStringLiteral("%1: %2x%3")
                                             .arg(overlay.label)
                                             .arg(overlay.source_size.width())
                                             .arg(overlay.source_size.height());
        const QString legend_text = legend_metrics.elidedText(
            legend_text_full, Qt::ElideRight, legend_column_width - 18
        );
        painter.drawText(
            QRect(
                legend_x + 16, legend_y, legend_column_width - 16,
                legend_line_height
            ),
            Qt::AlignLeft | Qt::AlignVCenter, legend_text
        );
    }
}

monitor_resize_history_widget::monitor_resize_history_widget(QWidget* parent)
    : QWidget(parent)
    , recent_entries() {
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(QStringLiteral(
        "X axis: resize events over time (oldest to newest).\n"
        "Purple bar: prewarm completion time in milliseconds."
    ));
}

void monitor_resize_history_widget::set_entries(
    const QVector<resize_entry>& entries
) {
    recent_entries = entries;
    update();
}

QSize monitor_resize_history_widget::minimumSizeHint() const {
    return { 360, 200 };
}

void monitor_resize_history_widget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    draw_focus_indicator(&painter, this);

    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(
        QRect(12, 8, width() - 24, 20), Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("Resize history timeline")
    );

    if (recent_entries.isEmpty()) {
        painter.drawText(
            QRect(12, 32, width() - 24, height() - 44), Qt::AlignCenter,
            QStringLiteral("No resize events")
        );
        return;
    }

    const int recent_entry_count = static_cast<int>(recent_entries.size());
    const int entry_start = std::max(0, recent_entry_count - 24);
    const QVector<resize_entry> entries
        = recent_entries.mid(entry_start, recent_entry_count - entry_start);
    const QRect plot(20, 42, width() - 40, height() - 72);
    const int base_y = plot.bottom() - 8;

    qint64 max_prewarm_ms = 0;
    for (const resize_entry& entry : entries) {
        max_prewarm_ms = std::max(
            max_prewarm_ms, std::max<qint64>(0, entry.prewarm_completion_ms)
        );
    }
    if (max_prewarm_ms <= 0) {
        max_prewarm_ms = 1;
    }

    painter.setPen(QPen(palette().color(QPalette::Mid), 1));
    painter.drawLine(plot.left(), base_y, plot.right(), base_y);

    for (int i = 0; i < entries.size(); ++i) {
        const resize_entry& entry = entries.at(i);
        const double x_ratio = entries.size() <= 1
            ? 0.0
            : static_cast<double>(i) / static_cast<double>(entries.size() - 1);
        const int x
            = plot.left()
            + static_cast<int>(
                  std::lround(x_ratio * static_cast<double>(plot.width()))
            );
        const bool bucket_changed
            = entry.old_active_bucket_px != entry.new_active_bucket_px;
        const QColor marker_color
            = bucket_changed ? QColor(230, 159, 0) : QColor(0, 114, 178);

        painter.setPen(QPen(marker_color, 2));
        painter.drawLine(x, base_y, x, plot.top());
        painter.setBrush(marker_color);
        painter.drawEllipse(QPoint(x, base_y), 3, 3);

        const int bar_height = static_cast<int>(std::lround(
            (static_cast<double>(
                 std::max<qint64>(0, entry.prewarm_completion_ms)
             )
             / static_cast<double>(max_prewarm_ms))
            * static_cast<double>(plot.height() - 14)
        ));
        painter.fillRect(
            QRect(x - 2, base_y - bar_height, 4, bar_height),
            QColor(204, 121, 167, 160)
        );

        if (entries.size() <= 10) {
            const QString label = QStringLiteral("%1x%2")
                                      .arg(entry.new_window_size.width())
                                      .arg(entry.new_window_size.height());
            painter.setPen(palette().color(QPalette::Text));
            painter.drawText(
                QRect(x - 32, base_y + 4, 64, 12), Qt::AlignHCenter, label
            );
        }
    }

    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(
        QRect(12, height() - 24, width() - 24, 16),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral(
            "X axis: resize event order | Vertical bar: prewarm completion ms"
        )
    );
}
