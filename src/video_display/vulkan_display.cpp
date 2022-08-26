/**
 * @file   video_display/vulkan_display.cpp
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

vk::ShaderModule create_shader(

        const std::filesystem::path& file_path,
        const vk::Device& device)
{
        std::ifstream file(file_path, std::ios::binary);
        if(!file.is_open()){
                throw vulkan_display_exception{"Failed to open file:"s + file_path.string()};
        }
        auto size = std::filesystem::file_size(file_path);
        assert(size % 4 == 0);
        std::vector<std::uint32_t> shader_code(size / 4);
        file.read(reinterpret_cast<char*>(shader_code.data()), static_cast<std::streamsize>(size));
        
        if(!file.good()){
                throw vulkan_display_exception{"Error reading from file:"s + file_path.string()};
        }

        vk::ShaderModuleCreateInfo shader_info;
        shader_info
                .setCodeSize(shader_code.size() * 4)
                .setPCode(shader_code.data());
        return device.createShaderModule(shader_info);
}

void update_render_area_viewport_scissor(render_area& render_area, vk::Viewport& viewport, vk::Rect2D& scissor, 
        vk::Extent2D window_size, vk::Extent2D transfer_image_size) {

        double wnd_aspect = static_cast<double>(window_size.width) / window_size.height;
        double img_aspect = static_cast<double>(transfer_image_size.width) / transfer_image_size.height;

        if (wnd_aspect > img_aspect) {
                render_area.height = window_size.height;
                render_area.width = static_cast<uint32_t>(std::round(window_size.height * img_aspect));
                render_area.x = (window_size.width - render_area.width) / 2;
                render_area.y = 0;
        } else {
                render_area.width = window_size.width;
                render_area.height = static_cast<uint32_t>(std::round(window_size.width / img_aspect));
                render_area.x = 0;
                render_area.y = (window_size.height - render_area.height) / 2;
        }

        viewport
                .setX(static_cast<float>(render_area.x))
                .setY(static_cast<float>(render_area.y))
                .setWidth(static_cast<float>(render_area.width))
                .setHeight(static_cast<float>(render_area.height))
                .setMinDepth(0.f)
                .setMaxDepth(1.f);
        scissor
                .setOffset({ static_cast<int32_t>(render_area.x), static_cast<int32_t>(render_area.y) })
                .setExtent({ render_area.width, render_area.height });
}

[[nodiscard]] transfer_image& acquire_transfer_image(std::vector<transfer_image*>& available_images,
        concurrent_queue<transfer_image*>& available_img_queue) 
{
        transfer_image* result = nullptr;
        if (!available_images.empty()){
                result = available_images.back();
                available_images.pop_back();
        }
        else{
                available_img_queue.wait_dequeue(result);
        }
        return *result;
}

vk::PresentModeKHR get_present_mode(bool vsync_enabled, bool tearing_permitted){
        using e = vk::PresentModeKHR;
        if (vsync_enabled){
                return tearing_permitted ? e::eFifoRelaxed : e::eFifo;
        }
        return tearing_permitted ? e::eImmediate : e::eMailbox;
}

void discard_filled_image(concurrent_circular_buffer<transfer_image*>& filled_img_queue, 
        concurrent_queue<transfer_image*>& available_img_queue)
{
        transfer_image* transfer_image = nullptr;
        bool dequeued = filled_img_queue.try_dequeue(transfer_image);
        if (dequeued && transfer_image) {
                available_img_queue.enqueue(transfer_image);
        }
}

vk::CommandPool create_command_pool(vk::Device device, uint32_t queue_family_index) {
        vk::CommandPoolCreateInfo pool_info{};
        using bits = vk::CommandPoolCreateFlagBits;
        pool_info
                .setQueueFamilyIndex(queue_family_index)
                .setFlags(bits::eTransient | bits::eResetCommandBuffer);
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

vk::RenderPass create_render_pass(vk::Device device, vk::Format swapchain_image_format) {
        vk::RenderPassCreateInfo render_pass_info;

        vk::AttachmentDescription color_attachment;
        color_attachment
                .setFormat(swapchain_image_format)
                .setSamples(vk::SampleCountFlagBits::e1)
                .setLoadOp(vk::AttachmentLoadOp::eClear)
                .setStoreOp(vk::AttachmentStoreOp::eStore)
                .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
                .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                .setInitialLayout(vk::ImageLayout::eUndefined)
                .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
        render_pass_info
                .setAttachmentCount(1)
                .setPAttachments(&color_attachment);

        vk::AttachmentReference attachment_reference;
        attachment_reference
                .setAttachment(0)
                .setLayout(vk::ImageLayout::eColorAttachmentOptimal);
        vk::SubpassDescription subpass;
        subpass
                .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
                .setColorAttachmentCount(1)
                .setPColorAttachments(&attachment_reference);
        render_pass_info
                .setSubpassCount(1)
                .setPSubpasses(&subpass);

        vk::SubpassDependency subpass_dependency{};
        subpass_dependency
                .setSrcSubpass(VK_SUBPASS_EXTERNAL)
                .setDstSubpass(0)
                .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
                .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
                .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite);
        render_pass_info
                .setDependencyCount(1)
                .setPDependencies(&subpass_dependency);

        return device.createRenderPass(render_pass_info);
}

vk::DescriptorPool create_descriptor_pool(vk::Device device, size_t descriptor_count){
        vk::DescriptorPoolSize descriptor_sizes{};
        descriptor_sizes
                .setType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(descriptor_count);
        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info
                .setPoolSizeCount(1)
                .setPPoolSizes(&descriptor_sizes)
                .setMaxSets(descriptor_count);
        return device.createDescriptorPool(pool_info);
}

vk::DescriptorSetLayout create_descriptor_set_layout(vk::Device device, vk::Sampler sampler) {
        vk::DescriptorSetLayoutBinding descriptor_set_layout_bindings;
        descriptor_set_layout_bindings
                .setBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setStageFlags(vk::ShaderStageFlagBits::eFragment)
                .setPImmutableSamplers(&sampler);

        vk::DescriptorSetLayoutCreateInfo descriptor_set_layout_info{};
        descriptor_set_layout_info
                .setBindingCount(1)
                .setPBindings(&descriptor_set_layout_bindings);

        return device.createDescriptorSetLayout(descriptor_set_layout_info);
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

vk::Sampler create_texture_sampler(vk::Device device, vk::SamplerYcbcrConversion yCbCr_conversion) {
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

vk::PipelineLayout create_pipeline_layout(vk::Device device, vk::DescriptorSetLayout descriptor_set_layout){
        vk::PipelineLayoutCreateInfo pipeline_layout_info{};

        vk::PushConstantRange push_constants;
        push_constants
                .setOffset(0)
                .setSize(sizeof(render_area))
                .setStageFlags(vk::ShaderStageFlagBits::eFragment);
        pipeline_layout_info
                .setPushConstantRangeCount(1)
                .setPPushConstantRanges(&push_constants)
                .setSetLayoutCount(1)
                .setPSetLayouts(&descriptor_set_layout);
        return device.createPipelineLayout(pipeline_layout_info);
}

vk::Pipeline create_graphics_pipeline(vk::Device device, vk::PipelineLayout pipeline_layout, vk::RenderPass render_pass,    
        vk::ShaderModule vertex_shader, vk::ShaderModule fragment_shader) 
{
        vk::GraphicsPipelineCreateInfo pipeline_info{};

        std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages_infos;
        shader_stages_infos[0]
                .setModule(vertex_shader)
                .setPName("main")
                .setStage(vk::ShaderStageFlagBits::eVertex);
        shader_stages_infos[1]
                .setModule(fragment_shader)
                .setPName("main")
                .setStage(vk::ShaderStageFlagBits::eFragment);
        pipeline_info
                .setStageCount(static_cast<uint32_t>(shader_stages_infos.size()))
                .setPStages(shader_stages_infos.data());

        vk::PipelineVertexInputStateCreateInfo vertex_input_state_info{};
        pipeline_info.setPVertexInputState(&vertex_input_state_info);

        vk::PipelineInputAssemblyStateCreateInfo input_assembly_state_info{};
        input_assembly_state_info.setTopology(vk::PrimitiveTopology::eTriangleList);
        pipeline_info.setPInputAssemblyState(&input_assembly_state_info);

        vk::PipelineViewportStateCreateInfo viewport_state_info;
        viewport_state_info
                .setScissorCount(1)
                .setViewportCount(1);
        pipeline_info.setPViewportState(&viewport_state_info);

        vk::PipelineRasterizationStateCreateInfo rasterization_info{};
        rasterization_info
                .setPolygonMode(vk::PolygonMode::eFill)
                .setLineWidth(1.f);
        pipeline_info.setPRasterizationState(&rasterization_info);

        vk::PipelineMultisampleStateCreateInfo multisample_info;
        multisample_info
                .setSampleShadingEnable(false)
                .setRasterizationSamples(vk::SampleCountFlagBits::e1);
        pipeline_info.setPMultisampleState(&multisample_info);

        using color_flags = vk::ColorComponentFlagBits;
        vk::PipelineColorBlendAttachmentState color_blend_attachment{};
        color_blend_attachment
                .setBlendEnable(false)
                .setColorWriteMask(color_flags::eR | color_flags::eG | color_flags::eB | color_flags::eA);
        vk::PipelineColorBlendStateCreateInfo color_blend_info{};
        color_blend_info
                .setAttachmentCount(1)
                .setPAttachments(&color_blend_attachment);
        pipeline_info.setPColorBlendState(&color_blend_info);

        std::array dynamic_states{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic_state_info{};
        dynamic_state_info
                .setDynamicStateCount(static_cast<uint32_t>(dynamic_states.size()))
                .setPDynamicStates(dynamic_states.data());
        pipeline_info.setPDynamicState(&dynamic_state_info);

        pipeline_info
                .setLayout(pipeline_layout)
                .setRenderPass(render_pass);

        vk::Pipeline pipeline;
        auto result = device.createGraphicsPipelines(nullptr, 1, &pipeline_info, nullptr, &pipeline);
        if(result != vk::Result::eSuccess){
                throw vulkan_display_exception{"Pipeline cannot be created."};
        }
        return pipeline;
}

} //namespace -------------------------------------------------------------


namespace vulkan_display {

void vulkan_display::init(vulkan_instance&& instance, VkSurfaceKHR surface, uint32_t initial_image_count,
        window_changed_callback& window, uint32_t gpu_index, std::filesystem::path path_to_shaders, bool vsync, bool tearing_permitted) {
        // Order of following calls is important
        assert(surface);
        this->window = &window;
        auto window_parameters = window.get_window_parameters();
        
        context.init(std::move(instance), surface, window_parameters, gpu_index, get_present_mode(vsync, tearing_permitted));
        device = context.get_device();
        
        command_pool = create_command_pool(device, context.get_queue_familt_index());
        descriptor_pool = create_descriptor_pool(device, gpu_commands.size());
        vertex_shader = create_shader(path_to_shaders / "vert.spv", device);
        fragment_shader = create_shader(path_to_shaders / "frag.spv", device);
        render_pass = create_render_pass(device, context.get_swapchain_image_format());
        
        vk::ClearColorValue clear_color_value{};
        clear_color_value.setFloat32({ 0.01f, 0.01f, 0.01f, 1.0f });
        clear_color.setColor(clear_color_value);

        context.create_framebuffers(render_pass);

        available_images.reserve(initial_image_count);
        for (uint32_t i = 0; i < initial_image_count; i++) {
                transfer_images.emplace_back(device, i);
                available_images.push_back(&transfer_images.back());
        }

        auto command_buffers = create_command_buffers(device, command_pool, gpu_commands.size());
        for (size_t i = 0; i < gpu_commands.size(); i++){
                auto& commands = gpu_commands[i];
                commands.image_acquired = create_semaphore(device);
                commands.image_rendered = create_semaphore(device);
                commands.command_buffer = command_buffers[i];
        }

        free_gpu_commands.reserve(gpu_commands.size());
        for(auto& command: gpu_commands){
                free_gpu_commands.emplace_back(&command);
        }
}

void vulkan_display::destroy() {
        if (!destroyed) {
                destroyed = true;
                if (device) {
                        device.waitIdle();
                        device.destroy(descriptor_pool);

                        for (auto& image : transfer_images) {
                                image.destroy(device);
                        }
                        device.destroy(command_pool);
                        device.destroy(render_pass);
                        device.destroy(fragment_shader);
                        device.destroy(vertex_shader);
                        for (auto& commands : gpu_commands) {
                                device.destroy(commands.image_acquired);
                                device.destroy(commands.image_rendered);
                        }
                        device.destroy(pipeline);
                        device.destroy(pipeline_layout);
                        device.destroy(descriptor_set_layout);
                        device.destroy(sampler);
                        if (yCbCr_conversion) {
                                device.destroy(yCbCr_conversion);
                        }
                }
                context.destroy();
        }
}

bool vulkan_display::is_image_description_supported(image_description description) {
        if (!is_yCbCr_supported() && is_yCbCr_format(description.format)) {
                return false;
        }
        std::scoped_lock lock(device_mutex);
        return transfer_image::is_image_description_supported(context.get_gpu(), description);
}

void vulkan_display::record_graphics_commands(detail::gpu_commands& commands, 
        transfer_image& transfer_image, uint32_t swapchain_image_id) 
{
        vk::CommandBuffer cmd_buffer = commands.command_buffer;
        cmd_buffer.reset(vk::CommandBufferResetFlags{});

        vk::CommandBufferBeginInfo begin_info{};
        begin_info.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
        cmd_buffer.begin(begin_info);

        auto render_begin_memory_barrier = transfer_image.create_memory_barrier(
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eShaderRead);
        cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eFragmentShader,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, render_begin_memory_barrier);

        vk::RenderPassBeginInfo render_pass_begin_info;
        render_pass_begin_info
                .setRenderPass(render_pass)
                .setRenderArea(vk::Rect2D{ {0,0}, context.get_window_size() })
                .setClearValueCount(1)
                .setPClearValues(&clear_color)
                .setFramebuffer(context.get_framebuffer(swapchain_image_id));
        cmd_buffer.beginRenderPass(render_pass_begin_info, vk::SubpassContents::eInline);

        cmd_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

        cmd_buffer.setScissor(0, scissor);
        cmd_buffer.setViewport(0, viewport);
        cmd_buffer.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(render_area), &render_area);
        cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                pipeline_layout, 0, commands.descriptor_set, nullptr);
        cmd_buffer.draw(6, 1, 0, 0);

        cmd_buffer.endRenderPass();

        auto render_end_memory_barrier = transfer_image.create_memory_barrier(
                vk::ImageLayout::eGeneral, vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eHostRead);
        cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eHost,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, render_end_memory_barrier);

        cmd_buffer.end();
}

image vulkan_display::acquire_image(image_description description) {
        assert(description.size.width * description.size.height != 0);
        assert(description.format != vk::Format::eUndefined);
        if (!context.is_yCbCr_supported()) {
                if (is_yCbCr_format(description.format)) {
                        std::string error_msg{ "YCbCr formats are not supported."sv };
                        if (get_vulkan_version() == VK_API_VERSION_1_0) {
                                error_msg.append("\nVulkan 1.1 or higher is needed for YCbCr support."sv);
                        }
                        throw vulkan_display_exception{error_msg};
                }
        }
        transfer_image& transfer_image = acquire_transfer_image(available_images, available_img_queue);
        assert(transfer_image.get_id() != transfer_image::NO_ID);

        if (transfer_image.get_description() != description) {
                std::scoped_lock device_lock(device_mutex);
                transfer_image.create(device, context.get_gpu(), description);
        }
        
        return image{ transfer_image };
}

void vulkan_display::copy_and_queue_image(std::byte* frame, image_description description) {
        image image = acquire_image(description);
        memcpy(image.get_memory_ptr(), frame, image.get_size().height * image.get_row_pitch());
        queue_image(image, false);
}

bool vulkan_display::queue_image(image image, bool discardable) {   
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

bool vulkan_display::display_queued_image() {
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
                                free_gpu_commands.push_back(rendered_images.front().gpu_commands);
                                rendered_images.pop();
                                available_img_queue.enqueue(first_image);
                        }
                        else if (result == vk::Result::eTimeout){
                                break;
                        }
                        else {
                                throw vulkan_display_exception{"Waiting for fence failed."};
                        }
                }
        }

        if(free_gpu_commands.empty()){
                assert(false);
                return false;
        }
        auto& commands = *free_gpu_commands.back();
        free_gpu_commands.pop_back();

        transfer_image* transfer_image_ptr = nullptr;
        bool dequeued = filled_img_queue.wait_dequeue_timed(transfer_image_ptr, waiting_time_for_filled_image);
        if (!dequeued || !transfer_image_ptr) {
                return false;
        }

        transfer_image& transfer_image = *transfer_image_ptr;
        transfer_image.preprocess();

        std::unique_lock lock(device_mutex);
        if (transfer_image.get_description() != current_image_description) {
                auto image_format = transfer_image.get_description().format;
                if (image_format != current_image_description.format) {
                        log_msg("Recreating pipeline");
                        context.get_queue().waitIdle();
                        device.resetDescriptorPool(descriptor_pool);
                        device.destroy(pipeline);
                        device.destroy(pipeline_layout);
                        device.destroy(descriptor_set_layout);
                        device.destroy(sampler);
                        device.destroy(yCbCr_conversion);

                        yCbCr_conversion = createYCbCrConversion(device, image_format);
                        sampler = create_texture_sampler(device, yCbCr_conversion);
                        descriptor_set_layout = create_descriptor_set_layout(device, sampler);
                        pipeline_layout = create_pipeline_layout(device, descriptor_set_layout);
                        pipeline = create_graphics_pipeline(device, pipeline_layout, render_pass, vertex_shader, fragment_shader);
                        auto descriptor_sets = allocate_description_sets(device, descriptor_pool, 
                                descriptor_set_layout, gpu_commands.size());
                        for(size_t i = 0; i < gpu_commands.size(); i++){
                                gpu_commands[i].descriptor_set = descriptor_sets[i];
                        }
                }
                current_image_description = transfer_image.get_description();
                auto parameters = context.get_window_parameters();
                update_render_area_viewport_scissor(render_area, viewport, scissor,
                        { parameters.width, parameters.height }, current_image_description.size);
        }

        uint32_t swapchain_image_id = context.acquire_next_swapchain_image(commands.image_acquired);
        int swapchain_recreation_attempt = 0;
        while (swapchain_image_id == swapchain_image_out_of_date || swapchain_image_id == swapchain_image_timeout) 
        {
                swapchain_recreation_attempt++;
                if (swapchain_recreation_attempt > 3) {
                        throw vulkan_display_exception{"Cannot acquire swapchain image"}; 
                }
                
                auto window_parameters = window->get_window_parameters();
                if (window_parameters.is_minimized()) {
                        discard_filled_image(filled_img_queue, available_img_queue);
                        return false;
                }
                context.recreate_swapchain(window_parameters, render_pass);
                update_render_area_viewport_scissor(
                        render_area, viewport, scissor,
                        { window_parameters.width, window_parameters.height },
                        current_image_description.size);
                
                swapchain_image_id = context.acquire_next_swapchain_image(commands.image_acquired);
        }
        transfer_image.prepare_for_rendering(device, 
                commands.descriptor_set, sampler, yCbCr_conversion);
        lock.unlock();

        record_graphics_commands(commands, transfer_image, swapchain_image_id);
        std::vector<vk::PipelineStageFlags> wait_masks{ vk::PipelineStageFlagBits::eColorAttachmentOutput };
        vk::SubmitInfo submit_info{};
        submit_info
                .setCommandBufferCount(1)
                .setPCommandBuffers(&commands.command_buffer)
                .setPWaitDstStageMask(wait_masks.data())
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&commands.image_acquired)
                .setSignalSemaphoreCount(1)
                .setPSignalSemaphores(&commands.image_rendered);

        context.get_queue().submit(submit_info, transfer_image.is_available_fence);

        auto swapchain = context.get_swapchain();
        vk::PresentInfoKHR present_info{};
        present_info
                .setPImageIndices(&swapchain_image_id)
                .setSwapchainCount(1)
                .setPSwapchains(&swapchain)
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&commands.image_rendered);

        auto present_result = context.get_queue().presentKHR(&present_info);

        switch (present_result) {
                case vk::Result::eSuccess:
                        break;
                // skip recoverable errors, othervise return/throw error 
                case vk::Result::eErrorOutOfDateKHR: 
                case vk::Result::eSuboptimalKHR: 
                        break;  
                default: 
                        throw vulkan_display_exception{"Error presenting image:"s + vk::to_string(present_result)};
        }
        
        rendered_images.emplace(rendered_image{&transfer_image, &commands});
        return true;
}

void vulkan_display::window_parameters_changed(window_parameters new_parameters) {
        if (new_parameters != context.get_window_parameters() && !new_parameters.is_minimized()) {
                std::scoped_lock lock{device_mutex};
                context.recreate_swapchain(new_parameters, render_pass);
                update_render_area_viewport_scissor(render_area, viewport, scissor,
                        { new_parameters.width, new_parameters.height }, current_image_description.size);
        }
}

} //namespace vulkan_display
