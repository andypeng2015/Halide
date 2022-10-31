#include "HalideRuntimeWebGPU.h"
#include "device_buffer_utils.h"
#include "device_interface.h"
#include "gpu_context_common.h"
#include "printer.h"
#include "scoped_spin_lock.h"

#include "mini_webgpu.h"

namespace Halide {
namespace Runtime {
namespace Internal {
namespace WebGPU {

extern WEAK halide_device_interface_t webgpu_device_interface;

WEAK int create_webgpu_context(void *user_context);

// A WebGPU instance/adapter/device defined in this module with weak linkage.
WEAK WGPUInstance global_instance = nullptr;
WEAK WGPUAdapter global_adapter = nullptr;
WEAK WGPUDevice global_device = nullptr;
// Lock to synchronize access to the global WebGPU context.
volatile ScopedSpinLock::AtomicFlag WEAK context_lock = 0;

// Size of the staging buffer used for host<->device copies.
constexpr int kWebGpuStagingBufferSize = 4 * 1024 * 1024;
// A staging buffer used for host<->device copies.
WGPUBuffer WEAK staging_buffer = nullptr;

}  // namespace WebGPU
}  // namespace Internal
}  // namespace Runtime
}  // namespace Halide

using namespace Halide::Runtime::Internal;
using namespace Halide::Runtime::Internal::WebGPU;

extern "C" {
// TODO: Remove all of this when wgpuInstanceProcessEvents() is supported.
#ifdef WITH_DAWN_NATIVE
// Defined by Dawn, and used to yield execution to asynchronous commands.
void wgpuDeviceTick(WGPUDevice);
// From <unistd.h>, used to spin-lock while waiting for device initialization.
int usleep(uint32_t);
#else
// Defined by Emscripten, and used to yield execution to asynchronous Javascript
// work in combination with Emscripten's "Asyncify" mechanism.
void emscripten_sleep(unsigned int ms);
// Wrap emscripten_sleep in wgpuDeviceTick() to unify usage with Dawn.
void wgpuDeviceTick(WGPUDevice) {
    emscripten_sleep(1);
}
#endif

// The default implementation of halide_webgpu_acquire_context uses the global
// pointers above, and serializes access with a spin lock.
// Overriding implementations of acquire/release must implement the following
// behavior:
// - halide_webgpu_acquire_context should always store a valid
//   instance/adapter/device in instance_ret/adapter_ret/device_ret, or return
//   an error code.
// - A call to halide_webgpu_acquire_context is followed by a matching call to
//   halide_webgpu_release_context. halide_webgpu_acquire_context should block
//   while a previous call (if any) has not yet been released via
//   halide_webgpu_release_context.
WEAK int halide_webgpu_acquire_context(void *user_context,
                                       WGPUInstance *instance_ret,
                                       WGPUAdapter *adapter_ret,
                                       WGPUDevice *device_ret,
                                       bool create = true) {
    halide_abort_if_false(user_context, &context_lock != nullptr);
    while (__atomic_test_and_set(&context_lock, __ATOMIC_ACQUIRE)) {
    }

    if (create && (global_device == nullptr)) {
        int status = create_webgpu_context(user_context);
        if (status != halide_error_code_success) {
            __atomic_clear(&context_lock, __ATOMIC_RELEASE);
            return status;
        }
    }

    *instance_ret = global_instance;
    *adapter_ret = global_adapter;
    *device_ret = global_device;

    return halide_error_code_success;
}

WEAK int halide_webgpu_release_context(void *user_context) {
    __atomic_clear(&context_lock, __ATOMIC_RELEASE);
    return 0;
}

}  // extern "C" linkage

namespace Halide {
namespace Runtime {
namespace Internal {
namespace WebGPU {

// Helper object to acquire and release the WebGPU context.
class WgpuContext {
    void *user_context;

public:
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    int error_code = 0;

    ALWAYS_INLINE WgpuContext(void *user_context)
        : user_context(user_context) {
        error_code = halide_webgpu_acquire_context(
            user_context, &instance, &adapter, &device);
        if (error_code == halide_error_code_success) {
            queue = wgpuDeviceGetQueue(device);
        }
    }

    ALWAYS_INLINE ~WgpuContext() {
        if (queue) {
            wgpuQueueRelease(queue);
        }
        halide_webgpu_release_context(user_context);
    }
};

// Helper class for handling asynchronous errors for a set of WebGPU API calls
// within a particular scope.
class ErrorScope {
public:
    ALWAYS_INLINE ErrorScope(void *user_context, WGPUDevice device)
        : user_context(user_context), device(device) {
        // Capture validation and OOM errors.
        wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
        wgpuDevicePushErrorScope(device, WGPUErrorFilter_OutOfMemory);
        callbacks_remaining = 2;
    }

    ALWAYS_INLINE ~ErrorScope() {
        if (callbacks_remaining > 0) {
            // Pop the error scopes to flush any pending errors.
            wait();
        }
    }

    // Wait for all error callbacks in this scope to fire.
    // Returns the error code (or success).
    int wait() {
        if (callbacks_remaining == 0) {
            error(user_context) << "no outstanding error scopes\n";
            return halide_error_code_internal_error;
        }

        error_code = halide_error_code_success;
        wgpuDevicePopErrorScope(device, error_callback, this);
        wgpuDevicePopErrorScope(device, error_callback, this);

        // Wait for the error callbacks to fire.
        while (__sync_fetch_and_or(&callbacks_remaining, 0) > 0) {
            wgpuDeviceTick(device);
        }

        return error_code;
    }

private:
    void *user_context;
    WGPUDevice device;

    // The error code reported by the callback functions.
    volatile int error_code;

    // Used to track outstanding error callbacks.
    volatile int callbacks_remaining = 0;

    // The error callback function.
    // Logs any errors, and decrements the remaining callback count.
    static void error_callback(WGPUErrorType type,
                               char const *message,
                               void *userdata) {
        ErrorScope *context = (ErrorScope *)userdata;
        switch (type) {
        case WGPUErrorType_NoError:
            // Do not overwrite the error_code to avoid masking earlier errors.
            break;
        case WGPUErrorType_Validation:
            error(context->user_context) << "WGPU: validation error: "
                                         << message << "\n";
            context->error_code = halide_error_code_generic_error;
            break;
        case WGPUErrorType_OutOfMemory:
            error(context->user_context) << "WGPU: out-of-memory error: "
                                         << message << "\n";
            context->error_code = halide_error_code_out_of_memory;
            break;
        default:
            error(context->user_context) << "WGPU: unknown error (" << type
                                         << "): " << message << "\n";
            context->error_code = halide_error_code_generic_error;
        }

        __sync_fetch_and_add(&context->callbacks_remaining, -1);
    }
};

// WgpuBufferHandle represents a device buffer with an offset.
struct WgpuBufferHandle {
    uint64_t offset;
    WGPUBuffer buffer;
};

// A cache for compiled WGSL shader modules.
WEAK Halide::Internal::GPUCompilationCache<WGPUDevice, WGPUShaderModule>
    shader_cache;

namespace {

halide_error_code_t init_error_code = halide_error_code_success;

void device_lost_callback(WGPUDeviceLostReason reason,
                          char const *message,
                          void *user_context) {
    error(user_context) << "WGPU device lost (" << reason << "): "
                        << message << "\n";
}

void request_device_callback(WGPURequestDeviceStatus status,
                             WGPUDevice device,
                             char const *message,
                             void *user_context) {
    if (status != WGPURequestDeviceStatus_Success) {
        debug(user_context) << "wgpuAdapterRequestDevice failed ("
                            << status << "): " << message << "\n";
        init_error_code = halide_error_code_generic_error;
        return;
    }
    wgpuDeviceSetDeviceLostCallback(device, device_lost_callback, user_context);
    global_device = device;
}

void request_adapter_callback(WGPURequestAdapterStatus status,
                              WGPUAdapter adapter,
                              char const *message,
                              void *user_context) {
    if (status != WGPURequestAdapterStatus_Success) {
        debug(user_context) << "wgpuInstanceRequestAdapter failed: ("
                            << status << "): " << message << "\n";
        init_error_code = halide_error_code_generic_error;
        return;
    }
    global_adapter = adapter;

    // Use the defaults for most limits.
    WGPURequiredLimits requestedLimits{.nextInChain = nullptr};
    memset(&requestedLimits.limits, 0xFF, sizeof(WGPULimits));

    WGPUSupportedLimits supportedLimits{.nextInChain = nullptr};
    if (!wgpuAdapterGetLimits(adapter, &supportedLimits)) {
        debug(user_context) << "wgpuAdapterGetLimits failed\n";
    } else {
        // Raise the limits on buffer size and workgroup storage size.
        requestedLimits.limits.maxBufferSize =
            supportedLimits.limits.maxBufferSize;
        requestedLimits.limits.maxStorageBufferBindingSize =
            supportedLimits.limits.maxStorageBufferBindingSize;
        requestedLimits.limits.maxComputeWorkgroupStorageSize =
            supportedLimits.limits.maxComputeWorkgroupStorageSize;
    }

    WGPUDeviceDescriptor desc = {
        .nextInChain = nullptr,
        .label = nullptr,
        .requiredFeaturesCount = 0,
        .requiredFeatures = nullptr,
        .requiredLimits = &requestedLimits,
    };
    wgpuAdapterRequestDevice(adapter, &desc, request_device_callback,
                             user_context);
}

size_t roundUpToMultipleOf4(size_t x) {
    return (x + 3) & ~0x3;
}

}  // namespace

WEAK int create_webgpu_context(void *user_context) {
    // TODO: Unify this when Emscripten implements wgpuCreateInstance().
#ifdef WITH_DAWN_NATIVE
    WGPUInstanceDescriptor desc{
        .nextInChain = nullptr,
    };
    global_instance = wgpuCreateInstance(&desc);
#else
    global_instance = nullptr;
#endif

    wgpuInstanceRequestAdapter(
        global_instance, nullptr, request_adapter_callback, user_context);

    // Wait for device initialization to complete.
    while (!global_device && init_error_code == halide_error_code_success) {
        // TODO: Use wgpuInstanceProcessEvents() when it is supported.
#ifndef WITH_DAWN_NATIVE
        emscripten_sleep(10);
#else
        usleep(1000);
#endif
    }

    return init_error_code;
}

}  // namespace WebGPU
}  // namespace Internal
}  // namespace Runtime
}  // namespace Halide

using namespace Halide::Runtime::Internal::WebGPU;

extern "C" {

WEAK int halide_webgpu_device_malloc(void *user_context, halide_buffer_t *buf) {
    debug(user_context)
        << "WGPU: halide_webgpu_device_malloc (user_context: " << user_context
        << ", buf: " << buf << ")\n";

    if (buf->device) {
        return halide_error_code_success;
    }

    WgpuContext context(user_context);
    if (context.error_code) {
        return context.error_code;
    }

    ErrorScope error_scope(user_context, context.device);

    WGPUBufferDescriptor desc = {
        .nextInChain = nullptr,
        .label = nullptr,
        .usage = WGPUBufferUsage_Storage |
                 WGPUBufferUsage_CopyDst |
                 WGPUBufferUsage_CopySrc,
        .size = roundUpToMultipleOf4(buf->size_in_bytes()),
        .mappedAtCreation = false,
    };
    WgpuBufferHandle *device_handle =
        (WgpuBufferHandle *)malloc(sizeof(WgpuBufferHandle));
    device_handle->buffer = wgpuDeviceCreateBuffer(context.device, &desc);
    device_handle->offset = 0;

    int error_code = error_scope.wait();
    if (error_code != halide_error_code_success) {
        return error_code;
    }

    if (staging_buffer == nullptr) {
        ErrorScope error_scope(user_context, context.device);

        // Create a staging buffer for transfers if we haven't already.
        WGPUBufferDescriptor desc = {
            .nextInChain = nullptr,
            .label = nullptr,
            .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead,
            .size = kWebGpuStagingBufferSize,
            .mappedAtCreation = false,
        };
        staging_buffer = wgpuDeviceCreateBuffer(global_device, &desc);

        int error_code = error_scope.wait();
        if (error_code != halide_error_code_success) {
            staging_buffer = nullptr;
            return error_code;
        }
    }

    buf->device = (uint64_t)device_handle;
    buf->device_interface = &webgpu_device_interface;
    buf->device_interface->impl->use_module();

    debug(user_context)
        << "      Allocated device buffer " << (void *)buf->device << "\n";

    return halide_error_code_success;
}

WEAK int halide_webgpu_device_free(void *user_context, halide_buffer_t *buf) {
    if (buf->device == 0) {
        return 0;
    }

    WgpuBufferHandle *handle = (WgpuBufferHandle *)buf->device;

    debug(user_context)
        << "WGPU: halide_webgpu_device_free (user_context: " << user_context
        << ", buf: " << buf << ") WGPUBuffer: " << handle->buffer << "\n";

    WgpuContext context(user_context);
    if (context.error_code) {
        return context.error_code;
    }

    wgpuBufferRelease(handle->buffer);
    free(handle);
    buf->device = 0;
    buf->device_interface->impl->release_module();
    buf->device_interface = nullptr;

    return halide_error_code_success;
}

WEAK int halide_webgpu_device_sync(void *user_context, halide_buffer_t *) {
    WgpuContext context(user_context);
    if (context.error_code) {
        return context.error_code;
    }

    ErrorScope error_scope(user_context, context.device);

    // Wait for all work on the queue to finish.
    struct WorkDoneResult {
        volatile ScopedSpinLock::AtomicFlag complete = false;
        volatile WGPUQueueWorkDoneStatus status;
    };
    WorkDoneResult result;

    __atomic_test_and_set(&result.complete, __ATOMIC_RELAXED);
    wgpuQueueOnSubmittedWorkDone(
        context.queue, 0,
        [](WGPUQueueWorkDoneStatus status, void *userdata) {
            WorkDoneResult *result = (WorkDoneResult *)userdata;
            result->status = status;
            __atomic_clear(&result->complete, __ATOMIC_RELEASE);
        },
        &result);

    int error_code = error_scope.wait();
    if (error_code != halide_error_code_success) {
        return error_code;
    }

    while (__atomic_test_and_set(&result.complete, __ATOMIC_ACQUIRE)) {
        wgpuDeviceTick(context.device);
    }

    return result.status == WGPUQueueWorkDoneStatus_Success ?
               halide_error_code_success :
               halide_error_code_device_sync_failed;
}

WEAK int halide_webgpu_device_release(void *user_context) {
    debug(user_context)
        << "WGPU: halide_webgpu_device_release (user_context: " << user_context
        << ")\n";

    // The WgpuContext object does not allow the context storage to be modified,
    // so we use halide_acquire_context directly.
    int err;
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    err = halide_webgpu_acquire_context(user_context,
                                        &instance, &adapter, &device, false);
    if (err != halide_error_code_success) {
        return err;
    }

    if (device) {
        shader_cache.delete_context(user_context, device,
                                    wgpuShaderModuleRelease);

        if (staging_buffer) {
            wgpuBufferRelease(staging_buffer);
            staging_buffer = nullptr;
        }

        // Release the device/adapter/instance, if we created them.
        if (device == global_device) {
            wgpuDeviceSetDeviceLostCallback(device, nullptr, nullptr);
            wgpuDeviceRelease(device);
            global_device = nullptr;

            wgpuAdapterRelease(adapter);
            global_adapter = nullptr;

            // TODO: Unify this when Emscripten supports wgpuInstanceRelease().
#ifdef WITH_DAWN_NATIVE
            wgpuInstanceRelease(instance);
            global_instance = nullptr;
#endif
        }
    }

    halide_webgpu_release_context(user_context);

    return 1;
}

WEAK int halide_webgpu_device_and_host_malloc(void *user_context,
                                              struct halide_buffer_t *buf) {
    return halide_default_device_and_host_malloc(user_context, buf,
                                                 &webgpu_device_interface);
}

WEAK int halide_webgpu_device_and_host_free(void *user_context,
                                            struct halide_buffer_t *buf) {
    return halide_default_device_and_host_free(user_context, buf,
                                               &webgpu_device_interface);
}

namespace {

// Copy `size` bytes of data from buffer `src` to a host pointer `dst`.
int do_copy_to_host(void *user_context, WgpuContext *context, uint8_t *dst,
                    WGPUBuffer src, int64_t src_offset, int64_t size) {
    // Copy chunks via the staging buffer.
    for (int64_t offset = 0; offset < size;
         offset += kWebGpuStagingBufferSize) {
        int64_t num_bytes = kWebGpuStagingBufferSize;
        if (offset + num_bytes > size) {
            num_bytes = size - offset;
        }

        // Copy this chunk to the staging buffer.
        WGPUCommandEncoder encoder =
            wgpuDeviceCreateCommandEncoder(context->device, nullptr);
        wgpuCommandEncoderCopyBufferToBuffer(encoder, src, src_offset + offset,
                                             staging_buffer, 0, num_bytes);
        WGPUCommandBuffer command_buffer =
            wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(context->queue, 1, &command_buffer);

        struct BufferMapResult {
            volatile ScopedSpinLock::AtomicFlag map_complete;
            volatile WGPUBufferMapAsyncStatus map_status;
        };
        BufferMapResult result;

        // Map the staging buffer for reading.
        __atomic_test_and_set(&result.map_complete, __ATOMIC_RELAXED);
        wgpuBufferMapAsync(
            staging_buffer, WGPUMapMode_Read, 0, num_bytes,
            [](WGPUBufferMapAsyncStatus status, void *userdata) {
                BufferMapResult *result = (BufferMapResult *)userdata;
                result->map_status = status;
                __atomic_clear(&result->map_complete, __ATOMIC_RELEASE);
            },
            &result);

        while (__atomic_test_and_set(&result.map_complete, __ATOMIC_ACQUIRE)) {
            wgpuDeviceTick(context->device);
        }
        if (result.map_status != WGPUBufferMapAsyncStatus_Success) {
            debug(user_context) << "wgpuBufferMapAsync failed: "
                                << result.map_status << "\n";
            return halide_error_code_copy_to_host_failed;
        }

        // Copy the data from the mapped staging buffer to the host allocation.
        const void *src = wgpuBufferGetConstMappedRange(staging_buffer, 0,
                                                        num_bytes);
        memcpy(dst + offset, src, num_bytes);
        wgpuBufferUnmap(staging_buffer);
    }

    return halide_error_code_success;
}

int do_multidimensional_copy(void *user_context, WgpuContext *context,
                             const device_copy &c,
                             int64_t src_idx, int64_t dst_idx,
                             int d, bool from_host, bool to_host) {
    if (d > MAX_COPY_DIMS) {
        error(user_context)
            << "Buffer has too many dimensions to copy to/from GPU\n";
        return -1;
    } else if (d == 0) {
        int err = 0;

        WgpuBufferHandle *src = (WgpuBufferHandle *)(c.src);
        WgpuBufferHandle *dst = (WgpuBufferHandle *)(c.dst);

        debug(user_context) << "    from " << (from_host ? "host" : "device")
                            << " to " << (to_host ? "host" : "device") << ", "
                            << (void *)c.src << " + " << src_idx
                            << " -> " << (void *)c.dst << " + " << dst_idx
                            << ", " << c.chunk_size << " bytes\n";
        uint64_t copy_size = roundUpToMultipleOf4(c.chunk_size);
        if (!from_host && to_host) {
            err = do_copy_to_host(user_context, context,
                                  (uint8_t *)(c.dst + dst_idx),
                                  src->buffer, src_idx + src->offset,
                                  copy_size);
        } else if (from_host && !to_host) {
            wgpuQueueWriteBuffer(context->queue, dst->buffer,
                                 dst_idx + dst->offset,
                                 (void *)(c.src + src_idx), copy_size);
        } else if (!from_host && !to_host) {
            // Create a command encoder and encode a copy command.
            WGPUCommandEncoder encoder =
                wgpuDeviceCreateCommandEncoder(context->device, nullptr);
            wgpuCommandEncoderCopyBufferToBuffer(encoder,
                                                 src->buffer,
                                                 src_idx + src->offset,
                                                 dst->buffer,
                                                 dst_idx + dst->offset,
                                                 c.chunk_size);

            // Submit the copy command.
            WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, nullptr);
            wgpuQueueSubmit(context->queue, 1, &cmd);
            wgpuCommandEncoderRelease(encoder);
        } else if ((c.dst + dst_idx) != (c.src + src_idx)) {
            // Could reach here if a user called directly into the
            // WebGPU API for a device->host copy on a source buffer
            // with device_dirty = false.
            halide_debug_assert(user_context, false && "unimplemented");
        }

        return err;
    } else {
        ssize_t src_off = 0, dst_off = 0;
        for (int i = 0; i < (int)c.extent[d - 1]; i++) {
            int err = do_multidimensional_copy(user_context, context, c,
                                               src_idx + src_off,
                                               dst_idx + dst_off,
                                               d - 1, from_host, to_host);
            dst_off += c.dst_stride_bytes[d - 1];
            src_off += c.src_stride_bytes[d - 1];
            if (err) {
                return err;
            }
        }
    }
    return halide_error_code_success;
}

}  // namespace

WEAK int halide_webgpu_buffer_copy(void *user_context,
                                   struct halide_buffer_t *src,
                                   const struct halide_device_interface_t *dst_device_interface,
                                   struct halide_buffer_t *dst) {
    debug(user_context)
        << "WGPU: halide_webgpu_buffer_copy (user_context: " << user_context
        << ", src: " << src << ", dst: " << dst << ")\n";

    // We only handle copies between WebGPU devices or to/from the host.
    halide_abort_if_false(user_context,
                          dst_device_interface == nullptr ||
                              dst_device_interface == &webgpu_device_interface);

    if ((src->device_dirty() || src->host == nullptr) &&
        src->device_interface != &webgpu_device_interface) {
        halide_abort_if_false(user_context,
                              dst_device_interface == &webgpu_device_interface);
        // This is handled at the higher level.
        return halide_error_code_incompatible_device_interface;
    }

    bool from_host = (src->device_interface != &webgpu_device_interface) ||
                     (src->device == 0) ||
                     (src->host_dirty() && src->host != nullptr);
    bool to_host = !dst_device_interface;

    halide_abort_if_false(user_context, from_host || src->device);
    halide_abort_if_false(user_context, to_host || dst->device);

    device_copy c = make_buffer_copy(src, from_host, dst, to_host);

    int err = halide_error_code_success;
    {
        WgpuContext context(user_context);
        if (context.error_code) {
            return context.error_code;
        }

        ErrorScope error_scope(user_context, context.device);

        err = do_multidimensional_copy(user_context, &context, c,
                                       c.src_begin, 0, dst->dimensions,
                                       from_host, to_host);
        if (err == halide_error_code_success) {
            err = error_scope.wait();
        }
    }

    return err;
}

WEAK int halide_webgpu_copy_to_device(void *user_context,
                                      halide_buffer_t *buf) {
    return halide_webgpu_buffer_copy(user_context, buf,
                                     &webgpu_device_interface, buf);
}

WEAK int halide_webgpu_copy_to_host(void *user_context, halide_buffer_t *buf) {
    return halide_webgpu_buffer_copy(user_context, buf,
                                     nullptr, buf);
}

namespace {

WEAK int webgpu_device_crop_from_offset(void *user_context,
                                        const struct halide_buffer_t *src,
                                        int64_t offset,
                                        struct halide_buffer_t *dst) {
    WgpuContext context(user_context);
    if (context.error_code) {
        return context.error_code;
    }

    dst->device_interface = src->device_interface;

    WgpuBufferHandle *src_handle = (WgpuBufferHandle *)src->device;
    wgpuBufferReference(src_handle->buffer);

    WgpuBufferHandle *dst_handle =
        (WgpuBufferHandle *)malloc(sizeof(WgpuBufferHandle));
    dst_handle->buffer = src_handle->buffer;
    dst_handle->offset = src_handle->offset + offset;
    dst->device = (uint64_t)dst_handle;

    return 0;
}

}  // namespace

WEAK int halide_webgpu_device_crop(void *user_context,
                                   const struct halide_buffer_t *src,
                                   struct halide_buffer_t *dst) {
    const int64_t offset = calc_device_crop_byte_offset(src, dst);
    return webgpu_device_crop_from_offset(user_context, src, offset, dst);
}

WEAK int halide_webgpu_device_slice(void *user_context,
                                    const struct halide_buffer_t *src,
                                    int slice_dim,
                                    int slice_pos,
                                    struct halide_buffer_t *dst) {
    const int64_t offset =
        calc_device_slice_byte_offset(src, slice_dim, slice_pos);
    return webgpu_device_crop_from_offset(user_context, src, offset, dst);
}

WEAK int halide_webgpu_device_release_crop(void *user_context,
                                           struct halide_buffer_t *buf) {
    WgpuBufferHandle *handle = (WgpuBufferHandle *)buf->device;

    debug(user_context)
        << "WGPU: halide_webgpu_device_release_crop (user_context: "
        << user_context << ", buf: " << buf << ") WGPUBuffer: "
        << handle->buffer << " offset: " << handle->offset << "\n";

    WgpuContext context(user_context);
    if (context.error_code) {
        return context.error_code;
    }

    wgpuBufferRelease(handle->buffer);
    free(handle);
    buf->device = 0;

    return halide_error_code_success;
}

WEAK int halide_webgpu_wrap_native(void *user_context, struct halide_buffer_t *buf, uint64_t mem) {
    // TODO: Implement this.
    halide_debug_assert(user_context, false && "unimplemented");
    return 1;
}

WEAK int halide_webgpu_detach_native(void *user_context, halide_buffer_t *buf) {
    // TODO: Implement this.
    halide_debug_assert(user_context, false && "unimplemented");
    return 1;
}

WEAK int halide_webgpu_initialize_kernels(void *user_context, void **state_ptr, const char *src, int size) {
    debug(user_context)
        << "WGPU: halide_webgpu_initialize_kernels (user_context: " << user_context
        << ", state_ptr: " << state_ptr
        << ", program: " << (void *)src
        << ", size: " << size << ")\n";

    WgpuContext context(user_context);
    if (context.error_code) {
        return context.error_code;
    }

    // Get the shader module from the cache, compiling it if necessary.
    WGPUShaderModule shader_module;
    if (!shader_cache.kernel_state_setup(
            user_context, state_ptr, context.device, shader_module,
            [&]() -> WGPUShaderModule {
                ErrorScope error_scope(user_context, context.device);

                WGPUShaderModuleWGSLDescriptor wgsl_desc = {
                    .chain = {
                        .next = nullptr,
                        .sType = WGPUSType_ShaderModuleWGSLDescriptor,
                    },
                    .source = src,
                };
                WGPUShaderModuleDescriptor desc = {
                    .nextInChain = (const WGPUChainedStruct *)(&wgsl_desc),
                    .label = nullptr,
                };
                WGPUShaderModule shader_module =
                    wgpuDeviceCreateShaderModule(context.device, &desc);

                int error_code = error_scope.wait();
                if (error_code != halide_error_code_success) {
                    return nullptr;
                }

                return shader_module;
            })) {
        return halide_error_code_generic_error;
    }
    halide_abort_if_false(user_context, shader_module != nullptr);

    return halide_error_code_success;
}

WEAK void halide_webgpu_finalize_kernels(void *user_context, void *state_ptr) {
    debug(user_context)
        << "WGPU: halide_webgpu_finalize_kernels (user_context: "
        << user_context << ", state_ptr: " << state_ptr << "\n";

    WgpuContext context(user_context);
    if (context.error_code == halide_error_code_success) {
        shader_cache.release_hold(user_context, context.device, state_ptr);
    }
}

WEAK int halide_webgpu_run(void *user_context,
                           void *state_ptr,
                           const char *entry_name,
                           int groupsX, int groupsY, int groupsZ,
                           int threadsX, int threadsY, int threadsZ,
                           int workgroup_mem_bytes,
                           halide_type_t arg_types[],
                           void *args[],
                           int8_t arg_is_buffer[]) {
    debug(user_context)
        << "WGPU: halide_webgpu_run (user_context: " << user_context << ", "
        << "entry: " << entry_name << ", "
        << "groups: " << groupsX << "x" << groupsY << "x" << groupsZ << ", "
        << "threads: " << threadsX << "x" << threadsY << "x" << threadsZ << ", "
        << "workgroup_mem: " << workgroup_mem_bytes << "\n";

    WgpuContext context(user_context);
    if (context.error_code) {
        return context.error_code;
    }

    ErrorScope error_scope(user_context, context.device);

    WGPUShaderModule shader_module = nullptr;
    bool found = shader_cache.lookup(context.device, state_ptr, shader_module);
    halide_abort_if_false(user_context, found && shader_module != nullptr);

    // TODO: Add support for dynamically-sized shared memory via a
    // pipeline-overridable workgroup array.
    // halide_abort_if_false(user_context, workgroup_mem_bytes == 0);

    // Create the compute pipeline.
    WGPUProgrammableStageDescriptor stage_desc = {
        .nextInChain = nullptr,
        .module = shader_module,
        .entryPoint = entry_name,
        .constantCount = 0,
        .constants = nullptr,
    };
    WGPUComputePipelineDescriptor pipeline_desc = {
        .nextInChain = nullptr,
        .label = nullptr,
        .layout = nullptr,
        .compute = stage_desc,
    };
    WGPUComputePipeline pipeline =
        wgpuDeviceCreateComputePipeline(context.device, &pipeline_desc);

    // Set up a compute shader dispatch command.
    WGPUCommandEncoder encoder =
        wgpuDeviceCreateCommandEncoder(context.device, nullptr);
    WGPUComputePassEncoder pass =
        wgpuCommandEncoderBeginComputePass(encoder, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, pipeline);

    // Process function arguments.
    uint32_t num_args = 0;
    uint32_t num_buffers = 0;
    uint32_t uniform_size = 0;
    while (args[num_args] != nullptr) {
        if (arg_is_buffer[num_args]) {
            num_buffers++;
        } else {
            uint32_t arg_size = arg_types[num_args].bytes();
            halide_abort_if_false(user_context, arg_size <= 4);

            // Round up to 4 bytes.
            arg_size = (arg_size + 3) & ~3;

            uniform_size += arg_size;
        }
        num_args++;
    }
    if (num_buffers > 0) {
        // Set up a bind group entry for each buffer argument.
        WGPUBindGroupEntry *bind_group_entries =
            (WGPUBindGroupEntry *)malloc(
                num_buffers * sizeof(WGPUBindGroupEntry));
        for (uint32_t i = 0, b = 0; i < num_args; i++) {
            if (arg_is_buffer[i]) {
                halide_buffer_t *buffer = (halide_buffer_t *)args[i];
                WgpuBufferHandle *handle = (WgpuBufferHandle *)(buffer->device);
                bind_group_entries[b] = WGPUBindGroupEntry{
                    .nextInChain = nullptr,
                    .binding = i,
                    .buffer = handle->buffer,
                    .offset = handle->offset,
                    .size = roundUpToMultipleOf4(buffer->size_in_bytes()),
                    .sampler = nullptr,
                    .textureView = nullptr,
                };
                b++;
            }
        }

        // Create a bind group for the buffer arguments.
        WGPUBindGroupLayout layout =
            wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
        WGPUBindGroupDescriptor bindgroup_desc = {
            .nextInChain = nullptr,
            .label = nullptr,
            .layout = layout,
            .entryCount = num_buffers,
            .entries = bind_group_entries,
        };
        WGPUBindGroup bind_group =
            wgpuDeviceCreateBindGroup(context.device, &bindgroup_desc);
        wgpuComputePassEncoderSetBindGroup(pass, 0, bind_group, 0, nullptr);
        wgpuBindGroupRelease(bind_group);
        wgpuBindGroupLayoutRelease(layout);

        free(bind_group_entries);
    }
    if (num_args > num_buffers) {
        // Create a uniform buffer for the non-buffer arguments.
        WGPUBufferDescriptor desc = {
            .nextInChain = nullptr,
            .label = nullptr,
            .usage = WGPUBufferUsage_Uniform,
            .size = uniform_size,
            .mappedAtCreation = true,
        };
        WGPUBuffer arg_buffer = wgpuDeviceCreateBuffer(context.device, &desc);

        // Write the argument values to the uniform buffer.
        uint32_t *arg_values =
            (uint32_t *)wgpuBufferGetMappedRange(arg_buffer, 0, uniform_size);
        for (uint32_t a = 0, i = 0; a < num_args; a++) {
            if (arg_is_buffer[a]) {
                continue;
            }

            halide_type_t arg_type = arg_types[a];
            halide_abort_if_false(user_context, arg_type.lanes == 1);
            halide_abort_if_false(user_context, arg_type.bits > 0);
            halide_abort_if_false(user_context, arg_type.bits <= 32);

            void *arg_in = args[a];
            void *arg_out = &arg_values[i++];

            // Copy the argument value, expanding it to 32-bits.
            switch (arg_type.code) {
            case halide_type_float: {
                halide_abort_if_false(user_context, arg_type.bits == 32);
                *(float *)arg_out = *(float *)arg_in;
                break;
            }
            case halide_type_int: {
                switch (arg_type.bits) {
                case 1: {
                    *(int32_t *)arg_out = *((int8_t *)arg_in);
                }
                case 8: {
                    *(int32_t *)arg_out = *((int8_t *)arg_in);
                    break;
                }
                case 16: {
                    *(int32_t *)arg_out = *((int16_t *)arg_in);
                    break;
                }
                case 32: {
                    *(int32_t *)arg_out = *((int32_t *)arg_in);
                    break;
                }
                default: {
                    halide_abort_if_false(user_context, false);
                }
                }
                break;
            }
            case halide_type_uint: {
                switch (arg_type.bits) {
                case 1: {
                    *(uint32_t *)arg_out = *((uint8_t *)arg_in);
                }
                case 8: {
                    *(uint32_t *)arg_out = *((uint8_t *)arg_in);
                    break;
                }
                case 16: {
                    *(uint32_t *)arg_out = *((uint16_t *)arg_in);
                    break;
                }
                case 32: {
                    *(uint32_t *)arg_out = *((uint32_t *)arg_in);
                    break;
                }
                default: {
                    halide_abort_if_false(user_context, false);
                }
                }
                break;
            }
            default: {
                halide_abort_if_false(user_context, false && "unhandled type");
            }
            }
        }
        wgpuBufferUnmap(arg_buffer);

        // Create a bind group for the uniform buffer.
        WGPUBindGroupLayout layout =
            wgpuComputePipelineGetBindGroupLayout(pipeline, 1);
        WGPUBindGroupEntry entry = {
            .nextInChain = nullptr,
            .binding = 0,
            .buffer = arg_buffer,
            .offset = 0,
            .size = uniform_size,
            .sampler = nullptr,
            .textureView = nullptr,
        };
        WGPUBindGroupDescriptor bindgroup_desc = {
            .nextInChain = nullptr,
            .label = nullptr,
            .layout = layout,
            .entryCount = 1,
            .entries = &entry,
        };
        WGPUBindGroup bind_group =
            wgpuDeviceCreateBindGroup(context.device, &bindgroup_desc);
        wgpuComputePassEncoderSetBindGroup(pass, 1, bind_group, 0, nullptr);
        wgpuBindGroupRelease(bind_group);
        wgpuBindGroupLayoutRelease(layout);

        wgpuBufferRelease(arg_buffer);
    }

    wgpuComputePassEncoderDispatchWorkgroups(pass, groupsX, groupsY, groupsZ);
    wgpuComputePassEncoderEnd(pass);

    // Submit the compute command.
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(context.queue, 1, &commands);

    wgpuCommandEncoderRelease(encoder);
    wgpuComputePipelineRelease(pipeline);

    return error_scope.wait();
}

WEAK const struct halide_device_interface_t *halide_webgpu_device_interface() {
    return &webgpu_device_interface;
}

namespace {
WEAK __attribute__((destructor)) void halide_webgpu_cleanup() {
    shader_cache.release_all(nullptr, wgpuShaderModuleRelease);
    halide_webgpu_device_release(nullptr);
}
}  // namespace

}  // extern "C" linkage

namespace Halide {
namespace Runtime {
namespace Internal {
namespace WebGPU {

WEAK halide_device_interface_impl_t webgpu_device_interface_impl = {
    halide_use_jit_module,
    halide_release_jit_module,
    halide_webgpu_device_malloc,
    halide_webgpu_device_free,
    halide_webgpu_device_sync,
    halide_webgpu_device_release,
    halide_webgpu_copy_to_host,
    halide_webgpu_copy_to_device,
    halide_webgpu_device_and_host_malloc,
    halide_webgpu_device_and_host_free,
    halide_webgpu_buffer_copy,
    halide_webgpu_device_crop,
    halide_webgpu_device_slice,
    halide_webgpu_device_release_crop,
    halide_webgpu_wrap_native,
    halide_webgpu_detach_native,
};

WEAK halide_device_interface_t webgpu_device_interface = {
    halide_device_malloc,
    halide_device_free,
    halide_device_sync,
    halide_device_release,
    halide_copy_to_host,
    halide_copy_to_device,
    halide_device_and_host_malloc,
    halide_device_and_host_free,
    halide_buffer_copy,
    halide_device_crop,
    halide_device_slice,
    halide_device_release_crop,
    halide_device_wrap_native,
    halide_device_detach_native,
    nullptr,
    &webgpu_device_interface_impl};

}  // namespace WebGPU
}  // namespace Internal
}  // namespace Runtime
}  // namespace Halide
