/**
 * @file   video_display/vulkan_context.h
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

#ifdef VULKAN_DISPLAY_NO_EXCEPTIONS
#define VULKAN_HPP_VULKAN_DISPLAY_NO_EXCEPTIONS
#endif //VULKAN_DISPLAY_NO_EXCEPTIONS

#include <vulkan/vulkan.hpp>
static_assert(VK_HEADER_VERSION > 100); // minimum Vulkan SDK version is 1.1.101
//Newer versions can be downloaded from the official website:
//https://vulkan.lunarg.com/sdk/home

//remove leaking macros
#undef min
#undef max

#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <string>


struct  vulkan_display_exception : public std::runtime_error {
        explicit vulkan_display_exception(const std::string& msg) :
                std::runtime_error{ msg } { }
};

namespace vulkan_display {

struct window_parameters {
        uint32_t width;
        uint32_t height;

        constexpr bool operator==(const window_parameters& other) const {
                return width == other.width &&
                        height == other.height;
        }
        constexpr bool operator!=(const window_parameters& other) const {
                return !(*this == other);
        }
        constexpr bool is_minimized() const {
                return width * height == 0;
        }
};

constexpr uint32_t no_gpu_selected = UINT32_MAX;

class vulkan_instance;

} // namespace vulkan_display ---------------------------------------------


namespace vulkan_display_detail {

using c_str = const char*;
using namespace std::literals;

constexpr uint32_t no_queue_index_found = UINT32_MAX;
constexpr uint32_t swapchain_image_out_of_date = UINT32_MAX;
constexpr uint32_t swapchain_image_timeout = UINT32_MAX - 1;

inline std::function<void(std::string_view)> log_msg;

inline vk::ImageViewCreateInfo default_image_view_create_info(vk::Format format) {
        vk::ImageViewCreateInfo image_view_info{ {}, {}, vk::ImageViewType::e2D, format };
        image_view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        image_view_info.subresourceRange.levelCount = 1;
        image_view_info.subresourceRange.layerCount = 1;
        return image_view_info;
}

class vulkan_context {
        vk::Instance instance;
        std::unique_ptr<vk::DispatchLoaderDynamic> dynamic_dispatcher{};
        vk::DebugUtilsMessengerEXT messenger;
        uint32_t vulkan_version{};

        vk::PhysicalDevice gpu;
        vk::Device device;
        bool yCbCr_supported = false;

        uint32_t queue_family_index = no_queue_index_found;
        vk::Queue queue;

        vk::SurfaceKHR surface;
        vk::SwapchainKHR swapchain;
        struct {
                vk::SurfaceCapabilitiesKHR capabilities;
                vk::SurfaceFormatKHR format;
                vk::PresentModeKHR mode = vk::PresentModeKHR::eFifo;
        } swapchain_atributes;

        struct swapchain_image {
                vk::Image image;
                vk::ImageView view;
                vk::Framebuffer framebuffer;
        };
        std::vector<swapchain_image> swapchain_images{};

        vk::Extent2D window_size{ 0, 0 };
        vk::PresentModeKHR preferred_present_mode;
public:
        //getters
        uint32_t get_vulkan_version() const { return vulkan_version; }
        vk::PhysicalDevice get_gpu() { return gpu; }
        vk::Device get_device() { return device; }
        bool is_yCbCr_supported() const { return yCbCr_supported; }
        uint32_t get_queue_familt_index() { return queue_family_index; }
        vk::Queue get_queue() { return queue; }
        vk::SwapchainKHR get_swapchain() { return swapchain; }
        vk::Format get_swapchain_image_format() { return swapchain_atributes.format.format; };
        vk::Extent2D get_window_size() { return window_size; }
        size_t get_swapchain_image_count(){ return swapchain_images.size(); }
private:
        void create_physical_device(uint32_t gpu_index);

        void create_logical_device();

        void get_present_mode();

        void get_surface_format();

        void create_swap_chain(vk::SwapchainKHR old_swap_chain = vk::SwapchainKHR{});

        void create_swapchain_views();

        void destroy_swapchain_views() {
                for (auto& image : swapchain_images) {
                        device.destroy(image.view);
                }
        }

        void destroy_framebuffers() {
                for (auto& image : swapchain_images) {
                        device.destroy(image.framebuffer);
                }
        }

public:
        using window_parameters = vulkan_display::window_parameters;

        vulkan_context() = default;

        void init(vulkan_display::vulkan_instance&& instance, VkSurfaceKHR surface, 
                window_parameters, uint32_t gpu_index, vk::PresentModeKHR preferred_mode);

        void destroy();

        void create_framebuffers(vk::RenderPass render_pass);

        uint32_t acquire_next_swapchain_image(vk::Semaphore acquire_semaphore) const;

        vk::Framebuffer get_framebuffer(uint32_t framebuffer_id) {
                return swapchain_images[framebuffer_id].framebuffer;
        }

        window_parameters get_window_parameters() const {
                return { window_size.width, window_size.height };
        }

        void recreate_swapchain(window_parameters parameters, vk::RenderPass render_pass);
};

}//namespace vulkan_display_detail ----------------------------------------------------------------


namespace vulkan_display {

inline void cout_msg(std::string_view msg) {
        std::cout << msg << std::endl;
}

class vulkan_instance {
        vk::Instance instance{};
        std::unique_ptr<vk::DispatchLoaderDynamic> dynamic_dispatcher = nullptr;
        vk::DebugUtilsMessengerEXT messenger{};
        uint32_t vulkan_version = VK_API_VERSION_1_1;

        void init_validation_layers_error_messenger();

        friend void vulkan_display_detail::vulkan_context::init(vulkan_instance&&, 
                VkSurfaceKHR, window_parameters, uint32_t, vk::PresentModeKHR);
public:
        vulkan_instance() = default;
        vulkan_instance(const vulkan_instance& other) = delete;
        vulkan_instance& operator=(const vulkan_instance& other) = delete;
        vulkan_instance(vulkan_instance&& other) = delete;
        vulkan_instance& operator=(vulkan_instance&& other) = delete;
        
        ~vulkan_instance() {
                destroy();
        }

        /**
         * @param required_extensions   Vulkan instance extensions requested by aplication,
         *                              usually needed for creating vulkan surface
         * @param enable_validation     Enable vulkan validation layers, they should be disabled in release build.
         */
        void init(std::vector<const char*>& required_extensions, bool enable_validation, std::function<void(std::string_view sv)> logging_function = cout_msg);
        
        /**
         * @brief returns all available grafhics cards
         *  first parameter is gpu name,
         *  second parameter is true only if the gpu is suitable for vulkan_display
         */
        void get_available_gpus(std::vector<std::pair<std::string, bool>>& gpus);

        vk::Instance& get_instance() {
                return instance;
        }

        void destroy();
};

}
