// Dear ImGui: standalone example application for Glfw + Vulkan
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

// Important note to the reader who wish to integrate imgui_impl_vulkan.cpp/.h in their own engine/app.
// - Common ImGui_ImplVulkan_XXX functions and structures are used to interface with imgui_impl_vulkan.cpp/.h.
//   You will use those if you want to use this rendering backend in your engine/app.
// - Helper ImGui_ImplVulkanH_XXX functions and structures are only used by this example (main.cpp) and by
//   the backend itself (imgui_impl_vulkan.cpp), but should PROBABLY NOT be used by your own engine/app code.
// Read comments in imgui_impl_vulkan.h.

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#include <algorithm>
#include <iostream>
#include <string>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and compatibility with old VS compilers.
// To link with VS2010-era libraries, VS2015+ requires linking with legacy_stdio_definitions.lib, which we do using this pragma.
// Your own project should not be affected, as you are likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

//#define IMGUI_UNLIMITED_FRAME_RATE
#ifdef _DEBUG
#define IMGUI_VULKAN_DEBUG_REPORT
#endif

static VkAllocationCallbacks* g_Allocator = NULL;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static int                      g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

static void check_vk_result(VkResult err)
{
    if (err == 0)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
}

#ifdef IMGUI_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData)
{
    (void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix; // Unused arguments
    fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif // IMGUI_VULKAN_DEBUG_REPORT

static void SetupVulkan(const char** extensions, uint32_t extensions_count)
{
    VkResult err;

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.enabledExtensionCount = extensions_count;
        create_info.ppEnabledExtensionNames = extensions;
#ifdef IMGUI_VULKAN_DEBUG_REPORT
        // Enabling validation layers
        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;

        // Enable debug report extension (we need additional storage, so we duplicate the user array to add our new extension to it)
        const char** extensions_ext = (const char**)malloc(sizeof(const char*) * (extensions_count + 1));
        memcpy(extensions_ext, extensions, extensions_count * sizeof(const char*));
        extensions_ext[extensions_count] = "VK_EXT_debug_report";
        create_info.enabledExtensionCount = extensions_count + 1;
        create_info.ppEnabledExtensionNames = extensions_ext;

        // Create Vulkan Instance
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);
        free(extensions_ext);

        // Get the function pointer (required for any extensions)
        auto vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(vkCreateDebugReportCallbackEXT != NULL);

        // Setup the debug report callback
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = NULL;
        err = vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#else
        // Create Vulkan Instance without any debug feature
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);
        IM_UNUSED(g_DebugReport);
#endif
    }

    // Select GPU
    {
        uint32_t gpu_count;
        err = vkEnumeratePhysicalDevices(g_Instance, &gpu_count, NULL);
        check_vk_result(err);
        IM_ASSERT(gpu_count > 0);

        VkPhysicalDevice* gpus = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * gpu_count);
        err = vkEnumeratePhysicalDevices(g_Instance, &gpu_count, gpus);
        check_vk_result(err);

        // If a number >1 of GPUs got reported, find discrete GPU if present, or use first one available. This covers
        // most common cases (multi-gpu/integrated+dedicated graphics). Handling more complicated setups (multiple
        // dedicated GPUs) is out of scope of this sample.
        int use_gpu = 0;
        for (int i = 0; i < (int)gpu_count; i++)
        {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(gpus[i], &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                use_gpu = i;
                break;
            }
        }

        g_PhysicalDevice = gpus[use_gpu];
        free(gpus);
    }

    // Select graphics queue family
    {
        uint32_t count;
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, NULL);
        VkQueueFamilyProperties* queues = (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * count);
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, queues);
        for (uint32_t i = 0; i < count; i++)
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                g_QueueFamily = i;
                break;
            }
        free(queues);
        IM_ASSERT(g_QueueFamily != (uint32_t)-1);
    }

    // Create Logical Device (with 1 queue)
    {
        int device_extension_count = 1;
        const char* device_extensions[] = { "VK_KHR_swapchain" };
        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = device_extension_count;
        create_info.ppEnabledExtensionNames = device_extensions;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    // Create Descriptor Pool
    {
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

// All the ImGui_ImplVulkanH_XXX structures/functions are optional helpers used by the demo.
// Your real engine/app may not use them.
static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
    wd->Surface = surface;

    // Check for WSI support
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE)
    {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    // Select Surface Format
    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // Select Present Mode
#ifdef IMGUI_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));
    //printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

    // Create SwapChain, RenderPass, Framebuffer, etc.
    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
}

static void CleanupVulkan()
{
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef IMGUI_VULKAN_DEBUG_REPORT
    // Remove the debug report callback
    auto vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // IMGUI_VULKAN_DEBUG_REPORT

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow()
{
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data)
{
    VkResult err;

    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Record dear imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // Submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->ImageCount; // Now we can use the next set of semaphores
}

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

// Custom system start

constexpr ImGuiWindowFlags TestWindowFlag = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration;


void BasicTestLeftWindow()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::Begin("Left Window", nullptr, TestWindowFlag);
    ImGui::End();
    ImGui::PopStyleVar(1);
}

void BasicTestRightWindow()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::Begin("Right Window", nullptr, TestWindowFlag);
    ImGui::End();
    ImGui::PopStyleVar(1);
}

void BlenderStyleTestLeftUpWindow()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::Begin("Left-Up Window", nullptr, TestWindowFlag);
    ImGui::End();
    ImGui::PopStyleVar(1);
}

void BlenderStyleTestLeftDownWindow()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::Begin("Left-Down Window", nullptr, TestWindowFlag);
    ImGui::End();
    ImGui::PopStyleVar(1);
}

void BlenderStyleTestRightUpWindow()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::Begin("Right-Up Window", nullptr, TestWindowFlag);
    ImGui::End();
    ImGui::PopStyleVar(1);
}

void BlenderStyleTestRightDownWindow()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::Begin("Right-Down Window", nullptr, TestWindowFlag);
    ImGui::End();
    ImGui::PopStyleVar(1);
}

// Set custom windows properties. Names, styles...
// Positions and sizes are handled by the layout.
typedef void (*CustomWindowFunc)();

// Leaves are windows; All others are logical domain.
// Odd level domain splitters are left-right; even level domain splitters are top-down. (Level number starting from 1)
class CustomLayoutNode
{
public:
    CustomLayoutNode(bool isLogicalDomain, 
                     uint32_t level, 
                     ImVec2 domainPos, 
                     ImVec2 domainSize, 
                     float splitterPos = -1.f, 
                     CustomWindowFunc customFunc = nullptr)
        : m_pLeft(nullptr),
          m_pRight(nullptr),
          m_level(level),
          m_domainPos(domainPos),
          m_domainSize(domainSize),
          m_splitterStartCoordinate(splitterPos),
          m_splitterWidth(2.f),
          m_pfnCustomWindowFunc(customFunc),
          m_isLogicalDomain(isLogicalDomain)
    {}

    ~CustomLayoutNode()
    {
        if (m_pLeft) 
        {
            delete m_pLeft; 
        }

        if (m_pRight)
        {
            delete m_pRight;
        }
    }

    void SetLeftChild(CustomLayoutNode* pNode) { m_pLeft = pNode; }
    void SetRightChild(CustomLayoutNode* pNode) { m_pRight = pNode; }
    CustomLayoutNode* GetLeftChild() { return m_pLeft; }
    CustomLayoutNode* GetRightChild() { return m_pRight; }
    ImVec2 GetDomainPos() { return m_domainPos; }
    ImVec2 GetDomainSize() { return m_domainSize; }
    uint32_t GetLevel() { return m_level; }

    ImVec2 GetSplitterPos() 
    { 
        if (m_isLogicalDomain)
        {
            if (m_level % 2 == 1)
            {
                // Left-right splitter
                return ImVec2(m_domainPos.x + m_splitterStartCoordinate, m_domainPos.y);
            }
            else
            {
                // Top-down splitter
                return ImVec2(m_domainPos.x, m_domainPos.y + m_splitterStartCoordinate);
            }
        }
        else
        {
            assert("NOTE in GetSplitterPos(): This is not a splitter.");
            return ImVec2(-1.f, -1.f);
        }
    }

    ImVec2 GetSplitterSize()
    {
        if (m_isLogicalDomain)
        {
            if (m_level % 2 == 1)
            {
                // Left-right splitter
                return ImVec2(m_splitterWidth, m_domainSize.y);
            }
            else
            {
                // Top-down splitter
                return ImVec2(m_domainSize.x, m_splitterWidth);
            }
        }
        else
        {
            assert("NOTE in GetSplitterPos(): This is not a splitter.");
            return ImVec2(-1.f, -1.f);
        }
    }

    void BeginEndNodeAndChildren()
    {
        if ((m_pLeft != nullptr) || (m_pRight != nullptr))
        {
            // It is a logical domain. Calling its children instead.
            if (m_pLeft)
            {
                m_pLeft->BeginEndNodeAndChildren();
            }

            if (m_pRight)
            {
                m_pRight->BeginEndNodeAndChildren();
            }
        }
        else
        {
            // Begin the window itself. Calling the custom function pointer.
            ImGui::SetNextWindowPos(m_domainPos);
            ImGui::SetNextWindowSize(m_domainSize);

            // Set custom windows properties.
            if (m_pfnCustomWindowFunc)
            {
                m_pfnCustomWindowFunc();
            }
        }
    }

    CustomLayoutNode* GetHoverSplitter()
    {

    }

    void BuildWindows()
    {

    }

private:
    CustomLayoutNode* m_pLeft;  // Or top for the even level splitters.
    CustomLayoutNode* m_pRight; // Or down for the even level splitters.
    uint32_t          m_level;  // Used to determine whether it is a vertical splitter or a horizontal splitter. 
                                // Only for a splitter.
    
    // Domain represents the screen area that a node occpies. For windows, their domains are just same as the the their
    // starting position and size. But for splitters, their domains represent the area it splits
    ImVec2 m_domainPos;  
    ImVec2 m_domainSize;

    float    m_splitterStartCoordinate;

    const float m_splitterWidth;

    CustomWindowFunc m_pfnCustomWindowFunc;

    bool m_isLogicalDomain;
};

// We only need to build the splitter structure at first. We can auto-generate windows from the splitters.
class CustomLayout
{
public:
    CustomLayout()
        : m_pRoot(nullptr),
          m_splitterXCoordinate(-1.f),
          m_splitterHeld(false),
          m_splitterBottonDownDelta(ImVec2(0.f, 0.f)),
          m_lastViewport(ImVec2(0.f, 0.f)),
          m_heldMouseCursor(0)
    {
        
    }

    // A splitter in middle. Thin right window and wider left window.
    void TestingLayout()
    {
        ImGuiViewport* pViewport = ImGui::GetMainViewport();

        // Basic testing layout - Note: We first test the rendering (BeginEndNodeAndChildren(No Arg)). Then, test the
        // interaction. BeginEndNodeAndChildren(Current Cursior). Finally test the splitter build (BuildWindows()).

        // Central domain and its splitter.
        m_pRoot = new CustomLayoutNode(true, 1, pViewport->WorkPos, pViewport->WorkSize, 0.8f * pViewport->WorkSize.x);

        ImVec2 splitterPos = m_pRoot->GetSplitterPos();
        ImVec2 splitterSize = m_pRoot->GetSplitterSize();

        m_pRoot->SetLeftChild(new CustomLayoutNode(false, 2, pViewport->WorkPos, ImVec2(splitterPos.x, splitterSize.y),
                                                   -1.f, BasicTestLeftWindow));
        m_pRoot->SetRightChild(new CustomLayoutNode(false, 2, ImVec2(splitterPos.x + splitterSize.x, splitterPos.y),
                                                    ImVec2(pViewport->WorkSize.x - splitterPos.x - splitterSize.x,
                                                           splitterSize.y), -1.f, BasicTestRightWindow));
        
    }


    // 
    void BlenderStartLayout()
    {
        ImGuiViewport* pViewport = ImGui::GetMainViewport();

        // Blender default GUI layout
        m_pRoot = new CustomLayoutNode(true, 1, pViewport->WorkPos, pViewport->WorkSize, 0.8f * pViewport->WorkSize.x);

        ImVec2 rootSplitterPos = m_pRoot->GetSplitterPos();
        ImVec2 rootSplitterSize = m_pRoot->GetSplitterSize();

        // Left and right splitter
        m_pRoot->SetLeftChild(new CustomLayoutNode(true, 2, pViewport->WorkPos,
                                                   ImVec2(rootSplitterPos.x, rootSplitterSize.y), 
                                                   0.8f * pViewport->WorkSize.y));

        m_pRoot->SetRightChild(new CustomLayoutNode(false, 2, ImVec2(rootSplitterPos.x + rootSplitterSize.x, rootSplitterPos.y),
                                                    ImVec2(pViewport->WorkSize.x - rootSplitterPos.x - rootSplitterSize.x,
                                                           rootSplitterSize.y), -1.f, BasicTestRightWindow));
        /*
        m_pRoot->SetRightChild(new CustomLayoutNode(true, 2, pViewport->WorkPos, 
                                                    ImVec2(rootSplitterPos.x + rootSplitterSize.x, rootSplitterSize.y),
                                                    0.2f * pViewport->WorkSize.y));
        */

        // Left splitter's top and bottom windows
        CustomLayoutNode* pLeftDomain = m_pRoot->GetLeftChild();
        ImVec2 leftSplitterPos = pLeftDomain->GetSplitterPos();
        ImVec2 leftSplitterSize = pLeftDomain->GetSplitterSize();

        pLeftDomain->SetLeftChild(new CustomLayoutNode(false, 3, pLeftDomain->GetDomainPos(),
                                                       ImVec2(pLeftDomain->GetDomainSize().x, leftSplitterPos.y),
                                                       -1.f, BlenderStyleTestLeftUpWindow));
        pLeftDomain->SetRightChild(new CustomLayoutNode(false, 3, ImVec2(pLeftDomain->GetDomainPos().x, leftSplitterPos.y + leftSplitterSize.y),
                                                        ImVec2(leftSplitterSize.x, pLeftDomain->GetDomainSize().y - leftSplitterPos.y - leftSplitterSize.y),
                                                        -1.f, BlenderStyleTestLeftDownWindow));
            
        
        

        // Right splitter's top and bottom windows
        /*
        CustomLayoutNode* pRightDomain = m_pRoot->GetRightChild();
        ImVec2 rightSplitterPos = pRightDomain->GetSplitterPos();
        ImVec2 rightSplitterSize = pRightDomain->GetSplitterSize();

        pRightDomain->SetLeftChild(new CustomLayoutNode(false, 3, pRightDomain->GetDomainPos(),
                                                        ImVec2(pRightDomain->GetDomainSize().x, rightSplitterPos.y),
                                                        -1.f, BlenderStyleTestRightUpWindow));
        pRightDomain->SetRightChild(new CustomLayoutNode(false, 3, ImVec2(pRightDomain->GetDomainPos().x, rightSplitterPos.y + rightSplitterSize.y),
                                                         ImVec2(rightSplitterSize.x, pRightDomain->GetDomainSize().y - rightSplitterPos.y - rightSplitterSize.y),
                                                         -1.f, BlenderStyleTestRightDownWindow));
        */
    }

    // Update Dear ImGUI state
    void BeginEndLayout()
    {
        /*
        ImGuiViewport* pViewport = ImGui::GetMainViewport();

        // Dealing with the mouse interactions.
        if (m_splitterHeld == false)
        {
            CustomLayoutNode* pSplitterDomain = m_pRoot->GetHoverSplitter();
            if (pSplitterDomain)
            {
                // If the mouse cursor hovers on a splitter, then we need to change the appearance of the cursor and
                // check whether there is a click event in the event queue. If there is a click event, then the
                // splitter is occupied by the mouse.
                bool isLeftRightSplitter = (pSplitterDomain->GetLevel() % 2 == 1);

                isLeftRightSplitter ? ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW) : 
                                      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    m_splitterHeld = true;

                    ImVec2 splitterPos = pSplitterDomain->GetSplitterPos();
                    ImVec2 mousePos = ImGui::GetMousePos();
                    m_splitterBottonDownDelta = ImVec2(splitterPos.x - mousePos.x, splitterPos.y - mousePos.y);
                    
                    m_pHeldSplitterDomain = pSplitterDomain;
                }
            }
        }
        else
        {

        }
        */
        // Putting windows data into Dear ImGui's state.
        if (m_pRoot != nullptr)
        {
            m_pRoot->BeginEndNodeAndChildren();
        }

        // m_lastViewport = pViewport->WorkSize;
    }

    ~CustomLayout()
    {
        if (m_pRoot)
        {
            delete m_pRoot;
        }
    }

    CustomLayoutNode* m_pRoot;

    float m_splitterXCoordinate;

    bool m_splitterHeld;
    ImVec2 m_splitterBottonDownDelta;
    int m_heldMouseCursor; // ImGuiMouseCursor_ Enum.
    CustomLayoutNode* m_pHeldSplitterDomain;

    ImVec2 m_lastViewport;
};

namespace ImGui
{
    IMGUI_API bool BeginBottomMainMenuBar();
}

bool ImGui::BeginBottomMainMenuBar()
{
    ImGuiContext& g = *GImGui;
    ImGuiViewportP* viewport = (ImGuiViewportP*)(void*)GetMainViewport();
    // For the main menu bar, which cannot be moved, we honor g.Style.DisplaySafeAreaPadding to ensure text can be visible on a TV set.
    // FIXME: This could be generalized as an opt-in way to clamp window->DC.CursorStartPos to avoid SafeArea?
    // FIXME: Consider removing support for safe area down the line... it's messy. Nowadays consoles have support for TV calibration in OS settings.
    g.NextWindowData.MenuBarOffsetMinVal = ImVec2(g.Style.DisplaySafeAreaPadding.x, ImMax(g.Style.DisplaySafeAreaPadding.y - g.Style.FramePadding.y, 0.0f));
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    float height = GetFrameHeight();
    bool is_open = BeginViewportSideBar("##BottomMainMenuBar", viewport, ImGuiDir_Down, height, window_flags);
    g.NextWindowData.MenuBarOffsetMinVal = ImVec2(0.0f, 0.0f);

    if (is_open)
        BeginMenuBar();
    else
        End();
    return is_open;
}

int main(int, char**)
{
    // Setup GLFW window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Dear ImGui GLFW+Vulkan example", NULL, NULL);

    // Setup Vulkan
    if (!glfwVulkanSupported())
    {
        printf("GLFW: Vulkan Not Supported\n");
        return 1;
    }
    uint32_t extensions_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    SetupVulkan(extensions, extensions_count);

    // Create Window Surface
    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    check_vk_result(err);

    // Create Framebuffers
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.Subpass = 0;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = g_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info, wd->RenderPass);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != NULL);

    // Upload Fonts
    {
        // Use any command queue
        VkCommandPool command_pool = wd->Frames[wd->FrameIndex].CommandPool;
        VkCommandBuffer command_buffer = wd->Frames[wd->FrameIndex].CommandBuffer;

        err = vkResetCommandPool(g_Device, command_pool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(command_buffer, &begin_info);
        check_vk_result(err);

        ImGui_ImplVulkan_CreateFontsTexture(command_buffer);

        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &command_buffer;
        err = vkEndCommandBuffer(command_buffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &end_info, VK_NULL_HANDLE);
        check_vk_result(err);

        err = vkDeviceWaitIdle(g_Device);
        check_vk_result(err);
        ImGui_ImplVulkan_DestroyFontUploadObjects();
    }

    // Our state
    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // My Hack
    ImGuiViewport* pViewport = ImGui::GetMainViewport();
    bool firstFrame = true;

    CustomLayout myLayout;

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwPollEvents();

        // Resize swap chain?
        if (g_SwapChainRebuild)
        {
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            if (width > 0 && height > 0)
            {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
                g_MainWindowData.FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
        }

        // Start the Dear ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        /* Newly Added */

        // Main menu bars.
        {
            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Edit"))
                {
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }

            if (ImGui::BeginBottomMainMenuBar())
            {
                ImGui::EndMainMenuBar();
            }
        }

        
        if (firstFrame)
        {
            // myLayout.TestingLayout();
            myLayout.BlenderStartLayout();
            firstFrame = false;
        }
        
        myLayout.BeginEndLayout();

        /*
        if (ImGui::IsMouseHoveringRect(ImVec2(g_layout.m_splitterXCoordinate, pViewport->WorkPos.y),
                                       ImVec2(g_layout.m_splitterXCoordinate + 5.f, pViewport->WorkPos.y + pViewport->WorkSize.y), false))
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (g_layout.m_splitterXHeld == false)
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    g_layout.m_splitterXHeld = true;
                    g_layout.m_splitterBottonDownTLXDelta = g_layout.m_splitterXCoordinate - ImGui::GetMousePos().x;
                }
            }
        }

        if (g_layout.m_splitterXHeld)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                g_layout.m_splitterXCoordinate = ImGui::GetMousePos().x + g_layout.m_splitterBottonDownTLXDelta;
                g_layout.m_splitterXCoordinate = std::clamp(g_layout.m_splitterXCoordinate, 0.1f * pViewport->WorkSize.x, 0.9f * pViewport->WorkSize.x);
            }
            else
            {
                g_layout.m_splitterXHeld = false;
            }
        }
        else
        {
            if ((g_layout.m_lastViewport.x != pViewport->WorkSize.x) || (g_layout.m_lastViewport.y != pViewport->WorkSize.y))
            {
                float ratio = g_layout.m_splitterXCoordinate / g_layout.m_lastViewport.x;
                g_layout.m_splitterXCoordinate = ratio * pViewport->WorkSize.x;
                g_layout.m_splitterXCoordinate = std::clamp(g_layout.m_splitterXCoordinate, 0.1f * pViewport->WorkSize.x, 0.9f * pViewport->WorkSize.x);
            }
        }
        g_layout.m_lastViewport = pViewport->WorkSize;
        

        ImGuiWindowFlags WindowFlag = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration;
        ImGui::SetNextWindowPos(ImVec2(pViewport->WorkPos));
        ImVec2 leftWinSize = ImVec2(g_layout.m_splitterXCoordinate, pViewport->WorkSize.y);
        ImGui::SetNextWindowSize(leftWinSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::Begin("Window Left", &show_another_window, WindowFlag);   // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
        ImGui::End();
        ImGui::PopStyleVar(1);

        float rightWinStartPosX = g_layout.m_splitterXCoordinate + g_layout.m_splitterWidth;
        ImGui::SetNextWindowPos(ImVec2(rightWinStartPosX, pViewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(pViewport->WorkSize.x - rightWinStartPosX, pViewport->WorkSize.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::Begin("Window Right", &show_another_window, WindowFlag);   // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
        ImGui::End();
        ImGui::PopStyleVar(1);
        */


        /***************/

        
        // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);
        /*
        // 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
        {
            static float f = 0.0f;
            static int counter = 0;

            ImGui::Begin("Hello, world!");                          // Create a window called "Hello, world!" and append into it.

            ImGui::Text("This is some useful text.");               // Display some text (you can use a format strings too)
            ImGui::Checkbox("Demo Window", &show_demo_window);      // Edit bools storing our window open/close state
            ImGui::Checkbox("Another Window", &show_another_window);

            ImGui::SliderFloat("float", &f, 0.0f, 1.0f);            // Edit 1 float using a slider from 0.0f to 1.0f
            ImGui::ColorEdit3("clear color", (float*)&clear_color); // Edit 3 floats representing a color

            if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
                counter++;
            ImGui::SameLine();
            ImGui::Text("counter = %d", counter);

            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
            ImGui::End();
        }

        // 3. Show another simple window.
        if (show_another_window)
        {
            ImGui::Begin("Another Window", &show_another_window);   // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
            ImGui::Text("Hello from another window!");
            if (ImGui::Button("Close Me"))
                show_another_window = false;
            ImGui::End();
        }
        */

        // Rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            wd->ClearValue.color.float32[3] = clear_color.w;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
    }

    // Cleanup
    err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow();
    CleanupVulkan();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
