#include "vulkan_context.hpp"

#include <filesystem>

namespace vulkan_display_detail{

struct ImageSize{
        uint32_t width;
        uint32_t height;

        static ImageSize fromExtent2D(vk::Extent2D extent){
               return ImageSize{extent.width, extent.height};
        }
};

class ConversionPipeline {
        bool valid = false;
        vk::ShaderModule compute_shader{};
        vk::PipelineLayout pipeline_layout{};
        vk::Pipeline pipeline{};

        vk::DescriptorSetLayout source_desc_set_layout{};
        vk::DescriptorSetLayout destination_desc_set_layout{};

public:
        void create(vk::Device device, std::filesystem::path path_to_shaders, vk::Sampler sampler);

        void destroy(vk::Device device);

        void record_commands(vk::CommandBuffer cmd_buffer, ImageSize image_size, std::array<vk::DescriptorSet, 2> descriptor_set);

        vk::DescriptorSetLayout get_source_image_desc_set_layout(){ return source_desc_set_layout; }

        vk::DescriptorSetLayout get_destination_image_desc_set_layout() { return destination_desc_set_layout; }
};

struct RenderArea {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
};

class RenderPipeline {
        bool valid = false;
        RenderArea render_area;
        vk::Extent2D window_size;
        vk::Viewport viewport;
        vk::Rect2D scissor;

        vk::ShaderModule vertex_shader;
        vk::ShaderModule fragment_shader;

        vk::RenderPass render_pass;
        vk::ClearValue clear_color;

        vk::DescriptorSetLayout image_desc_set_layout;
        vk::PipelineLayout pipeline_layout;
        vk::Pipeline pipeline;

public:
        void create(VulkanContext& context, const std::filesystem::path& path_to_shaders);

        void destroy(vk::Device device);

        void update_render_area(vk::Extent2D window_size, vk::Extent2D image_size);

        /** Invalidates descriptor sets created from stored descriptor set layout**/
        void reconfigure(vk::Device device, vk::Sampler sampler);

        void record_commands(vk::CommandBuffer cmd_buffer, vk::DescriptorSet image, vk::Framebuffer framebuffer);

        vk::RenderPass get_render_pass(){ return render_pass; }

        vk::DescriptorSetLayout get_image_desc_set_layout(){ return image_desc_set_layout; }
};


} //vulkan_display_detail
