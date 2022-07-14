/**
 * @file   video_display/vulkan_context.cpp
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

#include "vulkan_context.hpp"
#include <cassert>
#include <iostream>

using namespace vulkan_display_detail;

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        [[maybe_unused]] VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        [[maybe_unused]] void* pUserData)
{
        log_msg("validation layer: "s + pCallbackData->pMessage);
        return VK_FALSE;
}

void check_validation_layers(const std::vector<c_str>& required_layers) {
        std::vector<vk::LayerProperties>  layers = vk::enumerateInstanceLayerProperties();
        //for (auto& l : layers) puts(l.layerName);

        for (const auto& req_layer : required_layers) {
                auto layer_equals = [req_layer](auto layer) { return strcmp(req_layer, layer.layerName) == 0; };
                bool found = std::any_of(layers.begin(), layers.end(), layer_equals);
                VKD_CHECK(found, "Layer "s + req_layer + " is not supported.");
        }
        return void();
}

void check_instance_extensions(const std::vector<c_str>& required_extensions) {
        std::vector<vk::ExtensionProperties> extensions = vk::enumerateInstanceExtensionProperties(nullptr);

        for (const auto& req_exten : required_extensions) {
                auto extension_equals = [req_exten](auto exten) { return strcmp(req_exten, exten.extensionName) == 0; };
                bool found = std::any_of(extensions.begin(), extensions.end(), extension_equals);
                VKD_CHECK(found, "Instance extension "s + req_exten + " is not supported.");
        }
        return void();
}


void check_device_extensions(bool& result, bool propagate_error,
        const std::vector<c_str>& required_extensions, const vk::PhysicalDevice& device)
{
        std::vector<vk::ExtensionProperties> extensions = device.enumerateDeviceExtensionProperties(nullptr);

        for (const auto& req_exten : required_extensions) {
                auto extension_equals = [req_exten](auto exten) { return strcmp(req_exten, exten.extensionName) == 0; };
                bool found = std::any_of(extensions.begin(), extensions.end(), extension_equals);
                if (!found) {
                        result = false;
                        if (propagate_error) {
                                VKD_CHECK(false, "Device extension "s + req_exten + " is not supported.");
                        }
                        return void();
                }
        }
        result = true;
        return void();
}

void get_queue_family_index(uint32_t& index, vk::PhysicalDevice gpu, vk::SurfaceKHR surface) {
        assert(gpu);

        std::vector<vk::QueueFamilyProperties> families = gpu.getQueueFamilyProperties();

        index = no_queue_index_found;
        for (uint32_t i = 0; i < families.size(); i++) {
                VkBool32 surface_supported = true;
                if (surface) {
                        surface_supported = gpu.getSurfaceSupportKHR(i, surface);
                }

                if (surface_supported && (families[i].queueFlags & vk::QueueFlagBits::eGraphics)) {
                        index = i;
                        break;
                }
        }
        return void();
}

const std::vector required_gpu_extensions = { "VK_KHR_swapchain" };

void is_gpu_suitable(bool& result, bool propagate_error, vk::PhysicalDevice gpu, vk::SurfaceKHR surface = nullptr) {
        check_device_extensions(result, propagate_error, required_gpu_extensions, gpu);
        if (!result) {
                return void();
        }
        uint32_t index = no_queue_index_found;
        get_queue_family_index(index, gpu, surface);
        if (index == no_queue_index_found) {
                result = false;
        }
        return void();
}

void choose_suitable_GPU(vk::PhysicalDevice& suitable_gpu, const std::vector<vk::PhysicalDevice>& gpus, vk::SurfaceKHR surface) {
        assert(surface);
        bool is_suitable = false;
        for (const auto& gpu : gpus) {
                auto properties = gpu.getProperties();
                if (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
                        is_gpu_suitable(is_suitable, false, gpu, surface);
                        if (is_suitable) {
                                suitable_gpu = gpu;
                                return void();
                        }
                }
        }

        for (const auto& gpu : gpus) {
                auto properties = gpu.getProperties();
                if (properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu) {
                        is_gpu_suitable(is_suitable, false, gpu, surface);
                        if (is_suitable) {
                                suitable_gpu = gpu;
                                return void();
                        }
                }
        }

        for (const auto& gpu : gpus) {
                is_gpu_suitable(is_suitable, false, gpu, surface);
                if (is_suitable) {
                        suitable_gpu = gpu;
                        return void();
                }
        }

        VKD_CHECK(false, "No suitable gpu found.");
        return void();
}

void choose_gpu_by_index(vk::PhysicalDevice& gpu, std::vector<vk::PhysicalDevice>& gpus, uint32_t gpu_index) {
        VKD_CHECK(gpu_index < gpus.size(), "GPU index is not valid.");
        std::vector<std::pair<std::string, vk::PhysicalDevice>> gpu_names;
        gpu_names.reserve(gpus.size());

        auto get_gpu_name = [](auto gpu) -> std::pair<std::string, vk::PhysicalDevice> {
                return { gpu.getProperties().deviceName, gpu };
        };

        std::transform(gpus.begin(), gpus.end(), std::back_inserter(gpu_names), get_gpu_name);

        std::sort(gpu_names.begin(), gpu_names.end());
        gpu = gpu_names[gpu_index].second;
        return void();
}

vk::CompositeAlphaFlagBitsKHR get_composite_alpha(vk::CompositeAlphaFlagsKHR capabilities) {
        uint32_t result = 1;
        while (!(result & static_cast<uint32_t>(capabilities))) {
                result <<= 1u;
        }
        return static_cast<vk::CompositeAlphaFlagBitsKHR>(result);
}

} //namespace ------------------------------------------------------------------------


namespace vulkan_display {

void vulkan_instance::init(std::vector<c_str>& required_extensions, bool enable_validation, std::function<void(std::string_view sv)> logging_function) {
        log_msg = std::move(logging_function);
        std::vector<c_str> validation_layers{};
        if (enable_validation) {
                validation_layers.push_back("VK_LAYER_KHRONOS_validation");
                check_validation_layers(validation_layers);

                const char* debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                required_extensions.push_back(debug_extension);
        }

        check_instance_extensions(required_extensions);

        vk::ApplicationInfo app_info{};
        app_info.setApiVersion(VK_API_VERSION_1_1);
        vulkan_version = VK_API_VERSION_1_1;

        vk::InstanceCreateInfo instance_info{};
        instance_info
                .setPApplicationInfo(&app_info)
                .setEnabledLayerCount(static_cast<uint32_t>(validation_layers.size()))
                .setPpEnabledLayerNames(validation_layers.data())
                .setEnabledExtensionCount(static_cast<uint32_t>(required_extensions.size()))
                .setPpEnabledExtensionNames(required_extensions.data());
        auto result = vk::createInstance(&instance_info, nullptr, &instance);
        if (result == vk::Result::eErrorIncompatibleDriver) {
                app_info.apiVersion = VK_API_VERSION_1_0;
                vulkan_version = VK_API_VERSION_1_0;
                result = vk::createInstance(&instance_info, nullptr, &instance);
        }
        VKD_CHECK(result, "Vulkan instance cannot be created: "s + vk::to_string(result));

        if (enable_validation) {
                dynamic_dispatcher = std::make_unique<vk::DispatchLoaderDynamic>(instance, vkGetInstanceProcAddr);
                init_validation_layers_error_messenger();
        }

        return void();
}

void vulkan_instance::init_validation_layers_error_messenger() {
        vk::DebugUtilsMessengerCreateInfoEXT messenger_info{};
        using severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
        using type = vk::DebugUtilsMessageTypeFlagBitsEXT;
        messenger_info
                .setMessageSeverity(severity::eError | severity::eInfo | severity::eWarning) // severity::eInfo |
                .setMessageType(type::eGeneral | type::ePerformance | type::eValidation)
                .setPfnUserCallback(debugCallback)
                .setPUserData(nullptr);
        messenger = instance.createDebugUtilsMessengerEXT(messenger_info, nullptr, *dynamic_dispatcher);
        return void();
}

void vulkan_instance::get_available_gpus(std::vector<std::pair<std::string, bool>>& gpus) {
        assert(instance);

        std::vector<vk::PhysicalDevice> physical_devices = instance.enumeratePhysicalDevices();
        gpus.clear();
        gpus.reserve(physical_devices.size());
        for (const auto& gpu : physical_devices) {
                auto properties = gpu.getProperties();
                gpus.emplace_back(properties.deviceName, true);
        }
        std::sort(gpus.begin(), gpus.end());
        return void();
}

void vulkan_instance::destroy() {
        if (instance) {
                instance.destroy();
                if (messenger) {
                        instance.destroy(messenger, nullptr, *dynamic_dispatcher);
                }
                dynamic_dispatcher = nullptr;
                instance = nullptr;
        }
        return void();
}


} // namespace vulkan_display ----------------------------------------------------------------------------


namespace vulkan_display_detail { //------------------------------------------------------------------------

void vulkan_context::create_physical_device(uint32_t gpu_index) {
        assert(instance);
        assert(surface);
        std::vector<vk::PhysicalDevice> gpus = instance.enumeratePhysicalDevices();

        if (gpu_index == vulkan_display::no_gpu_selected) {
                choose_suitable_GPU(gpu, gpus, surface);
        } else {
                choose_gpu_by_index(gpu, gpus, gpu_index);
                bool suitable = false;
                is_gpu_suitable(suitable, true, gpu, surface);
        }
        auto properties = gpu.getProperties();
        if (properties.apiVersion < VK_API_VERSION_1_1) {
                vulkan_version = VK_API_VERSION_1_0;
        }
        std::string device_name = properties.deviceName;
        log_msg("Vulkan uses GPU called: "s + device_name);
        std::string msg; //todo C++20 use std::format
        msg.reserve(20);
        msg += ("Used Vulkan API: ");
        msg += std::to_string(VK_VERSION_MAJOR(vulkan_version));
        msg += ".";
        msg += std::to_string(VK_VERSION_MINOR(vulkan_version));
        log_msg(msg);
        return void();
}

void vulkan_context::create_logical_device() {
        assert(gpu);
        assert(queue_family_index != no_queue_index_found);

        constexpr std::array priorities = { 1.0f };
        vk::DeviceQueueCreateInfo queue_info{};
        queue_info
                .setQueueFamilyIndex(queue_family_index)
                .setPQueuePriorities(priorities.data())
                .setQueueCount(1);

        vk::DeviceCreateInfo device_info{};
        device_info
                .setPNext(nullptr)
                .setQueueCreateInfoCount(1)
                .setPQueueCreateInfos(&queue_info)
                .setEnabledExtensionCount(static_cast<uint32_t>(required_gpu_extensions.size()))
                .setPpEnabledExtensionNames(required_gpu_extensions.data());

        vk::PhysicalDeviceFeatures2 features2{};
        vk::PhysicalDeviceSamplerYcbcrConversionFeatures yCbCr_feature{};
        if (vulkan_version == VK_API_VERSION_1_1) {
                features2.setPNext(&yCbCr_feature);
                gpu.getFeatures2(&features2);
                if (yCbCr_feature.samplerYcbcrConversion) {
                        yCbCr_supported = true;
                        device_info.setPNext(&features2);
                        log_msg("yCbCr feature supported.");
                }
        }

        device = gpu.createDevice(device_info);
        return void();

}

void vulkan_context::get_present_mode() {
        std::vector<vk::PresentModeKHR> modes = gpu.getSurfacePresentModesKHR(surface);

        vk::PresentModeKHR preferred = preferred_present_mode;
        if (std::any_of(modes.begin(), modes.end(), [preferred](auto mode) { return mode == preferred; })) {
                swapchain_atributes.mode = preferred;
                return void();
        }
        
        // Mailbox is alternative to Immediate, Fifo to everything else
        auto alternative = (preferred == vk::PresentModeKHR::eImmediate 
                ? vk::PresentModeKHR::eMailbox
                : vk::PresentModeKHR::eFifo);

        if (std::any_of(modes.begin(), modes.end(), [alternative](auto mode) { return mode == alternative; })) {
                swapchain_atributes.mode = alternative;
                return void();
        }

        swapchain_atributes.mode = modes[0];
        return void();
}

void vulkan_context::get_surface_format() {
        std::vector<vk::SurfaceFormatKHR> formats = gpu.getSurfaceFormatsKHR(surface);

        vk::SurfaceFormatKHR default_format{};
        default_format.format = vk::Format::eB8G8R8A8Srgb;
        default_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;

        if (std::any_of(formats.begin(), formats.end(), [default_format](auto& format) {return format == default_format; })) {
                swapchain_atributes.format = default_format;
                return void();
        }
        swapchain_atributes.format = formats[0];
        return void();
}

void vulkan_context::create_swap_chain(vk::SwapchainKHR old_swapchain) {
        auto& capabilities = swapchain_atributes.capabilities;
        capabilities = gpu.getSurfaceCapabilitiesKHR(surface);

        get_present_mode();
        get_surface_format();

        window_size.width = std::clamp(window_size.width,
                capabilities.minImageExtent.width,
                capabilities.maxImageExtent.width);
        window_size.height = std::clamp(window_size.height,
                capabilities.minImageExtent.height,
                capabilities.maxImageExtent.height);

        uint32_t image_count = std::max(uint32_t{2}, capabilities.minImageCount);
        if (capabilities.maxImageCount != 0) {
                image_count = std::min(image_count, capabilities.maxImageCount);
        }

        //assert(capabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferDst);
        vk::SwapchainCreateInfoKHR swapchain_info{};
        swapchain_info
                .setSurface(surface)
                .setImageFormat(swapchain_atributes.format.format)
                .setImageColorSpace(swapchain_atributes.format.colorSpace)
                .setPresentMode(swapchain_atributes.mode)
                .setMinImageCount(image_count)
                .setImageExtent(window_size)
                .setImageArrayLayers(1)
                .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
                .setImageSharingMode(vk::SharingMode::eExclusive)
                .setPreTransform(swapchain_atributes.capabilities.currentTransform)
                .setCompositeAlpha(get_composite_alpha(swapchain_atributes.capabilities.supportedCompositeAlpha))
                .setClipped(true)
                .setOldSwapchain(old_swapchain);
        swapchain = device.createSwapchainKHR(swapchain_info);
        return void();
}

void vulkan_context::create_swapchain_views() {
        std::vector<vk::Image> images = device.getSwapchainImagesKHR(swapchain);
        auto image_count = static_cast<uint32_t>(images.size());

        vk::ImageViewCreateInfo image_view_info = 
                vulkan_display::default_image_view_create_info(swapchain_atributes.format.format);

        swapchain_images.resize(image_count);
        for (uint32_t i = 0; i < image_count; i++) {
                swapchain_image& swapchain_image = swapchain_images[i];
                swapchain_image.image = images[i];

                image_view_info.setImage(swapchain_image.image);
                swapchain_image.view = device.createImageView(image_view_info);
        }
        return void();
}

void vulkan_context::init(vulkan_display::vulkan_instance&& instance, VkSurfaceKHR surface, 
        window_parameters parameters, uint32_t gpu_index, vk::PresentModeKHR preferredMode) 
{
        assert(!this->instance);
        this->instance = instance.instance;
        this->dynamic_dispatcher = std::move(instance.dynamic_dispatcher);
        this->messenger = instance.messenger;
        this->vulkan_version = instance.vulkan_version;
        instance.instance = nullptr;
        instance.messenger = nullptr;

        this->surface = surface;
        this->preferred_present_mode = preferredMode;
        window_size = vk::Extent2D{ parameters.width, parameters.height };

        create_physical_device(gpu_index);
        get_queue_family_index(queue_family_index, gpu, surface);
        create_logical_device();
        queue = device.getQueue(queue_family_index, 0);
        create_swap_chain();
        create_swapchain_views();
        return void();
}

void vulkan_context::create_framebuffers(vk::RenderPass render_pass) {
        vk::FramebufferCreateInfo framebuffer_info;
        framebuffer_info
                .setRenderPass(render_pass)
                .setWidth(window_size.width)
                .setHeight(window_size.height)
                .setLayers(1);

        for(auto& swapchain_image : swapchain_images){
                framebuffer_info
                        .setAttachmentCount(1)
                        .setPAttachments(&swapchain_image.view);
                swapchain_image.framebuffer = device.createFramebuffer(framebuffer_info);
        }
        return void();
}

void vulkan_context::recreate_swapchain(window_parameters parameters, vk::RenderPass render_pass) {
        window_size = vk::Extent2D{ parameters.width, parameters.height };
        
        log_msg("Recreating  swapchain");

        device.waitIdle();

        destroy_framebuffers();
        destroy_swapchain_views();
        vk::SwapchainKHR old_swap_chain = swapchain;
        create_swap_chain(old_swap_chain);
        device.destroySwapchainKHR(old_swap_chain);
        create_swapchain_views();
        create_framebuffers(render_pass);
        return void();
}

void vulkan_context::acquire_next_swapchain_image(uint32_t& image_index, vk::Semaphore acquire_semaphore) const {
        constexpr uint64_t timeout = 1'000'000'000; // 1s = 1 000 000 000 nanoseconds
        auto acquired = device.acquireNextImageKHR(swapchain, timeout, acquire_semaphore, nullptr, &image_index);
        switch (acquired) {
        case vk::Result::eSuboptimalKHR: [[fallthrough]];
        case vk::Result::eErrorOutOfDateKHR:
                image_index = swapchain_image_out_of_date;
                break;
        case vk::Result::eTimeout:
                image_index = swapchain_image_timeout;
                break;
        default:
                VKD_CHECK(acquired, "Next swapchain image cannot be acquired."s + vk::to_string(acquired));
        }
        return void();
}

void vulkan_context::destroy() {
        if (device) {
                // static_cast to silence nodiscard warning
                device.waitIdle();
                destroy_framebuffers();
                destroy_swapchain_views();
                device.destroy(swapchain);
                device.destroy();
        }
        if (instance) {
                instance.destroy(surface);
                if (messenger) {
                        instance.destroy(messenger, nullptr, *dynamic_dispatcher);
                }
                instance.destroy();
        }
        dynamic_dispatcher = nullptr;
        return void();
}


} //vulkan_display_detail

