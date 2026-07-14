#pragma once

#include "cache/cache_debug_snapshot.hpp"

#include <QString>

QString
cache_namespace_to_string(cache_debug_telemetry::cache_namespace name_space);
QString resource_kind_to_string(cache_debug_telemetry::resource_kind kind);
QString
debug_consumer_scope_to_string(cache_debug_telemetry::consumer_scope scope);
