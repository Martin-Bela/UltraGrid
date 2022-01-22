#pragma once

#include "concurrent_queue/readerwritercircularbuffer.h"
#include "vulkan_context.h"
#include "vulkan_transfer_image.h"


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


        struct image_semaphores {
                vk::Semaphore image_acquired;
                vk::Semaphore image_rendered;
        };
        std::vector<image_semaphores> image_semaphores;


        using transfer_image = detail::transfer_image;
        unsigned transfer_image_count = 0;
        std::vector<transfer_image> transfer_images{};
        image_description current_image_description;

        detail::concurrent_queue<transfer_image*> available_img_queue{8};
        detail::concurrent_queue<image> filled_img_queue{detail::filled_img_max_count};


        bool minimalised = false;
        bool destroyed = false;
private:

        VKD_RETURN_TYPE create_texture_sampler(vk::Format format);

        VKD_RETURN_TYPE create_render_pass();

        VKD_RETURN_TYPE create_descriptor_set_layout();

        VKD_RETURN_TYPE create_pipeline_layout();

        VKD_RETURN_TYPE create_graphics_pipeline();

        VKD_RETURN_TYPE create_command_pool();

        VKD_RETURN_TYPE create_command_buffers();

        VKD_RETURN_TYPE create_transfer_image(transfer_image*& result, image_description description);

        VKD_RETURN_TYPE create_image_semaphores();

        VKD_RETURN_TYPE allocate_description_sets();

        VKD_RETURN_TYPE record_graphics_commands(transfer_image& transfer_image, uint32_t swapchain_image_id);

public:
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

        VKD_RETURN_TYPE init(vulkan_instance&& instance, VkSurfaceKHR surface, uint32_t transfer_image_count,
                window_changed_callback& window, uint32_t gpu_index = no_gpu_selected,
                std::filesystem::path path_to_shaders = "./shaders", bool vsync = true, bool tearing_permitted = false);

        VKD_RETURN_TYPE destroy();

        VKD_RETURN_TYPE is_image_description_supported(bool& supported, image_description description);

        VKD_RETURN_TYPE acquire_image(image& image, image_description description);

        VKD_RETURN_TYPE queue_image(image img, bool discardable = true);

        VKD_RETURN_TYPE copy_and_queue_image(std::byte* frame, image_description description);

        VKD_RETURN_TYPE discard_image(image image) {
                auto* ptr = image.get_transfer_image();
                assert(ptr);
                available_img_queue.wait_enqueue(ptr);
                return VKD_RETURN_TYPE();
        }

        VKD_RETURN_TYPE display_queued_image(bool* displayed = nullptr);

        uint32_t get_vulkan_version() { return context.get_vulkan_version(); }
        
        bool is_yCbCr_supported() { return context.is_yCbCr_supported(); }

        /**
         * @brief Hint to vulkan display that some window parameters spicified in struct Window_parameters changed
         */
        VKD_RETURN_TYPE window_parameters_changed(window_parameters new_parameters);

        VKD_RETURN_TYPE window_parameters_changed() {
                VKD_PASS_RESULT(window_parameters_changed(window->get_window_parameters()));
                return VKD_RETURN_TYPE();
        }
};

} //vulkan_display
