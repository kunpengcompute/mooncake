#include "real_client.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

#include "device/accelerator_registry.h"
#include "nof_staging_layout.h"

namespace mooncake {

tl::expected<std::shared_ptr<ClientBufferAllocator>, ErrorCode>
RealClient::get_nof_staging_pool() {
    if (!client_buffer_allocator_ || client_buffer_allocator_->size() == 0) {
        LOG(ERROR) << "NoF device staging needs positive local_buffer_size";
        return tl::unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
    }
    if (client_buffer_allocator_->isSpdkHostDma()) {
        return client_buffer_allocator_;
    }
    if (!nof_runtime_.initiator) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    std::lock_guard<std::mutex> lock(nof_staging_pool_mutex_);
    if (!nof_staging_pool_) {
        // Preserve the legacy Ascend/mmap pool. Only a NoF device operation
        // needs this additional bounded DMA pool.
        try {
            nof_staging_pool_ = ClientBufferAllocator::create(
                client_buffer_allocator_->size(), "tcp", false,
                nof_runtime_.dma_allocator);
        } catch (const std::bad_alloc&) {
            return tl::unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
        }
        if (!nof_staging_pool_->isSpdkHostDma()) {
            nof_staging_pool_.reset();
            return tl::unexpected(ErrorCode::INVALID_PARAMS);
        }
    }
    return nof_staging_pool_;
}

tl::expected<std::optional<RealClient::NofStagedTransfer>, ErrorCode>
RealClient::prepare_nof_staging(const std::vector<Slice>& slices,
                                const Replica::Descriptor& replica,
                                bool for_write,
                                nof_staging::CopySession* copy_session) {
    if (!nof_host_staging_ || !replica.is_nof_replica()) return std::nullopt;
    if (!client_ || closed_ || !nof_staging_ready_) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    size_t capacity = 0;
    for (const auto& slice : slices) {
        if (slice.size > std::numeric_limits<size_t>::max() - capacity) {
            return tl::unexpected(ErrorCode::BUFFER_OVERFLOW);
        }
        capacity += slice.size;
    }
    const size_t length =
        for_write ? capacity : replica.get_nof_descriptor().logical_size();
    if (capacity < length) return tl::unexpected(ErrorCode::INVALID_PARAMS);

    std::vector<std::optional<nof_staging::Device>> devices(slices.size());
    std::vector<Slice> host_slices(slices.size(), Slice{nullptr, 0});
    std::vector<size_t> host_offsets(slices.size(), 0);
    nof_staging::HostLayout layout;
    bool has_device = false;
    bool has_ascend = false;
    auto runtime = device::GetAcceleratorRegistry().RuntimeAccelerators();
    size_t offset = 0;
    for (size_t i = 0; i < slices.size() && offset < length; ++i) {
        const auto& slice = slices[i];
        const size_t count = std::min(slice.size, length - offset);
        if (count == 0) continue;
        if (!nof_staging::ValidRange(reinterpret_cast<uintptr_t>(slice.ptr),
                                     count)) {
            return tl::unexpected(ErrorCode::INVALID_PARAMS);
        }
        devices[i] = nof_staging_regions_.Find(slice.ptr, count);
        if (!devices[i] && runtime.FindDeviceForPointer(slice.ptr)) {
            devices[i] = nof_staging::DetectDeviceRange(slice.ptr, count);
            if (!devices[i]) {
                // A recognized device range with an unsupported context must
                // never escape to SPDK as an ordinary host pointer.
                return tl::unexpected(ErrorCode::INVALID_PARAMS);
            }
        }
        has_device = has_device || devices[i].has_value();
        const bool is_ascend =
            devices[i] && devices[i]->backend == nof_staging::Backend::Ascend;
        has_ascend = has_ascend || is_ascend;
        // Host addresses are ours to align. An application's HBM address
        // cannot be rounded without changing which KV bytes are transferred.
        if (is_ascend && reinterpret_cast<uintptr_t>(slice.ptr) %
                                 nof_staging::kAscendCopyAlignment !=
                             0) {
            LOG(ERROR) << "NoF Ascend staging requires a 64-byte aligned "
                          "device address: slice="
                       << i << ", ptr=" << slice.ptr;
            return tl::unexpected(ErrorCode::INVALID_PARAMS);
        }
        const auto host_offset = layout.Append(
            count, is_ascend ? nof_staging::kAscendCopyAlignment : 1);
        if (!host_offset) return tl::unexpected(ErrorCode::BUFFER_OVERFLOW);
        host_offsets[i] = *host_offset;
        host_slices[i].size = count;
        offset += count;
    }
    // CPU NoF transfers, like all MEMORY transfers, retain the existing path.
    if (!has_device) return std::nullopt;
    if (!length || length > INT_MAX) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    auto requirements = client_->GetNofIoRequirements(replica, length);
    if (!requirements) return tl::unexpected(requirements.error());
    const auto alignment = nof_staging::CommonAlignment(
        requirements->buffer_alignment,
        has_ascend ? nof_staging::kAscendCopyAlignment : 1);
    if (!alignment) return tl::unexpected(ErrorCode::INVALID_PARAMS);
    auto pool = get_nof_staging_pool();
    if (!pool) return tl::unexpected(pool.error());

    auto buffer = AcquireNofStaging(*pool, layout.span(), *alignment);
    if (!buffer) return tl::unexpected(buffer.error());
    // The owner includes alignment gaps, but metadata and SPDK SGL lengths
    // describe only the original payload bytes. Never write gaps to the SSD.
    buffer->logical_size = length;
    std::vector<nof_staging::CopySlice> copies;
    if (for_write) copies.reserve(slices.size());
    offset = 0;
    for (size_t i = 0; i < slices.size() && offset < length; ++i) {
        const auto& slice = slices[i];
        const size_t count = std::min(slice.size, length - offset);
        if (count == 0) continue;
        if (!devices[i]) {
            const auto* last = static_cast<const char*>(slice.ptr) + count - 1;
            if (runtime.FindDeviceForPointer(slice.ptr) ||
                runtime.FindDeviceForPointer(last)) {
                return tl::unexpected(ErrorCode::INVALID_PARAMS);
            }
        }
        host_slices[i].ptr = buffer->data + host_offsets[i];
        if (for_write) {
            copies.push_back(
                {host_slices[i].ptr, slice.ptr, count, devices[i]});
        }
        offset += count;
    }
    // Validate the whole object before copying; keep host/device slice order.
    if (for_write) {
        const bool copied =
            copy_session
                ? copy_session->Copy(copies.data(), copies.size(), true)
                : nof_staging::Copy(copies.data(), copies.size(), true);
        if (!copied) return tl::unexpected(ErrorCode::TRANSFER_FAIL);
    }
    return std::optional<NofStagedTransfer>{NofStagedTransfer{
        std::move(*buffer), std::move(devices), std::move(host_slices)}};
}

tl::expected<void, ErrorCode> RealClient::restore_nof_staging(
    const NofStagedTransfer& staging, const std::vector<Slice>& slices,
    nof_staging::CopySession* copy_session) {
    if (staging.devices.size() != slices.size() ||
        staging.host_slices.size() != slices.size()) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    std::vector<nof_staging::CopySlice> copies;
    copies.reserve(slices.size());
    size_t offset = 0;
    const auto allocation =
        reinterpret_cast<uintptr_t>(staging.buffer.owner.ptr());
    for (size_t i = 0; i < slices.size(); ++i) {
        const size_t count =
            std::min(slices[i].size, staging.buffer.logical_size - offset);
        const auto& host_slice = staging.host_slices[i];
        if (host_slice.size != count) {
            return tl::unexpected(ErrorCode::INVALID_PARAMS);
        }
        if (count == 0) {
            if (host_slice.ptr != nullptr) {
                return tl::unexpected(ErrorCode::INVALID_PARAMS);
            }
            continue;
        }
        const auto host_address = reinterpret_cast<uintptr_t>(host_slice.ptr);
        if (host_address < allocation ||
            host_address - allocation > staging.buffer.owner.size() ||
            count > staging.buffer.owner.size() - (host_address - allocation)) {
            return tl::unexpected(ErrorCode::INVALID_PARAMS);
        }
        copies.push_back(
            {slices[i].ptr, host_slice.ptr, count, staging.devices[i]});
        offset += count;
    }
    if (offset != staging.buffer.logical_size) {
        return tl::unexpected(ErrorCode::TRANSFER_FAIL);
    }
    const bool copied =
        copy_session ? copy_session->Copy(copies.data(), copies.size(), false)
                     : nof_staging::Copy(copies.data(), copies.size(), false);
    if (!copied) return tl::unexpected(ErrorCode::TRANSFER_FAIL);
    return {};
}

}  // namespace mooncake
