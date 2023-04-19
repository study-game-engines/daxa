#pragma once

#if !DAXA_BUILT_WITH_UTILS_TASK_LIST
#error "[package management error] You must build Daxa with the DAXA_ENABLE_UTILS_TASK_LIST CMake option enabled, or request the utils-task-list feature in vcpkg"
#endif

#include <span>
#include <format>

#include <daxa/core.hpp>
#include <daxa/device.hpp>

namespace daxa
{
    enum struct TaskBufferAccess
    {
        NONE,
        SHADER_READ,
        VERTEX_SHADER_READ,
        TESSELLATION_CONTROL_SHADER_READ,
        TESSELLATION_EVALUATION_SHADER_READ,
        GEOMETRY_SHADER_READ,
        FRAGMENT_SHADER_READ,
        COMPUTE_SHADER_READ,
        SHADER_WRITE,
        VERTEX_SHADER_WRITE,
        TESSELLATION_CONTROL_SHADER_WRITE,
        TESSELLATION_EVALUATION_SHADER_WRITE,
        GEOMETRY_SHADER_WRITE,
        FRAGMENT_SHADER_WRITE,
        COMPUTE_SHADER_WRITE,
        SHADER_READ_WRITE,
        VERTEX_SHADER_READ_WRITE,
        TESSELLATION_CONTROL_SHADER_READ_WRITE,
        TESSELLATION_EVALUATION_SHADER_READ_WRITE,
        GEOMETRY_SHADER_READ_WRITE,
        FRAGMENT_SHADER_READ_WRITE,
        COMPUTE_SHADER_READ_WRITE,
        INDEX_READ,
        DRAW_INDIRECT_INFO_READ,
        TRANSFER_READ,
        TRANSFER_WRITE,
        HOST_TRANSFER_READ,
        HOST_TRANSFER_WRITE,
    };

    auto to_string(TaskBufferAccess const & usage) -> std::string_view;

    enum struct TaskImageAccess
    {
        NONE,
        SHADER_READ,
        VERTEX_SHADER_READ,
        TESSELLATION_CONTROL_SHADER_READ,
        TESSELLATION_EVALUATION_SHADER_READ,
        GEOMETRY_SHADER_READ,
        FRAGMENT_SHADER_READ,
        COMPUTE_SHADER_READ,
        SHADER_WRITE,
        VERTEX_SHADER_WRITE,
        TESSELLATION_CONTROL_SHADER_WRITE,
        TESSELLATION_EVALUATION_SHADER_WRITE,
        GEOMETRY_SHADER_WRITE,
        FRAGMENT_SHADER_WRITE,
        COMPUTE_SHADER_WRITE,
        SHADER_READ_WRITE,
        VERTEX_SHADER_READ_WRITE,
        TESSELLATION_CONTROL_SHADER_READ_WRITE,
        TESSELLATION_EVALUATION_SHADER_READ_WRITE,
        GEOMETRY_SHADER_READ_WRITE,
        FRAGMENT_SHADER_READ_WRITE,
        COMPUTE_SHADER_READ_WRITE,
        TRANSFER_READ,
        TRANSFER_WRITE,
        COLOR_ATTACHMENT,
        DEPTH_ATTACHMENT,
        STENCIL_ATTACHMENT,
        DEPTH_STENCIL_ATTACHMENT,
        DEPTH_ATTACHMENT_READ,
        STENCIL_ATTACHMENT_READ,
        DEPTH_STENCIL_ATTACHMENT_READ,
        RESOLVE_WRITE,
        PRESENT,
    };

    auto to_string(TaskImageAccess const & usage) -> std::string_view;

    namespace detail
    {
        template <typename T>
        struct ConstexprCompatibleSpan
        {
            std::array<u8, 16> raw = {};

            auto get() -> std::span<T> &
            {
                return *reinterpret_cast<std::span<T> *>(&raw);
            }

            auto get() const -> std::span<T> const &
            {
                return *reinterpret_cast<std::span<T> const *>(&raw);
            }
        };
    } // namespace detail

    using TaskResourceIndex = u32;

    struct TaskGPUResourceId
    {
        TaskResourceIndex task_list_index = {};
        TaskResourceIndex index = {};

        auto is_empty() const -> bool;
        auto is_persistent() const -> bool;

        auto operator<=>(TaskGPUResourceId const & other) const = default;
    };

    auto to_string(TaskGPUResourceId const & id) -> std::string;

    struct TaskBufferId : public TaskGPUResourceId
    {
    };

    struct TaskImageId : public TaskGPUResourceId
    {
    };

    struct TaskBufferUseInit
    {
        TaskBufferId id = {};
        TaskBufferAccess access = {};
        // Redirects in callback and shader use aliases to this use.
        std::string_view name = {};
    };

    struct TaskImageUseInit
    {
        TaskImageId id = {};
        TaskImageAccess access = {};
        ImageMipArraySlice slice = {};
        /// @brief  Determines the view type the runtime provides in the TaskInterface<>.
        ///         If no type is provided, the runtime images default view type is used.
        ImageViewType view_type = ImageViewType::MAX_ENUM;
        // Redirects in callback and shader use aliases to this use.
        std::string_view name = {};
    };

    struct ImageSliceState
    {
        Access latest_access = {};
        ImageLayout latest_layout = {};
        ImageMipArraySlice slice = {};
    };

    using UsedTaskBuffers = std::vector<TaskBufferUseInit>;
    using UsedTaskImages = std::vector<TaskImageUseInit>;

    enum class TaskResourceUseType : u32
    {
        NONE = 0,
        BUFFER = 1,
        IMAGE = 2,
        CONSTANT = 3,
    };

    static inline constexpr size_t TASK_INPUT_FIELD_SIZE = 128;

    struct GenericTaskResourceUse
    {
        TaskResourceUseType type = TaskResourceUseType::NONE;
        // This is nessecary for c++ to properly generate copy and move operators.
        [[maybe_unused]] u8 raw[TASK_INPUT_FIELD_SIZE - sizeof(TaskResourceUseType)] = {};
    };

    struct alignas(TASK_INPUT_FIELD_SIZE) TaskBufferUse
    {
      private:
        friend struct ImplTaskList;
        TaskResourceUseType const type = TaskResourceUseType::BUFFER;
        static constexpr inline TaskResourceUseType INPUT_TYPE = TaskResourceUseType::BUFFER;
        std::span<BufferId const> buffers = {};

      public:
        TaskBufferId id = {};
        TaskBufferAccess access = {};

        constexpr TaskBufferUse() = default;

        constexpr TaskBufferUse(TaskBufferUseInit const & init)
            : id{init.id},
              access{init.access}
        {
        }

        static auto from(GenericTaskResourceUse const & input) -> TaskBufferUse const &
        {
            DAXA_DBG_ASSERT_TRUE_M(input.type == TaskResourceUseType::BUFFER, "invalid TaskResourceUse cast");
            return *reinterpret_cast<TaskBufferUse const *>(&input);
        }

        static auto from(GenericTaskResourceUse & input) -> TaskBufferUse &
        {
            DAXA_DBG_ASSERT_TRUE_M(input.type == TaskResourceUseType::BUFFER, "invalid TaskResourceUse cast");
            return *reinterpret_cast<TaskBufferUse *>(&input);
        }

        auto buffer(usize index = 0) const -> BufferId
        {
            DAXA_DBG_ASSERT_TRUE_M(buffers.size() > 0, "this function is only allowed to be called within a task callback");
            return buffers[index];
        }

        auto to_generic() const -> GenericTaskResourceUse const &
        {
            return *reinterpret_cast<GenericTaskResourceUse const *>(this);
        }

        operator GenericTaskResourceUse const &() const
        {
            return to_generic();
        }
    };

    struct alignas(TASK_INPUT_FIELD_SIZE) TaskImageUse
    {
      private:
        friend struct ImplTaskList;
        TaskResourceUseType type = TaskResourceUseType::IMAGE;
        static constexpr inline TaskResourceUseType INPUT_TYPE = TaskResourceUseType::IMAGE;
        std::span<ImageId const> images = {};
        std::span<ImageViewId const> views = {};

      public:
        TaskImageId id = {};
        TaskImageAccess access = {};
        ImageMipArraySlice slice = {};
        /// @brief  Determines the view type the runtime provides in the TaskInterface<>.
        ///         If no type is provided, the runtime images default view type is used.
        ImageViewType view_type = ImageViewType::MAX_ENUM;

        constexpr TaskImageUse() = default;

        constexpr TaskImageUse(TaskImageUseInit const & init)
            : id{init.id},
              access{init.access},
              slice{init.slice},
              view_type{init.view_type}
        {
        }

        static auto from(GenericTaskResourceUse const & input) -> TaskImageUse const &
        {
            DAXA_DBG_ASSERT_TRUE_M(input.type == TaskResourceUseType::IMAGE, "invalid TaskResourceUse cast");
            return *reinterpret_cast<TaskImageUse const *>(&input);
        }

        static auto from(GenericTaskResourceUse & input) -> TaskImageUse &
        {
            DAXA_DBG_ASSERT_TRUE_M(input.type == TaskResourceUseType::IMAGE, "invalid TaskResourceUse cast");
            return *reinterpret_cast<TaskImageUse *>(&input);
        }

        auto image(u32 index = 0) const -> ImageId
        {
            DAXA_DBG_ASSERT_TRUE_M(images.size() > 0, "this function is only allowed to be called within a task callback");
            return images[index];
        }

        auto view(u32 index = 0) const -> ImageViewId
        {
            DAXA_DBG_ASSERT_TRUE_M(views.size() > 0, "this function is only allowed to be called within a task callback");
            return views[index];
        }

        auto to_generic() const -> GenericTaskResourceUse const &
        {
            return *reinterpret_cast<GenericTaskResourceUse const *>(this);
        }

        operator GenericTaskResourceUse const &() const
        {
            return to_generic();
        }
    };

    static inline constexpr size_t TASK_BUFFER_INPUT_SIZE = sizeof(TaskBufferUse);
    static inline constexpr size_t TASK_IMAGE_INPUT_SIZE = sizeof(TaskImageUse);

    static_assert(TASK_BUFFER_INPUT_SIZE == TASK_IMAGE_INPUT_SIZE, "should be impossible! contact Ipotrick");
    static_assert(TASK_BUFFER_INPUT_SIZE == TASK_INPUT_FIELD_SIZE, "should be impossible! contact Ipotrick");

    struct TaskUseOffsetType
    {
        u32 offset = {};
        TaskResourceUseType type = {};
    };

    template <typename ReflectedT, i32 SHADER_BINDING_T = -1>
    struct TaskUses
    {
        using FIRST_DERIVED = ReflectedT;
        static constexpr i32 SHADER_BINDING = SHADER_BINDING_T;
        static constexpr usize USE_COUNT = []()
        {
            static_assert(sizeof(ReflectedT) != 0, "TaskUse must be non zero size");
            static_assert(sizeof(ReflectedT) % TASK_INPUT_FIELD_SIZE == 0, "TaskUse struct must only contain task uses!");
            return sizeof(ReflectedT) / TASK_INPUT_FIELD_SIZE;
        }();
    };

    struct GenericTaskArgsContainer
    {
        std::vector<u8> memory = {};
        usize count = {};

        auto span() -> std::span<GenericTaskResourceUse>
        {
            return {reinterpret_cast<GenericTaskResourceUse *>(memory.data()), count};
        }

        auto span() const -> std::span<GenericTaskResourceUse const>
        {
            return {reinterpret_cast<GenericTaskResourceUse const *>(memory.data()), count};
        }

        template<typename BufFn, typename ImgFn> 
        void for_each(BufFn&& buf_fn, ImgFn&& img_fn)
        {
            auto s = span();
            for (u32 index = 0; index < s.size(); ++index)
            {
                auto type = s[index].type;
                switch(type)
                {
                    case TaskResourceUseType::BUFFER: 
                    {
                        auto & arg = TaskBufferUse::from(s[index]);
                        buf_fn(index, arg); 
                        break;
                    }
                    case TaskResourceUseType::IMAGE: 
                    {
                        auto & arg = TaskImageUse::from(s[index]);
                        img_fn(index, arg); 
                        break;
                    }
                    default: break;
                }
            }
        }

        template<typename BufFn, typename ImgFn> 
        void for_each(BufFn&& buf_fn, ImgFn&& img_fn) const
        {
            auto const s = span();
            for (u32 index = 0; index < s.size(); ++index)
            {
                auto type = s[index].type;
                switch(type)
                {
                    case TaskResourceUseType::BUFFER: 
                    {
                        auto const & arg = TaskBufferUse::from(s[index]);
                        buf_fn(index, arg); 
                        break;
                    }
                    case TaskResourceUseType::IMAGE: 
                    {
                        auto const & arg = TaskImageUse::from(s[index]);
                        img_fn(index, arg); 
                        break;
                    }
                    default: break;
                }
            }
        }

        operator std::span<GenericTaskResourceUse>()
        {
            return span();
        }

        operator std::span<GenericTaskResourceUse const> const()
        {
            return span();
        }
    };

    auto get_task_arg_shader_alignment(TaskResourceUseType type) -> u32;

    auto get_task_arg_shader_offsets_size(std::span<GenericTaskResourceUse> args) -> std::pair<std::vector<u32>, u32>;
} // namespace daxa