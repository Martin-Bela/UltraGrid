/**
 * @file   video_display/vulkan_transfer_image.cpp
 * @author Martin Bela      <492789@mail.muni.cz>
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

struct ImageDescription {
        vk::Extent2D size{};
        vk::Format format{};

        ImageDescription() = default;
        ImageDescription(vk::Extent2D size, vk::Format format) :
                size{ size }, format{ format } { }
        ImageDescription(uint32_t width, uint32_t height, vk::Format format) :
                ImageDescription{ vk::Extent2D{width, height}, format } { }

        bool operator==(const ImageDescription& other) const {
                return size.width == other.size.width 
                        && size.height == other.size.height 
                        && format == other.format;
        }

        bool operator!=(const ImageDescription& other) const {
                return !(*this == other);
        }
};

class TransferImage;

} // vulkan_display-----------------------------------------

namespace vulkan_display_detail {

enum class MemoryLocation{
        device_local, host_local        
};

enum class InitialImageData{
        preinitialised, undefined
};

class Image2D{
public:
        vk::DeviceMemory memory{};
        vk::Image image{};
        vk::ImageLayout layout{};
        vk::AccessFlags access{};

        vk::ImageView view{};

        size_t byte_size{};

        vk::Extent2D size{};
        vk::Format format{};
public:
        void init(VulkanContext& context,
                vulkan_display::ImageDescription description, vk::ImageUsageFlags usage,
                vk::AccessFlags initial_access, InitialImageData preinitialised, MemoryLocation memory_location);

        void init(VulkanContext& context,
                vulkan_display::ImageDescription description, vk::ImageUsageFlags usage,
                vk::AccessFlags initial_access, InitialImageData preinitialised, vk::ImageTiling tiling,
                vk::MemoryPropertyFlags requested_properties, vk::MemoryPropertyFlags optional_properties);

        void create_view(vk::Device device, vk::SamplerYcbcrConversion conversion);
        
        void destroy(vk::Device device);
public:
        vulkan_display::ImageDescription get_description() const { return {size, format}; }
};

class TransferImageImpl {
        Image2D image2D;
        uint32_t id = NO_ID;
        std::byte* ptr = nullptr;

        vk::DeviceSize row_pitch = 0;

        using PreprocessFunction = std::function<void(vulkan_display::TransferImage& image)>;
        PreprocessFunction preprocess_fun{ nullptr };
        
public:
        friend class vulkan_display::TransferImage;

        static constexpr uint32_t NO_ID = UINT32_MAX;

        vk::Fence is_available_fence; // is_available_fence becames signalled when gpu releases the image

        uint32_t get_id() { return id; }
        
        static bool is_image_description_supported(vk::PhysicalDevice gpu, vulkan_display::ImageDescription description);

        void init(vk::Device device, uint32_t id);

        void recreate(VulkanContext& context, vulkan_display::ImageDescription description);

        vulkan_display::ImageDescription get_description() const { return image2D.get_description(); }

        vk::ImageMemoryBarrier create_memory_barrier(
                vk::ImageLayout new_layout,
                vk::AccessFlags new_access_mask,
                uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED);

        void prepare_for_rendering(vk::Device device, vk::DescriptorSet descriptor_set, 
                vk::Sampler sampler, vk::SamplerYcbcrConversion conversion);

        void preprocess();

        void destroy(vk::Device device);

        TransferImageImpl() = default;
        TransferImageImpl(vk::Device device, uint32_t id) {
                init(device, id);
        }
};

} // vulkan_display_detail


namespace vulkan_display {

namespace detail = vulkan_display_detail;

class TransferImage {
        
        detail::TransferImageImpl* transfer_image = nullptr;
public:
        TransferImage() = default;
        TransferImage(std::nullptr_t){}

        explicit TransferImage(detail::TransferImageImpl& image) :
                transfer_image{ &image }
        { 
                assert(image.id != detail::TransferImageImpl::NO_ID);
        }

        uint32_t get_id() {
                assert(transfer_image); 
                return transfer_image->id;
        }

        std::byte* get_memory_ptr() {
                assert(transfer_image);
                return transfer_image->ptr;
        }

        ImageDescription get_description() {
                assert(transfer_image);
                return transfer_image->image2D.get_description();
        }

        vk::DeviceSize get_row_pitch() {
                assert(transfer_image);
                return transfer_image->row_pitch;
        }

        vk::Extent2D get_size() {
                return transfer_image->image2D.size;
        }

        vulkan_display_detail::TransferImageImpl* get_transfer_image() {
                return transfer_image;
        }

        void set_process_function(std::function<void(TransferImage& image)> function) {
                transfer_image->preprocess_fun = std::move(function);
        }

        bool operator==(TransferImage other){
                return transfer_image == other.transfer_image;
        }
};

} //namespace vulkan_display

