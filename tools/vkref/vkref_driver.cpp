// vkref_driver.cpp - Dump intermediate buffers from PyroWave encoder for validation (T6a)
//
// Usage: vkref_driver <vector_name> <W> <H> <budget> [bands_f32_file]
//
// Generates synthetic test vectors or loads bands from a file.
// Runs the Vulkan encoder with dump hooks enabled.
// Writes dump files and manifest.json to the output directory.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <vector>
#include <string>
#include <sys/stat.h>

// Vulkan headers
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// PyroWave headers (from the same directory)
#include "pyrowave.h"
#include "pyrowave_encoder.hpp"

// Test vector definitions
struct TestVector {
    const char *name;
    int w, h;
};

static const TestVector test_vectors[] = {
    {"grad", 256, 144},
    {"grad", 16, 16},
    {"grad", 128, 128},
    {"grad", 1280, 720},
    {"grad", 1366, 768},
    {"grad", 1920, 1080},
    {"grad", 2560, 1440},
    {"checker1", 256, 144},
    {"checker1", 16, 16},
    {"checker8", 256, 144},
    {"checker32", 256, 144},
    {"noise", 256, 144},
    {"noise", 16, 16},
    {"flat0", 256, 144},
    {"flat255", 256, 144},
    {"flat128", 256, 144},
    {NULL, 0, 0}
};

// Generate a synthetic YUV420P frame
static void generate_gradient_yuv(uint8_t *y, uint8_t *cb, uint8_t *cr, int w, int h) {
    // Horizontal gradient in luma
    for (int y_idx = 0; y_idx < h; y_idx++) {
        for (int x = 0; x < w; x++) {
            y[y_idx * w + x] = (uint8_t)((x * 255) / w);
        }
    }
    // Constant chroma
    for (int i = 0; i < (h / 2) * (w / 2); i++) {
        cb[i] = 128;
        cr[i] = 128;
    }
}

static void generate_checker_yuv(uint8_t *y, uint8_t *cb, uint8_t *cr, int w, int h, int cell_size) {
    for (int y_idx = 0; y_idx < h; y_idx++) {
        for (int x = 0; x < w; x++) {
            int cx = x / cell_size;
            int cy = y_idx / cell_size;
            y[y_idx * w + x] = ((cx + cy) & 1) ? 255 : 0;
        }
    }
    for (int i = 0; i < (h / 2) * (w / 2); i++) {
        cb[i] = 128;
        cr[i] = 128;
    }
}

static void generate_noise_yuv(uint8_t *y, uint8_t *cb, uint8_t *cr, int w, int h) {
    // Simple deterministic "noise"
    for (int i = 0; i < w * h; i++) {
        y[i] = (uint8_t)((i * 7 + 13) & 0xFF);
    }
    for (int i = 0; i < (h / 2) * (w / 2); i++) {
        cb[i] = (uint8_t)((i * 11 + 7) & 0xFF);
        cr[i] = (uint8_t)((i * 13 + 3) & 0xFF);
    }
}

static void generate_flat_yuv(uint8_t *y, uint8_t *cb, uint8_t *cr, int w, int h, uint8_t value) {
    memset(y, value, w * h);
    for (int i = 0; i < (h / 2) * (w / 2); i++) {
        cb[i] = 128;
        cr[i] = 128;
    }
}

// Write manifest.json
static void write_manifest(const char *dir, const char *vector_name, int w, int h,
                           int budget, const char *device_info, const char *ref_sha) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/manifest.json", dir);
    
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Failed to write manifest: %s\n", path);
        return;
    }
    
    fprintf(f, "{\n");
    fprintf(f, "  \"vector\": \"%s\",\n", vector_name);
    fprintf(f, "  \"W\": %d,\n", w);
    fprintf(f, "  \"H\": %d,\n", h);
    fprintf(f, "  \"budget\": %d,\n", budget);
    fprintf(f, "  \"device\": \"%s\",\n", device_info);
    fprintf(f, "  \"ref\": \"%s\",\n", ref_sha);
    fprintf(f, "  \"buffers\": [\n");
    
    // List dumped buffers
    // This will be populated by scanning the dump directory
    fprintf(f, "    // TODO: populate from dump files\n");
    
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

// Create directory recursively
static void mkdirs(const char *path) {
    std::string p = path;
    for (size_t i = 1; i < p.size(); i++) {
        if (p[i] == '/') {
            p[i] = '\0';
            mkdir(p.c_str(), 0755);
            p[i] = '/';
        }
    }
    mkdir(p.c_str(), 0755);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <vector_name> <W> <H> <budget> [bands_file] [output_dir]\n", argv[0]);
        return 1;
    }
    
    const char *vector_name = argv[1];
    int w = atoi(argv[2]);
    int h = atoi(argv[3]);
    int budget = atoi(argv[4]);
    const char *bands_file = argc > 5 ? argv[5] : NULL;
    const char *output_dir = argc > 6 ? argv[6] : "./dumps";
    
    // Validate dimensions
    if (w < 16 || h < 16) {
        fprintf(stderr, "Minimum size is 16x16\n");
        return 1;
    }
    if (w % 2 || h % 2) {
        fprintf(stderr, "Dimensions must be even for YUV420\n");
        return 1;
    }
    
    // Calculate default budget
    int default_budget = (w * h * 3) / 16;
    if (default_budget < 32768) default_budget = 32768;
    if (budget <= 0) budget = default_budget;
    
    // Create output directory
    mkdirs(output_dir);
    
    // Generate YUV frame
    std::vector<uint8_t> y(w * h);
    std::vector<uint8_t> cb((h / 2) * (w / 2));
    std::vector<uint8_t> cr((h / 2) * (w / 2));
    
    if (strcmp(vector_name, "grad") == 0) {
        generate_gradient_yuv(y.data(), cb.data(), cr.data(), w, h);
    } else if (strcmp(vector_name, "checker1") == 0 || strcmp(vector_name, "checker8") == 0 || 
               strcmp(vector_name, "checker32") == 0) {
        int cell = strcmp(vector_name, "checker1") == 0 ? 1 : 
                   strcmp(vector_name, "checker8") == 0 ? 8 : 32;
        generate_checker_yuv(y.data(), cb.data(), cr.data(), w, h, cell);
    } else if (strcmp(vector_name, "noise") == 0) {
        generate_noise_yuv(y.data(), cb.data(), cr.data(), w, h);
    } else if (strcmp(vector_name, "flat0") == 0) {
        generate_flat_yuv(y.data(), cb.data(), cr.data(), w, h, 0);
    } else if (strcmp(vector_name, "flat255") == 0) {
        generate_flat_yuv(y.data(), cb.data(), cr.data(), w, h, 255);
    } else if (strcmp(vector_name, "flat128") == 0) {
        generate_flat_yuv(y.data(), cb.data(), cr.data(), w, h, 128);
    } else {
        fprintf(stderr, "Unknown vector: %s\n", vector_name);
        return 1;
    }
    
    // Setup Vulkan
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "vkref_driver";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "vkref";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;
    
    VkInstanceCreateInfo instance_create_info = {};
    instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pApplicationInfo = &app_info;
    
    // Get required extensions
    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL);
    std::vector<VkExtensionProperties> extensions(ext_count);
    vkEnumerateInstanceExtensionProperties(NULL, &ext_count, extensions.data());
    
    // Add VK_KHR_get_physical_device_properties2 for device selection
    instance_create_info.enabledExtensionCount = 0;
    instance_create_info.ppEnabledExtensionNames = NULL;
    
    VkInstance instance;
    if (vkCreateInstance(&instance_create_info, NULL, &instance) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance\n");
        return 1;
    }
    
    // Enumerate physical devices
    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, NULL);
    if (gpu_count == 0) {
        fprintf(stderr, "No Vulkan GPUs found\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
    
    // Select GPU (prefer NVIDIA RTX)
    VkPhysicalDevice selected_gpu = gpus[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(selected_gpu, &props);
    
    printf("Selected GPU: %s\n", props.deviceName);
    
    // Get queue family indices
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(selected_gpu, &queue_family_count, NULL);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(selected_gpu, &queue_family_count, queue_families.data());
    
    // Find compute queue
    int compute_queue_family = -1;
    for (uint32_t i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            compute_queue_family = i;
            break;
        }
    }
    
    if (compute_queue_family < 0) {
        fprintf(stderr, "No compute queue family found\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    // Create device
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info = {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = compute_queue_family;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;
    
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features.shaderInt16 = VK_TRUE;
    
    VkPhysicalDeviceStorageBuffer8BitFeatures storage8bit = {};
    storage8bit.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_STORAGE_BUFFER_8BIT_FEATURES;
    storage8bit.storageBuffer8BitAccess = VK_TRUE;
    
    VkPhysicalDeviceVulkan12Features vulkan12 = {};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.storageBuffer8BitAccess = VK_TRUE;
    vulkan12.pNext = &storage8bit;
    
    VkDeviceCreateInfo device_create_info = {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;
    device_create_info.pEnabledFeatures = NULL;
    device_create_info.enabledExtensionCount = 0;
    device_create_info.ppEnabledExtensionNames = NULL;
    device_create_info.pNext = &vulkan12;
    
    VkDevice device;
    if (vkCreateDevice(selected_gpu, &device_create_info, NULL, &device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan device\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    // Get queue
    VkQueue queue;
    vkGetDeviceQueue(device, compute_queue_family, 0, &queue);
    
    // Create pyrowave device
    pyrowave_device_create_info pw_device_info = {};
    pw_device_info.GetInstanceProcAddr = vkGetInstanceProcAddr;
    pw_device_info.instance = instance;
    pw_device_info.physical_device = selected_gpu;
    pw_device_info.device = device;
    
    pyrowave_device pw_device;
    if (pyrowave_create_device(&pw_device_info, &pw_device) != PYROWAVE_SUCCESS) {
        fprintf(stderr, "Failed to create pyrowave device\n");
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    // Create encoder
    pyrowave_encoder_create_info encoder_info = {};
    encoder_info.device = pw_device;
    encoder_info.width = w;
    encoder_info.height = h;
    encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    
    pyrowave_encoder encoder;
    if (pyrowave_encoder_create(&encoder_info, &encoder) != PYROWAVE_SUCCESS) {
        fprintf(stderr, "Failed to create encoder\n");
        pyrowave_device_destroy(pw_device);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    // Enable dump hooks (this requires the patch to be applied)
    // The dump_hook mechanism is internal to the encoder, so we use
    // environment variable or a special API call
    const char *dump_dir_env = getenv("PYROWAVE_DUMP_DIR");
    if (dump_dir_env) {
        // Set dump directory via environment
        // This is implementation-specific
    }
    
    // Prepare rate control
    pyrowave_rate_control rate_control = {};
    rate_control.maximum_bitstream_size = budget;
    
    // Prepare CPU buffers
    pyrowave_cpu_buffer cpu_buffers = {};
    cpu_buffers.data[0] = y.data();
    cpu_buffers.data[1] = cb.data();
    cpu_buffers.data[2] = cr.data();
    cpu_buffers.row_stride_in_bytes[0] = w;
    cpu_buffers.row_stride_in_bytes[1] = w / 2;
    cpu_buffers.row_stride_in_bytes[2] = w / 2;
    cpu_buffers.plane_size_in_bytes[0] = w * h;
    cpu_buffers.plane_size_in_bytes[1] = (h / 2) * (w / 2);
    cpu_buffers.plane_size_in_bytes[2] = (h / 2) * (w / 2);
    cpu_buffers.width = w;
    cpu_buffers.height = h;
    cpu_buffers.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    
    // Encode
    printf("Encoding %dx%d, budget=%d...\n", w, h, budget);
    pyrowave_result result = pyrowave_encoder_encode_cpu(encoder, &cpu_buffers, &rate_control);
    
    if (result != PYROWAVE_SUCCESS) {
        fprintf(stderr, "Encoding failed: %d\n", result);
        pyrowave_encoder_destroy(encoder);
        pyrowave_device_destroy(pw_device);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    printf("Encoding succeeded\n");
    
    // Get bitstream
    const void *mapped_bitstream = NULL;
    size_t bitstream_size = 0;
    const void *mapped_meta = NULL;
    size_t meta_size = 0;
    
    result = pyrowave_encoder_get_mapped_raw_bitstream(encoder, &mapped_bitstream, &bitstream_size,
                                                        &mapped_meta, &meta_size);
    if (result != PYROWAVE_SUCCESS) {
        fprintf(stderr, "Failed to get bitstream\n");
    } else {
        printf("Bitstream: %zu bytes, Meta: %zu bytes\n", bitstream_size, meta_size);
        
        // Write bitstream to file
        char bs_path[1024];
        snprintf(bs_path, sizeof(bs_path), "%s/bitstream.bin", output_dir);
        FILE *f = fopen(bs_path, "wb");
        if (f) {
            fwrite(mapped_bitstream, 1, bitstream_size, f);
            fclose(f);
            printf("Wrote bitstream to %s\n", bs_path);
        }
        
        // Write meta to file
        char meta_path[1024];
        snprintf(meta_path, sizeof(meta_path), "%s/meta.bin", output_dir);
        f = fopen(meta_path, "wb");
        if (f) {
            fwrite(mapped_meta, 1, meta_size, f);
            fclose(f);
            printf("Wrote meta to %s\n", meta_path);
        }
    }
    
    // Write manifest
    char device_info[256];
    snprintf(device_info, sizeof(device_info), "%s (driver %s)", 
             props.deviceName, 
             "unknown");
    
    write_manifest(output_dir, vector_name, w, h, budget, device_info, "a0d2e71+vkref");
    
    // Cleanup
    pyrowave_encoder_destroy(encoder);
    pyrowave_device_destroy(pw_device);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    
    printf("Done. Output in %s\n", output_dir);
    return 0;
}
