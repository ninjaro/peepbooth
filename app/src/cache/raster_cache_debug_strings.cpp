#include "cache/raster_cache_debug_strings.hpp"

QString
cache_namespace_to_string(cache_debug_telemetry::cache_namespace name_space) {
    switch (name_space) {
    case cache_debug_telemetry::cache_namespace::settings:
        return QStringLiteral("settings");
    case cache_debug_telemetry::cache_namespace::main:
    default:
        return QStringLiteral("main");
    }
}

QString resource_kind_to_string(cache_debug_telemetry::resource_kind kind) {
    switch (kind) {
    case cache_debug_telemetry::resource_kind::card_sheet_faces:
        return QStringLiteral("card_sheet_faces");
    case cache_debug_telemetry::resource_kind::single_svg:
    default:
        return QStringLiteral("single_svg");
    }
}

QString
debug_consumer_scope_to_string(cache_debug_telemetry::consumer_scope scope) {
    switch (scope) {
    case cache_debug_telemetry::consumer_scope::table_slots:
        return QStringLiteral("table_slots");
    case cache_debug_telemetry::consumer_scope::settings_theme_carousel:
        return QStringLiteral("settings_theme_carousel");
    case cache_debug_telemetry::consumer_scope::settings_strategy_preview:
        return QStringLiteral("settings_strategy_preview");
    case cache_debug_telemetry::consumer_scope::image_cacher:
        return QStringLiteral("image_cacher");
    case cache_debug_telemetry::consumer_scope::unknown:
    default:
        return QStringLiteral("unknown");
    }
}
