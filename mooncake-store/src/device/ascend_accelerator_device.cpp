#include "device/accelerator_registry.h"
#include "pinned_host_buffer.h"

#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || \
    defined(USE_UBSHMEM) || defined(USE_ASCEND_NDR) ||   \
    defined(USE_ASCEND_STAGING)
#include <acl/acl_rt.h>
#endif

#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || \
    defined(USE_UBSHMEM) || defined(USE_ASCEND_NDR) ||   \
    defined(USE_ASCEND_STAGING)

namespace mooncake {
namespace device {
namespace {

void FreeAscendPinnedHostBuffer(void* addr) { aclrtFreeHost(addr); }

class AscendAcceleratorDevice final : public ProbeCachedAcceleratorDevice {
   public:
    AcceleratorVendor Vendor() const override {
        return AcceleratorVendor::kAscend;
    }

    bool ProbeAvailable() const override {
        uint32_t count = 0;
        return aclrtGetDeviceCount(&count) == ACL_SUCCESS && count > 0;
    }

    PointerInfo QueryPointer(const void* ptr) const override {
        aclrtPtrAttributes attr{};
        if (aclrtPointerGetAttributes(const_cast<void*>(ptr), &attr) ==
                ACL_SUCCESS &&
            attr.location.type == ACL_MEM_LOCATION_TYPE_DEVICE) {
            return PointerInfo{
                .kind = MemoryKind::kDevice,
                .device_id = static_cast<int32_t>(attr.location.id),
            };
        }
        return PointerInfo{.kind = MemoryKind::kHost, .device_id = -1};
    }

    int32_t CurrentDeviceId() const override {
        int32_t logic_dev = 0;
        if (aclrtGetDevice(&logic_dev) != ACL_SUCCESS) {
            return -1;
        }
        return logic_dev;
    }

    void SetContext(int32_t device_id) const override {
        if (device_id >= 0) aclrtSetDevice(device_id);
    }

    bool Copy(void* dst, const void* src, size_t size,
              CopyDirection direction) const override {
        aclrtMemcpyKind kind = ACL_MEMCPY_HOST_TO_HOST;
        switch (direction) {
            case CopyDirection::kHostToDevice:
                kind = ACL_MEMCPY_HOST_TO_DEVICE;
                break;
            case CopyDirection::kDeviceToHost:
                kind = ACL_MEMCPY_DEVICE_TO_HOST;
                break;
            case CopyDirection::kDeviceToDevice:
                kind = ACL_MEMCPY_DEVICE_TO_DEVICE;
                break;
            case CopyDirection::kHostToHost:
                kind = ACL_MEMCPY_HOST_TO_HOST;
                break;
            case CopyDirection::kAuto: {
                auto src_info = QueryPointer(src);
                auto dst_info = QueryPointer(dst);
                if (src_info.kind == MemoryKind::kDevice &&
                    dst_info.kind == MemoryKind::kDevice) {
                    kind = ACL_MEMCPY_DEVICE_TO_DEVICE;
                } else if (src_info.kind == MemoryKind::kDevice) {
                    kind = ACL_MEMCPY_DEVICE_TO_HOST;
                } else if (dst_info.kind == MemoryKind::kDevice) {
                    kind = ACL_MEMCPY_HOST_TO_DEVICE;
                }
                break;
            }
        }
        return aclrtMemcpy(dst, size, src, size, kind) == ACL_SUCCESS;
    }

    bool ValidateRange(const void* ptr, size_t length) const override {
        void* base = nullptr;
        size_t capacity = 0;
        if (aclrtMemGetAddressRange(const_cast<void*>(ptr), &base, &capacity) !=
            ACL_SUCCESS)
            return false;
        const auto address = reinterpret_cast<uintptr_t>(ptr);
        const auto start = reinterpret_cast<uintptr_t>(base);
        return address >= start && length <= capacity &&
               address - start <= capacity - length;
    }

    bool CopyWithContext(void* dst, const void* src, size_t size,
                         CopyDirection direction,
                         int32_t device_id) const override {
        aclrtContext previous = nullptr;
        (void)aclrtGetCurrentContext(&previous);
        if (device_id >= 0 && aclrtSetDevice(device_id) != ACL_SUCCESS)
            return false;
        bool ok = Copy(dst, src, size, direction);
        if (previous && aclrtSetCurrentContext(previous) != ACL_SUCCESS)
            ok = false;
        return ok;
    }

    PinnedHostBuffer AllocatePinnedHost(size_t size) const override {
        void* addr = nullptr;
        if (aclrtMallocHost(&addr, size) != ACL_SUCCESS) {
            return PinnedHostBuffer();
        }
        return PinnedHostBuffer(addr, size, FreeAscendPinnedHostBuffer);
    }
};

const AcceleratorDevice& AscendDeviceInstance() {
    static AscendAcceleratorDevice device;
    return device;
}

const AcceleratorDeviceRegistrar registered_ascend_device(
    AscendDeviceInstance());

}  // namespace

void EnsureAscendAcceleratorDeviceLinked() { (void)AscendDeviceInstance(); }

}  // namespace device
}  // namespace mooncake

#endif
