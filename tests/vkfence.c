/* vkfence.c - minimal Vulkan fence round-trip, for isolating the Bitwig/Skia "Got an
 * invalid fence status" under the Linuxulator.  dlopen()s libvulkan, so it needs no
 * link-time Vulkan.  Result codes: 0=SUCCESS, 1=NOT_READY, -13=ERROR_UNKNOWN. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(void)
{
        const char *pause_s = getenv("VKFENCE_SLEEP");
        if (pause_s != NULL)
                sleep((unsigned)atoi(pause_s));
        void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
        if (lib == NULL) {
                printf("no libvulkan.so.1: %s\n", dlerror());
                return 2;
        }
        PFN_vkGetInstanceProcAddr gipa =
            (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
        PFN_vkCreateInstance create_instance =
            (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

        VkApplicationInfo app = { 0 };
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "vkfence";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici = { 0 };
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;

        VkInstance inst = VK_NULL_HANDLE;
        VkResult r = create_instance(&ici, NULL, &inst);
        printf("createInstance: %d\n", r);
        if (r != VK_SUCCESS)
                return 1;

        PFN_vkEnumeratePhysicalDevices enum_phys =
            (PFN_vkEnumeratePhysicalDevices)gipa(inst,
            "vkEnumeratePhysicalDevices");
        uint32_t npd = 0;
        enum_phys(inst, &npd, NULL);
        VkPhysicalDevice *pds = calloc(npd, sizeof(*pds));
        enum_phys(inst, &npd, pds);
        if (npd == 0) {
                printf("no physical devices\n");
                return 1;
        }
        VkPhysicalDevice pd = pds[0];
        PFN_vkGetPhysicalDeviceProperties get_props =
            (PFN_vkGetPhysicalDeviceProperties)gipa(inst,
            "vkGetPhysicalDeviceProperties");
        VkPhysicalDeviceProperties props;
        get_props(pd, &props);
        printf("device0: %s (type %d)\n", props.deviceName, props.deviceType);

        PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qfp =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst,
            "vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t nq = 0;
        get_qfp(pd, &nq, NULL);
        VkQueueFamilyProperties *qfp = calloc(nq, sizeof(*qfp));
        get_qfp(pd, &nq, qfp);
        uint32_t qf = 0;
        for (uint32_t i = 0; i < nq; i++) {
                if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                        qf = i;
                        break;
                }
        }

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = { 0 };
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qf;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkPhysicalDeviceFeatures feats = { 0 };
        VkDeviceCreateInfo dci = { 0 };
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &feats;
        PFN_vkCreateDevice create_device =
            (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
        VkDevice dev = VK_NULL_HANDLE;
        r = create_device(pd, &dci, NULL, &dev);
        printf("createDevice: %d\n", r);
        if (r != VK_SUCCESS)
                return 1;

        PFN_vkGetDeviceProcAddr gdpa =
            (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define GETD(T, n) T n = (T)gdpa(dev, #n)
        GETD(PFN_vkGetDeviceQueue, vkGetDeviceQueue);
        GETD(PFN_vkCreateCommandPool, vkCreateCommandPool);
        GETD(PFN_vkAllocateCommandBuffers, vkAllocateCommandBuffers);
        GETD(PFN_vkBeginCommandBuffer, vkBeginCommandBuffer);
        GETD(PFN_vkEndCommandBuffer, vkEndCommandBuffer);
        GETD(PFN_vkCreateFence, vkCreateFence);
        GETD(PFN_vkGetFenceStatus, vkGetFenceStatus);
        GETD(PFN_vkWaitForFences, vkWaitForFences);
        GETD(PFN_vkQueueSubmit, vkQueueSubmit);
#undef GETD

        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(dev, qf, 0, &queue);

        VkCommandPoolCreateInfo pci = { 0 };
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = qf;
        VkCommandPool pool = VK_NULL_HANDLE;
        r = vkCreateCommandPool(dev, &pci, NULL, &pool);
        printf("createCommandPool: %d\n", r);

        VkCommandBufferAllocateInfo ai = { 0 };
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        r = vkAllocateCommandBuffers(dev, &ai, &cb);
        printf("allocateCommandBuffer: %d\n", r);

        VkCommandBufferBeginInfo bi = { 0 };
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &bi);
        vkEndCommandBuffer(cb);

        VkFenceCreateInfo fi = { 0 };
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = 0;
        VkFence fence = VK_NULL_HANDLE;
        r = vkCreateFence(dev, &fi, NULL, &fence);
        printf("createFence: %d\n", r);

        printf("getFenceStatus(before submit): %d\n",
            vkGetFenceStatus(dev, fence));

        VkSubmitInfo si = { 0 };
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        r = vkQueueSubmit(queue, 1, &si, fence);
        printf("queueSubmit: %d\n", r);

        r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ULL);
        printf("waitForFences: %d\n", r);
        printf("getFenceStatus(after): %d\n", vkGetFenceStatus(dev, fence));
        return 0;
}
