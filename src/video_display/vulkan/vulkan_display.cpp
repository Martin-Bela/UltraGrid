/**
 * @file   video_display/vulkan_display.cpp
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

#include "vulkan_display.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

using namespace vulkan_display_detail;
using namespace vulkan_display;

namespace {
vk::PresentModeKHR get_present_mode(bool vsync_enabled, bool tearing_permitted){
        using Mode = vk::PresentModeKHR;
        if (vsync_enabled){
                return tearing_permitted ? Mode::eFifoRelaxed : Mode::eFifo;
        }
        return tearing_permitted ? Mode::eImmediate : Mode::eMailbox;
}

void discard_filled_image(ConcurrentCircularBuffer<TransferImageImpl*>& filled_img_queue,
        ConcurrentQueue<TransferImageImpl*>& available_img_queue)
{
        TransferImageImpl* transfer_image = nullptr;
        bool dequeued = filled_img_queue.try_dequeue(transfer_image);
        if (dequeued && transfer_image) {
                available_img_queue.enqueue(transfer_image);
        }
}

vk::CommandPool create_command_pool(vk::Device device, uint32_t queue_family_index) {
        vk::CommandPoolCreateInfo pool_info{};
        using Bits = vk::CommandPoolCreateFlagBits;
        pool_info
                .setQueueFamilyIndex(queue_family_index)
                .setFlags(Bits::eTransient | Bits::eResetCommandBuffer);
        return device.createCommandPool(pool_info);
}

std::vector<vk::CommandBuffer> create_command_buffers(vk::Device device, vk::CommandPool command_pool, uint32_t count) {
        vk::CommandBufferAllocateInfo allocate_info{};
        allocate_info
                .setCommandPool(command_pool)
                .setLevel(vk::CommandBufferLevel::ePrimary)
                .setCommandBufferCount(count);
        return device.allocateCommandBuffers(allocate_info);
}

vk::Semaphore create_semaphore(vk::Device device) {
        vk::SemaphoreCreateInfo semaphore_info{};
        return device.createSemaphore(semaphore_info);
}

vk::DescriptorPool create_descriptor_pool(vk::Device device, size_t descriptor_count){
        std::array<vk::DescriptorPoolSize,2> descriptor_sizes{};
        descriptor_sizes[0]
                .setType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(descriptor_count * 2);

        descriptor_sizes[1]
                .setType(vk::DescriptorType::eStorageImage)
                .setDescriptorCount(descriptor_count);

        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info
                .setPoolSizeCount(descriptor_sizes.size())
                .setPPoolSizes(descriptor_sizes.data())
                .setMaxSets(descriptor_count * 3);
        return device.createDescriptorPool(pool_info);
}

std::vector<vk::DescriptorSet> allocate_description_sets(vk::Device device, vk::DescriptorPool pool, vk::DescriptorSetLayout layout, size_t descriptor_count) {
        std::vector<vk::DescriptorSetLayout> layouts(descriptor_count, layout);

        vk::DescriptorSetAllocateInfo allocate_info;
        allocate_info
                .setDescriptorPool(pool)
                .setDescriptorSetCount(static_cast<uint32_t>(layouts.size()))
                .setPSetLayouts(layouts.data());

        return device.allocateDescriptorSets(allocate_info);
}

vk::SamplerYcbcrConversion createYCbCrConversion(vk::Device device, vk::Format format){
        vk::SamplerYcbcrConversion yCbCr_conversion = nullptr;
        if (is_yCbCr_format(format)) {
                vk::SamplerYcbcrConversionCreateInfo conversion_info;
                conversion_info
                        .setFormat(format)
                        .setYcbcrModel(vk::SamplerYcbcrModelConversion::eYcbcr709)
                        .setYcbcrRange(vk::SamplerYcbcrRange::eItuNarrow)
                        .setComponents({})
                        .setChromaFilter(vk::Filter::eLinear)
                        .setXChromaOffset(vk::ChromaLocation::eMidpoint)
                        .setYChromaOffset(vk::ChromaLocation::eMidpoint)
                        .setForceExplicitReconstruction(false);
                yCbCr_conversion = device.createSamplerYcbcrConversion(conversion_info);
        }
        return yCbCr_conversion;
}

vk::Sampler create_sampler(vk::Device device, vk::SamplerYcbcrConversion yCbCr_conversion) {
        vk::SamplerYcbcrConversionInfo yCbCr_info{ yCbCr_conversion };
        
        vk::SamplerCreateInfo sampler_info;
        sampler_info
                .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
                .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
                .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
                .setMagFilter(vk::Filter::eLinear)
                .setMinFilter(vk::Filter::eLinear)
                .setAnisotropyEnable(false)
                .setUnnormalizedCoordinates(false)
                .setPNext(yCbCr_conversion ? &yCbCr_info : nullptr);
        return device.createSampler(sampler_info);
}

template<size_t frame_count>
void bind_conversion_images(vk::Device device, vk::Sampler sampler,
        std::array<PerFrameResources, frame_count>& frame_resources)
{
        std::vector<vk::DescriptorImageInfo> descriptor_image_infos;
        descriptor_image_infos.reserve(frame_resources.size() * 2);

        std::vector<vk::WriteDescriptorSet> descriptor_writes;
        descriptor_writes.reserve(frame_resources.size() * 2);


        vk::DescriptorImageInfo store_image_info;
        store_image_info
                .setImageLayout(vk::ImageLayout::eGeneral);

        vk::WriteDescriptorSet store_write{};
        store_write
                .setDstBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageImage)
                .setDescriptorCount(1);

        vk::DescriptorImageInfo sample_image_info;
        sample_image_info
                .setSampler(sampler)
                .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

        vk::WriteDescriptorSet sample_write{};
        sample_write
                .setDstBinding(0)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);


        for(size_t i = 0; i < frame_resources.size(); i++){
                auto view = frame_resources[i].converted_image.get_image_view(device, nullptr);
                store_image_info.imageView = view;
                sample_image_info.imageView = view;

                store_write.setDstSet(frame_resources[i].conversion_destination_descriptor_set);
                sample_write.setDstSet(frame_resources[i].render_descriptor_set);

                descriptor_image_infos.push_back(store_image_info);
                store_write.setPImageInfo(&descriptor_image_infos.back());
                descriptor_writes.push_back(store_write);

                descriptor_image_infos.push_back(sample_image_info);
                sample_write.setPImageInfo(&descriptor_image_infos.back());
                descriptor_writes.push_back(sample_write);
        }
        device.updateDescriptorSets(descriptor_writes, nullptr);
}

} //namespace -------------------------------------------------------------


namespace vulkan_display {

void VulkanDisplay::init(VulkanInstance&& instance, VkSurfaceKHR surface, uint32_t initial_image_count,
        WindowChangedCallback& window, uint32_t gpu_index, std::filesystem::path shaders_path, bool vsync, bool tearing_permitted) {
        assert(surface);
        this->window = &window;
        this->path_to_shaders = std::move(shaders_path);

        auto window_parameters = window.get_window_parameters();

        context.init(std::move(instance), surface, window_parameters, gpu_index, get_present_mode(vsync, tearing_permitted));
        device = context.get_device();
        
        command_pool = create_command_pool(device, context.get_queue_family_index());
        descriptor_pool = create_descriptor_pool(device, frame_resources.size());

        regular_sampler = create_sampler(device, nullptr);

        render_pipeline.create(context, path_to_shaders);

        context.create_framebuffers(render_pipeline.get_render_pass());

        available_images.reserve(initial_image_count);
        for (uint32_t i = 0; i < initial_image_count; i++) {
                transfer_images.emplace_back(device, i);
                available_images.push_back(&transfer_images.back());
        }

        auto command_buffers = create_command_buffers(device, command_pool, frame_resources.size());
        for (size_t i = 0; i < frame_resources.size(); i++){
                auto& resources = frame_resources[i];
                resources.image_acquired_semaphore = create_semaphore(device);
                resources.image_rendered_semaphore = create_semaphore(device);
                resources.command_buffer = command_buffers[i];
        }

        free_frame_resources.reserve(frame_resources.size());
        for(auto& resources: frame_resources){
                free_frame_resources.emplace_back(&resources);
        }
}

void VulkanDisplay::destroy_format_dependent_resources(){
        device.destroy(yCbCr_sampler);
        device.destroy(yCbCr_conversion);

        conversion_pipeline.destroy(device);

        for(auto& resorces: frame_resources){
                resorces.converted_image.destroy(device);
        }
}

void VulkanDisplay::destroy() {
        if (!destroyed) {
                destroyed = true;
                if (device) {
                        device.waitIdle();
                        device.destroy(descriptor_pool);

                        for (auto& image : transfer_images) {
                                image.destroy(device);
                        }
                        device.destroy(command_pool);
                        device.destroy(regular_sampler);
                        for (auto& resources : frame_resources) {
                                device.destroy(resources.image_acquired_semaphore);
                                device.destroy(resources.image_rendered_semaphore);
                        }
                        destroy_format_dependent_resources();
                        render_pipeline.destroy(device);
                        conversion_pipeline.destroy(device);
                }
                context.destroy();
        }
}

bool VulkanDisplay::is_image_description_supported(ImageDescription description) {
        if (!is_yCbCr_supported() && is_yCbCr_format(description.format)) {
                return false;
        }
        std::scoped_lock lock(device_mutex);
        return TransferImageImpl::is_image_description_supported(context.get_gpu(), description);
}

void VulkanDisplay::bind_transfer_image(TransferImageImpl& transfer_image, PerFrameResources& resources) {
    auto view = transfer_image.get_image_view(device, yCbCr_conversion);

    auto image_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    auto descriptor_type = vk::DescriptorType::eCombinedImageSampler;
    auto descriptor_set = format_conversion_enabled ? resources.conversion_source_descriptor_set : resources.render_descriptor_set;

    vk::DescriptorImageInfo description_image_info;
    description_image_info
            .setImageLayout(image_layout)
            .setSampler(current_sampler())
            .setImageView(view);

    vk::WriteDescriptorSet descriptor_writes{};
    descriptor_writes
            .setDstBinding(0)
            .setDescriptorType(descriptor_type)
            .setPImageInfo(&description_image_info)
            .setDescriptorCount(1)
            .setDstSet(descriptor_set);

    device.updateDescriptorSets(descriptor_writes, nullptr);
}

void VulkanDisplay::record_graphics_commands(PerFrameResources& frame_resources, 
        TransferImageImpl& transfer_image, uint32_t swapchain_image_id)
{
        vk::CommandBuffer cmd_buffer = frame_resources.command_buffer;
        cmd_buffer.reset(vk::CommandBufferResetFlags{});

        vk::CommandBufferBeginInfo begin_info{};
        begin_info.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
        cmd_buffer.begin(begin_info);

        if(format_conversion_enabled){
                if(format_conversion_enabled){
                        auto conversion_image_memory_barrier = frame_resources.converted_image.create_memory_barrier(
                                vk::ImageLayout::eGeneral, vk::AccessFlagBits::eShaderWrite);
                        cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eComputeShader,
                                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, conversion_image_memory_barrier);
                }

                auto transfer_image_memory_barrier = transfer_image.get_image2D().create_memory_barrier(
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eShaderRead);
                cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader,
                        vk::DependencyFlagBits::eByRegion, nullptr, nullptr, transfer_image_memory_barrier);

                auto image_size = ImageSize::fromExtent2D(transfer_image.get_description().size);
                conversion_pipeline.record_commands(cmd_buffer, image_size,
                        {frame_resources.conversion_source_descriptor_set,
                         frame_resources.conversion_destination_descriptor_set});
        }

        Image2D& rendered_image = format_conversion_enabled ? frame_resources.converted_image : transfer_image.get_image2D();
        auto render_begin_memory_barrier = rendered_image.create_memory_barrier(
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eShaderRead);
         auto previous_stage = format_conversion_enabled ?
                 vk::PipelineStageFlagBits::eComputeShader : vk::PipelineStageFlagBits::eHost;
        cmd_buffer.pipelineBarrier(previous_stage, vk::PipelineStageFlagBits::eFragmentShader,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, render_begin_memory_barrier);

        render_pipeline.record_commands(cmd_buffer, frame_resources.render_descriptor_set,
                context.get_framebuffer(swapchain_image_id));

        auto transfer_image_memory_barrier = transfer_image.get_image2D().create_memory_barrier(
                vk::ImageLayout::eGeneral, vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eHostRead);
        auto transfer_image_last_stage = format_conversion_enabled ?
                vk::PipelineStageFlagBits::eComputeShader : vk::PipelineStageFlagBits::eFragmentShader;
        cmd_buffer.pipelineBarrier(transfer_image_last_stage, vk::PipelineStageFlagBits::eHost,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, transfer_image_memory_barrier);
        cmd_buffer.end();
}

TransferImageImpl& VulkanDisplay::acquire_transfer_image() {
        TransferImageImpl* result = nullptr;
        if (!available_images.empty()){
                result = available_images.back();
                available_images.pop_back();
                return *result;
        }

        if (available_img_queue.wait_dequeue_timed(result, 5ms)){
                return *result;
        }
        uint32_t id = transfer_images.size();
        transfer_images.emplace_back(device, id);
        return transfer_images.back();
}

TransferImage VulkanDisplay::acquire_image(ImageDescription description) {
        assert(description.size.width * description.size.height != 0);
        assert(description.format != vk::Format::eUndefined);
        if (!context.is_yCbCr_supported()) {
                if (is_yCbCr_format(description.format)) {
                        std::string error_msg{ "YCbCr formats are not supported."sv };
                        if (get_vulkan_version() == VK_API_VERSION_1_0) {
                                error_msg.append("\nVulkan 1.1 or higher is needed for YCbCr support."sv);
                        }
                        throw VulkanError{error_msg};
                }
        }
        TransferImageImpl& transfer_image = acquire_transfer_image();
        assert(transfer_image.get_id() != TransferImageImpl::NO_ID);

        if (transfer_image.get_description() != description) {
                std::scoped_lock device_lock(device_mutex);
                transfer_image.recreate(context, description);
        }
        
        return TransferImage{ transfer_image };
}

void VulkanDisplay::copy_and_queue_image(std::byte* frame, ImageDescription description) {
        TransferImage image = acquire_image(description);
        memcpy(image.get_memory_ptr(), frame, image.get_size().height * image.get_row_pitch());
        queue_image(image, false);
}

bool VulkanDisplay::queue_image(TransferImage image, bool discardable) {
        // if image is discardable and the filled_img_queue is full
        if(!discardable){
                filled_img_queue.wait_enqueue(image.get_transfer_image());
                return false;
        }

        if (filled_img_queue.wait_enqueue_timed(image.get_transfer_image(), 1ms)){
                return true;
        }

        available_images.push_back(image.get_transfer_image());
        return true;
        
        /*if (discardable && filled_img_queue.size_approx() > filled_img_max_count){
                available_images.push_back(image.get_transfer_image());
                return true;
        }
        else{
                filled_img_queue.wait_enqueue(image.get_transfer_image());
                return false;
        }*/
}

void VulkanDisplay::reconfigure(const TransferImageImpl& transfer_image){
        auto image_format = transfer_image.get_description().format;
        if (image_format != current_image_description.format) {
                log_msg("Recreating render_pipeline");
                context.get_queue().waitIdle();
                device.resetDescriptorPool(descriptor_pool);

                destroy_format_dependent_resources();

                if(is_yCbCr_format(image_format)){
                        yCbCr_conversion = createYCbCrConversion(device, image_format);
                        yCbCr_sampler = create_sampler(device, yCbCr_conversion);
                }
                else{
                        yCbCr_conversion = nullptr;
                        yCbCr_sampler = nullptr;
                }

                render_pipeline.reconfigure(device, current_sampler());

                auto descriptor_sets = allocate_description_sets(device, descriptor_pool, 
                        render_pipeline.get_image_desc_set_layout(), frame_resources.size());
                for(size_t i = 0; i < frame_resources.size(); i++){
                        frame_resources[i].render_descriptor_set = descriptor_sets[i];
                }

                format_conversion_enabled = false;
                if(image_format == vk::Format::eR8G8B8A8Unorm){
                        format_conversion_enabled = true;
                        conversion_pipeline.create(device, path_to_shaders, regular_sampler);
                        for(size_t i = 0; i < frame_resources.size(); i++){
                                frame_resources[i].converted_image.init(
                                        context,
                                        ImageDescription{transfer_image.get_description().size, vk::Format::eR8G8B8A8Unorm},
                                        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                        vk::AccessFlagBits::eShaderWrite,
                                        InitialImageData::undefined, MemoryLocation::device_local);
                        }
                        auto conversion_source_descriptor_sets = allocate_description_sets(device, descriptor_pool,
                                conversion_pipeline.get_source_image_desc_set_layout(), frame_resources.size());
                        auto conversion_destination_descriptor_sets = allocate_description_sets(device, descriptor_pool,
                                conversion_pipeline.get_destination_image_desc_set_layout(), frame_resources.size());
                        for(size_t i = 0; i < frame_resources.size(); i++){
                                assert(conversion_source_descriptor_sets.size() == frame_resources.size());
                                assert(conversion_destination_descriptor_sets.size() == frame_resources.size());
                                auto& resources = frame_resources[i];
                                resources.conversion_source_descriptor_set = conversion_source_descriptor_sets[i];
                                resources.conversion_destination_descriptor_set = conversion_destination_descriptor_sets[i];
                        }
                        
                        bind_conversion_images(device, regular_sampler, frame_resources);
                }
        }

        current_image_description = transfer_image.get_description();
        auto parameters = context.get_window_parameters();
        render_pipeline.update_render_area( { parameters.width, parameters.height }, current_image_description.size);
}

bool VulkanDisplay::display_queued_image() {
        auto window_parameters = window->get_window_parameters();
        if (window_parameters.is_minimized()) {
                discard_filled_image(filled_img_queue, available_img_queue);
                return false;
        }

        {
                std::scoped_lock lock{device_mutex};
                while (!rendered_images.empty()){
                        auto* first_image = rendered_images.front().image;
                        auto result = device.waitForFences(first_image->is_available_fence, VK_TRUE, 0);
                        if (result == vk::Result::eSuccess){
                                device.resetFences(first_image->is_available_fence);
                                free_frame_resources.push_back(rendered_images.front().gpu_commands);
                                rendered_images.pop();
                                available_img_queue.enqueue(first_image);
                        }
                        else if (result == vk::Result::eTimeout){
                                break;
                        }
                        else {
                                throw VulkanError{"Waiting for fence failed."};
                        }
                }
        }

        if(free_frame_resources.empty()){
                return false;
        }
        auto& resources = *free_frame_resources.back();
        free_frame_resources.pop_back();

        TransferImageImpl* transfer_image_ptr = nullptr;
        bool dequeued = filled_img_queue.wait_dequeue_timed(transfer_image_ptr, waiting_time_for_filled_image);
        if (!dequeued || !transfer_image_ptr) {
                return false;
        }

        TransferImageImpl& transfer_image = *transfer_image_ptr;
        transfer_image.preprocess();

        std::unique_lock lock(device_mutex);
        if (transfer_image.get_description() != current_image_description) {
                reconfigure(transfer_image);
        }

        uint32_t swapchain_image_id = context.acquire_next_swapchain_image(resources.image_acquired_semaphore);
        int swapchain_recreation_attempt = 0;
        while (swapchain_image_id == swapchain_image_out_of_date || swapchain_image_id == swapchain_image_timeout) 
        {
                swapchain_recreation_attempt++;
                if (swapchain_recreation_attempt > 3) {
                        throw VulkanError{"Cannot acquire swapchain image"};
                }
                
                auto window_parameters = window->get_window_parameters();
                if (window_parameters.is_minimized()) {
                        discard_filled_image(filled_img_queue, available_img_queue);
                        return false;
                }
                context.recreate_swapchain(window_parameters, render_pipeline.get_render_pass());
                render_pipeline.update_render_area( { window_parameters.width, window_parameters.height },
                        current_image_description.size);
                
                swapchain_image_id = context.acquire_next_swapchain_image(resources.image_acquired_semaphore);
        }

        bind_transfer_image(transfer_image, resources);
        lock.unlock();

        record_graphics_commands(resources, transfer_image, swapchain_image_id);
        std::vector<vk::PipelineStageFlags> wait_masks{ vk::PipelineStageFlagBits::eColorAttachmentOutput };
        vk::SubmitInfo submit_info{};
        submit_info
                .setCommandBufferCount(1)
                .setPCommandBuffers(&resources.command_buffer)
                .setPWaitDstStageMask(wait_masks.data())
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&resources.image_acquired_semaphore)
                .setSignalSemaphoreCount(1)
                .setPSignalSemaphores(&resources.image_rendered_semaphore);

        context.get_queue().submit(submit_info, transfer_image.is_available_fence);

        auto swapchain = context.get_swapchain();
        vk::PresentInfoKHR present_info{};
        present_info
                .setPImageIndices(&swapchain_image_id)
                .setSwapchainCount(1)
                .setPSwapchains(&swapchain)
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&resources.image_rendered_semaphore);

        auto present_result = context.get_queue().presentKHR(&present_info);

        switch (present_result) {
                case vk::Result::eSuccess:
                        break;
                // skip recoverable errors, othervise return/throw error 
                case vk::Result::eErrorOutOfDateKHR: 
                case vk::Result::eSuboptimalKHR: 
                        break;
                default: 
                        throw VulkanError{"Error presenting image:"s + vk::to_string(present_result)};
        }
        
        rendered_images.emplace(RenderedImage{&transfer_image, &resources});
        return true;
}

void VulkanDisplay::window_parameters_changed(WindowParameters new_parameters) {
        if (new_parameters != context.get_window_parameters() && !new_parameters.is_minimized()) {
                std::scoped_lock lock{device_mutex};
                context.recreate_swapchain(new_parameters, render_pipeline.get_render_pass());
                render_pipeline.update_render_area({ new_parameters.width, new_parameters.height },
                        current_image_description.size);
        }
}

} //namespace vulkan_display
