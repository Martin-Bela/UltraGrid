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

#include "vulkan_transfer_image.hpp"

using namespace vulkan_display_detail;
namespace vkd = vulkan_display;
namespace {

constexpr vk::DeviceSize add_padding(vk::DeviceSize size, vk::DeviceSize allignment) {
        vk::DeviceSize remainder = size % allignment;
        if (remainder == 0) {
                return size;
        }
        return size + allignment - remainder;
}


/**
 * Check if the required flags are present among the provided flags
 */
template<typename T>
constexpr bool flags_present(T provided_flags, T required_flags) {
        return (provided_flags & required_flags) == required_flags;
}

void get_memory_type(
        uint32_t& memory_type, uint32_t memory_type_bits,
        vk::MemoryPropertyFlags requested_properties, vk::MemoryPropertyFlags optional_properties,
        vk::PhysicalDevice gpu)
{
        uint32_t possible_memory_type = UINT32_MAX;
        auto supported_properties = gpu.getMemoryProperties();
        for (uint32_t i = 0; i < supported_properties.memoryTypeCount; i++) {
                // if i-th bit in memory_type_bits is set, than i-th memory type can be used
                bool is_type_usable = (1u << i) & memory_type_bits;
                auto& mem_type = supported_properties.memoryTypes[i];
                if (flags_present(mem_type.propertyFlags, requested_properties) && is_type_usable) {
                        if (flags_present(mem_type.propertyFlags, optional_properties)) {
                                memory_type = i;
                                return void();
                        }
                        possible_memory_type = i;
                }
        }
        if (possible_memory_type != UINT32_MAX) {
                memory_type = possible_memory_type;
                return void();
        }
        VKD_CHECK(false, "No available memory for transfer images found.");
        return void();
}

constexpr vk::ImageType image_type = vk::ImageType::e2D;
constexpr vk::ImageTiling image_tiling = vk::ImageTiling::eLinear;
const vk::ImageUsageFlags image_usage_flags = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
constexpr vk::ImageCreateFlags image_create_flags = {};
} //namespace -------------------------------------------------------------

namespace vulkan_display_detail{

void transfer_image::is_image_description_supported(bool& supported, vk::PhysicalDevice gpu, 
        vkd::image_description description)
{
        vk::ImageFormatProperties properties;
        auto result = gpu.getImageFormatProperties(
                description.format,
                image_type,
                image_tiling,
                image_usage_flags,
                image_create_flags,
                &properties);
        if (result == vk::Result::eErrorFormatNotSupported) {
                supported = false;
                return void();
        }
        VKD_CHECK(result, "Error queriing image properties:")
        supported = description.size.height <= properties.maxExtent.height
                && description.size.width <= properties.maxExtent.width;
        return void();
}

void transfer_image::init(vk::Device device, uint32_t id) {
        this->id = id;
        vk::FenceCreateInfo fence_info{ vk::FenceCreateFlagBits::eSignaled };
        is_available_fence = device.createFence(fence_info);
        return void();
}

void transfer_image::create(vk::Device device, vk::PhysicalDevice gpu,
        vkd::image_description description)
{
        assert(id != NO_ID);
        destroy(device, false);
        
        this->view = nullptr;
        this->description = description;
        this->layout = vk::ImageLayout::ePreinitialized;
        this->access = vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eHostRead;

        vk::ImageCreateInfo image_info;
        image_info
                .setFlags(image_create_flags)
                .setImageType(image_type)
                .setExtent(vk::Extent3D{ description.size, 1 })
                .setMipLevels(1)
                .setArrayLayers(1)
                .setFormat(description.format)
                .setTiling(image_tiling)
                .setInitialLayout(vk::ImageLayout::ePreinitialized)
                .setUsage(image_usage_flags)
                .setSharingMode(vk::SharingMode::eExclusive)
                .setSamples(vk::SampleCountFlagBits::e1);
        image = device.createImage(image_info);

        vk::MemoryRequirements memory_requirements = device.getImageMemoryRequirements(image);
        vk::DeviceSize byte_size = add_padding(memory_requirements.size, memory_requirements.alignment);

        using mem_bits = vk::MemoryPropertyFlagBits;
        uint32_t memory_type = 0;
        get_memory_type(memory_type, memory_requirements.memoryTypeBits,
                mem_bits::eHostVisible | mem_bits::eHostCoherent, mem_bits::eHostCached, gpu);

        vk::MemoryAllocateInfo allocInfo{ byte_size , memory_type };
        memory = device.allocateMemory(allocInfo);

        device.bindImageMemory(image, memory, 0);

        void* void_ptr = device.mapMemory(memory, 0, memory_requirements.size);
        VKD_CHECK(void_ptr != nullptr, "Image memory cannot be mapped.");
        ptr = reinterpret_cast<std::byte*>(void_ptr);

        vk::ImageSubresource subresource{ vk::ImageAspectFlagBits::eColor, 0, 0 };
        row_pitch = device.getImageSubresourceLayout(image, subresource).rowPitch;
        return void();
}

vk::ImageMemoryBarrier  transfer_image::create_memory_barrier(
        vk::ImageLayout new_layout, vk::AccessFlags new_access_mask,
        uint32_t src_queue_family_index, uint32_t dst_queue_family_index)
{
        vk::ImageMemoryBarrier memory_barrier{};
        memory_barrier
                .setImage(image)
                .setOldLayout(layout)
                .setNewLayout(new_layout)
                .setSrcAccessMask(access)
                .setDstAccessMask(new_access_mask)
                .setSrcQueueFamilyIndex(src_queue_family_index)
                .setDstQueueFamilyIndex(dst_queue_family_index);
        memory_barrier.subresourceRange
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setLayerCount(1)
                .setLevelCount(1);

        layout = new_layout;
        access = new_access_mask;
        return memory_barrier;
}

void transfer_image::prepare_for_rendering(vk::Device device, 
        vk::DescriptorSet descriptor_set, vk::Sampler sampler, vk::SamplerYcbcrConversion conversion) 
{
        if (!view) {
                device.destroy(view);
                vk::ImageViewCreateInfo view_info = 
                        vkd::default_image_view_create_info(description.format);
                view_info.setImage(image);

                vk::SamplerYcbcrConversionInfo yCbCr_info{ conversion };
                view_info.setPNext(conversion ? &yCbCr_info : nullptr);
                view = device.createImageView(view_info);

                vk::DescriptorImageInfo description_image_info;
                description_image_info
                        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
                        .setSampler(sampler)
                        .setImageView(view);

                vk::WriteDescriptorSet descriptor_writes{};
                descriptor_writes
                        .setDstBinding(1)
                        .setDstArrayElement(0)
                        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                        .setPImageInfo(&description_image_info)
                        .setDescriptorCount(1)
                        .setDstSet(descriptor_set);

                device.updateDescriptorSets(descriptor_writes, nullptr);
        }
        return void();
}

void transfer_image::preprocess() {
        if (preprocess_fun) {
                vulkan_display::image img{ *this };
                preprocess_fun(img);
                img.set_process_function(nullptr);
        }
}

void transfer_image::destroy(vk::Device device, bool destroy_fence) {
        if (is_available_fence) {
                auto result = device.waitForFences(is_available_fence, true, UINT64_MAX);
                VKD_CHECK(result, "Waiting for transfer image fence failed.");
        }

        device.destroy(view);
        device.destroy(image);

        if (memory) {
                device.unmapMemory(memory);
                device.freeMemory(memory);
        }
        if (destroy_fence) {
                device.destroy(is_available_fence);
        }
        return void();
}

} //vulkan_display_detail
