#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace cache_debug_telemetry {

enum class cache_namespace {
    main,
    settings,
};

enum class resource_kind {
    single_svg,
    card_sheet_faces,
};

enum class consumer_scope {
    unknown,
    table_slots,
    settings_theme_carousel,
    settings_strategy_preview,
    image_cacher,
};

struct delta_counters {
    int entries_added = 0;
    int entries_removed = 0;
    qint64 bytes_added = 0;
    qint64 bytes_removed = 0;
    int images_added = 0;
    int images_removed = 0;
};

struct snapshot {
    enum class timing_stage {
        raster_lifecycle,
        coalesced_wait,
    };

    struct debug_size_bucket {
        int target_bucket_px = 0;
        int entry_count = 0;
        qint64 total_bytes = 0;
    };

    struct debug_largest_entry {
        cache_namespace name_space = cache_namespace::main;
        resource_kind kind = resource_kind::single_svg;
        QString source_id;
        QString render_scope;
        int target_bucket_px = 0;
        qint64 estimated_bytes = 0;
    };

    struct debug_requested_entry {
        cache_namespace name_space = cache_namespace::main;
        resource_kind kind = resource_kind::single_svg;
        QString source_id;
        QString render_scope;
        int target_bucket_px = 0;
        int request_count = 0;
    };

    struct debug_expensive_task {
        timing_stage stage = timing_stage::raster_lifecycle;
        cache_namespace name_space = cache_namespace::main;
        resource_kind kind = resource_kind::single_svg;
        QString source_id;
        QString render_scope;
        int target_bucket_px = 0;
        int completed_samples = 0;
        qint64 avg_elapsed_ms = 0;
        qint64 max_elapsed_ms = 0;
    };

    struct debug_subsystem_summary {
        cache_namespace name_space = cache_namespace::main;
        resource_kind kind = resource_kind::single_svg;
        int ready_entries = 0;
        qint64 ready_bytes = 0;
        int request_samples = 0;
        int timing_samples = 0;
        qint64 timing_max_elapsed_ms = 0;
    };

    struct debug_consumer_summary {
        consumer_scope consumer = consumer_scope::unknown;
        int displayed_recent_entries = 0;
        int displayed_recent_images = 0;
        qint64 displayed_recent_ready_bytes = 0;
        qint64 displayed_recent_widget_local_bytes_estimated = 0;
    };

    qint64 snapshot_sequence = 0;
    qint64 timestamp_ms = 0;
    int ready_entries = 0;
    qint64 ready_bytes = 0;
    int ready_images = 0;
    int in_flight_families = 0;
    int pending_families = 0;
    int displayed_ready_entries = 0;
    int cached_only_ready_entries = 0;
    int displayed_ready_images = 0;
    int cached_only_ready_images = 0;
    qint64 widget_local_rasterized_bytes_estimated = 0;
    qint64 widget_local_scaled_bytes_estimated = 0;
    qint64 widget_local_display_bytes_estimated = 0;
    int fallback_active_theme_keys_ready = 0;
    int fallback_default_theme_keys_ready = 0;
    int fallback_placeholder_keys_ready = 0;
    qint64 displayed_entry_window_ms = 0;
    int displayed_entry_coverage_percent = 0;
    int high_water_ready_entries = 0;
    qint64 high_water_ready_bytes = 0;
    int high_water_ready_images = 0;
    delta_counters lifetime_deltas;
    delta_counters interval_deltas;
    int raster_timing_samples = 0;
    qint64 raster_timing_avg_ms = 0;
    qint64 raster_timing_max_ms = 0;
    int coalesced_wait_samples = 0;
    qint64 coalesced_wait_avg_ms = 0;
    qint64 coalesced_wait_max_ms = 0;
    int deadline_readiness_samples = 0;
    int deadline_ready_early = 0;
    int deadline_ready_on_time = 0;
    int deadline_ready_late = 0;
    int unique_size_buckets = 0;
    QVector<debug_size_bucket> size_buckets;
    QVector<debug_largest_entry> largest_entries;
    QVector<debug_requested_entry> top_requested_entries;
    QVector<debug_expensive_task> top_expensive_tasks;
    QVector<debug_subsystem_summary> subsystem_summaries;
    QVector<debug_consumer_summary> consumer_summaries;
};

} // namespace cache_debug_telemetry
