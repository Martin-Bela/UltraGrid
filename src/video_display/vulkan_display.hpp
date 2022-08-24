/**
 * @file   video_display/vulkan_display.h
 * @author Martin Be¾a      <492789@mail.muni.cz>
 */
/*
 * Copyright (c) 2021-2022 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "ext-deps/readerwriterqueue/readerwritercircularbuffer.h"
#include "vulkan_context.hpp"
#include "vulkan_transfer_image.hpp"


#include <queue>
#include <mutex>
#include <filesystem>
#include <utility>

namespace vulkan_display_detail {

constexpr static unsigned filled_img_max_count = 1;
constexpr auto waiting_time_for_filled_image = 50ms;

template<typename T>
using concurrent_queue = moodycamel::BlockingReaderWriterCircularBuffer<T>;

struct render_area {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
};

} // vulkan_display_detail

namespace vulkan_display {

namespace detail = vulkan_display_detail;

constexpr inline bool is_yCbCr_format(vk::Format format) {
        auto f = static_cast<VkFormat>(format);
        return VK_FORMAT_G8B8G8R8_422_UNORM <= f && f <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM;
}

constexpr inline bool is_compressed_format(vk::Format format) {
        auto f = static_cast<VkFormat>(format);
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK <= f && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}

class window_changed_callback {
protected:
        ~window_changed_callback() = default;
public:
        virtual window_parameters get_window_parameters() = 0;
};

class vulkan_display {
        window_changed_callback* window = nullptr;
        detail::vulkan_context context;
        vk::Device device;
        std::mutex device_mutex{};

        detail::render_area render_area{};
        vk::Viewport viewport;
        vk::Rect2D scissor;

        vk::ShaderModule vertex_shader;
        vk::ShaderModule fragment_shader;

        vk::RenderPass render_pass;
        vk::ClearValue clear_color;

        vk::SamplerYcbcrConversion yCbCr_conversion;
        vk::Sampler sampler{};

        vk::DescriptorSetLayout descriptor_set_layout;
        vk::DescriptorPool descriptor_pool;
        std::vector<vk::DescriptorSet> descriptor_sets{};

        vk::PipelineLayout pipeline_layout;
        vk::Pipeline pipeline;

        vk::CommandPool command_pool;
        std::vector<vk::CommandBuffer> command_buffers{};

        image_description current_image_description;

        struct image_semaphores {
                vk::Semaphore image_acquired;
                vk::Semaphore image_rendered;
        };
        std::vector<image_semaphores> image_semaphores;


        unsigned transfer_image_count = 0;
        using transfer_image = detail::transfer_image;
        std::vector<transfer_image> transfer_images{};

        /// available_img_queue - producer is the render thread, consumer is the provided thread
        detail::concurrent_queue<transfer_image*> available_img_queue{8};
        /// filled_img_queue - producer is the provider thread, consumer is the render thread
        detail::concurrent_queue<transfer_image*> filled_img_queue{8};
        /// local to provider thread
        std::vector<transfer_image*> available_images;

        std::queue<transfer_image*> rendered_images;

        bool minimalised = false;
        bool destroyed = false;
private:

        void create_texture_sampler(vk::Format format);

        void create_descriptor_set_layout();

        void create_pipeline_layout();

        void create_graphics_pipeline();

        void create_transfer_image(transfer_image*& result, image_description description);

        void create_image_semaphores();

        void allocate_description_sets();

        void record_graphics_commands(transfer_image& transfer_image, uint32_t swapchain_image_id);

public:
        /// TERMINOLOGY:
        /// render thread - thread which renders queued images on the screen 
        /// provider thread - thread which calls getf and putf and fills image queue with newly filled images

        vulkan_display() = default;

        vulkan_display(const vulkan_display& other) = delete;
        vulkan_display& operator=(const vulkan_display& other) = delete;
        vulkan_display(vulkan_display&& other) = delete;
        vulkan_display& operator=(vulkan_display&& other) = delete;

        ~vulkan_display() noexcept {
                if (!destroyed) {
                        destroy();
                }
        }

        void init(vulkan_instance&& instance, VkSurfaceKHR surface, uint32_t transfer_image_count,
                window_changed_callback& window, uint32_t gpu_index = no_gpu_selected,
                std::filesystem::path path_to_shaders = "./shaders", bool vsync = true, bool tearing_permitted = false);

        void destroy();

        /** Thread-safe */
        bool is_image_description_supported(image_description description);

        /** Thread-safe to call from provider thread.*/
        image acquire_image(image_description description);

        /** Thread-safe to call from provider thread.
         **
         ** @return true if image was discarded
         */
        bool queue_image(image img, bool discardable);

        /** Thread-safe to call from provider thread.*/
        void copy_and_queue_image(std::byte* frame, image_description description);

        /** Thread-safe to call from provider thread.*/
        void discard_image(image image) {
                auto* ptr = image.get_transfer_image();
                assert(ptr);
                available_images.push_back(ptr);
        }

        /** Thread-safe to call from render thread.
         **
         ** @return true if image was displayed
         */
        bool display_queued_image();

        /** Thread-safe*/
        uint32_t get_vulkan_version() const { return context.get_vulkan_version(); }
        
        /** Thread-safe*/
        bool is_yCbCr_supported() const { return context.is_yCbCr_supported(); }

        /**
         * @brief Hint to vulkan display that some window parameters spicified in struct Window_parameters changed.
         * Thread-safe.
         */
        void window_parameters_changed(window_parameters new_parameters);

        
        /** Thread-safe */
        void window_parameters_changed() {
                window_parameters_changed(window->get_window_parameters());
        }
};

} //vulkan_display
