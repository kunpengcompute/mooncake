"""Exercise the production NoF copy code with a deferred, CPU-only ACL mock.

The fake runtime copies bytes only when its stream is synchronized. This checks
ordering and completion without claiming to validate real CANN/SPDK DMA support.
Run with --cxx /path/to/a/C++20/compiler if no compiler is available on PATH.
"""

from nof_test_support import CppTestCase, main


ACL_HEADER = r"""
#pragma once
#include <cstddef>
#include <cstdint>
using aclError = int;
using aclrtContext = void*;
using aclrtStream = void*;
constexpr aclError ACL_SUCCESS = 0;
constexpr int ACL_MEM_LOCATION_TYPE_DEVICE = 1;
enum aclrtMemcpyKind { ACL_MEMCPY_HOST_TO_DEVICE, ACL_MEMCPY_DEVICE_TO_HOST };
struct aclrtPtrAttributes { struct { int type; uint32_t id; } location; };
aclError aclrtGetCurrentContext(aclrtContext*);
aclError aclrtSetCurrentContext(aclrtContext);
aclError aclrtGetDevice(int32_t*);
aclError aclrtPointerGetAttributes(void*, aclrtPtrAttributes*);
aclError aclrtCreateStream(aclrtStream*);
aclError aclrtDestroyStream(aclrtStream);
aclError aclrtMemcpyAsync(void*, size_t, const void*, size_t,
                        aclrtMemcpyKind, aclrtStream);
aclError aclrtSynchronizeStream(aclrtStream);
// A reference to either old API is a compile-time regression.
aclError aclrtMemcpy(void*, size_t, const void*, size_t, aclrtMemcpyKind) = delete;
aclError aclrtSynchronizeDevice() = delete;
"""


HARNESS = r"""
#include "nof_staging_device.h"
#include <acl/acl_rt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using mooncake::nof_staging::Backend;
using mooncake::nof_staging::Copy;
using mooncake::nof_staging::CopySession;
using mooncake::nof_staging::CopySlice;
using mooncake::nof_staging::CopyToHost;
using mooncake::nof_staging::Device;

void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Pending { void* dst; const void* src; size_t size; };
struct Stream {
    aclrtContext context;
    std::vector<Pending> pending;
};
struct Runtime {
    int initial_context = 0;
    int context_a = 1;
    int context_b = 2;
    aclrtContext current = &initial_context;
    int context_sets = 0;
    int creates = 0;
    int destroys = 0;
    int async_copies = 0;
    int syncs = 0;
    int fail_copy = 0;
    int fail_sync = 0;
    int fail_destroy = 0;
    bool partial_failed_enqueue = false;
    std::vector<Stream*> live;
    std::vector<size_t> sync_batch_sizes;
    std::vector<aclrtContext> creation_contexts;
    std::vector<aclrtMemcpyKind> directions;
} runtime;

aclError aclrtGetCurrentContext(aclrtContext* context) {
    *context = runtime.current;
    return ACL_SUCCESS;
}
aclError aclrtSetCurrentContext(aclrtContext context) {
    ++runtime.context_sets;
    runtime.current = context;
    return ACL_SUCCESS;
}
aclError aclrtGetDevice(int32_t* device) {
    *device = runtime.current == &runtime.context_b ? 1 : 0;
    return ACL_SUCCESS;
}
aclError aclrtPointerGetAttributes(void*, aclrtPtrAttributes* attr) {
    attr->location = {ACL_MEM_LOCATION_TYPE_DEVICE, 0};
    return ACL_SUCCESS;
}
aclError aclrtCreateStream(aclrtStream* handle) {
    ++runtime.creates;
    auto stream = new Stream{runtime.current, {}};
    runtime.live.push_back(stream);
    runtime.creation_contexts.push_back(runtime.current);
    *handle = stream;
    return ACL_SUCCESS;
}
aclError aclrtDestroyStream(aclrtStream handle) {
    auto* stream = static_cast<Stream*>(handle);
    Check(stream != nullptr, "destroy received the implicit/default stream");
    Check(stream->context == runtime.current, "stream destroyed in wrong context");
    ++runtime.destroys;
    if (runtime.destroys == runtime.fail_destroy) return 1;
    // ACL DestroyStream waits for outstanding tasks before successful return.
    for (const auto& copy : stream->pending) {
        std::memcpy(copy.dst, copy.src, copy.size);
    }
    stream->pending.clear();
    auto found = std::find(runtime.live.begin(), runtime.live.end(), stream);
    Check(found != runtime.live.end(), "stream destroyed twice");
    runtime.live.erase(found);
    delete stream;
    return ACL_SUCCESS;
}
aclError aclrtMemcpyAsync(void* dst, size_t dst_size, const void* src,
                         size_t count, aclrtMemcpyKind kind,
                         aclrtStream handle) {
    auto* stream = static_cast<Stream*>(handle);
    Check(stream != nullptr, "copy received the implicit/default stream");
    Check(stream->context == runtime.current, "copy submitted in wrong context");
    Check(dst_size >= count, "copy exceeds destination capacity");
    Check(reinterpret_cast<uintptr_t>(dst) % 64 == 0 &&
          reinterpret_cast<uintptr_t>(src) % 64 == 0,
          "async copy used an address that violates ACL's 64-byte alignment");
    ++runtime.async_copies;
    runtime.directions.push_back(kind);
    if (runtime.async_copies == runtime.fail_copy) {
        if (runtime.partial_failed_enqueue) {
            stream->pending.push_back({dst, src, 1});
        }
        return 1;
    }
    // Intentionally do not snapshot src or update dst before stream sync.
    stream->pending.push_back({dst, src, count});
    return ACL_SUCCESS;
}
aclError aclrtSynchronizeStream(aclrtStream handle) {
    auto* stream = static_cast<Stream*>(handle);
    Check(stream != nullptr, "sync received the implicit/default stream");
    Check(stream->context == runtime.current, "stream synchronized in wrong context");
    ++runtime.syncs;
    runtime.sync_batch_sizes.push_back(stream->pending.size());
    if (runtime.syncs == runtime.fail_sync) return 1;
    for (const auto& copy : stream->pending) {
        std::memcpy(copy.dst, copy.src, copy.size);
    }
    stream->pending.clear();
    return ACL_SUCCESS;
}
Device A() { return {Backend::Ascend, 0, &runtime.context_a, 0}; }
Device B() { return {Backend::Ascend, 1, &runtime.context_b, 1}; }
void CheckCleanup() {
    Check(runtime.live.empty(), "owned stream leaked");
    Check(runtime.current == &runtime.initial_context, "caller context not restored");
}
void ContextGroups() {
    alignas(64) unsigned char source[256]{};
    alignas(64) unsigned char destination[256]{};
    source[0] = 1; source[64] = 2; source[128] = 3; source[192] = 4;
    CopySlice slices[] = {
        {destination, source, 1, A()},
        {destination+64, source+64, 1, A()},
        {destination+128, source+128, 1, B()},
        {destination+192, source+192, 1, A()},
    };
    Check(Copy(slices, 4, true), "multiple-context copy failed");
    Check(std::memcmp(destination, source, sizeof(source)) == 0,
          "multiple-context bytes incorrect");
    Check(runtime.creation_contexts == std::vector<aclrtContext>{
              &runtime.context_a, &runtime.context_b},
          "copy groups failed to reuse the earlier context's stream");
    Check(runtime.sync_batch_sizes == std::vector<size_t>{2, 1, 1},
          "multiple contexts did not complete separately");
    CheckCleanup();
}
void MixedOrder() {
    alignas(64) unsigned char device_source = 9;
    alignas(64) unsigned char host_stage = 0;
    unsigned char host_middle = 0;
    alignas(64) unsigned char host_tail = 0;
    CopySlice slices[] = {
        {&host_stage, &device_source, 1, A()},
        {&host_middle, &host_stage, 1, std::nullopt},
        {&host_tail, &device_source, 1, A()},
    };
    Check(Copy(slices, 3, true), "mixed host/device slices failed");
    Check(host_stage == 9 && host_middle == 9 && host_tail == 9,
          "host memcpy ran before preceding device copy completed");
    Check(runtime.async_copies == 2, "host slice was sent to ACL");
    CheckCleanup();
}
void CopyFailure(bool partial = false) {
    runtime.fail_copy = 2;
    runtime.partial_failed_enqueue = partial;
    alignas(64) unsigned char source[192]{};
    alignas(64) unsigned char destination[192]{};
    source[0] = 11; source[64] = 12; source[128] = 13;
    source[65] = 22;
    CopySlice slices[] = {
        {destination, source, 4, A()},
        {destination+64, source+64, 4, A()},
        {destination+128, source+128, 4, A()},
    };
    Check(!Copy(slices, 3, true), "enqueue failure was reported as success");
    Check(runtime.async_copies == 2, "copy continued after enqueue failure");
    Check(destination[0] == 11 && destination[64] == (partial ? 12 : 0) &&
          destination[65] == 0 && destination[128] == 0,
          "previously submitted copy was not drained after enqueue failure");
    Check(runtime.destroys == 1, "stream leaked after enqueue failure");
    CheckCleanup();
}
void BothCleanupFailures() {
    std::set_terminate([] { std::_Exit(77); });
    runtime.fail_sync = 1;
    runtime.fail_destroy = 1;
    alignas(64) unsigned char source = 42;
    alignas(64) unsigned char destination = 0;
    CopyToHost(&destination, &source, 1, A());
    Check(false, "both cleanup failures returned and exposed in-flight buffers");
}
void InvalidDeviceAfterHost(bool bad_source) {
    alignas(64) unsigned char source[192]{}, destination[192]{};
    source[1] = 11;
    source[64] = 12;
    CopySlice slices[] = {
        {destination+1, source+1, 1, std::nullopt},
        {destination+64+(bad_source ? 0 : 1), source+64+(bad_source ? 1 : 0), 1, A()},
    };
    Check(!Copy(slices, 2, true), "invalid device after host copy was accepted");
    Check(destination[1] == 0 && destination[65] == 0,
          "Host slice copied before full-object alignment validation");
    Check(runtime.context_sets == 0 && runtime.creates == 0,
          "invalid mixed object called the runtime");
    CheckCleanup();
}
void SessionReuse(bool to_host) {
    alignas(64) unsigned char source[192]{}, destination[192]{};
    CopySession session;
    for (size_t i = 0; i < 3; ++i) {
        source[i * 64] = static_cast<unsigned char>(i + 11);
        CopySlice slice{destination+i*64, source+i*64, 1, A()};
        Check(session.Copy(&slice, 1, to_host), "session copy failed");
        Check(destination[i * 64] == source[i * 64],
              "session returned before this object's DMA completed");
        Check(runtime.current == &runtime.initial_context,
              "session copy retained its device context between objects");
        Check(runtime.live.size() == 1 && runtime.live[0]->pending.empty(),
              "session retained DMA after an object returned");
        // Object storage may be reused immediately after Copy returns.
        source[i * 64] = 0;
    }
    Check(runtime.creates == 1 && runtime.destroys == 0,
          "same-context objects recreated or prematurely destroyed the stream");
    Check(runtime.sync_batch_sizes == std::vector<size_t>{1, 1, 1},
          "session did not synchronize each object separately");
    const auto direction = to_host ? ACL_MEMCPY_DEVICE_TO_HOST
                                   : ACL_MEMCPY_HOST_TO_DEVICE;
    Check(runtime.directions == std::vector<aclrtMemcpyKind>(3, direction),
          "session selected the wrong copy direction");
    Check(session.Finish() && session.Finish(), "session close was not idempotent");
    Check(runtime.destroys == 1, "session did not destroy exactly one stream");
    CopySlice slice{destination, source, 1, A()};
    Check(!session.Copy(&slice, 1, to_host), "closed session accepted new work");
    Check(runtime.creates == 1, "closed session created another stream");
    CheckCleanup();
}
void SessionFailureRecovery(bool sync_failure, bool partial = false) {
    alignas(64) unsigned char source = 7, destination = 0;
    CopySlice slice{&destination, &source, 1, A()};
    CopySession session;
    Check(session.Copy(&slice, 1, true), "initial session copy failed");
    source = 9;
    if (sync_failure) runtime.fail_sync = 2;
    else runtime.fail_copy = 2;
    runtime.partial_failed_enqueue = partial;
    Check(!session.Copy(&slice, 1, true), "failed copy was accepted");
    Check(runtime.live.empty() && runtime.destroys == 1,
          "failed stream was retained instead of being drained and retired");
    Check(runtime.current == &runtime.initial_context,
          "failed copy did not restore caller context");
    if (sync_failure || partial) {
        Check(destination == source, "failed copy left submitted bytes in flight");
    }
    source = 11;
    Check(session.Copy(&slice, 1, true) && destination == 11,
          "session did not recover using a fresh stream");
    Check(runtime.creates == 2, "session reused its failed stream");
    Check(session.Finish(), "recovered session cleanup failed");
    CheckCleanup();
}
void SessionUnwind() {
    alignas(64) unsigned char source = 7, destination = 0;
    try {
        CopySession session;
        CopySlice a{&destination, &source, 1, A()};
        CopySlice b{&destination, &source, 1, B()};
        Check(session.Copy(&a, 1, true) && session.Copy(&b, 1, true),
              "pre-unwind copies failed");
        throw std::runtime_error("caller unwinding");
    } catch (const std::runtime_error&) {
    }
    Check(destination == source && runtime.destroys == 2,
          "exception unwinding did not release idle session streams");
    CheckCleanup();
}
int main(int argc, char** argv) {
    Check(argc == 2, "one scenario required");
    const std::string scenario = argv[1];
    if (scenario == "contexts") ContextGroups();
    else if (scenario == "mixed_order") MixedOrder();
    else if (scenario == "partial_copy_failure") CopyFailure(true);
    else if (scenario == "both_cleanup_failures") BothCleanupFailures();
    else if (scenario == "unaligned_source") InvalidDeviceAfterHost(true);
    else if (scenario == "unaligned_destination") InvalidDeviceAfterHost(false);
    else if (scenario == "session_d2h") SessionReuse(true);
    else if (scenario == "session_h2d") SessionReuse(false);
    else if (scenario == "session_partial_recovery") SessionFailureRecovery(false, true);
    else if (scenario == "session_sync_recovery") SessionFailureRecovery(true);
    else if (scenario == "session_unwind") SessionUnwind();
    else Check(false, "unknown scenario");
}
"""


CUDA_HEADER = r"""
#pragma once
#include <cstddef>
using cudaError_t = int;
constexpr cudaError_t cudaSuccess = 0;
constexpr int cudaMemoryTypeDevice = 2;
enum cudaMemcpyKind { cudaMemcpyDeviceToHost, cudaMemcpyHostToDevice };
struct cudaPointerAttributes { int type; int device; };
cudaError_t cudaGetDevice(int*);
cudaError_t cudaSetDevice(int);
cudaError_t cudaDeviceSynchronize();
cudaError_t cudaPointerGetAttributes(cudaPointerAttributes*, const void*);
cudaError_t cudaGetLastError();
cudaError_t cudaMemcpy(void*, const void*, size_t, cudaMemcpyKind);
"""


CUDA_HARNESS = r"""
#include "nof_staging_device.h"
#include <cuda_runtime.h>
#include <cstring>
#include <iostream>
#include <vector>
using namespace mooncake::nof_staging;
int current_device = 7;
std::vector<int> events;
cudaError_t cudaGetDevice(int* value) { *value = current_device; return cudaSuccess; }
cudaError_t cudaSetDevice(int value) { current_device = value; return cudaSuccess; }
cudaError_t cudaDeviceSynchronize() { events.push_back(0); return cudaSuccess; }
cudaError_t cudaPointerGetAttributes(cudaPointerAttributes* attr, const void*) {
    attr->type = cudaMemoryTypeDevice;
    attr->device = current_device;
    return cudaSuccess;
}
cudaError_t cudaGetLastError() { return cudaSuccess; }
cudaError_t cudaMemcpy(void* dst, const void* src, size_t size, cudaMemcpyKind kind) {
    events.push_back(kind == cudaMemcpyDeviceToHost ? 1 : 2);
    std::memcpy(dst, src, size);
    return cudaSuccess;
}
int main() {
    unsigned char source[] = {3, 1}, destination[2]{};
    Device device{Backend::Cuda, 0};
    CopySlice slices[] = {
        {destination, source, 1, device},
        {destination+1, source+1, 1, device},
    };
    if (!Copy(slices, 2, true) || std::memcmp(destination, source, 2) != 0 ||
        events != std::vector<int>{0, 1, 1, 0} || current_device != 7) return 1;
    events.clear();
    destination[0] = 0;
    if (!CopyFromHost(destination, source, 1, device) || destination[0] != source[0] ||
        events != std::vector<int>{0, 2, 0} || current_device != 7) return 2;
    std::cout << "PASS: cuda_unchanged\n";
}
"""


class NofStagingAsyncCopyTest(CppTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        sources = ["mooncake-store/src/nof_staging_device.cpp"]
        cls.ascend = cls.build(
            HARNESS,
            "ascend",
            sources=sources,
            headers={"acl/acl_rt.h": ACL_HEADER},
            defines=["-DUSE_ASCEND"],
        )
        cls.cuda = cls.build(
            CUDA_HARNESS,
            "cuda",
            sources=sources,
            headers={"cuda_runtime.h": CUDA_HEADER},
            defines=["-DUSE_CUDA"],
        )

    def test_copy_order_alignment_and_stream_reuse(self):
        for scenario in (
            "contexts",
            "mixed_order",
            "unaligned_source",
            "unaligned_destination",
            "session_d2h",
            "session_h2d",
        ):
            with self.subTest(scenario=scenario):
                self.run_case(self.ascend, scenario)

    def test_failed_copies_are_drained_and_streams_recovered(self):
        for scenario in (
            "partial_copy_failure",
            "session_partial_recovery",
            "session_sync_recovery",
            "session_unwind",
        ):
            with self.subTest(scenario=scenario):
                self.run_case(self.ascend, scenario)
        self.run_case(self.ascend, "both_cleanup_failures", expected=77)

    def test_cuda_preserves_existing_synchronization(self):
        self.run_case(self.cuda)


if __name__ == "__main__":
    main()
