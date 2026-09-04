#include "vkexp/presets/PresetRegistry.hpp"
#include "vkexp/profiling/CpuProfiler.hpp"
#include "vkexp/profiling/ProfilerTypes.hpp"

#include <cmath>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool closeTo(const float left, const float right) { return std::abs(left - right) < 0.0001F; }

void testTimingSeries() {
    vkexp::TimingSeries series;
    series.add(1.0);
    series.add(2.0);
    series.add(3.0);
    series.add(4.0);

    const auto statistics = series.statistics();
    check(statistics.sampleCount == 4, "TimingSeries sample count");
    check(closeTo(statistics.currentMs, 4.0F), "TimingSeries current value");
    check(closeTo(statistics.averageMs, 2.5F), "TimingSeries average");
    check(closeTo(statistics.minimumMs, 1.0F), "TimingSeries minimum");
    check(closeTo(statistics.maximumMs, 4.0F), "TimingSeries maximum");
    check(closeTo(statistics.percentile95Ms, 4.0F), "TimingSeries p95");
}

void testCpuProfiler() {
    constexpr vkexp::ProfileMetricId frameMetric = 0;
    constexpr vkexp::ProfileMetricId cpuWorkMetric = 1;
    constexpr vkexp::ProfileMetricId customMetric = 2;
    vkexp::CpuProfiler profiler;
    profiler.beginFrame();
    profiler.addDuration(customMetric, 1.25);
    const vkexp::CpuProfiler::FrameSample sample = profiler.endFrame(frameMetric, cpuWorkMetric, 3);

    check(sample.wallMilliseconds >= 0.0, "CPU profiler wall time");
    check(sample.cpuMilliseconds >= 0.0, "CPU profiler process time");
    check(profiler.series(frameMetric).statistics().sampleCount == 1, "CPU frame sample");
    check(profiler.series(cpuWorkMetric).statistics().sampleCount == 1, "CPU work sample");
    check(closeTo(profiler.series(customMetric).statistics().currentMs, 1.25F),
          "CPU custom duration");
}

void testPresetRegistry() {
    vkexp::PresetRegistry registry;
    check(registry.all().size() == 3, "Built-in preset count");
    check(registry.require("mixed").graphicsEnabled, "Mixed preset graphics");
    check(registry.require("mixed").computeEnabled, "Mixed preset compute");

    registry.loadWindowPreset(VKEXP_TEST_WINDOW_PRESET);
    for (const auto& preset : registry.all()) {
        check(preset.windowWidth >= 320 && preset.windowWidth <= 7680, "Loaded preset width range");
        check(preset.windowHeight >= 240 && preset.windowHeight <= 4320,
              "Loaded preset height range");
    }

    bool rejectedUnknown = false;
    try {
        static_cast<void>(registry.require("does-not-exist"));
    } catch (const std::exception&) {
        rejectedUnknown = true;
    }
    check(rejectedUnknown, "Unknown preset rejection");
}

} // namespace

int main() {
    testTimingSeries();
    testCpuProfiler();
    testPresetRegistry();
    return failures == 0 ? 0 : 1;
}
