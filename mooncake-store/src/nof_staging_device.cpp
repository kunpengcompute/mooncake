#include "nof_staging_device.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
#define MC_NOF_STAGING_ASCEND_RUNTIME
#include <acl/acl_rt.h>
#elif defined(USE_CUDA) && !defined(USE_HIP) && !defined(USE_MUSA) &&   \
    !defined(USE_MACA) && !defined(USE_HYGON) && !defined(USE_COREX) && \
    !defined(USE_MLU) && !defined(USE_SUNRISE)
#define MC_NOF_STAGING_CUDA_RUNTIME
#include <cuda_runtime.h>
#endif

namespace mooncake::nof_staging {
namespace {

// Deliberately separate from gpu_staging_utils: unknown pointers must never
// silently take a host memcpy path, and restoration failures are errors here.
class ScopedDevice {
   public:
    explicit ScopedDevice(const Device& device) {
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
        if (device.backend != Backend::Ascend || device.id < 0 ||
            device.context == nullptr ||
            aclrtGetCurrentContext(&saved_context_) != ACL_SUCCESS ||
            saved_context_ == nullptr) {
            return;
        }
        restore_needed_ = true;
        ready_ = aclrtSetCurrentContext(device.context) == ACL_SUCCESS;
#elif defined(MC_NOF_STAGING_CUDA_RUNTIME)
        if (device.backend != Backend::Cuda || device.id < 0 ||
            cudaGetDevice(&saved_device_) != cudaSuccess) {
            return;
        }
        restore_needed_ = true;
        ready_ = cudaSetDevice(device.id) == cudaSuccess;
#else
        (void)device;
#endif
    }

    ScopedDevice(const ScopedDevice&) = delete;
    ScopedDevice& operator=(const ScopedDevice&) = delete;
    ~ScopedDevice() { Restore(); }
    bool ready() const { return ready_; }

    bool Finish(bool result) {
        const bool restored = Restore();
        return result && restored;
    }

   private:
    bool Restore() {
        if (!restore_needed_) return true;
        bool restored = false;
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
        restored = aclrtSetCurrentContext(saved_context_) == ACL_SUCCESS;
#elif defined(MC_NOF_STAGING_CUDA_RUNTIME)
        restored = cudaSetDevice(saved_device_) == cudaSuccess;
#endif
        if (restored) restore_needed_ = false;
        return restored;
    }

    bool ready_ = false;
    bool restore_needed_ = false;
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
    aclrtContext saved_context_ = nullptr;
#elif defined(MC_NOF_STAGING_CUDA_RUNTIME)
    int saved_device_ = -1;
#endif
};

#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
// Owned by one batch session. Each copy group drains before returning, while
// destruction selects the stream's context even during exception unwinding.
class ScopedCopyStream {
   public:
    explicit ScopedCopyStream(const Device& device) : device_(device) {
        ready_ =
            aclrtCreateStream(&stream_) == ACL_SUCCESS && stream_ != nullptr;
    }
    ScopedCopyStream(const ScopedCopyStream&) = delete;
    ScopedCopyStream& operator=(const ScopedCopyStream&) = delete;
    ~ScopedCopyStream() { Finish(); }

    bool ready() const { return ready_; }

    bool Copy(const CopySlice& slice, bool to_host) {
        const auto kind =
            to_host ? ACL_MEMCPY_DEVICE_TO_HOST : ACL_MEMCPY_HOST_TO_DEVICE;
        // A failed enqueue can still have submitted part of a large copy.
        pending_ = true;
        return aclrtMemcpyAsync(slice.dst, slice.size, slice.src, slice.size,
                                kind, stream_) == ACL_SUCCESS;
    }

    bool Synchronize() {
        if (!pending_) return true;
        if (aclrtSynchronizeStream(stream_) != ACL_SUCCESS) return false;
        pending_ = false;
        return true;
    }

    bool Finish(bool synchronization_failed = false) {
        if (!stream_) return true;
        ScopedDevice scope(device_);
        if (!scope.ready()) {
            // Successful Copy calls leave no pending DMA. If context teardown
            // prevents cleanup, report the idle resource leak rather than
            // invoking runtime APIs on an unrelated context.
            std::fprintf(stderr, "Cannot select NoF copy stream context\n");
            if (pending_) std::terminate();
            stream_ = nullptr;
            ready_ = false;
            return scope.Finish(false);
        }
        const bool completed = !synchronization_failed && Synchronize();
        // DestroyStream also waits for outstanding tasks. If neither operation
        // succeeds, returning would let callers reuse memory still used by DMA.
        const auto destroyed = aclrtDestroyStream(stream_);
        if (destroyed != ACL_SUCCESS) {
            std::fprintf(stderr, "NoF staging stream destruction failed: %d\n",
                         static_cast<int>(destroyed));
            if (!completed) std::terminate();
        }
        stream_ = nullptr;
        ready_ = false;
        return scope.Finish(completed && destroyed == ACL_SUCCESS);
    }

   private:
    Device device_;
    aclrtStream stream_ = nullptr;
    bool ready_ = false;
    bool pending_ = false;
};
#elif defined(MC_NOF_STAGING_CUDA_RUNTIME)
bool Synchronize() { return cudaDeviceSynchronize() == cudaSuccess; }
#endif

}  // namespace

struct CopySession::Impl {
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
    struct Entry {
        Device device;
        std::unique_ptr<ScopedCopyStream> stream;
    };
    std::vector<Entry> streams;

    ScopedCopyStream* Get(const Device& device) {
        for (auto& entry : streams) {
            if (entry.device == device) {
                if (!entry.stream || !entry.stream->ready()) {
                    entry.stream = std::make_unique<ScopedCopyStream>(device);
                }
                return entry.stream.get();
            }
        }
        // Grow bookkeeping before creating the runtime resource.
        streams.push_back({device, nullptr});
        auto& entry = streams.back();
        entry.stream = std::make_unique<ScopedCopyStream>(device);
        return entry.stream.get();
    }
#endif
};

CopySession::CopySession() = default;
CopySession::~CopySession() { Finish(); }

bool CopySession::Finish() {
    if (finished_) return finish_result_;
    finished_ = true;
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
    if (impl_) {
        for (auto& entry : impl_->streams) {
            if (entry.stream && !entry.stream->Finish()) finish_result_ = false;
        }
    }
#endif
    impl_.reset();
    return finish_result_;
}

std::optional<Device> DetectDeviceRange(const void* ptr, size_t size) {
    const auto start = reinterpret_cast<uintptr_t>(ptr);
    if (!ValidRange(start, size)) return std::nullopt;
    const auto last = reinterpret_cast<const void*>(start + size - 1);
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
    aclrtPtrAttributes first_attr{}, last_attr{};
    aclrtContext context = nullptr;
    int32_t logical_device = -1;
    if (aclrtPointerGetAttributes(const_cast<void*>(ptr), &first_attr) !=
            ACL_SUCCESS ||
        aclrtPointerGetAttributes(const_cast<void*>(last), &last_attr) !=
            ACL_SUCCESS ||
        first_attr.location.type != ACL_MEM_LOCATION_TYPE_DEVICE ||
        last_attr.location.type != ACL_MEM_LOCATION_TYPE_DEVICE ||
        first_attr.location.id != last_attr.location.id ||
        aclrtGetDevice(&logical_device) != ACL_SUCCESS || logical_device < 0 ||
        aclrtGetCurrentContext(&context) != ACL_SUCCESS || context == nullptr) {
        return std::nullopt;
    }
    // Follow gpu_staging::IsDevicePointer's device-ID convention without
    // allocating device memory. Only borrow a context for the same device.
    if (first_attr.location.id != static_cast<uint32_t>(logical_device)) {
        return std::nullopt;
    }
    return Device{Backend::Ascend, logical_device, context,
                  first_attr.location.id};
#elif defined(MC_NOF_STAGING_CUDA_RUNTIME)
    cudaPointerAttributes first_attr{}, last_attr{};
    if (cudaPointerGetAttributes(&first_attr, ptr) != cudaSuccess) {
        cudaGetLastError();
        return std::nullopt;
    }
    if (cudaPointerGetAttributes(&last_attr, last) != cudaSuccess) {
        cudaGetLastError();
        return std::nullopt;
    }
    if (first_attr.type != cudaMemoryTypeDevice ||
        last_attr.type != cudaMemoryTypeDevice || first_attr.device < 0 ||
        first_attr.device != last_attr.device) {
        return std::nullopt;
    }
    return Device{Backend::Cuda, first_attr.device};
#else
    (void)last;
    return std::nullopt;
#endif
}

bool CopyToHost(void* host, const void* device_ptr, size_t size,
                const Device& device) {
    const CopySlice slice{host, device_ptr, size, device};
    return Copy(&slice, 1, true);
}

bool CopyFromHost(void* device_ptr, const void* host, size_t size,
                  const Device& device) {
    const CopySlice slice{device_ptr, host, size, device};
    return Copy(&slice, 1, false);
}

bool Copy(const CopySlice* slices, size_t count, bool to_host) {
    CopySession session;
    const bool copied = session.Copy(slices, count, to_host);
    const bool finished = session.Finish();
    return copied && finished;
}

bool CopySession::Copy(const CopySlice* slices, size_t count, bool to_host) {
    if (finished_) return false;
    if (count == 0) return true;
    if (!slices) return false;
    // Validate the entire object before changing contexts or copying any bytes.
    for (size_t i = 0; i < count; ++i) {
        const auto& slice = slices[i];
        if (!ValidRange(reinterpret_cast<uintptr_t>(slice.dst), slice.size) ||
            !ValidRange(reinterpret_cast<uintptr_t>(slice.src), slice.size)) {
            return false;
        }
        if (slice.device && slice.device->backend == Backend::Ascend &&
            (reinterpret_cast<uintptr_t>(slice.dst) % kAscendCopyAlignment !=
                 0 ||
             reinterpret_cast<uintptr_t>(slice.src) % kAscendCopyAlignment !=
                 0)) {
            return false;
        }
    }

    const auto* begin = slices;
    const auto* last = slices + count;
    while (begin != last) {
        // A stream cannot cover another device/Context. Split only when the
        // device identity changes, preserving host/device slice order.
        std::optional<Device> device;
        auto end = begin;
        for (; end != last; ++end) {
            if (!end->device) continue;
            if (!device) {
                device = end->device;
            } else if (!(*device == *end->device)) {
                break;
            }
        }

        std::optional<ScopedDevice> scope;
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
        ScopedCopyStream* stream = nullptr;
#endif
        if (device) {
            scope.emplace(*device);
            if (!scope->ready()) return scope->Finish(false);
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
            // The caller has already waited for its producer stream/event.
            // This stream orders only this object's copies, not other work.
            if (!impl_) impl_ = std::make_unique<Impl>();
            stream = impl_->Get(*device);
            if (!stream->ready()) {
                stream->Finish();
                return scope->Finish(false);
            }
#elif defined(MC_NOF_STAGING_CUDA_RUNTIME)
            // No producer stream/event is supplied. Wait once before the
            // batch; the caller must avoid concurrent mutation afterwards.
            if (!Synchronize()) return scope->Finish(false);
#endif
        }

        bool copied = true;
        for (auto slice = begin; slice != end; ++slice) {
            if (!slice->device) {
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
                // Preserve ordering if a host slice depends on an earlier
                // device slice in the same object.
                if (stream && !stream->Synchronize()) {
                    copied = false;
                    break;
                }
#endif
                std::memcpy(slice->dst, slice->src, slice->size);
                continue;
            }
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
            copied = stream->Copy(*slice, to_host);
#elif defined(MC_NOF_STAGING_CUDA_RUNTIME)
            copied =
                cudaMemcpy(slice->dst, slice->src, slice->size,
                           to_host ? cudaMemcpyDeviceToHost
                                   : cudaMemcpyHostToDevice) == cudaSuccess;
#else
            (void)to_host;
            copied = false;
#endif
            if (!copied) break;
        }
        if (scope) {
#if defined(MC_NOF_STAGING_ASCEND_RUNTIME)
            const bool completed = stream->Synchronize();
            if (!copied || !completed) {
                // Retire a failed stream before this object's buffers can be
                // released; a later object may create a fresh one.
                stream->Finish(!completed);
            }
#elif defined(MC_NOF_STAGING_CUDA_RUNTIME)
            // CUDA H2D may return before device completion. Fence even after
            // a copy failure, before restoring the context/reusing scratch.
            const bool completed = Synchronize();
#else
            const bool completed = false;
#endif
            if (!scope->Finish(copied && completed)) return false;
        }
        begin = end;
    }
    return true;
}

}  // namespace mooncake::nof_staging
