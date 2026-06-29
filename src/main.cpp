// vulkan-water — C++/Vulkan port of Evan Wallace's "WebGL Water".
// Zero-dependency: native Win32 windowing (no GLFW), own math (no GLM), own
// glTF loader (no cgltf). Hand-rolled Vulkan init and allocator. Features:
// heightfield water sim, raytraced reflection/refraction, real-time caustics,
// soft sphere shadows, box & cylinder pools, glTF objects.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "math3d.h"
#include "gltf_min.h"
#include "udf_bake.h"
#include "png_decode.h"

#include <vector>
#include <array>
#include <set>
#include <string>
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <functional>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <optional>
#include <chrono>

#ifndef SHADER_DIR
#define SHADER_DIR "shaders"
#endif

#define VK_CHECK(x) do { VkResult e=(x); if(e){ std::cerr<<"Vulkan error "<<e<<" at "<<__FILE__<<":"<<__LINE__<<"\n"; std::abort(); } } while(0)

static constexpr uint32_t WATER_RES     = 256;
static constexpr uint32_t CAUSTIC_RES   = 1024;
static constexpr uint32_t GRID_N        = 200;
static constexpr float    SPHERE_RADIUS = 0.25f;

struct AllocatedBuffer { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkDeviceSize size=0; void* mapped=nullptr; };
struct AllocatedImage  { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; VkFormat format=VK_FORMAT_UNDEFINED; VkExtent2D extent={}; };

struct SceneUBO {
    glm::mat4 viewProj;
    glm::mat4 objectModel;
    glm::vec4 eye;
    glm::vec4 light;
    glm::vec4 sphere;
    glm::vec4 misc;     // x=time, y=poolShape
    glm::vec4 objMin;   // xyz = world-space AABB min of the object, w = hasObject (1/0)
    glm::vec4 objMax;   // xyz = world-space AABB max of the object, w = voxel size (world units)
};
struct SimPush { glm::vec4 p0; glm::vec4 p1; };

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[validation] " << data->pMessage << "\n";
    return VK_FALSE;
}

class WaterApp {
public:
    std::string modelPath;
    void run(){ initWindow(); initVulkan(); mainLoop(); cleanup(); }
private:
    HWND hwnd=nullptr;
    HINSTANCE hinstance=nullptr;
    bool running=true;
    uint32_t width=1280, height=800;
    bool framebufferResized=false;

    float camYaw=0.6f, camPitch=0.55f, camDist=4.0f;
    glm::vec3 camTarget{0.0f,-0.3f,0.0f};
    bool dragging=false, drawingRipples=false;
    double lastX=0.0, lastY=0.0;
    bool paused=false, gravity=false;
    int  poolShape=0; // 0=box, 1=cylinder
    glm::vec3 lightDir=glm::normalize(glm::vec3(2.0f,2.0f,-1.0f));

    glm::vec3 sphereCenter{-0.4f,0.0f,0.2f};
    glm::vec3 spherePrev=sphereCenter;
    glm::vec3 sphereVel{0.0f};
    std::vector<glm::vec2> pendingDrops;

    // hand-rolled core handles
    VkInstance instance=VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger=VK_NULL_HANDLE;
    bool validationEnabled=false;
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    uint32_t graphicsFamily=0, presentFamily=0;
    VkQueue graphicsQueue=VK_NULL_HANDLE, presentQueue=VK_NULL_HANDLE;

    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkFormat swapFormat=VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent={};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    AllocatedImage depthImage;
    std::vector<VkFramebuffer> mainFramebuffers;

    VkRenderPass simPass=VK_NULL_HANDLE, causticPass=VK_NULL_HANDLE, mainPass=VK_NULL_HANDLE;

    AllocatedImage water[2];
    VkFramebuffer waterFB[2]={VK_NULL_HANDLE,VK_NULL_HANDLE};
    int current=0;
    AllocatedImage caustic; VkFramebuffer causticFB=VK_NULL_HANDLE;
    AllocatedImage tiles;

    VkSampler sampler=VK_NULL_HANDLE, tileSampler=VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkDescriptorPool descPool=VK_NULL_HANDLE;
    VkDescriptorSet descSet[2]={VK_NULL_HANDLE,VK_NULL_HANDLE};

    AllocatedBuffer uboBuffer;
    VkPipelineLayout simLayout=VK_NULL_HANDLE, gfxLayout=VK_NULL_HANDLE;
    VkPipeline pipeDrop=VK_NULL_HANDLE, pipeUpdate=VK_NULL_HANDLE, pipeNormal=VK_NULL_HANDLE, pipeSphereDisp=VK_NULL_HANDLE;
    VkPipeline pipeCaustic=VK_NULL_HANDLE, pipePool=VK_NULL_HANDLE, pipeSphere=VK_NULL_HANDLE, pipeWater=VK_NULL_HANDLE, pipeObject=VK_NULL_HANDLE;

    AllocatedBuffer gridVB, gridIB; uint32_t gridIndexCount=0;
    AllocatedBuffer poolBoxVB, poolBoxIB; uint32_t poolBoxCount=0;
    AllocatedBuffer poolCylVB, poolCylIB; uint32_t poolCylCount=0;
    AllocatedBuffer sphereVB, sphereIB; uint32_t sphereIndexCount=0;
    AllocatedBuffer objectVB, objectIB; uint32_t objectIndexCount=0;
    bool hasObject=false; glm::mat4 objectBaseModel{1.0f};
    AllocatedImage udf;                 // unsigned distance field of the loaded object (3D)
    AllocatedImage objTex;              // base-colour texture of the loaded object (2D, RGBA8)
    glm::vec3 objLocalMin{0.0f}, objLocalMax{0.0f};  // object AABB in object-base space (world = +sphereCenter)
    float objVoxel=1.0f;

    VkCommandPool cmdPool=VK_NULL_HANDLE;
    VkCommandBuffer cmd=VK_NULL_HANDLE;
    VkSemaphore acquireSem=VK_NULL_HANDLE;
    std::vector<VkSemaphore> renderFinishedSems;
    VkFence inFlight=VK_NULL_HANDLE;
    std::chrono::high_resolution_clock::time_point startTime;

    // ---------------- window / input (native Win32) ----------------
    void initWindow(){
        hinstance=GetModuleHandle(nullptr);
        WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc);
        wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=WndProc; wc.hInstance=hinstance;
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.lpszClassName=L"VulkanWaterWindow";
        RegisterClassExW(&wc);
        RECT r{0,0,(LONG)width,(LONG)height};
        AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
        hwnd=CreateWindowExW(0,L"VulkanWaterWindow",L"vulkan-water",WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,nullptr,nullptr,hinstance,this);
        ShowWindow(hwnd,SW_SHOW);
        RECT cr; GetClientRect(hwnd,&cr); width=(uint32_t)(cr.right-cr.left); height=(uint32_t)(cr.bottom-cr.top);
        startTime=std::chrono::high_resolution_clock::now();
    }
    static LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
        if(msg==WM_NCCREATE){
            auto* cs=reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtr(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* a=reinterpret_cast<WaterApp*>(GetWindowLongPtr(h,GWLP_USERDATA));
        if(a) return a->handleMsg(h,msg,wp,lp);
        return DefWindowProc(h,msg,wp,lp);
    }
    LRESULT handleMsg(HWND h,UINT msg,WPARAM wp,LPARAM lp){
        switch(msg){
        case WM_SIZE: {
            uint32_t w=(uint32_t)LOWORD(lp), ht=(uint32_t)HIWORD(lp);
            if(w&&ht){ width=w; height=ht; framebufferResized=true; }
            return 0; }
        case WM_LBUTTONDOWN: {
            lastX=(double)GET_X_LPARAM(lp); lastY=(double)GET_Y_LPARAM(lp);
            glm::vec2 xz; SetCapture(h);
            if(pickWater(lastX,lastY,xz)){ drawingRipples=true; pendingDrops.push_back(xz); }
            else dragging=true;
            return 0; }
        case WM_LBUTTONUP: dragging=false; drawingRipples=false; ReleaseCapture(); return 0;
        case WM_MOUSEMOVE: {
            double x=(double)GET_X_LPARAM(lp), y=(double)GET_Y_LPARAM(lp);
            double dx=x-lastX, dy=y-lastY; lastX=x; lastY=y;
            if(dragging){ camYaw+=(float)dx*0.01f; camPitch+=(float)dy*0.01f; camPitch=glm::clamp(camPitch,-1.5f,1.5f); }
            else if(drawingRipples){ glm::vec2 xz; if(pickWater(x,y,xz)) pendingDrops.push_back(xz); }
            return 0; }
        case WM_MOUSEWHEEL: {
            float yoff=(float)GET_WHEEL_DELTA_WPARAM(wp)/120.0f;
            camDist=glm::clamp(camDist-yoff*0.2f,1.5f,12.0f);
            return 0; }
        case WM_KEYDOWN:
            if(wp==VK_SPACE) paused=!paused;
            else if(wp=='G') gravity=!gravity;
            else if(wp=='L') lightDir=cameraForward();
            else if(wp=='P') poolShape=(poolShape+1)%2;
            else if(wp==VK_ESCAPE) running=false;
            return 0;
        case WM_CLOSE:   running=false; return 0;
        case WM_DESTROY: running=false; PostQuitMessage(0); return 0;
        }
        return DefWindowProc(h,msg,wp,lp);
    }
    glm::vec3 cameraEye() const {
        glm::vec3 d{ std::cos(camPitch)*std::sin(camYaw), std::sin(camPitch), std::cos(camPitch)*std::cos(camYaw) };
        return camTarget+d*camDist;
    }
    glm::vec3 cameraForward() const { return glm::normalize(camTarget-cameraEye()); }
    glm::mat4 viewMatrix() const { return glm::lookAt(cameraEye(),camTarget,glm::vec3(0,1,0)); }
    glm::mat4 projMatrix() const {
        glm::mat4 p=glm::perspective(glm::radians(50.0f),(float)swapExtent.width/(float)swapExtent.height,0.05f,100.0f);
        p[1][1]*=-1.0f; return p;
    }
    bool pickWater(double sx,double sy,glm::vec2& out){
        if(swapExtent.width==0||swapExtent.height==0) return false;
        float ndcX=2.0f*(float)sx/(float)width-1.0f, ndcY=2.0f*(float)sy/(float)height-1.0f;
        glm::mat4 invVP=glm::inverse(projMatrix()*viewMatrix());
        glm::vec4 pn=invVP*glm::vec4(ndcX,ndcY,0.0f,1.0f), pf=invVP*glm::vec4(ndcX,ndcY,1.0f,1.0f);
        pn/=pn.w; pf/=pf.w;
        glm::vec3 o=glm::vec3(pn), d=glm::normalize(glm::vec3(pf)-glm::vec3(pn));
        if(std::abs(d.y)<1e-5f) return false;
        float t=-o.y/d.y; if(t<0.0f) return false;
        glm::vec3 hit=o+d*t;
        float lim = (poolShape==1) ? (hit.x*hit.x+hit.z*hit.z) : std::max(std::abs(hit.x),std::abs(hit.z));
        if(poolShape==1){ if(lim>1.0f) return false; } else if(lim>1.0f) return false;
        out=glm::vec2(hit.x,hit.z); return true;
    }

    // ---------------- hand-rolled Vulkan init ----------------
    void initVulkan(){
        createInstance(); setupDebug(); createSurface();
        pickPhysicalDevice(); createLogicalDevice(); createSwapchain();
        createCommandPool();
        createRenderPasses(); createDepthAndFramebuffers();
        createWaterResources(); createTilesTexture();
        createSamplersAndDescriptors(); createPipelines();
        loadObject(); createGeometry(); createSync(); clearWaterImages();
    }
    bool hasLayer(const char* name){
        uint32_t n=0; vkEnumerateInstanceLayerProperties(&n,nullptr);
        std::vector<VkLayerProperties> l(n); vkEnumerateInstanceLayerProperties(&n,l.data());
        for(auto& p:l) if(std::strcmp(p.layerName,name)==0) return true; return false;
    }
    void createInstance(){
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName="vulkan-water"; app.apiVersion=VK_API_VERSION_1_2;
        std::vector<const char*> exts{ VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        const char* vlayer="VK_LAYER_KHRONOS_validation";
        validationEnabled=hasLayer(vlayer);
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo=&app;
        ci.enabledExtensionCount=(uint32_t)exts.size(); ci.ppEnabledExtensionNames=exts.data();
        VkDebugUtilsMessengerCreateInfoEXT dci=debugInfo();
        if(validationEnabled){ ci.enabledLayerCount=1; ci.ppEnabledLayerNames=&vlayer; ci.pNext=&dci; }
        VK_CHECK(vkCreateInstance(&ci,nullptr,&instance));
    }
    VkDebugUtilsMessengerCreateInfoEXT debugInfo(){
        VkDebugUtilsMessengerCreateInfoEXT d{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        d.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        d.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        d.pfnUserCallback=debugCallback; return d;
    }
    void setupDebug(){
        if(!validationEnabled) return;
        auto fn=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");
        if(fn){ VkDebugUtilsMessengerCreateInfoEXT d=debugInfo(); fn(instance,&d,nullptr,&debugMessenger); }
    }
    void createSurface(){
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance=hinstance; ci.hwnd=hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance,&ci,nullptr,&surface));
    }

    bool findQueues(VkPhysicalDevice dev,uint32_t& gfx,uint32_t& pres){
        uint32_t n=0; vkGetPhysicalDeviceQueueFamilyProperties(dev,&n,nullptr);
        std::vector<VkQueueFamilyProperties> q(n); vkGetPhysicalDeviceQueueFamilyProperties(dev,&n,q.data());
        std::optional<uint32_t> g,p;
        for(uint32_t i=0;i<n;++i){
            if(q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) g=i;
            VkBool32 ps=VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(dev,i,surface,&ps);
            if(ps) p=i;
            if(g&&p) break;
        }
        if(!g||!p) return false; gfx=*g; pres=*p; return true;
    }
    bool deviceHasSwapchain(VkPhysicalDevice dev){
        uint32_t n=0; vkEnumerateDeviceExtensionProperties(dev,nullptr,&n,nullptr);
        std::vector<VkExtensionProperties> e(n); vkEnumerateDeviceExtensionProperties(dev,nullptr,&n,e.data());
        for(auto& x:e) if(std::strcmp(x.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME)==0) return true; return false;
    }
    void pickPhysicalDevice(){
        uint32_t n=0; vkEnumeratePhysicalDevices(instance,&n,nullptr);
        if(n==0) throw std::runtime_error("no Vulkan GPUs");
        std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(instance,&n,devs.data());
        VkPhysicalDevice fallback=VK_NULL_HANDLE;
        for(auto d:devs){
            uint32_t g,p; if(!deviceHasSwapchain(d)||!findQueues(d,g,p)) continue;
            VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(d,&pr);
            if(pr.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){ physicalDevice=d; graphicsFamily=g; presentFamily=p; break; }
            if(fallback==VK_NULL_HANDLE){ fallback=d; graphicsFamily=g; presentFamily=p; }
        }
        if(physicalDevice==VK_NULL_HANDLE) physicalDevice=fallback;
        if(physicalDevice==VK_NULL_HANDLE) throw std::runtime_error("no suitable GPU");
    }
    void createLogicalDevice(){
        std::set<uint32_t> fams={graphicsFamily,presentFamily};
        std::vector<VkDeviceQueueCreateInfo> qs; float prio=1.0f;
        for(uint32_t f:fams){ VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            q.queueFamilyIndex=f; q.queueCount=1; q.pQueuePriorities=&prio; qs.push_back(q); }
        const char* devExt=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkPhysicalDeviceFeatures feats{}; feats.fillModeNonSolid=VK_TRUE;
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount=(uint32_t)qs.size(); ci.pQueueCreateInfos=qs.data();
        ci.enabledExtensionCount=1; ci.ppEnabledExtensionNames=&devExt; ci.pEnabledFeatures=&feats;
        VK_CHECK(vkCreateDevice(physicalDevice,&ci,nullptr,&device));
        vkGetDeviceQueue(device,graphicsFamily,0,&graphicsQueue);
        vkGetDeviceQueue(device,presentFamily,0,&presentQueue);
    }
    void createSwapchain(){
        VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice,surface,&caps);
        uint32_t fn=0; vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice,surface,&fn,nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fn); vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice,surface,&fn,fmts.data());
        VkSurfaceFormatKHR chosen=fmts[0];
        for(auto& f:fmts) if(f.format==VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen=f;
        swapFormat=chosen.format;
        if(caps.currentExtent.width!=UINT32_MAX) swapExtent=caps.currentExtent;
        else{ RECT cr; GetClientRect(hwnd,&cr); uint32_t w=(uint32_t)(cr.right-cr.left), h=(uint32_t)(cr.bottom-cr.top);
            swapExtent.width=std::clamp(w,caps.minImageExtent.width,caps.maxImageExtent.width);
            swapExtent.height=std::clamp(h,caps.minImageExtent.height,caps.maxImageExtent.height); }
        uint32_t imgCount=caps.minImageCount+1;
        if(caps.maxImageCount>0 && imgCount>caps.maxImageCount) imgCount=caps.maxImageCount;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface=surface; ci.minImageCount=imgCount; ci.imageFormat=swapFormat; ci.imageColorSpace=chosen.colorSpace;
        ci.imageExtent=swapExtent; ci.imageArrayLayers=1; ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        uint32_t fi[2]={graphicsFamily,presentFamily};
        if(graphicsFamily!=presentFamily){ ci.imageSharingMode=VK_SHARING_MODE_CONCURRENT; ci.queueFamilyIndexCount=2; ci.pQueueFamilyIndices=fi; }
        else ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform=caps.currentTransform; ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode=VK_PRESENT_MODE_FIFO_KHR; ci.clipped=VK_TRUE;
        VK_CHECK(vkCreateSwapchainKHR(device,&ci,nullptr,&swapchain));
        uint32_t ic=0; vkGetSwapchainImagesKHR(device,swapchain,&ic,nullptr);
        swapImages.resize(ic); vkGetSwapchainImagesKHR(device,swapchain,&ic,swapImages.data());
        swapViews.resize(ic);
        for(uint32_t i=0;i<ic;++i){
            VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            v.image=swapImages[i]; v.viewType=VK_IMAGE_VIEW_TYPE_2D; v.format=swapFormat;
            v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            VK_CHECK(vkCreateImageView(device,&v,nullptr,&swapViews[i]));
        }
        renderFinishedSems.resize(ic);
        VkSemaphoreCreateInfo sm{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for(auto& s:renderFinishedSems) VK_CHECK(vkCreateSemaphore(device,&sm,nullptr,&s));
    }
    void createCommandPool(){
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cp.queueFamilyIndex=graphicsFamily;
        VK_CHECK(vkCreateCommandPool(device,&cp,nullptr,&cmdPool));
    }

    // ---------------- hand-rolled allocator ----------------
    uint32_t findMemoryType(uint32_t typeBits,VkMemoryPropertyFlags props){
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physicalDevice,&mp);
        for(uint32_t i=0;i<mp.memoryTypeCount;++i)
            if((typeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&props)==props) return i;
        throw std::runtime_error("no suitable memory type");
    }
    AllocatedBuffer createBuffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags props){
        AllocatedBuffer b; b.size=size;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size=size; bci.usage=usage; bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device,&bci,nullptr,&b.buffer));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,b.buffer,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=req.size; ai.memoryTypeIndex=findMemoryType(req.memoryTypeBits,props);
        VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&b.memory));
        VK_CHECK(vkBindBufferMemory(device,b.buffer,b.memory,0));
        if(props&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) VK_CHECK(vkMapMemory(device,b.memory,0,size,0,&b.mapped));
        return b;
    }
    AllocatedImage createImage(uint32_t w,uint32_t h,VkFormat fmt,VkImageUsageFlags usage,VkImageAspectFlags aspect){
        AllocatedImage img; img.format=fmt; img.extent={w,h};
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType=VK_IMAGE_TYPE_2D; ici.format=fmt; ici.extent={w,h,1};
        ici.mipLevels=1; ici.arrayLayers=1; ici.samples=VK_SAMPLE_COUNT_1_BIT;
        ici.tiling=VK_IMAGE_TILING_OPTIMAL; ici.usage=usage; ici.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device,&ici,nullptr,&img.image));
        VkMemoryRequirements req; vkGetImageMemoryRequirements(device,img.image,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=req.size; ai.memoryTypeIndex=findMemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&img.memory));
        VK_CHECK(vkBindImageMemory(device,img.image,img.memory,0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image=img.image; v.viewType=VK_IMAGE_VIEW_TYPE_2D; v.format=fmt; v.subresourceRange={aspect,0,1,0,1};
        VK_CHECK(vkCreateImageView(device,&v,nullptr,&img.view));
        return img;
    }
    AllocatedImage createImage3D(uint32_t w,uint32_t h,uint32_t d,VkFormat fmt,VkImageUsageFlags usage){
        AllocatedImage img; img.format=fmt; img.extent={w,h};
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType=VK_IMAGE_TYPE_3D; ici.format=fmt; ici.extent={w,h,d};
        ici.mipLevels=1; ici.arrayLayers=1; ici.samples=VK_SAMPLE_COUNT_1_BIT;
        ici.tiling=VK_IMAGE_TILING_OPTIMAL; ici.usage=usage; ici.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device,&ici,nullptr,&img.image));
        VkMemoryRequirements req; vkGetImageMemoryRequirements(device,img.image,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=req.size; ai.memoryTypeIndex=findMemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&img.memory));
        VK_CHECK(vkBindImageMemory(device,img.image,img.memory,0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image=img.image; v.viewType=VK_IMAGE_VIEW_TYPE_3D; v.format=fmt; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VK_CHECK(vkCreateImageView(device,&v,nullptr,&img.view));
        return img;
    }
    // build a 3D R32_SFLOAT distance image from float data, left in SHADER_READ_ONLY layout
    AllocatedImage makeUDFImage(uint32_t res,const std::vector<float>& data){
        AllocatedImage im=createImage3D(res,res,res,VK_FORMAT_R32_SFLOAT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkDeviceSize bytes=data.size()*sizeof(float);
        AllocatedBuffer st=createBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        std::memcpy(st.mapped,data.data(),(size_t)bytes);
        immediateSubmit([&](VkCommandBuffer c){
            transition(c,im.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy rg{}; rg.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; rg.imageExtent={res,res,res};
            vkCmdCopyBufferToImage(c,st.buffer,im.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rg);
            transition(c,im.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
        destroyBuffer(st);
        return im;
    }
    // build an RGBA8 2D texture from pixel data, left in SHADER_READ_ONLY layout
    AllocatedImage make2DTexture(uint32_t w,uint32_t h,const std::vector<uint8_t>& rgba){
        AllocatedImage im=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
        AllocatedBuffer st=createBuffer(rgba.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        std::memcpy(st.mapped,rgba.data(),rgba.size());
        immediateSubmit([&](VkCommandBuffer c){
            transition(c,im.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy rg{}; rg.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; rg.imageExtent={w,h,1};
            vkCmdCopyBufferToImage(c,st.buffer,im.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rg);
            transition(c,im.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
        destroyBuffer(st);
        return im;
    }
    void destroyBuffer(AllocatedBuffer& b){ if(b.mapped) vkUnmapMemory(device,b.memory); if(b.buffer) vkDestroyBuffer(device,b.buffer,nullptr); if(b.memory) vkFreeMemory(device,b.memory,nullptr); b={}; }
    void destroyImage(AllocatedImage& im){ if(im.view) vkDestroyImageView(device,im.view,nullptr); if(im.image) vkDestroyImage(device,im.image,nullptr); if(im.memory) vkFreeMemory(device,im.memory,nullptr); im={}; }

    void immediateSubmit(const std::function<void(VkCommandBuffer)>& fn){
        VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb.commandPool=cmdPool; cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cb.commandBufferCount=1;
        VkCommandBuffer c; VK_CHECK(vkAllocateCommandBuffers(device,&cb,&c));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(c,&bi)); fn(c); VK_CHECK(vkEndCommandBuffer(c));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&c;
        VK_CHECK(vkQueueSubmit(graphicsQueue,1,&si,VK_NULL_HANDLE)); VK_CHECK(vkQueueWaitIdle(graphicsQueue));
        vkFreeCommandBuffers(device,cmdPool,1,&c);
    }
    static void transition(VkCommandBuffer c,VkImage img,VkImageAspectFlags aspect,VkImageLayout from,VkImageLayout to,
                           VkAccessFlags sa,VkAccessFlags da,VkPipelineStageFlags ss,VkPipelineStageFlags ds){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout=from; b.newLayout=to; b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=img; b.subresourceRange={aspect,0,1,0,1}; b.srcAccessMask=sa; b.dstAccessMask=da;
        vkCmdPipelineBarrier(c,ss,ds,0,0,nullptr,0,nullptr,1,&b);
    }
    AllocatedBuffer uploadBuffer(const void* data,VkDeviceSize size,VkBufferUsageFlags usage){
        AllocatedBuffer dst=createBuffer(size,usage|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        AllocatedBuffer st=createBuffer(size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        std::memcpy(st.mapped,data,size);
        immediateSubmit([&](VkCommandBuffer c){ VkBufferCopy r{0,0,size}; vkCmdCopyBuffer(c,st.buffer,dst.buffer,1,&r); });
        destroyBuffer(st); return dst;
    }

    // ---------------- render passes / framebuffers ----------------
    VkRenderPass makeColorPass(VkFormat fmt,VkImageLayout init,VkImageLayout fin,VkAttachmentLoadOp load){
        VkAttachmentDescription color{}; color.format=fmt; color.samples=VK_SAMPLE_COUNT_1_BIT;
        color.loadOp=load; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout=init; color.finalLayout=fin;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=1; rp.pAttachments=&color; rp.subpassCount=1; rp.pSubpasses=&sub;
        VkRenderPass r; VK_CHECK(vkCreateRenderPass(device,&rp,nullptr,&r)); return r;
    }
    void createRenderPasses(){
        simPass    =makeColorPass(VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        causticPass=makeColorPass(VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ATTACHMENT_LOAD_OP_CLEAR);
        VkAttachmentDescription color{}; color.format=swapFormat; color.samples=VK_SAMPLE_COUNT_1_BIT;
        color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; color.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentDescription depth{}; depth.format=VK_FORMAT_D32_SFLOAT; depth.samples=VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; depth.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; depth.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1; sub.pColorAttachments=&colorRef; sub.pDepthStencilAttachment=&depthRef;
        VkSubpassDependency dep{}; dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0;
        dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask=0; dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        std::array<VkAttachmentDescription,2> atts{color,depth};
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount=(uint32_t)atts.size(); rp.pAttachments=atts.data(); rp.subpassCount=1; rp.pSubpasses=&sub;
        rp.dependencyCount=1; rp.pDependencies=&dep;
        VK_CHECK(vkCreateRenderPass(device,&rp,nullptr,&mainPass));
    }
    void createDepthAndFramebuffers(){
        depthImage=createImage(swapExtent.width,swapExtent.height,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        mainFramebuffers.resize(swapViews.size());
        for(size_t i=0;i<swapViews.size();++i){
            std::array<VkImageView,2> att{swapViews[i],depthImage.view};
            VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            f.renderPass=mainPass; f.attachmentCount=(uint32_t)att.size(); f.pAttachments=att.data();
            f.width=swapExtent.width; f.height=swapExtent.height; f.layers=1;
            VK_CHECK(vkCreateFramebuffer(device,&f,nullptr,&mainFramebuffers[i]));
        }
    }

    void createWaterResources(){
        for(int i=0;i<2;++i){
            water[i]=createImage(WATER_RES,WATER_RES,VK_FORMAT_R32G32B32A32_SFLOAT,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
            VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            f.renderPass=simPass; f.attachmentCount=1; f.pAttachments=&water[i].view; f.width=WATER_RES; f.height=WATER_RES; f.layers=1;
            VK_CHECK(vkCreateFramebuffer(device,&f,nullptr,&waterFB[i]));
        }
        caustic=createImage(CAUSTIC_RES,CAUSTIC_RES,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
        VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        f.renderPass=causticPass; f.attachmentCount=1; f.pAttachments=&caustic.view; f.width=CAUSTIC_RES; f.height=CAUSTIC_RES; f.layers=1;
        VK_CHECK(vkCreateFramebuffer(device,&f,nullptr,&causticFB));
    }
    void createTilesTexture(){
        const uint32_t T=256; std::vector<uint8_t> pix(T*T*4);
        for(uint32_t y=0;y<T;++y) for(uint32_t x=0;x<T;++x){
            float fx=(float)(x%(T/8))/(float)(T/8), fy=(float)(y%(T/8))/(float)(T/8);
            float grout=(fx<0.06f||fx>0.94f||fy<0.06f||fy>0.94f)?0.45f:1.0f;
            float tile=0.8f+0.1f*std::sin((float)((x/(T/8))*12.9898+(y/(T/8))*78.233));
            float v=grout*tile; uint8_t* p=&pix[(y*T+x)*4];
            p[0]=(uint8_t)(std::min(1.0f,v*0.85f)*255); p[1]=(uint8_t)(std::min(1.0f,v*0.95f)*255);
            p[2]=(uint8_t)(std::min(1.0f,v*1.00f)*255); p[3]=255;
        }
        tiles=createImage(T,T,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
        AllocatedBuffer st=createBuffer(pix.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        std::memcpy(st.mapped,pix.data(),pix.size());
        immediateSubmit([&](VkCommandBuffer c){
            transition(c,tiles.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy rg{}; rg.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; rg.imageExtent={T,T,1};
            vkCmdCopyBufferToImage(c,st.buffer,tiles.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rg);
            transition(c,tiles.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
        destroyBuffer(st);
    }
    void createSamplersAndDescriptors(){
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter=VK_FILTER_LINEAR; si.minFilter=VK_FILTER_LINEAR;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VK_CHECK(vkCreateSampler(device,&si,nullptr,&sampler));
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VK_CHECK(vkCreateSampler(device,&si,nullptr,&tileSampler));
        std::array<VkDescriptorSetLayoutBinding,6> b{}; VkShaderStageFlags vf=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
        b[0]={0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,vf,nullptr};
        b[1]={1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,vf,nullptr};
        b[2]={2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,vf,nullptr};
        b[3]={3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,vf,nullptr};
        b[4]={4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,vf,nullptr};   // udfTex (sampler3D)
        b[5]={5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,vf,nullptr};   // objTex (base colour)
        VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; l.bindingCount=(uint32_t)b.size(); l.pBindings=b.data();
        VK_CHECK(vkCreateDescriptorSetLayout(device,&l,nullptr,&setLayout));
        std::array<VkDescriptorPoolSize,2> ps{}; ps[0]={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2}; ps[1]={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,10};
        VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pc.maxSets=2; pc.poolSizeCount=(uint32_t)ps.size(); pc.pPoolSizes=ps.data();
        VK_CHECK(vkCreateDescriptorPool(device,&pc,nullptr,&descPool));
        uboBuffer=createBuffer(sizeof(SceneUBO),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for(int i=0;i<2;++i){ VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            a.descriptorPool=descPool; a.descriptorSetCount=1; a.pSetLayouts=&setLayout; VK_CHECK(vkAllocateDescriptorSets(device,&a,&descSet[i])); }
        udf=makeUDFImage(1,std::vector<float>{1.0e3f});   // default: no object -> far field (never sampled while objMin.w==0)
        objTex=make2DTexture(1,1,std::vector<uint8_t>{255,255,255,255}); // default: white (no texture -> procedural shading unchanged)
        writeDescriptors();
    }
    void writeDescriptors(){
        for(int i=0;i<2;++i){
            VkDescriptorBufferInfo bi{uboBuffer.buffer,0,sizeof(SceneUBO)};
            VkDescriptorImageInfo wi{sampler,water[i].view,VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo ci{sampler,caustic.view,VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo ti{tileSampler,tiles.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo ui{sampler,udf.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo oi{tileSampler,objTex.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            std::array<VkWriteDescriptorSet,6> w{};
            w[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[0].dstSet=descSet[i]; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo=&bi;
            w[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[1].dstSet=descSet[i]; w[1].dstBinding=1; w[1].descriptorCount=1; w[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo=&wi;
            w[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[2].dstSet=descSet[i]; w[2].dstBinding=2; w[2].descriptorCount=1; w[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo=&ci;
            w[3]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[3].dstSet=descSet[i]; w[3].dstBinding=3; w[3].descriptorCount=1; w[3].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo=&ti;
            w[4]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[4].dstSet=descSet[i]; w[4].dstBinding=4; w[4].descriptorCount=1; w[4].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[4].pImageInfo=&ui;
            w[5]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[5].dstSet=descSet[i]; w[5].dstBinding=5; w[5].descriptorCount=1; w[5].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[5].pImageInfo=&oi;
            vkUpdateDescriptorSets(device,(uint32_t)w.size(),w.data(),0,nullptr);
        }
    }
    VkShaderModule loadShader(const std::string& path){
        std::ifstream f(path,std::ios::ate|std::ios::binary);
        if(!f.is_open()) throw std::runtime_error("cannot open shader: "+path);
        size_t sz=(size_t)f.tellg(); std::vector<char> buf(sz); f.seekg(0); f.read(buf.data(),sz); f.close();
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=sz; ci.pCode=reinterpret_cast<const uint32_t*>(buf.data());
        VkShaderModule m; VK_CHECK(vkCreateShaderModule(device,&ci,nullptr,&m)); return m;
    }
    struct VtxAttr { uint32_t loc; VkFormat fmt; uint32_t offset; };
    struct PipelineConfig { std::string vert,frag; VkRenderPass renderPass; VkPipelineLayout layout; uint32_t stride=0; std::vector<VtxAttr> attrs; bool depthTest=false; };
    VkPipeline buildPipeline(const PipelineConfig& c){
        VkShaderModule vs=loadShader(std::string(SHADER_DIR)+"/"+c.vert+".spv");
        VkShaderModule fs=loadShader(std::string(SHADER_DIR)+"/"+c.frag+".spv");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
        stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";
        VkVertexInputBindingDescription bind{0,c.stride,VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attrs;
        for(auto& a:c.attrs) attrs.push_back({a.loc,0,a.fmt,a.offset});
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if(c.stride>0){ vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind; vi.vertexAttributeDescriptionCount=(uint32_t)attrs.size(); vi.pVertexAttributeDescriptions=attrs.data(); }
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=1; vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable=c.depthTest?VK_TRUE:VK_FALSE; ds.depthWriteEnable=c.depthTest?VK_TRUE:VK_FALSE; ds.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT; cba.blendEnable=VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount=1; cb.pAttachments=&cba;
        VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dc{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dc.dynamicStateCount=2; dc.pDynamicStates=dyn;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount=2; pci.pStages=stages; pci.pVertexInputState=&vi; pci.pInputAssemblyState=&ia; pci.pViewportState=&vp;
        pci.pRasterizationState=&rs; pci.pMultisampleState=&ms; pci.pDepthStencilState=&ds; pci.pColorBlendState=&cb; pci.pDynamicState=&dc;
        pci.layout=c.layout; pci.renderPass=c.renderPass; pci.subpass=0;
        VkPipeline p; VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&p));
        vkDestroyShaderModule(device,vs,nullptr); vkDestroyShaderModule(device,fs,nullptr); return p;
    }
    void createPipelines(){
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(SimPush)};
        VkPipelineLayoutCreateInfo pls{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pls.setLayoutCount=1; pls.pSetLayouts=&setLayout; pls.pushConstantRangeCount=1; pls.pPushConstantRanges=&push;
        VK_CHECK(vkCreatePipelineLayout(device,&pls,nullptr,&simLayout));
        VkPipelineLayoutCreateInfo plg{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plg.setLayoutCount=1; plg.pSetLayouts=&setLayout;
        VK_CHECK(vkCreatePipelineLayout(device,&plg,nullptr,&gfxLayout));
        pipeDrop      =buildPipeline({"fullscreen.vert","water_drop.frag",  simPass,simLayout});
        pipeUpdate    =buildPipeline({"fullscreen.vert","water_update.frag",simPass,simLayout});
        pipeNormal    =buildPipeline({"fullscreen.vert","water_normal.frag",simPass,simLayout});
        pipeSphereDisp=buildPipeline({"fullscreen.vert","water_sphere.frag",simPass,simLayout});
        pipeCaustic=buildPipeline({"caustics.vert","caustics.frag",causticPass,gfxLayout,sizeof(glm::vec2),{{0,VK_FORMAT_R32G32_SFLOAT,0}},false});
        pipePool  =buildPipeline({"pool.vert","pool.frag",mainPass,gfxLayout,sizeof(glm::vec3),{{0,VK_FORMAT_R32G32B32_SFLOAT,0}},true});
        pipeSphere=buildPipeline({"sphere.vert","sphere.frag",mainPass,gfxLayout,sizeof(glm::vec3),{{0,VK_FORMAT_R32G32B32_SFLOAT,0}},true});
        pipeWater =buildPipeline({"water_surface.vert","water_surface.frag",mainPass,gfxLayout,sizeof(glm::vec2),{{0,VK_FORMAT_R32G32_SFLOAT,0}},true});
        pipeObject=buildPipeline({"object.vert","object.frag",mainPass,gfxLayout,8*sizeof(float),{{0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,VK_FORMAT_R32G32B32_SFLOAT,3*sizeof(float)},{2,VK_FORMAT_R32G32_SFLOAT,6*sizeof(float)}},true});
    }

    // ---------------- glTF object ----------------
    void loadObject(){
        if(modelPath.empty()) return;
        gltfmin::Result g=gltfmin::load(modelPath);
        if(!g.ok || g.positions.empty()){ std::cerr<<"glTF load failed: "<<g.error<<" ("<<modelPath<<")\n"; return; }
        size_t vc=g.positions.size()/3;
        std::vector<glm::vec3> P(vc), N(vc,glm::vec3(0.0f));
        for(size_t i=0;i<vc;++i) P[i]=glm::vec3(g.positions[i*3],g.positions[i*3+1],g.positions[i*3+2]);
        bool haveN = g.normals.size()==g.positions.size();
        if(haveN) for(size_t i=0;i<vc;++i) N[i]=glm::vec3(g.normals[i*3],g.normals[i*3+1],g.normals[i*3+2]);
        std::vector<uint32_t> I;
        if(!g.indices.empty()) I=std::move(g.indices);
        else { I.resize(vc); for(uint32_t i=0;i<(uint32_t)vc;++i) I[i]=i; }
        if(!haveN){
            for(size_t i=0;i+2<I.size();i+=3){ uint32_t a=I[i],b=I[i+1],c=I[i+2];
                glm::vec3 fn=glm::cross(P[b]-P[a],P[c]-P[a]); N[a]+=fn; N[b]+=fn; N[c]+=fn; }
            for(auto& n:N){ float L=glm::length(n); n=(L>1e-6f)?n/L:glm::vec3(0,1,0); }
        }
        glm::vec3 mn(1e9f),mx(-1e9f); for(auto& p:P){ mn=glm::min(mn,p); mx=glm::max(mx,p); }
        glm::vec3 bsCenter=(mn+mx)*0.5f; float bsRadius=0.0f;
        for(auto& p:P) bsRadius=std::max(bsRadius,glm::length(p-bsCenter));
        if(bsRadius<1e-6f) bsRadius=1.0f;
        bool haveUV = g.uvs.size()==vc*2;
        std::vector<float> verts; verts.reserve(vc*8);
        for(size_t i=0;i<vc;++i){ verts.push_back(P[i].x); verts.push_back(P[i].y); verts.push_back(P[i].z);
                                  verts.push_back(N[i].x); verts.push_back(N[i].y); verts.push_back(N[i].z);
                                  verts.push_back(haveUV?g.uvs[i*2]:0.0f); verts.push_back(haveUV?g.uvs[i*2+1]:0.0f); }
        objectVB=uploadBuffer(verts.data(),verts.size()*sizeof(float),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        objectIB=uploadBuffer(I.data(),I.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        objectIndexCount=(uint32_t)I.size();

        // ---- base-colour texture (decode PNG -> RGBA8) ----
        if(!g.baseColorImage.empty()){
            png::Image tex=png::decode(g.baseColorImage.data(),g.baseColorImage.size());
            if(tex.ok){ destroyImage(objTex); objTex=make2DTexture((uint32_t)tex.w,(uint32_t)tex.h,tex.rgba);
                        std::cerr<<"object texture: "<<tex.w<<"x"<<tex.h<<"\n"; }
            else std::cerr<<"object texture decode failed: "<<tex.error<<"\n";
        }
        float s=SPHERE_RADIUS/bsRadius;
        objectBaseModel=glm::scale(glm::mat4(1.0f),glm::vec3(s))*glm::translate(glm::mat4(1.0f),-bsCenter);

        // ---- bake an unsigned distance field for refraction (object-base space) ----
        // object-base space = objectBaseModel * P = s*(P - bsCenter). World = that + sphereCenter
        // (per-frame transform is a pure translation), so distances are in world units directly.
        std::vector<glm::vec3> L(vc);
        for(size_t i=0;i<vc;++i) L[i]=(P[i]-bsCenter)*s;
        glm::vec3 lmn(1e9f),lmx(-1e9f); for(auto& p:L){ lmn=glm::min(lmn,p); lmx=glm::max(lmx,p); }
        const int UDF_RES=48;
        glm::vec3 ext=lmx-lmn; float vox=std::max(ext.x,std::max(ext.y,ext.z))/(float)UDF_RES;
        lmn=lmn-glm::vec3(2.0f*vox); lmx=lmx+glm::vec3(2.0f*vox);   // pad so the surface has gradient room
        std::cerr<<"baking object SDF "<<UDF_RES<<"^3 ...\n";
        udf::Field fld=udf::bake(L,I,lmn,lmx,UDF_RES);
        destroyImage(udf);
        udf=makeUDFImage(UDF_RES,fld.data);
        objLocalMin=lmn; objLocalMax=lmx; objVoxel=fld.voxel;
        writeDescriptors();   // re-point binding 4 to the real field

        hasObject=true;
        std::cerr<<"glTF loaded: "<<objectIndexCount/3<<" tris\n";
    }

    void createGeometry(){
        // water / caustic grid (vec2 in [-1,1])
        std::vector<glm::vec2> gv; std::vector<uint32_t> gi;
        for(uint32_t j=0;j<=GRID_N;++j) for(uint32_t i=0;i<=GRID_N;++i){
            float x=-1.0f+2.0f*(float)i/(float)GRID_N, z=-1.0f+2.0f*(float)j/(float)GRID_N; gv.emplace_back(x,z); }
        auto gidx=[&](uint32_t i,uint32_t j){ return j*(GRID_N+1)+i; };
        for(uint32_t j=0;j<GRID_N;++j) for(uint32_t i=0;i<GRID_N;++i){
            gi.push_back(gidx(i,j)); gi.push_back(gidx(i+1,j)); gi.push_back(gidx(i+1,j+1));
            gi.push_back(gidx(i,j)); gi.push_back(gidx(i+1,j+1)); gi.push_back(gidx(i,j+1)); }
        gridIndexCount=(uint32_t)gi.size();
        gridVB=uploadBuffer(gv.data(),gv.size()*sizeof(glm::vec2),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        gridIB=uploadBuffer(gi.data(),gi.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        // box pool
        { std::vector<glm::vec3> pv; std::vector<uint32_t> pi;
          auto quad=[&](glm::vec3 a,glm::vec3 b,glm::vec3 c,glm::vec3 d){ uint32_t base=(uint32_t)pv.size();
            pv.push_back(a);pv.push_back(b);pv.push_back(c);pv.push_back(d);
            pi.push_back(base);pi.push_back(base+1);pi.push_back(base+2); pi.push_back(base);pi.push_back(base+2);pi.push_back(base+3); };
          const float F=-1.0f,U=0.0f;
          quad({-1,F,-1},{1,F,-1},{1,F,1},{-1,F,1});
          quad({-1,F,-1},{-1,U,-1},{-1,U,1},{-1,F,1});
          quad({1,F,-1},{1,F,1},{1,U,1},{1,U,-1});
          quad({-1,F,-1},{-1,U,-1},{1,U,-1},{1,F,-1});
          quad({-1,F,1},{1,F,1},{1,U,1},{-1,U,1});
          poolBoxCount=(uint32_t)pi.size();
          poolBoxVB=uploadBuffer(pv.data(),pv.size()*sizeof(glm::vec3),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
          poolBoxIB=uploadBuffer(pi.data(),pi.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT); }

        // cylinder pool (floor disk + side wall)
        { std::vector<glm::vec3> pv; std::vector<uint32_t> pi; const uint32_t SEG=64; const float F=-1.0f,U=0.0f;
          uint32_t center=(uint32_t)pv.size(); pv.push_back({0,F,0});
          uint32_t ring0=(uint32_t)pv.size();
          for(uint32_t i=0;i<=SEG;++i){ float a=(float)i/SEG*glm::two_pi<float>(); pv.push_back({std::cos(a),F,std::sin(a)}); }
          for(uint32_t i=0;i<SEG;++i){ pi.push_back(center); pi.push_back(ring0+i); pi.push_back(ring0+i+1); }
          for(uint32_t i=0;i<SEG;++i){ float a=(float)i/SEG*glm::two_pi<float>(), a2=(float)(i+1)/SEG*glm::two_pi<float>();
            glm::vec3 b0{std::cos(a),F,std::sin(a)}, b1{std::cos(a2),F,std::sin(a2)}, t0{std::cos(a),U,std::sin(a)}, t1{std::cos(a2),U,std::sin(a2)};
            uint32_t base=(uint32_t)pv.size(); pv.push_back(b0);pv.push_back(t0);pv.push_back(t1);pv.push_back(b1);
            pi.push_back(base);pi.push_back(base+1);pi.push_back(base+2); pi.push_back(base);pi.push_back(base+2);pi.push_back(base+3); }
          poolCylCount=(uint32_t)pi.size();
          poolCylVB=uploadBuffer(pv.data(),pv.size()*sizeof(glm::vec3),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
          poolCylIB=uploadBuffer(pi.data(),pi.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT); }

        // built-in sphere (fallback object)
        std::vector<glm::vec3> sv; std::vector<uint32_t> sidx; const uint32_t STK=32,SEC=48;
        for(uint32_t i=0;i<=STK;++i){ float phi=(float)i/STK*glm::pi<float>();
            for(uint32_t j=0;j<=SEC;++j){ float th=(float)j/SEC*glm::two_pi<float>();
                sv.emplace_back(std::sin(phi)*std::cos(th),std::cos(phi),std::sin(phi)*std::sin(th)); } }
        for(uint32_t i=0;i<STK;++i) for(uint32_t j=0;j<SEC;++j){ uint32_t a=i*(SEC+1)+j,b=a+SEC+1;
            sidx.push_back(a);sidx.push_back(b);sidx.push_back(a+1); sidx.push_back(a+1);sidx.push_back(b);sidx.push_back(b+1); }
        sphereIndexCount=(uint32_t)sidx.size();
        sphereVB=uploadBuffer(sv.data(),sv.size()*sizeof(glm::vec3),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        sphereIB=uploadBuffer(sidx.data(),sidx.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }
    void createSync(){
        VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb.commandPool=cmdPool; cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cb.commandBufferCount=1;
        VK_CHECK(vkAllocateCommandBuffers(device,&cb,&cmd));
        VkSemaphoreCreateInfo sm{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device,&sm,nullptr,&acquireSem));
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device,&fc,nullptr,&inFlight));
    }
    void clearWaterImages(){
        immediateSubmit([&](VkCommandBuffer c){
            VkClearColorValue clear{}; VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            for(int i=0;i<2;++i){
                transition(c,water[i].image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
                           0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                vkCmdClearColorImage(c,water[i].image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&rng);
            }
            transition(c,caustic.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
                       0,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
    }

    // ---------------- per-frame ----------------
    void updateSpherePhysics(float dt){
        spherePrev=sphereCenter; if(paused) return;
        const float r=SPHERE_RADIUS;
        const float g=4.0f;                         // constant downward gravity
        // submerged fraction relative to the still water level (y = 0): 0 = above, 1 = fully under
        float sub=glm::clamp((r-sphereCenter.y)/(2.0f*r),0.0f,1.0f);
        // 'G' makes it a heavy object that sinks; otherwise buoyancy balances gravity at
        // sub = g/buoy = 0.5, i.e. the ball floats half-submerged at the water line.
        float buoy = gravity ? 0.0f : 2.0f*g;
        sphereVel.y += (-g + sub*buoy)*dt;
        sphereVel.y *= std::exp(-3.0f*dt);          // unconditionally stable damping
        sphereVel.y  = glm::clamp(sphereVel.y,-3.0f,3.0f);
        sphereCenter.y += sphereVel.y*dt;
        // clamp to the pool, killing velocity at the stops so it can't pin against a wall
        const float top=1.0f-r, bot=-1.0f+r;
        if(sphereCenter.y>top){ sphereCenter.y=top; if(sphereVel.y>0.0f) sphereVel.y=0.0f; }
        if(sphereCenter.y<bot){ sphereCenter.y=bot; if(sphereVel.y<0.0f) sphereVel.y=0.0f; }
        sphereCenter.x=glm::clamp(sphereCenter.x,bot,top);
        sphereCenter.z=glm::clamp(sphereCenter.z,bot,top);
    }
    void updateUBO(){
        SceneUBO u{};
        u.viewProj=projMatrix()*viewMatrix();
        u.objectModel = hasObject ? glm::translate(glm::mat4(1.0f),sphereCenter)*objectBaseModel : glm::mat4(1.0f);
        u.eye=glm::vec4(cameraEye(),1.0f);
        u.light=glm::vec4(glm::normalize(lightDir),0.0f);
        u.sphere=glm::vec4(sphereCenter,SPHERE_RADIUS);
        float t=std::chrono::duration<float>(std::chrono::high_resolution_clock::now()-startTime).count();
        u.misc=glm::vec4(t,(float)poolShape,0,0);
        if(hasObject){
            u.objMin=glm::vec4(objLocalMin+sphereCenter,1.0f);   // world AABB = local + translation
            u.objMax=glm::vec4(objLocalMax+sphereCenter,objVoxel);
        } else {
            u.objMin=glm::vec4(0,0,0,0); u.objMax=glm::vec4(0,0,0,1);
        }
        std::memcpy(uboBuffer.mapped,&u,sizeof(u));
    }
    void setFullViewport(uint32_t w,uint32_t h){ VkViewport vp{0,0,(float)w,(float)h,0.0f,1.0f}; VkRect2D sc{{0,0},{w,h}}; vkCmdSetViewport(cmd,0,1,&vp); vkCmdSetScissor(cmd,0,1,&sc); }
    void simBarrier(VkImage img){
        transition(cmd,img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    void simPassRun(VkPipeline pipe,const SimPush* push){
        int dst=1-current;
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass=simPass; rp.framebuffer=waterFB[dst]; rp.renderArea={{0,0},{WATER_RES,WATER_RES}};
        vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
        setFullViewport(WATER_RES,WATER_RES);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,simLayout,0,1,&descSet[current],0,nullptr);
        if(push) vkCmdPushConstants(cmd,simLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(SimPush),push);
        vkCmdDraw(cmd,3,1,0,0); vkCmdEndRenderPass(cmd);
        simBarrier(water[dst].image); current=dst;
    }
    void recordFrame(uint32_t imageIndex){
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cmd,&bi));
        if(!paused){
            for(auto& d:pendingDrops){ SimPush p{}; p.p0=glm::vec4(d.x,d.y,0.03f,0.4f); simPassRun(pipeDrop,&p); }
            SimPush sp{}; sp.p0=glm::vec4(spherePrev,0); sp.p1=glm::vec4(sphereCenter,0); simPassRun(pipeSphereDisp,&sp);
            simPassRun(pipeUpdate,nullptr); simPassRun(pipeNormal,nullptr);
        }
        pendingDrops.clear();
        { VkClearValue clear{}; clear.color={{0,0,0,1}};
          VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
          rp.renderPass=causticPass; rp.framebuffer=causticFB; rp.renderArea={{0,0},{CAUSTIC_RES,CAUSTIC_RES}}; rp.clearValueCount=1; rp.pClearValues=&clear;
          vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE); setFullViewport(CAUSTIC_RES,CAUSTIC_RES);
          vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeCaustic);
          vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gfxLayout,0,1,&descSet[current],0,nullptr);
          VkDeviceSize off=0; vkCmdBindVertexBuffers(cmd,0,1,&gridVB.buffer,&off); vkCmdBindIndexBuffer(cmd,gridIB.buffer,0,VK_INDEX_TYPE_UINT32);
          vkCmdDrawIndexed(cmd,gridIndexCount,1,0,0,0); vkCmdEndRenderPass(cmd);
          transition(cmd,caustic.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); }
        { std::array<VkClearValue,2> clears{}; clears[0].color={{0.0f,0.0f,0.0f,1.0f}}; clears[1].depthStencil={1.0f,0};
          VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
          rp.renderPass=mainPass; rp.framebuffer=mainFramebuffers[imageIndex]; rp.renderArea={{0,0},swapExtent}; rp.clearValueCount=(uint32_t)clears.size(); rp.pClearValues=clears.data();
          vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE); setFullViewport(swapExtent.width,swapExtent.height);
          VkDeviceSize off=0;
          vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gfxLayout,0,1,&descSet[current],0,nullptr);
          // pool (box or cylinder)
          vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipePool);
          if(poolShape==1){ vkCmdBindVertexBuffers(cmd,0,1,&poolCylVB.buffer,&off); vkCmdBindIndexBuffer(cmd,poolCylIB.buffer,0,VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd,poolCylCount,1,0,0,0); }
          else            { vkCmdBindVertexBuffers(cmd,0,1,&poolBoxVB.buffer,&off); vkCmdBindIndexBuffer(cmd,poolBoxIB.buffer,0,VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd,poolBoxCount,1,0,0,0); }
          // object (glTF) or built-in sphere
          if(hasObject){ vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeObject);
            vkCmdBindVertexBuffers(cmd,0,1,&objectVB.buffer,&off); vkCmdBindIndexBuffer(cmd,objectIB.buffer,0,VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd,objectIndexCount,1,0,0,0); }
          else { vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeSphere);
            vkCmdBindVertexBuffers(cmd,0,1,&sphereVB.buffer,&off); vkCmdBindIndexBuffer(cmd,sphereIB.buffer,0,VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd,sphereIndexCount,1,0,0,0); }
          // water surface last
          vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeWater);
          vkCmdBindVertexBuffers(cmd,0,1,&gridVB.buffer,&off); vkCmdBindIndexBuffer(cmd,gridIB.buffer,0,VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd,gridIndexCount,1,0,0,0);
          vkCmdEndRenderPass(cmd); }
        VK_CHECK(vkEndCommandBuffer(cmd));
    }
    void recreateSwapchain(){
        RECT cr; GetClientRect(hwnd,&cr); int w=cr.right-cr.left, h=cr.bottom-cr.top;
        while((w==0||h==0) && running){
            MSG msg; if(GetMessage(&msg,nullptr,0,0)){ TranslateMessage(&msg); DispatchMessage(&msg); }
            GetClientRect(hwnd,&cr); w=cr.right-cr.left; h=cr.bottom-cr.top;
        }
        width=(uint32_t)w; height=(uint32_t)h; vkDeviceWaitIdle(device);
        for(auto fb:mainFramebuffers) vkDestroyFramebuffer(device,fb,nullptr); mainFramebuffers.clear();
        destroyImage(depthImage);
        for(auto v:swapViews) vkDestroyImageView(device,v,nullptr);
        for(auto s:renderFinishedSems) vkDestroySemaphore(device,s,nullptr); renderFinishedSems.clear();
        vkDestroySwapchainKHR(device,swapchain,nullptr);
        createSwapchain(); createDepthAndFramebuffers();
    }
    void drawFrame(){
        VK_CHECK(vkWaitForFences(device,1,&inFlight,VK_TRUE,UINT64_MAX));
        uint32_t imageIndex;
        VkResult acq=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquireSem,VK_NULL_HANDLE,&imageIndex);
        if(acq==VK_ERROR_OUT_OF_DATE_KHR){ recreateSwapchain(); return; }
        if(acq!=VK_SUCCESS&&acq!=VK_SUBOPTIMAL_KHR) throw std::runtime_error("acquire failed");
        VK_CHECK(vkResetFences(device,1,&inFlight)); VK_CHECK(vkResetCommandBuffer(cmd,0));
        updateUBO(); recordFrame(imageIndex);
        VkPipelineStageFlags wait=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount=1; si.pWaitSemaphores=&acquireSem; si.pWaitDstStageMask=&wait;
        si.commandBufferCount=1; si.pCommandBuffers=&cmd; si.signalSemaphoreCount=1; si.pSignalSemaphores=&renderFinishedSems[imageIndex];
        VK_CHECK(vkQueueSubmit(graphicsQueue,1,&si,inFlight));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&renderFinishedSems[imageIndex]; pi.swapchainCount=1; pi.pSwapchains=&swapchain; pi.pImageIndices=&imageIndex;
        VkResult pres=vkQueuePresentKHR(presentQueue,&pi);
        if(pres==VK_ERROR_OUT_OF_DATE_KHR||pres==VK_SUBOPTIMAL_KHR||framebufferResized){ framebufferResized=false; recreateSwapchain(); }
        else if(pres!=VK_SUCCESS) throw std::runtime_error("present failed");
    }
    void mainLoop(){
        auto prev=std::chrono::high_resolution_clock::now();
        while(running){
            MSG msg;
            while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
                if(msg.message==WM_QUIT){ running=false; break; }
                TranslateMessage(&msg); DispatchMessage(&msg);
            }
            if(!running) break;
            auto now=std::chrono::high_resolution_clock::now();
            float dt=std::chrono::duration<float>(now-prev).count(); prev=now; dt=std::min(dt,0.033f);
            updateSpherePhysics(dt); drawFrame();
        }
        vkDeviceWaitIdle(device);
    }
    void cleanup(){
        vkDeviceWaitIdle(device);
        vkDestroyFence(device,inFlight,nullptr);
        vkDestroySemaphore(device,acquireSem,nullptr);
        for(auto s:renderFinishedSems) vkDestroySemaphore(device,s,nullptr);
        destroyBuffer(gridVB); destroyBuffer(gridIB);
        destroyBuffer(poolBoxVB); destroyBuffer(poolBoxIB); destroyBuffer(poolCylVB); destroyBuffer(poolCylIB);
        destroyBuffer(sphereVB); destroyBuffer(sphereIB);
        if(hasObject){ destroyBuffer(objectVB); destroyBuffer(objectIB); }
        for(auto p:{pipeDrop,pipeUpdate,pipeNormal,pipeSphereDisp,pipeCaustic,pipePool,pipeSphere,pipeWater,pipeObject}) vkDestroyPipeline(device,p,nullptr);
        vkDestroyPipelineLayout(device,simLayout,nullptr); vkDestroyPipelineLayout(device,gfxLayout,nullptr);
        destroyBuffer(uboBuffer);
        vkDestroyDescriptorPool(device,descPool,nullptr); vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
        vkDestroySampler(device,sampler,nullptr); vkDestroySampler(device,tileSampler,nullptr);
        destroyImage(tiles); destroyImage(caustic); destroyImage(udf); destroyImage(objTex); vkDestroyFramebuffer(device,causticFB,nullptr);
        for(int i=0;i<2;++i){ vkDestroyFramebuffer(device,waterFB[i],nullptr); destroyImage(water[i]); }
        for(auto fb:mainFramebuffers) vkDestroyFramebuffer(device,fb,nullptr);
        destroyImage(depthImage);
        vkDestroyRenderPass(device,simPass,nullptr); vkDestroyRenderPass(device,causticPass,nullptr); vkDestroyRenderPass(device,mainPass,nullptr);
        for(auto v:swapViews) vkDestroyImageView(device,v,nullptr);
        vkDestroySwapchainKHR(device,swapchain,nullptr);
        vkDestroyCommandPool(device,cmdPool,nullptr);
        vkDestroyDevice(device,nullptr);
        vkDestroySurfaceKHR(instance,surface,nullptr);
        if(debugMessenger){ auto fn=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"); if(fn) fn(instance,debugMessenger,nullptr); }
        vkDestroyInstance(instance,nullptr);
        if(hwnd) DestroyWindow(hwnd);
        UnregisterClassW(L"VulkanWaterWindow",hinstance);
    }
};

int main(int argc,char** argv){
    try { WaterApp app; if(argc>1) app.modelPath=argv[1]; app.run(); }
    catch(const std::exception& e){ std::cerr<<"fatal: "<<e.what()<<"\n"; return 1; }
    return 0;
}
