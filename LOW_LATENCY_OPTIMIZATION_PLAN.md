# Low-Latency Rendering Optimization Plan

## Research Summary

### BBR Algorithm Principles (Google)
- **Bottleneck Bandwidth & RTT**: Measures network capacity and delay
- **Probe Phases**: PROBE_BW (bandwidth) and PROBE_RTT (delay)
- **Pacing Gains**: Uses cycle_gain values (1.25, 0.75, 1.0) to detect bottleneck
- **Adaptive**: Continuously adjusts sending rate based on measurements

### LatencyFleX BBR-Inspired Approach
- **Core Insight**: Game rendering queue ≈ Network buffer
  - Buffer → Queue
  - RTT → Latency
  - Bandwidth → Throughput/FPS
- **EWMA Formula**: `current = (1 - alpha) * current + alpha * value`
- **Negative Gain**: -1.5% to gradually reduce existing queue
- **Two-Phase Tracking**: Up/Down factors (1.10 up, 0.985 down)
- **Delay Compensation**: Predicts frame end time, compensates for errors
- **Goal**: Run CPU slightly below GPU bottleneck to create latency-optimal point

### NVIDIA Reflex
- Uses EWMA for frame time prediction
- Paces CPU to prevent running ahead of GPU
- Just-in-time submission strategy

## Critical Bugs Found

### 🔴 BUG 1: Frame Timing Never Measured
**Issue**: `update_frame_timing()` is never called anywhere!
- EWMA never updates from actual measurements
- `stats.predicted_frame_time_us` stays at initial value
- `target_frame_time_us` remains hardcoded at 16667μs (60Hz)
- Algorithm cannot adapt to actual frame rate

**Impact**: SEVERE - Makes adaptive pacing completely non-functional

**Fix**: Call `update_frame_timing()` after each frame with actual frame duration

---

### 🔴 BUG 2: Incorrect Sleep Logic
**Issue**: Current logic sleeps based on elapsed time since LAST frame start
```cpp
auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - frame_start_time);
if (elapsed.count() < target_frame_time_us) {
    std::this_thread::sleep_for(target_frame_time_us - elapsed.count());
}
```

**Problem**: This measures time between `begin_frame()` calls, not actual frame work time!
- Sleeps at START of frame, not end
- Doesn't account for actual GPU work duration
- Creates double-sleep: sleep + actual frame work

**Impact**: SEVERE - Causes unnecessary delays and performance loss

**Correct Approach**:
- Measure frame END time (after present)
- Sleep BEFORE next frame based on predicted frame time
- Use LatencyFleX approach: predict end time, compensate for errors

---

### 🟡 BUG 3: No Negative Gain Applied
**Issue**: Missing LatencyFleX's -1.5% negative gain
- Current: `target_frame_time_us = stats.predicted_frame_time_us`
- Should be: `target_frame_time_us = stats.predicted_frame_time_us * 0.985` (1.5% reduction)

**Impact**: MEDIUM - Prevents queue reduction, doesn't achieve latency-optimal point

---

### 🟡 BUG 4: No Frame Counter Increment
**Issue**: `frame_counter` is incremented but never used for CPU sleep method
- Only used for timeline semaphore (which is disabled)
- First frame check `if (frame_counter > 0)` works, but counter tracking is incomplete

**Impact**: LOW - Minor, but shows incomplete implementation

---

### 🟡 BUG 5: EWMA Alpha Too High
**Issue**: `EWMA_ALPHA = 0.25f` is very responsive
- LatencyFleX uses slower smoothing for stability
- High alpha makes it jittery and unstable
- Should be lower (0.05 - 0.15) for smoother adaptation

**Impact**: MEDIUM - Causes unstable frame pacing

---

## Optimization Plan

### Phase 1: Fix Critical Bugs (Immediate)

#### 1.1 Measure Actual Frame Time
**Goal**: Track actual frame duration from start to present completion

**Implementation**:
```cpp
// In begin_frame():
frame_start_time = std::chrono::high_resolution_clock::now();

// After vkQueuePresentKHR in command_queue_execute_and_present():
if (low_latency_manager.is_enabled()) {
    auto frame_end_time = std::chrono::high_resolution_clock::now();
    auto frame_duration = std::chrono::duration_cast<std::chrono::microseconds>(
        frame_end_time - low_latency_manager.get_frame_start_time()
    );
    low_latency_manager.update_frame_timing(frame_duration.count());
    low_latency_manager.increment_frame_counter();
}
```

#### 1.2 Fix Sleep Logic
**Goal**: Sleep BEFORE frame, not during frame work

**New Approach**:
```cpp
void VulkanLowLatency::begin_frame() {
    if (!enabled || active_method != METHOD_CPU_SLEEP) {
        return;
    }

    if (frame_counter > 0) {
        // Calculate time until next frame should start
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - frame_start_time);

        // Target: start next frame at predicted_frame_time + negative_gain
        uint64_t target_frame_interval = (uint64_t)(target_frame_time_us * NEGATIVE_GAIN);

        if (elapsed.count() < target_frame_interval) {
            auto sleep_duration = std::chrono::microseconds(target_frame_interval - elapsed.count());
            std::this_thread::sleep_for(sleep_duration);
        }
    }

    frame_start_time = std::chrono::high_resolution_clock::now();
}
```

#### 1.3 Apply Negative Gain
**Goal**: Run CPU slightly below bottleneck to reduce queue

**Implementation**:
```cpp
// In vulkan_low_latency.h:
static constexpr float NEGATIVE_GAIN = 0.985f;  // -1.5% like LatencyFleX

// In update_frame_timing():
if (active_method == METHOD_CPU_SLEEP && stats.predicted_frame_time_us > 0) {
    target_frame_time_us = (uint64_t)(stats.predicted_frame_time_us * NEGATIVE_GAIN);
}
```

---

### Phase 2: Implement BBR-Inspired Adaptive Pacing

#### 2.1 Add Probe Phases
**Goal**: Periodically probe for bottleneck bandwidth changes

**Implementation**:
```cpp
enum ProbePhase {
    PROBE_STABLE,     // Normal operation
    PROBE_UP,         // Test if we can go faster (gain 1.10)
    PROBE_DOWN        // Test bandwidth floor (gain 0.90)
};

ProbePhase probe_phase = PROBE_STABLE;
uint32_t frames_in_phase = 0;
static constexpr uint32_t PROBE_INTERVAL = 60;  // Probe every 60 frames

void begin_frame() {
    // Cycle through probe phases
    if (++frames_in_phase >= PROBE_INTERVAL) {
        frames_in_phase = 0;
        probe_phase = (ProbePhase)((probe_phase + 1) % 3);
    }

    // Apply probe gain
    float probe_gain = 1.0f;
    switch (probe_phase) {
        case PROBE_UP: probe_gain = 1.10f; break;
        case PROBE_DOWN: probe_gain = 0.90f; break;
        default: probe_gain = 1.0f; break;
    }

    uint64_t adjusted_target = (uint64_t)(target_frame_time_us * probe_gain);
    // ... use adjusted_target for sleep calculation
}
```

#### 2.2 Delay Compensation (LatencyFleX Style)
**Goal**: Compensate for prediction errors

**Implementation**:
```cpp
// Track prediction error
uint64_t predicted_frame_end_us = 0;
int64_t last_compensation_us = 0;

void begin_frame() {
    // Predict when this frame will end
    predicted_frame_end_us = current_time_us + target_frame_time_us;
}

void update_frame_timing(uint64_t actual_time_us) {
    // Calculate prediction error
    int64_t actual_end_us = frame_start_time_us + actual_time_us;
    int64_t error_us = actual_end_us - predicted_frame_end_us;

    // Compensate only for delays (positive error)
    int64_t compensation = max(0, error_us - last_compensation_us);
    last_compensation_us = error_us;

    // Apply compensation to next frame's target
    target_frame_time_us += compensation;
}
```

---

### Phase 3: Add Telemetry & Debugging

#### 3.1 Enhanced Stats Structure
```cpp
struct Stats {
    Method active_method = METHOD_NONE;

    // Frame timing
    uint64_t predicted_frame_time_us = 0;
    uint64_t actual_frame_time_us = 0;
    uint64_t target_frame_time_us = 0;

    // EWMA tracking
    float ewma_alpha = 0.0f;
    uint64_t ewma_samples = 0;

    // Probe phase tracking
    ProbePhase current_probe_phase = PROBE_STABLE;
    uint64_t probe_up_frames = 0;
    uint64_t probe_down_frames = 0;

    // Performance metrics
    uint64_t cpu_sleep_frames = 0;
    uint64_t total_sleep_time_us = 0;
    int64_t avg_prediction_error_us = 0;

    // Queue depth estimation
    uint64_t estimated_queue_depth_frames = 0;

    // Latency metrics
    uint64_t min_frame_time_us = UINT64_MAX;
    uint64_t max_frame_time_us = 0;
};
```

#### 3.2 Debug Logging
```cpp
void print_stats() {
    if (frame_counter % 60 == 0) {  // Every second at 60fps
        print_verbose(vformat(
            "Low-latency stats: actual=%.2fms predicted=%.2fms target=%.2fms phase=%s",
            actual_frame_time_us / 1000.0f,
            predicted_frame_time_us / 1000.0f,
            target_frame_time_us / 1000.0f,
            probe_phase_name[current_probe_phase]
        ));
    }
}
```

---

### Phase 4: Parameter Optimization

#### 4.1 EWMA Alpha Tuning
**Test values**: 0.05, 0.10, 0.15, 0.20, 0.25
**Goal**: Find balance between responsiveness and stability
**Metric**: Measure frame time variance and prediction error

#### 4.2 Negative Gain Tuning
**Test values**: 0.970 (-3%), 0.980 (-2%), 0.985 (-1.5%), 0.990 (-1%)
**Goal**: Find optimal queue reduction rate
**Metric**: Measure input latency and frame time stability

#### 4.3 Probe Interval Tuning
**Test values**: 30, 60, 120 frames
**Goal**: Balance adaptation speed vs stability
**Metric**: Measure responsiveness to workload changes

---

## Success Metrics

### Latency Reduction
- **Target**: 20-40% reduction in input-to-photon latency
- **Measurement**: Compare frame queue depth before/after

### Frame Time Stability
- **Target**: < 5% frame time variance
- **Measurement**: Standard deviation of frame times

### GPU Utilization
- **Target**: 95-98% GPU utilization (slight headroom)
- **Measurement**: GPU busy percentage

### Adaptability
- **Target**: < 1 second to adapt to frame rate changes
- **Measurement**: Time to stabilize after workload spike

---

## Implementation Order

1. ✅ **[DONE]** Research BBR, LatencyFleX, Reflex
2. ⏳ **[NEXT]** Fix Bug 1: Measure actual frame time
3. ⏳ Fix Bug 2: Correct sleep logic
4. ⏳ Fix Bug 3: Apply negative gain
5. ⏳ Fix Bug 5: Tune EWMA alpha
6. ⏳ Add telemetry and debug logging
7. ⏳ Test and measure baseline performance
8. ⏳ Implement BBR probe phases
9. ⏳ Implement delay compensation
10. ⏳ Optimize parameters through testing
11. ⏳ Final validation and latency measurement

---

## References

- [BBR Congestion Control](https://queue.acm.org/detail.cfm?id=3022184)
- [LatencyFleX Blog Post](http://blog.ishitatsuy.uk/post/latencyflex/)
- [LatencyFleX Source](https://github.com/ishitatsuyuki/LatencyFleX)
- NVIDIA Reflex EWMA approach
