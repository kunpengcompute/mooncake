"""Check the production NoF staging layout and prepare/restore paths on the CPU.

The actual layout header, buffer helpers and RealClient method bodies are
compiled with small allocator/runtime stand-ins. This verifies byte layout and
validation, not real SPDK/CANN DMA or accelerator memory registration.
"""

from nof_test_support import CppTestCase, REPO, definition, main


def extracted_code():
    include = REPO / "mooncake-store/include"
    types = (include / "types.h").read_text(encoding="utf-8")
    header = (include / "real_client.h").read_text(encoding="utf-8")
    buffer = (include / "nof_staging_buffer.h").read_text(encoding="utf-8")
    source = (REPO / "mooncake-store/src/real_client_nof_staging.cpp").read_text(
        encoding="utf-8"
    )
    start = source.index(
        "tl::expected<std::optional<RealClient::NofStagedTransfer>, ErrorCode>"
    )
    end = source.rindex("}  // namespace mooncake")
    return {
        "ERROR": definition(types, "enum class ErrorCode"),
        "BUFFER": buffer[buffer.index("namespace mooncake {") :],
        "STAGED": definition(header, "    struct NofStagedTransfer {"),
        "PREPARE_RESTORE": source[start:end],
    }


HARNESS = r"""
#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "nof_staging_device.h"
#include "nof_staging_layout.h"

struct NullLog {
    template<class T> NullLog& operator<<(const T&) { return *this; }
};
#define LOG(level) NullLog()

namespace tl {
template<class E> struct unexpected {
    E value;
    explicit unexpected(E error) : value(error) {}
};
template<class T, class E> class expected {
 public:
    template<class U, std::enable_if_t<std::is_constructible_v<T, U&&>, int> = 0>
    expected(U&& value) : value_(std::in_place, std::forward<U>(value)) {}
    expected(unexpected<E> error) : error_(error.value) {}
    explicit operator bool() const { return value_.has_value(); }
    T& operator*() { return value_.value(); }
    const T& operator*() const { return value_.value(); }
    T* operator->() { return &value_.value(); }
    const T* operator->() const { return &value_.value(); }
    E error() const { return error_.value(); }
 private:
    std::optional<T> value_;
    std::optional<E> error_;
};
template<class E> class expected<void, E> {
 public:
    expected() = default;
    expected(unexpected<E> error) : error_(error.value) {}
    explicit operator bool() const { return !error_; }
    E error() const { return error_.value(); }
 private:
    std::optional<E> error_;
};
}

namespace mooncake {
@@ERROR@@
struct BufferHandle {
    std::shared_ptr<std::vector<char>> bytes;
    void* ptr() const { return bytes ? bytes->data() : nullptr; }
    size_t size() const { return bytes ? bytes->size() : 0; }
};
class ClientBufferAllocator {
 public:
    bool isSpdkHostDma() const { return true; }
    std::optional<BufferHandle> allocate(size_t size) {
        ++allocations;
        return BufferHandle{std::make_shared<std::vector<char>>(size, '\x5a')};
    }
    int allocations = 0;
};
}
@@BUFFER@@

namespace mooncake {
struct Slice { void* ptr; size_t size; };
struct Replica {
    struct Descriptor {
        size_t length;
        bool nof = true;
        bool is_nof_replica() const { return nof; }
        const Descriptor& get_nof_descriptor() const { return *this; }
        size_t logical_size() const { return length; }
    };
};
struct Requirements { size_t buffer_alignment; };
struct Client {
    size_t alignment = 4096;
    tl::expected<Requirements, ErrorCode> GetNofIoRequirements(
        const Replica::Descriptor&, size_t) { return Requirements{alignment}; }
};
namespace device {
struct Runtime {
    const void* FindDeviceForPointer(const void*) const { return nullptr; }
};
struct Registry { Runtime RuntimeAccelerators() const { return {}; } };
Registry GetAcceleratorRegistry() { return {}; }
}
namespace nof_staging {
size_t copy_calls = 0;
struct SessionCopy {
    const CopySession* session;
    bool to_host;
};
std::vector<SessionCopy> session_copies;
std::optional<Device> DetectDeviceRange(const void*, size_t) {
    return std::nullopt;
}
bool Copy(const CopySlice* copies, size_t count, bool) {
    ++copy_calls;
    for (size_t i = 0; i < count; ++i) {
        const auto& c = copies[i];
        if (c.device && c.device->backend == Backend::Ascend &&
            (reinterpret_cast<uintptr_t>(c.src) % kAscendCopyAlignment ||
             reinterpret_cast<uintptr_t>(c.dst) % kAscendCopyAlignment)) {
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(copies[i].dst, copies[i].src, copies[i].size);
    }
    return true;
}
struct CopySession::Impl {};
CopySession::CopySession() = default;
CopySession::~CopySession() = default;
bool CopySession::Finish() { return true; }
bool CopySession::Copy(const CopySlice* copies, size_t count, bool to_host) {
    session_copies.push_back({this, to_host});
    return nof_staging::Copy(copies, count, to_host);
}
}
class RealClient {
 public:
    @@STAGED@@
    tl::expected<std::optional<NofStagedTransfer>, ErrorCode> prepare_nof_staging(
        const std::vector<Slice>&, const Replica::Descriptor&, bool,
        nof_staging::CopySession* = nullptr);
    tl::expected<void, ErrorCode> restore_nof_staging(
        const NofStagedTransfer&, const std::vector<Slice>&,
        nof_staging::CopySession* = nullptr);
    tl::expected<std::shared_ptr<ClientBufferAllocator>, ErrorCode>
    get_nof_staging_pool() { return pool; }
    bool nof_host_staging_ = true;
    bool closed_ = false;
    bool nof_staging_ready_ = true;
    std::shared_ptr<Client> client_ = std::make_shared<Client>();
    std::shared_ptr<ClientBufferAllocator> pool =
        std::make_shared<ClientBufferAllocator>();
    nof_staging::RegionRegistry nof_staging_regions_;
};
@@PREPARE_RESTORE@@

void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
using Staging = RealClient::NofStagedTransfer;
using Preparation = tl::expected<std::optional<Staging>, ErrorCode>;
Staging Take(Preparation result) {
    Check(bool(result), "prepare failed");
    Check(result->has_value(), "expected staged device object");
    return std::move(**result);
}
struct alignas(64) Bytes { std::array<char, 512> data{}; };
void Register(RealClient& client, Bytes& bytes) {
    const nof_staging::Device device{nof_staging::Backend::Ascend, 0};
    Check(client.nof_staging_regions_.Insert(bytes.data.data(), bytes.data.size(),
                                           device), "device registration failed");
}
std::vector<char> Payload(size_t length) {
    std::vector<char> result(length);
    for (size_t i = 0; i < length; ++i) result[i] = char((i * 31 + 7) % 251);
    return result;
}
std::vector<char> Gather(const std::vector<Slice>& slices) {
    std::vector<char> bytes;
    for (const auto& s : slices) {
        if (s.size == 0) continue;
        const auto* p = static_cast<const char*>(s.ptr);
        bytes.insert(bytes.end(), p, p + s.size);
    }
    return bytes;
}
void Scatter(const std::vector<char>& payload, const std::vector<Slice>& slices) {
    size_t offset = 0;
    for (const auto& s : slices) {
        Check(s.size <= payload.size() - offset, "SGL exceeds logical payload");
        if (s.size) std::memcpy(s.ptr, payload.data() + offset, s.size);
        offset += s.size;
    }
    Check(offset == payload.size(), "SGL did not consume logical payload");
}
void CheckAligned(const Staging& staging) {
    for (const auto& s : staging.host_slices) {
        if (s.size == 0) continue;
        Check(reinterpret_cast<uintptr_t>(s.ptr) % 64 == 0,
              "Ascend host slice is not 64-byte aligned");
    }
}
void CheckPadding(const Staging& staging) {
    const auto* base = static_cast<const char*>(staging.buffer.owner.ptr());
    std::vector<bool> payload(staging.buffer.owner.size(), false);
    for (const auto& s : staging.io_slices()) {
        const auto start = static_cast<const char*>(s.ptr) - base;
        for (size_t i = 0; i < s.size; ++i) payload.at(start + i) = true;
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        Check(payload[i] || base[i] == '\x5a', "copy touched alignment padding");
    }
}
void LayoutBoundaries() {
    nof_staging::HostLayout layout;
    Check(layout.Append(65, 64) == 0, "first offset");
    Check(layout.Append(3, 64) == 128, "second offset");
    Check(layout.Append(128, 64) == 192 && layout.span() == 320, "last offset/span");
    Check(!layout.Append(0, 64) && !layout.Append(1, 0) && layout.span() == 320,
          "invalid append changed layout");
    const size_t max = std::numeric_limits<size_t>::max();
    nof_staging::HostLayout large;
    Check(large.Append(max, 1) == 0, "large valid span rejected");
    Check(!large.Append(1, 64) && large.span() == max, "padding overflow accepted");
    nof_staging::HostLayout addition;
    Check(addition.Append(max - 1, 1) == 0, "near-limit span rejected");
    Check(!addition.Append(2, 1) && addition.span() == max - 1,
          "payload span overflow accepted");
    Check(nof_staging::CommonAlignment(96, 64) == 192, "LCM is wrong");
    Check(nof_staging::CommonAlignment(4096, 64) == 4096, "SPDK alignment lost");
    Check(!nof_staging::CommonAlignment(0, 64) &&
          !nof_staging::CommonAlignment(max, 2), "invalid/overflow LCM accepted");
}
void RoundTrip() {
    // Cover fully contiguous Host slices and contiguous runs separated by a gap.
    for (const auto sizes : {std::array<size_t, 3>{64, 64, 64}, {64, 65, 3}}) {
        RealClient writer, reader;
        Bytes a, b, c, x, y, z, unused;
        Register(writer, a); Register(writer, b); Register(writer, c);
        Register(reader, x); Register(reader, y); Register(reader, z);
        std::vector<Slice> src{{a.data.data(),sizes[0]}, {b.data.data(),sizes[1]},
                               {c.data.data(),sizes[2]}};
        const size_t length = sizes[0] + sizes[1] + sizes[2];
        const auto payload = Payload(length);
        Scatter(payload, src);
        nof_staging::CopySession session;
        const auto before = nof_staging::session_copies.size();
        auto put = Take(writer.prepare_nof_staging(src, {length}, true, &session));
        const auto stored = Gather(put.io_slices());
        Check(put.io_slices().size() == (sizes[1] == 64 ? 1u : 2u),
              "adjacent Host ranges were not coalesced or padding was included");
        Check(put.buffer.logical_size == length && stored == payload, "PUT bytes changed");
        CheckAligned(put);
        CheckPadding(put);

        x.data.fill('!'); y.data.fill('!'); z.data.fill('!'); unused.data.fill('!');
        std::vector<Slice> dst{{x.data.data(),7},{nullptr,0},{y.data.data(),61},
                               {z.data.data(),length-68+32},{unused.data.data(),33}};
        auto get = Take(reader.prepare_nof_staging(dst, {length}, false, &session));
        Check(nof_staging::session_copies.size() == before + 1,
              "GET preparation copied before SSD data arrived");
        CheckAligned(get);
        Scatter(stored, get.io_slices());
        Check(bool(reader.restore_nof_staging(get, dst, &session)), "GET restoration failed");
        Check(Gather({{x.data.data(),7},{y.data.data(),61},{z.data.data(),length-68}})
                  == payload, "resegmented GET changed KV bytes");
        Check(x.data[7] == '!' && y.data[61] == '!' && z.data[length-68] == '!' &&
              unused.data[0] == '!', "GET exceeded logical ranges");
        CheckPadding(get);
        const auto& copies = nof_staging::session_copies;
        Check(copies.size() == before + 2 && copies[before].session == &session &&
              copies.back().session == &session && copies[before].to_host &&
              !copies.back().to_host, "prepare/restore lost the supplied copy session");
    }
}
void MixedHostDevice() {
    RealClient client;
    Bytes a, c;
    Register(client, a); Register(client, c);
    std::array<char, 3> host{};
    std::vector<Slice> slices{{a.data.data(),65},{host.data(),3},{c.data.data(),128}};
    auto payload = Payload(196);
    Scatter(payload, slices);
    auto put = Take(client.prepare_nof_staging(slices, {196}, true));
    Check(Gather(put.io_slices()) == payload, "mixed Host/device payload changed");
    Check(reinterpret_cast<uintptr_t>(put.host_slices[2].ptr) % 64 == 0,
          "device range following Host range lost alignment");
    CheckPadding(put);
    auto cpu = client.prepare_nof_staging({{host.data(),host.size()}}, {3}, true);
    Check(bool(cpu) && !cpu->has_value(), "Host-only operation changed path");
    auto memory = client.prepare_nof_staging(slices, {196,false}, true);
    Check(bool(memory) && !memory->has_value(), "memory replica changed path");
    auto empty = client.prepare_nof_staging({{nullptr,0}}, {0}, false);
    Check(bool(empty) && !empty->has_value(), "empty object should not stage");
}
void InvalidInputs() {
    RealClient client;
    Bytes a, b;
    Register(client, a); Register(client, b);
    const size_t before = nof_staging::copy_calls;
    auto bad = client.prepare_nof_staging(
        {{a.data.data(),65},{b.data.data()+1,3}}, {68}, true);
    Check(!bad && bad.error() == ErrorCode::INVALID_PARAMS &&
          nof_staging::copy_calls == before && client.pool->allocations == 0,
          "unaligned HBM was not rejected before allocation/copy");
    auto overflow = client.prepare_nof_staging(
        {{a.data.data(),std::numeric_limits<size_t>::max()},{b.data.data(),1}},
        {1}, true);
    Check(!overflow && overflow.error() == ErrorCode::BUFFER_OVERFLOW,
          "capacity addition overflow accepted");
    auto short_read = client.prepare_nof_staging({{a.data.data(),3}}, {4}, false);
    Check(!short_read && short_read.error() == ErrorCode::INVALID_PARAMS,
          "undersized read target accepted");
}
}  // namespace mooncake
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string name = argv[1];
    using namespace mooncake;
    if (name == "layout") LayoutBoundaries();
    else if (name == "roundtrip") RoundTrip();
    else if (name == "mixed") MixedHostDevice();
    else if (name == "invalid") InvalidInputs();
    else return 2;
}
"""


class NofStagingLayoutTest(CppTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        source = HARNESS
        for marker, code in extracted_code().items():
            source = source.replace(f"@@{marker}@@", code)
        cls.executable = cls.build(source)

    def test_layout_and_prepare_restore(self):
        for scenario in ("layout", "roundtrip", "mixed", "invalid"):
            with self.subTest(scenario=scenario):
                self.run_case(self.executable, scenario)


if __name__ == "__main__":
    main()
