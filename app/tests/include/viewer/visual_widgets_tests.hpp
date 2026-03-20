#pragma once

#include <QObject>

class monitor_visual_widgets_tests : public QObject {
    Q_OBJECT

private slots:
    void project_size_preserving_aspect_preserves_ratio();
    void project_size_preserving_aspect_respects_available_bounds();
    void project_size_preserving_aspect_handles_invalid_sizes();
    void spread_region_prefers_outer_rect_and_inner_rect();
    void spread_region_requires_containment();
};
