/**
 * @file   video_display/vulkan_transfer_image.cpp
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
#include "vulkan_context.hpp"
#include <functional>

namespace vulkan_display {

struct image_description {
        vk::Extent2D size;
        vk::Format format{};

        image_description() = default;
        image_description(vk::Extent2D size, vk::Format format) :
                size{ size }, format{ format } { }
        image_description(uint32_t width, uint32_t height, vk::Format format) :
                image_description{ vk::Extent2D{width, height}, format } { }

        bool operator==(const image_description& other) const {
                return size.width == other.size.width 
                        && size.height == other.size.height 
                        && format == other.format;
        }

        bool operator!=(const image_description& other) const {
                return !(*this == other);
        }
};

class image;

} // vulkan_display-----------------------------------------

namespace vulkan_display_detail {

class transfer_image {
        vk::DeviceMemory memory;
        vk::Image image;
        vk::ImageLayout layout{};
        vk::AccessFlags access;

        uint32_t id = NO_ID;
        vk::ImageView view;
        std::byte* ptr = nullptr;
        vulkan_display::image_description description;

        vk::DeviceSize row_pitch = 0;

        using preprocess_function = std::function<void(vulkan_display::image& image)>;
        preprocess_function preprocess_fun{ nullptr };
        
public:
        friend class vulkan_display::image;

        static constexpr uint32_t NO_ID = UINT32_MAX;

        bool fence_set = false;       // true if waiting for is_available_fence is neccessary
        vk::Fence is_available_fence; // is_available_fence isn't signalled when gpu uses the image

        const vulkan_display::image_description& get_description() { return description; }
        uint32_t get_id() { return id; }

        static void is_image_description_supported(bool& supported, vk::PhysicalDevice gpu,
                vulkan_display::image_description description);
        
        void init(vk::Device device, uint32_t id);

        void create(vk::Device device, vk::PhysicalDevice gpu, vulkan_display::image_description description);

        vk::ImageMemoryBarrier create_memory_barrier(
                vk::ImageLayout new_layout,
                vk::AccessFlags new_access_mask,
                uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED);

        void prepare_for_rendering(vk::Device device, vk::DescriptorSet descriptor_set, 
                vk::Sampler sampler, vk::SamplerYcbcrConversion conversion);

        void preprocess();

        void destroy(vk::Device device, bool destroy_fence = true);

        transfer_image() = default;
        transfer_image(vk::Device device, uint32_t id) {
                init(device, id);
        }
};

} // vulkan_display_detail


namespace vulkan_display {

namespace detail = vulkan_display_detail;

class image {
        
        detail::transfer_image* transfer_image = nullptr;
public:
        image() = default;
        explicit image(detail::transfer_image& image) :
                transfer_image{ &image }
        { 
                assert(image.id != detail::transfer_image::NO_ID);
        }

        uint32_t get_id() {
                assert(transfer_image); 
                return transfer_image->id;
        }

        std::byte* get_memory_ptr() {
                assert(transfer_image);
                return transfer_image->ptr;
        }

        image_description get_description() {
                assert(transfer_image);
                return transfer_image->description;
        }

        vk::DeviceSize get_row_pitch() {
                assert(transfer_image);
                return transfer_image->row_pitch;
        }

        vk::Extent2D get_size() {
                return transfer_image->description.size;
        }

        vulkan_display_detail::transfer_image* get_transfer_image() {
                return transfer_image;
        }

        void set_process_function(std::function<void(image& image)> function) {
                transfer_image->preprocess_fun = std::move(function);
        }
};

} //namespace vulkan_display

