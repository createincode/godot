/**************************************************************************/
/*  vulkan_low_latency.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
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

#ifndef VULKAN_LOW_LATENCY_H
#define VULKAN_LOW_LATENCY_H

#include "core/os/mutex.h"
#include "rendering_context_driver_vulkan.h"
#include <vulkan/vulkan.h>

class VulkanLowLatency {
public:
	enum Method {
		METHOD_NONE = 0,
		METHOD_TIMELINE_SEMAPHORE = 1, // Vulkan 1.2 timeline semaphores
		METHOD_CPU_SLEEP = 2 // CPU-based frame pacing fallback
	};

	struct Stats {
		Method active_method = METHOD_NONE;
		uint64_t predicted_frame_time_us = 0;
		uint64_t actual_frame_time_us = 0;
		uint64_t timeline_waits = 0;
		uint64_t cpu_sleep_frames = 0;
		uint64_t timeout_fallbacks = 0;
	};

private:
	bool enabled = false;
	Method active_method = METHOD_NONE;
	Stats stats;
	mutable Mutex stats_mutex;
	VkDevice device = VK_NULL_HANDLE;

	// Timeline semaphore for frame pacing
	VkSemaphore timeline_semaphore = VK_NULL_HANDLE;
	uint64_t frame_counter = 0;
	static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

	// CPU pacing
	std::chrono::high_resolution_clock::time_point frame_start_time;
	uint64_t target_frame_time_us = 16667; // 60 FPS default

	// EWMA parameters (lowered from 0.25 for stability)
	static constexpr float EWMA_ALPHA = 0.10f;
	static constexpr float NEGATIVE_GAIN = 0.985f; // -1.5% like LatencyFleX
	static constexpr uint64_t DEFAULT_TIMEOUT_NS = 2000000000ULL; // 2 seconds

	// Vulkan function pointers
	PFN_vkWaitSemaphores vkWaitSemaphores = nullptr;
	PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue = nullptr;

public:
	void initialize(VkDevice p_device, VkPhysicalDevice p_physical_device, const RenderingContextDriverVulkan::Functions *p_functions);
	void cleanup(VkDevice p_device);
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }

	// Frame pacing methods
	void begin_frame();
	VkSemaphore get_timeline_semaphore() const { return timeline_semaphore; }
	uint64_t get_frame_counter() const { return frame_counter; }
	void increment_frame_counter() { frame_counter++; }

	// Frame timing
	void update_frame_timing(uint64_t actual_time_us);
	uint64_t get_predicted_frame_time() const;
	std::chrono::high_resolution_clock::time_point get_frame_start_time() const { return frame_start_time; }

	Stats get_stats() const;
	Method get_active_method() const { return active_method; }
};

#endif // VULKAN_LOW_LATENCY_H
