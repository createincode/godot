/**************************************************************************/
/*  vulkan_low_latency.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).*/
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "vulkan_low_latency.h"

#include "core/config/project_settings.h"
#include "core/string/print_string.h"

#include <chrono>
#include <thread>

void VulkanLowLatency::initialize(VkDevice p_device, VkPhysicalDevice p_physical_device, const RenderingContextDriverVulkan::Functions *p_functions) {
	device = p_device;

	if (!enabled) {
		active_method = METHOD_NONE;
		return;
	}

	// Use CPU sleep-based frame pacing.
	// Timeline semaphores have driver compatibility issues on some AMD RADV versions.
	active_method = METHOD_CPU_SLEEP;
	print_verbose("Low-latency rendering: Using CPU sleep-based frame pacing");
}

void VulkanLowLatency::cleanup(VkDevice p_device) {
	if (timeline_semaphore != VK_NULL_HANDLE) {
		vkDestroySemaphore(p_device, timeline_semaphore, nullptr);
		timeline_semaphore = VK_NULL_HANDLE;
	}
}

void VulkanLowLatency::set_enabled(bool p_enabled) {
	enabled = p_enabled;
}

void VulkanLowLatency::begin_frame() {
	if (!enabled) {
		return;
	}

	if (active_method == METHOD_TIMELINE_SEMAPHORE) {
		// Wait for frame N-MAX_FRAMES_IN_FLIGHT to complete before starting frame N
		if (frame_counter >= MAX_FRAMES_IN_FLIGHT) {
			VkSemaphoreWaitInfo wait_info = {};
			wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			wait_info.semaphoreCount = 1;
			wait_info.pSemaphores = &timeline_semaphore;
			uint64_t wait_value = frame_counter - MAX_FRAMES_IN_FLIGHT;
			wait_info.pValues = &wait_value;

			VkResult result = vkWaitSemaphores(device, &wait_info, DEFAULT_TIMEOUT_NS);

			{
				MutexLock lock(stats_mutex);
				stats.timeline_waits++;
			}

			if (result == VK_TIMEOUT) {
				MutexLock lock(stats_mutex);
				stats.timeout_fallbacks++;
				print_error("Low-latency timeline semaphore wait timeout");
			} else if (result != VK_SUCCESS) {
				print_error(vformat("Low-latency timeline semaphore wait failed: error %d", result));
			}
		}

		frame_start_time = std::chrono::high_resolution_clock::now();

	} else if (active_method == METHOD_CPU_SLEEP) {
		// CPU-based frame pacing (LatencyFleX/BBR inspired)
		auto now = std::chrono::high_resolution_clock::now();

		if (frame_counter > 0) {
			// Calculate elapsed time since last frame START
			auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - frame_start_time);

			// Target: next frame should start at (predicted_time * negative_gain)
			// This creates CPU headroom and prevents queue buildup
			uint64_t target_interval_us = target_frame_time_us;

			// Sleep if we need to wait before starting next frame
			if (elapsed.count() < static_cast<int64_t>(target_interval_us)) {
				auto sleep_duration = std::chrono::microseconds(target_interval_us - elapsed.count());
				std::this_thread::sleep_for(sleep_duration);

				MutexLock lock(stats_mutex);
				stats.cpu_sleep_frames++;
			}
		}

		// Mark the start of this frame
		frame_start_time = std::chrono::high_resolution_clock::now();
	}
}

void VulkanLowLatency::update_frame_timing(uint64_t actual_time_us) {
	MutexLock lock(stats_mutex);
	stats.actual_frame_time_us = actual_time_us;

	// EWMA: new = alpha * actual + (1-alpha) * old
	// Lower alpha (0.10) provides smoother, more stable predictions
	if (stats.predicted_frame_time_us == 0) {
		stats.predicted_frame_time_us = actual_time_us;
	} else {
		stats.predicted_frame_time_us = (uint64_t)(EWMA_ALPHA * actual_time_us + (1.0f - EWMA_ALPHA) * stats.predicted_frame_time_us);
	}

	// Update target frame time for CPU sleep method
	// Apply negative gain (-1.5%) to run slightly below bottleneck, reducing queue depth
	if (active_method == METHOD_CPU_SLEEP && stats.predicted_frame_time_us > 0) {
		target_frame_time_us = (uint64_t)(stats.predicted_frame_time_us * NEGATIVE_GAIN);
	}

	// Debug logging every 60 frames (approximately 1 second at 60fps)
	if (frame_counter % 60 == 0 && frame_counter > 0) {
		print_verbose(vformat(
			"Low-latency stats: actual=%.2fms predicted=%.2fms target=%.2fms (%.1f fps)",
			actual_time_us / 1000.0f,
			stats.predicted_frame_time_us / 1000.0f,
			target_frame_time_us / 1000.0f,
			1000000.0f / stats.predicted_frame_time_us
		));
	}
}

uint64_t VulkanLowLatency::get_predicted_frame_time() const {
	MutexLock lock(stats_mutex);
	return stats.predicted_frame_time_us > 0 ? stats.predicted_frame_time_us : 16667; // 60Hz default
}

VulkanLowLatency::Stats VulkanLowLatency::get_stats() const {
	MutexLock lock(stats_mutex);
	return stats;
}
