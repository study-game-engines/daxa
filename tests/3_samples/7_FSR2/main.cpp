#include <0_common/window.hpp>
#include <0_common/player.hpp>
#include <0_common/hlsl_util.hpp>
#include <thread>
#include <iostream>

#include <daxa/utils/task_list.hpp>

#include <daxa/utils/imgui.hpp>
#include <0_common/imgui/imgui_impl_glfw.h>

#include <daxa/utils/math_operators.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define APPNAME "Daxa Sample: FSR2"
#define APPNAME_PREFIX(x) ("[" APPNAME "] " x)

#include <daxa/utils/fsr2.hpp>
using UpscaleContext = daxa::Fsr2Context;

struct RasterInput
{
    glm::mat4 view_mat;
    glm::mat4 prev_view_mat;
    f32vec2 jitter;
    daxa::ImageId texture_array_id;
    daxa::SamplerId sampler_id;
};

struct RasterPush
{
    glm::vec3 chunk_pos = {};
    u32 mode = {};
    daxa::BufferId input_buffer_id = {};
    daxa::BufferId vertex_buffer_id = {};
};

#include <0_common/voxels.hpp>

using namespace daxa::types;
using namespace daxa::math_operators;
using Clock = std::chrono::high_resolution_clock;

struct App : AppWindow<App>
{
    daxa::Context daxa_ctx = daxa::create_context({
        .enable_validation = false,
    });
    daxa::Device device = daxa_ctx.create_device({});

    daxa::Swapchain swapchain = device.create_swapchain({
        .native_window = get_native_handle(),
        .width = size_x,
        .height = size_y,
        .surface_format_selector = [](daxa::Format format)
        {
            switch (format)
            {
            case daxa::Format::R8G8B8A8_UINT: return 100;
            default: return daxa::default_format_score(format);
            }
        },
        .present_mode = daxa::PresentMode::DO_NOT_WAIT_FOR_VBLANK,
        .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST,
        .debug_name = APPNAME_PREFIX("swapchain"),
    });

    daxa::PipelineCompiler pipeline_compiler = device.create_pipeline_compiler({
        .shader_compile_options = {
            .root_paths = {
                "tests/0_common/shaders",
                "tests/3_samples/7_FSR2/shaders",
                "include",
            },
        },
        .debug_name = APPNAME_PREFIX("pipeline_compiler"),
    });

    // clang-format off
    daxa::RasterPipeline raster_pipeline = pipeline_compiler.create_raster_pipeline({
        .vertex_shader_info = {.source = daxa::ShaderFile{"draw.hlsl"}, .compile_options = {.entry_point = "vs_main"}},
        .fragment_shader_info = {.source = daxa::ShaderFile{"draw.hlsl"}, .compile_options = {.entry_point = "fs_main"}},
        .color_attachments = {
            {.format = daxa::Format::R16G16B16A16_SFLOAT, .blend = {.blend_enable = true, .src_color_blend_factor = daxa::BlendFactor::SRC_ALPHA, .dst_color_blend_factor = daxa::BlendFactor::ONE_MINUS_SRC_ALPHA}},
            {.format = daxa::Format::R16G16_SFLOAT, .blend = {.blend_enable = true, .src_color_blend_factor = daxa::BlendFactor::SRC_ALPHA, .dst_color_blend_factor = daxa::BlendFactor::ONE_MINUS_SRC_ALPHA}},
        },
        .depth_test = {
            .depth_attachment_format = daxa::Format::D32_SFLOAT,
            .enable_depth_test = true,
            .enable_depth_write = true,
        },
        .raster = {
            .face_culling = daxa::FaceCullFlagBits::BACK_BIT,
        },
        .push_constant_size = sizeof(RasterPush) * 2,
        .debug_name = APPNAME_PREFIX("raster_pipeline"),
    }).value();
    // clang-format on

    daxa::ImGuiRenderer imgui_renderer = create_imgui_renderer();
    auto create_imgui_renderer() -> daxa::ImGuiRenderer
    {
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForVulkan(glfw_window_ptr, true);
        return daxa::ImGuiRenderer({
            .device = device,
            .pipeline_compiler = pipeline_compiler,
            .format = swapchain.get_format(),
        });
    }

    daxa::BinarySemaphore binary_semaphore = device.create_binary_semaphore({
        .debug_name = APPNAME_PREFIX("binary_semaphore"),
    });

    static inline constexpr u64 FRAMES_IN_FLIGHT = 1;
    daxa::TimelineSemaphore gpu_framecount_timeline_sema = device.create_timeline_semaphore(daxa::TimelineSemaphoreInfo{
        .initial_value = 0,
        .debug_name = APPNAME_PREFIX("gpu_framecount_timeline_semaphore"),
    });
    u64 cpu_framecount = FRAMES_IN_FLIGHT - 1;

    Clock::time_point start = Clock::now(), prev_time = start;
    f32 elapsed_s = 1.0f;

    RenderableVoxelWorld renderable_world{device};
    Player3D player = {
        .rot = {2.0f, 0.0f, 0.0f},
    };
    bool should_resize = false, paused = true;

    RasterInput raster_input;
    daxa::BufferId raster_input_buffer = device.create_buffer({
        .size = sizeof(RasterInput),
        .debug_name = APPNAME_PREFIX("raster_input_buffer"),
    });
    daxa::BufferId staging_raster_input_buffer = device.create_buffer({
        .memory_flags = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .size = sizeof(RasterInput),
        .debug_name = APPNAME_PREFIX("staging_raster_input_buffer"),
    });
    daxa::TaskBufferId task_raster_input_buffer;
    daxa::TaskBufferId task_staging_raster_input_buffer;

    UpscaleContext upscale_context = UpscaleContext{{.device = device}};
    f32 render_scl = 1.0f;
    daxa::ImageId swapchain_image;
    daxa::ImageId color_image, display_image, motion_vectors_image, depth_image;
    u32 render_size_x, render_size_y;
    f32vec2 jitter = {0.0f, 0.0f};
    daxa::TaskImageId task_swapchain_image;
    daxa::TaskImageId task_color_image, task_display_image, task_motion_vectors_image, task_depth_image;
    daxa::TaskList loop_task_list = record_loop_task_list();
    bool fsr_enabled = false;

    void create_render_images()
    {
        render_size_x = std::max<u32>(1, static_cast<u32>(static_cast<f32>(size_x) * render_scl));
        render_size_y = std::max<u32>(1, static_cast<u32>(static_cast<f32>(size_y) * render_scl));

        color_image = device.create_image({
            .format = daxa::Format::R16G16B16A16_SFLOAT,
            .aspect = daxa::ImageAspectFlagBits::COLOR,
            .size = {render_size_x, render_size_y, 1},
            .usage = daxa::ImageUsageFlagBits::COLOR_ATTACHMENT | daxa::ImageUsageFlagBits::SHADER_READ_ONLY | daxa::ImageUsageFlagBits::SHADER_READ_WRITE | daxa::ImageUsageFlagBits::TRANSFER_SRC,
            .debug_name = APPNAME_PREFIX("color_image"),
        });
        display_image = device.create_image({
            .format = daxa::Format::R16G16B16A16_SFLOAT,
            .aspect = daxa::ImageAspectFlagBits::COLOR,
            .size = {size_x, size_y, 1},
            .usage = daxa::ImageUsageFlagBits::COLOR_ATTACHMENT | daxa::ImageUsageFlagBits::SHADER_READ_ONLY | daxa::ImageUsageFlagBits::SHADER_READ_WRITE | daxa::ImageUsageFlagBits::TRANSFER_SRC | daxa::ImageUsageFlagBits::TRANSFER_DST,
            .debug_name = APPNAME_PREFIX("display_image"),
        });
        motion_vectors_image = device.create_image({
            .format = daxa::Format::R16G16_SFLOAT,
            .aspect = daxa::ImageAspectFlagBits::COLOR,
            .size = {render_size_x, render_size_y, 1},
            .usage = daxa::ImageUsageFlagBits::COLOR_ATTACHMENT | daxa::ImageUsageFlagBits::SHADER_READ_ONLY | daxa::ImageUsageFlagBits::SHADER_READ_WRITE,
            .debug_name = APPNAME_PREFIX("motion_vectors_image"),
        });
        depth_image = device.create_image({
            .format = daxa::Format::D32_SFLOAT,
            .aspect = daxa::ImageAspectFlagBits::DEPTH,
            .size = {render_size_x, render_size_y, 1},
            .usage = daxa::ImageUsageFlagBits::DEPTH_STENCIL_ATTACHMENT | daxa::ImageUsageFlagBits::SHADER_READ_ONLY,
            .debug_name = APPNAME_PREFIX("depth_image"),
        });

        upscale_context.resize({
            .render_size_x = render_size_x,
            .render_size_y = render_size_y,
            .display_size_x = size_x,
            .display_size_y = size_y,
        });
    }
    void destroy_render_images()
    {
        device.destroy_image(color_image);
        device.destroy_image(display_image);
        device.destroy_image(motion_vectors_image);
        device.destroy_image(depth_image);
    }

    App() : AppWindow<App>(APPNAME)
    {
        create_render_images();
    }

    ~App()
    {
        device.wait_idle();
        ImGui_ImplGlfw_Shutdown();
        destroy_render_images();
        device.destroy_buffer(raster_input_buffer);
        device.destroy_buffer(staging_raster_input_buffer);
    }

    bool update()
    {
        glfwPollEvents();
        if (glfwWindowShouldClose(glfw_window_ptr))
        {
            return true;
        }

        if (!minimized)
        {
            draw();
        }
        else
        {
            using namespace std::literals;
            std::this_thread::sleep_for(1ms);
        }

        return false;
    }

    void draw()
    {
        auto now = Clock::now();
        elapsed_s = std::chrono::duration<f32>(now - prev_time).count();
        prev_time = now;

        ui_update();

        player.camera.resize(static_cast<i32>(size_x), static_cast<i32>(size_y));
        player.camera.set_pos(player.pos);
        player.camera.set_rot(player.rot.x, player.rot.y);
        player.update(elapsed_s);

        if (pipeline_compiler.check_if_sources_changed(raster_pipeline))
        {
            auto new_pipeline = pipeline_compiler.recreate_raster_pipeline(raster_pipeline);
            std::cout << new_pipeline.to_string() << std::endl;
            if (new_pipeline.is_ok())
            {
                raster_pipeline = new_pipeline.value();
            }
        }

        if (should_resize)
        {
            do_resize();
        }

        swapchain_image = swapchain.acquire_next_image();

        loop_task_list.execute();
        auto command_lists = loop_task_list.command_lists();
        auto cmd_list = device.create_command_list({});
        cmd_list.pipeline_barrier_image_transition({
            .awaited_pipeline_access = loop_task_list.last_access(task_swapchain_image),
            .before_layout = loop_task_list.last_layout(task_swapchain_image),
            .after_layout = daxa::ImageLayout::PRESENT_SRC,
            .image_id = swapchain_image,
        });
        cmd_list.complete();
        ++cpu_framecount;
        command_lists.push_back(cmd_list);
        device.submit_commands({
            .command_lists = command_lists,
            .signal_binary_semaphores = {binary_semaphore},
            .signal_timeline_semaphores = {{gpu_framecount_timeline_sema, cpu_framecount}},
        });
        device.present_frame({
            .wait_binary_semaphores = {binary_semaphore},
            .swapchain = swapchain,
        });
        gpu_framecount_timeline_sema.wait_for_value(cpu_framecount - 1);
    }

    void on_mouse_move(f32 x, f32 y)
    {
        if (!paused)
        {
            f32 center_x = static_cast<f32>(size_x / 2);
            f32 center_y = static_cast<f32>(size_y / 2);
            auto offset = glm::vec2{x - center_x, center_y - y};
            player.on_mouse_move(static_cast<f64>(offset.x), static_cast<f64>(offset.y));
            set_mouse_pos(center_x, center_y);
        }
    }

    void on_key(int key, int action)
    {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        {
            toggle_pause();
        }

        if (!paused)
        {
            player.on_key(key, action);
        }
    }

    void on_resize(u32 sx, u32 sy)
    {
        size_x = sx;
        size_y = sy;
        minimized = (sx == 0 || sy == 0);

        if (!minimized)
        {
            should_resize = true;
            do_resize();
        }
    }

    void do_resize()
    {
        should_resize = false;
        swapchain.resize(size_x, size_y);
        destroy_render_images();
        create_render_images();
        draw();
    }

    void toggle_pause()
    {
        set_mouse_capture(paused);
        paused = !paused;
    }

    void ui_update()
    {
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Debug");
        f32 new_scl = render_scl;
        ImGui::SliderFloat("Render Scl", &new_scl, 1.0f / static_cast<f32>(std::min(size_x, size_y)), 1.0f);
        if (new_scl != render_scl)
        {
            render_scl = new_scl;
            destroy_render_images();
            create_render_images();
        }
        if (ImGui::Button("Clear Console"))
        {
            system("CLS");
        }
        ImGui::Checkbox("Enable FSR", &fsr_enabled);
        ImGui::End();
        ImGui::Render();
    }

    auto record_loop_task_list() -> daxa::TaskList
    {
        daxa::TaskList new_task_list = daxa::TaskList({
            .device = device,
            .debug_name = APPNAME_PREFIX("task_list"),
        });
        task_swapchain_image = new_task_list.create_task_image({
            .fetch_callback = [this]()
            { return swapchain_image; },
            .debug_name = APPNAME_PREFIX("task_swapchain_image"),
        });
        task_color_image = new_task_list.create_task_image({
            .fetch_callback = [this]()
            { return color_image; },
            .debug_name = APPNAME_PREFIX("task_color_image"),
        });
        task_display_image = new_task_list.create_task_image({
            .fetch_callback = [this]()
            { return display_image; },
            .debug_name = APPNAME_PREFIX("task_display_image"),
        });
        task_motion_vectors_image = new_task_list.create_task_image({
            .fetch_callback = [this]()
            { return motion_vectors_image; },
            .debug_name = APPNAME_PREFIX("task_motion_vectors_image"),
        });
        task_depth_image = new_task_list.create_task_image({
            .fetch_callback = [this]()
            { return depth_image; },
            .slice = {.image_aspect = daxa::ImageAspectFlagBits::DEPTH},
            .debug_name = APPNAME_PREFIX("task_depth_image"),
        });

        task_raster_input_buffer = new_task_list.create_task_buffer({
            .fetch_callback = [this]()
            { return raster_input_buffer; },
            .debug_name = APPNAME_PREFIX("task_raster_input_buffer"),
        });
        task_staging_raster_input_buffer = new_task_list.create_task_buffer({
            .fetch_callback = [this]()
            { return staging_raster_input_buffer; },
            .debug_name = APPNAME_PREFIX("task_staging_raster_input_buffer"),
        });

        new_task_list.add_task({
            .resources = {
                .buffers = {
                    {task_staging_raster_input_buffer, daxa::TaskBufferAccess::HOST_TRANSFER_WRITE},
                },
            },
            .task = [this](daxa::TaskInterface /* interf */)
            {
                this->raster_input.prev_view_mat = this->raster_input.view_mat;

                this->raster_input.view_mat = player.camera.get_vp();
                auto prev_jitter = jitter;
                jitter = upscale_context.get_jitter(cpu_framecount);
                auto jitter_vec = glm::vec3{
                    jitter.x * 2.0f / static_cast<f32>(render_size_x),
                    jitter.y * 2.0f / static_cast<f32>(render_size_y),
                    0.0f,
                };
                this->raster_input.view_mat = glm::translate(glm::identity<glm::mat4>(), jitter_vec) * this->raster_input.view_mat;
                this->raster_input.jitter = (jitter - prev_jitter) * f32vec2{2.0f / static_cast<f32>(render_size_x), 2.0f / static_cast<f32>(render_size_y)};
                this->raster_input.texture_array_id = renderable_world.atlas_texture_array;
                this->raster_input.sampler_id = renderable_world.atlas_sampler;

                RasterInput * buffer_ptr = device.map_memory_as<RasterInput>(staging_raster_input_buffer);
                *buffer_ptr = this->raster_input;
                device.unmap_memory(staging_raster_input_buffer);
            },
            .debug_name = APPNAME_PREFIX("Input MemMap"),
        });
        new_task_list.add_task({
            .resources = {
                .buffers = {
                    {task_raster_input_buffer, daxa::TaskBufferAccess::TRANSFER_WRITE},
                    {task_staging_raster_input_buffer, daxa::TaskBufferAccess::TRANSFER_READ},
                },
            },
            .task = [this](daxa::TaskInterface interf)
            {
                auto cmd_list = interf.get_command_list();
                cmd_list.copy_buffer_to_buffer({
                    .src_buffer = staging_raster_input_buffer,
                    .dst_buffer = raster_input_buffer,
                    .size = sizeof(RasterInput),
                });
            },
            .debug_name = APPNAME_PREFIX("Input Transfer"),
        });

        new_task_list.add_task({
            .resources = {
                .buffers = {
                    {task_raster_input_buffer, daxa::TaskBufferAccess::VERTEX_SHADER_READ_ONLY},
                },
                .images = {
                    {task_color_image, daxa::TaskImageAccess::COLOR_ATTACHMENT},
                    {task_motion_vectors_image, daxa::TaskImageAccess::COLOR_ATTACHMENT},
                    {task_depth_image, daxa::TaskImageAccess::DEPTH_ATTACHMENT},
                },
            },
            .task = [this](daxa::TaskInterface interf)
            {
                auto cmd_list = interf.get_command_list();
                cmd_list.begin_renderpass({
                    .color_attachments = {
                        {
                            .image_view = color_image.default_view(),
                            .load_op = daxa::AttachmentLoadOp::CLEAR,
                            .clear_value = std::array<f32, 4>{0.2f, 0.4f, 1.0f, 1.0f},
                        },
                        {
                            .image_view = motion_vectors_image.default_view(),
                            .load_op = daxa::AttachmentLoadOp::CLEAR,
                            .clear_value = std::array<f32, 4>{0.0f, 0.0f, 0.0f, 0.0f},
                        },
                    },
                    .depth_attachment = {{
                        .image_view = depth_image.default_view(),
                        .load_op = daxa::AttachmentLoadOp::CLEAR,
                        .clear_value = daxa::DepthValue{1.0f, 0},
                    }},
                    .render_area = {.x = 0, .y = 0, .width = render_size_x, .height = render_size_y},
                });
                cmd_list.set_pipeline(raster_pipeline);
                auto push = RasterPush{.input_buffer_id = raster_input_buffer};
                renderable_world.draw(cmd_list, push);
                cmd_list.end_renderpass();
            },
            .debug_name = APPNAME_PREFIX("Draw Task"),
        });

        new_task_list.add_task({
            .resources = {
                .images = {
                    {task_color_image, daxa::TaskImageAccess::TRANSFER_READ},
                    {task_display_image, daxa::TaskImageAccess::TRANSFER_WRITE},
                },
            },
            .task = [this](daxa::TaskInterface interf)
            {
                if (!fsr_enabled)
                {
                    auto cmd_list = interf.get_command_list();
                    cmd_list.blit_image_to_image({
                        .src_image = color_image,
                        .src_image_layout = daxa::ImageLayout::TRANSFER_SRC_OPTIMAL,
                        .dst_image = display_image,
                        .dst_image_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
                        .src_slice = {.image_aspect = daxa::ImageAspectFlagBits::COLOR},
                        .src_offsets = {{{0, 0, 0}, {static_cast<i32>(render_size_x), static_cast<i32>(render_size_y), 1}}},
                        .dst_slice = {.image_aspect = daxa::ImageAspectFlagBits::COLOR},
                        .dst_offsets = {{{0, 0, 0}, {static_cast<i32>(size_x), static_cast<i32>(size_y), 1}}},
                    });
                }
            },
            .debug_name = APPNAME_PREFIX("Blit Task (render to display)"),
        });

        new_task_list.add_task({
            .resources = {
                .images = {
                    {task_color_image, daxa::TaskImageAccess::SHADER_READ_ONLY},
                    {task_motion_vectors_image, daxa::TaskImageAccess::SHADER_READ_ONLY},
                    {task_depth_image, daxa::TaskImageAccess::SHADER_READ_ONLY},
                    {task_display_image, daxa::TaskImageAccess::SHADER_WRITE_ONLY},
                },
            },
            .task = [this](daxa::TaskInterface interf)
            {
                if (fsr_enabled)
                {
                    auto cmd_list = interf.get_command_list();
                    upscale_context.upscale(
                        cmd_list,
                        {
                            .color = color_image,
                            .depth = depth_image,
                            .motion_vectors = motion_vectors_image,
                            .output = display_image,
                            .should_reset = false,
                            .delta_time = elapsed_s,
                            .jitter = jitter,
                            .should_sharpen = false,
                            .sharpening = 0.0f,
                            .camera_info = {
                                .near_plane = player.camera.near_clip,
                                .far_plane = player.camera.far_clip,
                                .vertical_fov = glm::radians(player.camera.fov),
                            },
                        });
                }
            },
            .debug_name = APPNAME_PREFIX("Upscale Task"),
        });

        new_task_list.add_task({
            .resources = {
                .images = {
                    {task_display_image, daxa::TaskImageAccess::TRANSFER_READ},
                    {task_swapchain_image, daxa::TaskImageAccess::TRANSFER_WRITE},
                },
            },
            .task = [this](daxa::TaskInterface interf)
            {
                auto cmd_list = interf.get_command_list();
                cmd_list.blit_image_to_image({
                    .src_image = display_image,
                    .src_image_layout = daxa::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    .dst_image = swapchain_image,
                    .dst_image_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
                    .src_slice = {.image_aspect = daxa::ImageAspectFlagBits::COLOR},
                    .src_offsets = {{{0, 0, 0}, {static_cast<i32>(size_x), static_cast<i32>(size_y), 1}}},
                    .dst_slice = {.image_aspect = daxa::ImageAspectFlagBits::COLOR},
                    .dst_offsets = {{{0, 0, 0}, {static_cast<i32>(size_x), static_cast<i32>(size_y), 1}}},
                });
            },
            .debug_name = APPNAME_PREFIX("Blit Task (display to swapchain)"),
        });

        new_task_list.add_task({
            .resources = {
                .images = {
                    {task_swapchain_image, daxa::TaskImageAccess::COLOR_ATTACHMENT},
                },
            },
            .task = [this](daxa::TaskInterface interf)
            {
                auto cmd_list = interf.get_command_list();
                imgui_renderer.record_commands(ImGui::GetDrawData(), cmd_list, swapchain_image, size_x, size_y);
            },
            .debug_name = APPNAME_PREFIX("ImGui Task"),
        });

        new_task_list.compile();

        return new_task_list;
    }
};

int main()
{
    App app = {};
    while (true)
    {
        if (app.update())
            break;
    }

    std::cout << std::flush;
}