#pragma once
// AI GENERATED SVG plots of matcher latency samples. No dependencies beyond the standard
// library and matcher::Duration: each function writes a self-contained .svg.

#include <matcher/matcher.h>

#include <filesystem>
#include <span>

namespace plots
{

// Frequency per 10ns latency bucket on a log2 frequency axis, one gridline per
// doubling. Reads exact counts across the whole range, from the
// tens-of-thousands mode down to buckets holding a single sample.
void WriteLatencyHistogramLog(std::span<const matcher::Duration> durations,
                              const std::filesystem::path& out);

// The same buckets on a plain linear frequency axis. A count of one is far too
// small to have height there, so every occupied bucket is drawn at a floor
// height instead. That flattens the tail to a single level, and in exchange an
// empty bucket is unmistakably empty, so gaps in the tail are visible.
void WriteLatencyHistogramLinear(std::span<const matcher::Duration> durations,
                                 const std::filesystem::path& out);

// Mean and worst latency per order-id bin, for spotting drift over the run.
void WriteLatencyByOrderId(std::span<const matcher::Duration> durations,
                           const std::filesystem::path& out);

// Mean latency per fill count with min/max whiskers.
void WriteLatencyByFills(std::span<const matcher::Duration> durations,
                         const std::filesystem::path& out);

}   // namespace plots
