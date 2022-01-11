#pragma once

#include "Daxa.hpp"

class RenderContext {
public:
    RenderContext(daxa::Window& window) 
		: device{ daxa::gpu::Device::create() }
		, queue{ this->device->createQueue() }
		, swapchain{ this->device->createSwapchain({
			.surface = window.getSurface(),
			.width = window.getWidth(),
			.height = window.getHeight(),
			.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
		})}
		, swapchainImage{ this->swapchain->aquireNextImage() }
		, presentSignal{ this->device->createSignal() }
    {
		this->depthImage = device->createImage2d({
			.width = window.getWidth(),
			.height = window.getHeight(),
			.format = VK_FORMAT_D32_SFLOAT,
			.imageUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			.imageAspekt = VK_IMAGE_ASPECT_DEPTH_BIT,
		});

		this->normalsBuffer = device->createImage2d({
			.width = window.getWidth(),
			.height = window.getHeight(),
			.format = VK_FORMAT_R16G16_SNORM,
			.imageUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.imageAspekt = VK_IMAGE_ASPECT_COLOR_BIT,
		});
    }

    ~RenderContext() {
		waitIdle();
    }

    void resize(u32 width, u32 height) {
        swapchain->resize(VkExtent2D{ .width = width, .height = height });
        swapchainImage = swapchain->aquireNextImage();
        depthImage = device->createImage2d({
            .width = width,
            .height = height,
            .format = VK_FORMAT_D32_SFLOAT,
            .imageUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .imageAspekt = VK_IMAGE_ASPECT_DEPTH_BIT,
            .memoryPropertyFlags = VMA_MEMORY_USAGE_GPU_ONLY,
        });
    }

	void present() {
		queue->present(std::move(swapchainImage), presentSignal);
		swapchainImage = swapchain->aquireNextImage();
		queue->nextBatch();
		queue->checkForFinishedSubmits();
	}

	void waitIdle() {
		queue->waitIdle();
		queue->checkForFinishedSubmits();
		device->waitIdle();
	}

    daxa::gpu::DeviceHandle device = {};
	daxa::gpu::QueueHandle queue = {};
	daxa::gpu::SwapchainHandle swapchain = {};
	daxa::gpu::SwapchainImage swapchainImage = {};
	daxa::gpu::ImageHandle depthImage = {};
	daxa::gpu::ImageHandle normalsBuffer = {};
	daxa::gpu::SignalHandle presentSignal = {};
};