#include "client_service.h"

#include <glog/logging.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numa.h>
#include <optional>
#include <ranges>
#include <sched.h>
#include <thread>

#include "transfer_engine.h"
#include "transfer_task.h"
#include "config.h"
#include "types.h"

namespace mooncake {

namespace {

std::optional<int> GetConfiguredNumaSocketId() {
    const char* raw_value = std::getenv("MC_STORE_NUMA_SOCKET_ID");
    if (!raw_value || raw_value[0] == '\0') {
        return std::nullopt;
    }

    char* end_ptr = nullptr;
    errno = 0;
    long parsed = std::strtol(raw_value, &end_ptr, 10);
    if (errno != 0 || end_ptr == raw_value ||
        (end_ptr != nullptr && *end_ptr != '\0') || parsed < 0 ||
        parsed > std::numeric_limits<int>::max()) {
        LOG(WARNING) << "Invalid MC_STORE_NUMA_SOCKET_ID=" << raw_value
                     << ", falling back to auto-detect";
        return std::nullopt;
    }

    return static_cast<int>(parsed);
}

int GetCurrentNumaSocketId() {
    if (numa_available() < 0) {
        return 0;
    }
    int cpu = sched_getcpu();
    if (cpu < 0) {
        return 0;
    }
    int node = numa_node_of_cpu(cpu);
    return node < 0 ? 0 : node;
}

struct ReplicaTransferSummary {
    size_t allocated_memory_replicas = 0;
    size_t allocated_nof_replicas = 0;
    size_t successful_memory_transfers = 0;
    size_t successful_nof_transfers = 0;
    size_t failed_memory_transfers = 0;
    size_t failed_nof_transfers = 0;
    ErrorCode first_error = ErrorCode::OK;

    void RecordAllocatedReplica(const Replica::Descriptor& replica) {
        if (replica.is_memory_replica()) {
            ++allocated_memory_replicas;
        } else if (replica.is_nof_replica()) {
            ++allocated_nof_replicas;
        }
    }

    void RecordSuccess(ReplicaType replica_type) {
        if (replica_type == ReplicaType::MEMORY) {
            ++successful_memory_transfers;
        } else if (replica_type == ReplicaType::NOF_SSD) {
            ++successful_nof_transfers;
        }
    }

    void RecordFailure(ReplicaType replica_type, ErrorCode error) {
        if (replica_type == ReplicaType::MEMORY) {
            ++failed_memory_transfers;
        } else if (replica_type == ReplicaType::NOF_SSD) {
            ++failed_nof_transfers;
        }
        if (first_error == ErrorCode::OK) {
            first_error = error;
        }
    }
};

bool HasExpectedReplicaAllocation(const ReplicateConfig& config,
                                  const ReplicaTransferSummary& summary) {
    if (DetermineReplicaWriteMode(config) ==
        ReplicaWriteMode::FLEXIBLE_DUAL_REPLICA) {
        return summary.allocated_memory_replicas +
                   summary.allocated_nof_replicas >
               0;
    }
    return summary.allocated_memory_replicas == config.replica_num &&
           summary.allocated_nof_replicas == config.nof_replica_num;
}

struct FinalizeDecision {
    std::optional<ReplicaType> end_type;
    std::optional<ReplicaType> revoke_type;
    bool success = false;
    ErrorCode error = ErrorCode::OK;
};

FinalizeDecision DetermineFinalizeDecision(
    const ReplicateConfig& config, const ReplicaTransferSummary& summary) {
    const auto write_mode = DetermineReplicaWriteMode(config);
    const bool allocation_satisfied =
        HasExpectedReplicaAllocation(config, summary);

    if (write_mode != ReplicaWriteMode::FLEXIBLE_DUAL_REPLICA) {
        const bool all_transfers_succeeded =
            summary.successful_memory_transfers ==
                summary.allocated_memory_replicas &&
            summary.successful_nof_transfers ==
                summary.allocated_nof_replicas &&
            summary.failed_memory_transfers == 0 &&
            summary.failed_nof_transfers == 0;
        if (allocation_satisfied && all_transfers_succeeded) {
            return {.end_type = ReplicaType::ALL,
                    .revoke_type = std::nullopt,
                    .success = true,
                    .error = ErrorCode::OK};
        }
        return {.end_type = std::nullopt,
                .revoke_type = ReplicaType::ALL,
                .success = false,
                .error = allocation_satisfied
                             ? (summary.first_error == ErrorCode::OK
                                    ? ErrorCode::TRANSFER_FAIL
                                    : summary.first_error)
                             : ErrorCode::NO_AVAILABLE_HANDLE};
    }

    const bool memory_succeeded = summary.successful_memory_transfers > 0;
    const bool nof_succeeded = summary.successful_nof_transfers > 0;

    if (memory_succeeded && nof_succeeded) {
        return {.end_type = ReplicaType::ALL,
                .revoke_type = std::nullopt,
                .success = true,
                .error = ErrorCode::OK};
    }
    if (memory_succeeded) {
        return {.end_type = ReplicaType::MEMORY,
                .revoke_type = ReplicaType::NOF_SSD,
                .success = true,
                .error = ErrorCode::OK};
    }
    if (nof_succeeded) {
        return {.end_type = ReplicaType::NOF_SSD,
                .revoke_type = ReplicaType::MEMORY,
                .success = true,
                .error = ErrorCode::OK};
    }

    return {.end_type = std::nullopt,
            .revoke_type = ReplicaType::ALL,
            .success = false,
            .error = summary.first_error == ErrorCode::OK
                         ? ErrorCode::NO_AVAILABLE_HANDLE
                         : summary.first_error};
}

}  // namespace

[[nodiscard]] size_t CalculateSliceSize(const std::vector<Slice>& slices) {
    size_t slice_size = 0;
    for (const auto& slice : slices) {
        slice_size += slice.size;
    }
    return slice_size;
}

[[nodiscard]] size_t CalculateSliceSize(std::span<const Slice> slices) {
    size_t slice_size = 0;
    for (const auto& slice : slices) {
        slice_size += slice.size;
    }
    return slice_size;
}

Client::Client(const std::string& local_hostname,
               const std::string& metadata_connstring,
               const std::map<std::string, std::string>& labels)
    : client_id_(generate_uuid()),
      metrics_(ClientMetric::Create(merge_labels(labels))),
      master_client_(client_id_,
                     metrics_ ? &metrics_->master_client_metric : nullptr),
      local_hostname_(local_hostname),
      metadata_connstring_(metadata_connstring),
      write_thread_pool_(2) {
    LOG(INFO) << "client_id=" << client_id_;

    if (metrics_) {
        if (metrics_->GetReportingInterval() > 0) {
            LOG(INFO) << "Client metrics enabled with reporting thread started "
                         "(interval: "
                      << metrics_->GetReportingInterval() << "s)";
        } else {
            LOG(INFO)
                << "Client metrics enabled but reporting disabled (interval=0)";
        }
    } else {
        LOG(INFO) << "Client metrics disabled (set MC_STORE_CLIENT_METRIC=1 to "
                     "enable)";
    }
}

Client::~Client() {
    // Make a copy of mounted_segments_ to avoid modifying while iterating
    std::vector<Segment> segments_to_unmount;
    {
        std::lock_guard<std::mutex> lock(mounted_segments_mutex_);
        segments_to_unmount.reserve(mounted_segments_.size());
        for (auto& entry : mounted_segments_) {
            segments_to_unmount.emplace_back(entry.second);
        }
    }

    for (auto& segment : segments_to_unmount) {
        auto result =
            UnmountSegment(reinterpret_cast<void*>(segment.base), segment.size);
        if (!result) {
            LOG(ERROR) << "Failed to unmount segment: "
                       << toString(result.error());
        }
    }

    // Clear any remaining segments
    {
        std::lock_guard<std::mutex> lock(mounted_segments_mutex_);
        mounted_segments_.clear();
    }

    // Stop ping thread only after no need to contact master anymore
    if (ping_running_) {
        ping_running_ = false;
        if (ping_thread_.joinable()) {
            ping_thread_.join();
        }
    }
}

static std::optional<bool> get_auto_discover() {
    const char* ev_ad = std::getenv("MC_MS_AUTO_DISC");
    if (ev_ad) {
        int iv = std::stoi(ev_ad);
        if (iv == 1) {
            LOG(INFO) << "auto discovery set by env MC_MS_AUTO_DISC";
            return true;
        } else if (iv == 0) {
            LOG(INFO) << "auto discovery not set by env MC_MS_AUTO_DISC";
            return false;
        } else {
            LOG(WARNING)
                << "invalid MC_MS_AUTO_DISC value: " << ev_ad
                << ", should be 0 or 1, using default: auto discovery not set";
        }
    }
    return std::nullopt;
}

static inline void ltrim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
}

static inline void rtrim(std::string& s) {
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
}

static std::vector<std::string> get_auto_discover_filters() {
    std::vector<std::string> whitelst_filters;
    char* ev_ad = std::getenv("MC_MS_FILTERS");
    if (ev_ad) {
        LOG(INFO) << "whitelist filters: " << ev_ad;
        char delimiter = ',';
        char* end = ev_ad + std::strlen(ev_ad);
        char *start = ev_ad, *pos = ev_ad;
        while ((pos = std::find(start, end, delimiter)) != end) {
            std::string str(start, pos);
            ltrim(str);
            rtrim(str);
            whitelst_filters.emplace_back(std::move(str));
            start = pos + 1;
        }
        if (start != (end + 1)) {
            std::string str(start, end);
            ltrim(str);
            rtrim(str);
            whitelst_filters.emplace_back(std::move(str));
        }
    }
    return whitelst_filters;
}

tl::expected<void, ErrorCode> CheckRegisterMemoryParams(const void* addr,
                                                        size_t length) {
    if (addr == nullptr) {
        LOG(ERROR) << "addr is nullptr";
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    if (length == 0) {
        LOG(ERROR) << "length is 0";
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    // Tcp is not limited by max_mr_size, but we ignore it for now.
    auto max_mr_size = globalConfig().max_mr_size;  // Max segment size
    if (length > max_mr_size) {
        LOG(ERROR) << "length " << length
                   << " is larger than max_mr_size: " << max_mr_size;
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    return {};
}

ErrorCode Client::ConnectToMaster(const std::string& master_server_entry) {
    if (master_server_entry.find("etcd://") == 0) {
        std::string etcd_entry = master_server_entry.substr(strlen("etcd://"));

        // Get master address from etcd
        auto err = master_view_helper_.ConnectToEtcd(etcd_entry);
        if (err != ErrorCode::OK) {
            LOG(ERROR) << "Failed to connect to etcd";
            return err;
        }
        std::string master_address;
        ViewVersionId master_version = 0;
        err = master_view_helper_.GetMasterView(master_address, master_version);
        if (err != ErrorCode::OK) {
            LOG(ERROR) << "Failed to get master address";
            return err;
        }

        err = master_client_.Connect(master_address);
        if (err != ErrorCode::OK) {
            LOG(ERROR) << "Failed to connect to master";
            return err;
        }

        // Start ping thread to monitor master health and trigger remount if
        // needed.
        ping_running_ = true;
        bool is_ha_mode = true;
        std::string current_master_address = master_address;
        ping_thread_ = std::thread([this, is_ha_mode,
                                    current_master_address]() mutable {
            this->PingThreadMain(is_ha_mode, std::move(current_master_address));
        });

        return ErrorCode::OK;
    } else {
        auto err = master_client_.Connect(master_server_entry);
        if (err != ErrorCode::OK) {
            return err;
        }
        // Non-HA mode also enables heartbeat/ping
        ping_running_ = true;
        bool is_ha_mode = false;
        std::string current_master_address = master_server_entry;
        ping_thread_ = std::thread([this, is_ha_mode,
                                    current_master_address]() mutable {
            this->PingThreadMain(is_ha_mode, std::move(current_master_address));
        });
        return ErrorCode::OK;
    }
}

ErrorCode Client::InitTransferEngine(
    const std::string& local_hostname, const std::string& metadata_connstring,
    const std::string& protocol,
    const std::optional<std::string>& device_names) {
    // get auto_discover and filters from env
    std::optional<bool> env_auto_discover = get_auto_discover();
    bool auto_discover = false;
    if (env_auto_discover.has_value()) {
        // Use user-specified auto-discover setting
        auto_discover = env_auto_discover.value();
    } else {
        // Enable auto-discover for RDMA if no devices are specified
        if (protocol == "rdma" && !device_names.has_value()) {
            LOG(INFO) << "Set auto discovery ON by default for RDMA protocol, "
                         "since no "
                         "device names provided";
            auto_discover = true;
        }
    }
    transfer_engine_->setAutoDiscover(auto_discover);

    // Honor filters when auto-discovery is enabled; otherwise warn once
    if (auto_discover) {
        LOG(INFO) << "Transfer engine auto discovery is enabled for protocol: "
                  << protocol;
        auto filters = get_auto_discover_filters();
        transfer_engine_->setWhitelistFilters(std::move(filters));
    } else {
        const char* env_filters = std::getenv("MC_MS_FILTERS");
        if (env_filters && *env_filters != '\0') {
            LOG(WARNING)
                << "MC_MS_FILTERS is set but auto discovery is disabled; "
                << "ignoring whitelist: " << env_filters;
        }
    }

    auto [hostname, port] = parseHostNameWithPort(local_hostname);
    int rc = transfer_engine_->init(metadata_connstring, local_hostname,
                                    hostname, port);
    if (rc != 0) {
        LOG(ERROR) << "Failed to initialize transfer engine, rc=" << rc;
        return ErrorCode::INTERNAL_ERROR;
    }

    if (!auto_discover) {
        LOG(INFO) << "Transfer engine auto discovery is disabled for protocol: "
                  << protocol;

        Transport* transport = nullptr;

        if (protocol == "rdma") {
            if (!device_names.has_value() || device_names->empty()) {
                LOG(ERROR) << "RDMA protocol requires device names when auto "
                              "discovery is disabled";
                return ErrorCode::INVALID_PARAMS;
            }

            LOG(INFO) << "Using specified RDMA devices: "
                      << device_names.value();

            std::vector<std::string> devices =
                splitString(device_names.value(), ',', /*skip_empty=*/true);

            // Manually discover topology with specified devices only
            auto topology = transfer_engine_->getLocalTopology();
            if (topology) {
                topology->discover(devices);
                LOG(INFO) << "Topology discovery complete with specified "
                             "devices. Found "
                          << topology->getHcaList().size() << " HCAs";
            }

            transport = transfer_engine_->installTransport("rdma", nullptr);
            if (!transport) {
                LOG(ERROR) << "Failed to install RDMA transport with specified "
                              "devices";
                return ErrorCode::INTERNAL_ERROR;
            }
        } else if (protocol == "tcp") {
            if (device_names.has_value()) {
                LOG(WARNING)
                    << "TCP protocol does not use device names, ignoring";
            }

            try {
                transport = transfer_engine_->installTransport("tcp", nullptr);
            } catch (std::exception& e) {
                LOG(ERROR) << "tcp_transport_install_failed error_message=\""
                           << e.what() << "\"";
                return ErrorCode::INTERNAL_ERROR;
            }

            if (!transport) {
                LOG(ERROR) << "Failed to install TCP transport";
                return ErrorCode::INTERNAL_ERROR;
            }
        } else if (protocol == "ascend") {
            if (device_names.has_value()) {
                LOG(WARNING) << "Ascend protocol does not use device "
                                "names, ignoring";
            }
            try {
                transport =
                    transfer_engine_->installTransport("ascend", nullptr);
            } catch (std::exception& e) {
                LOG(ERROR) << "ascend_transport_install_failed error_message=\""
                           << e.what() << "\"";
                return ErrorCode::INTERNAL_ERROR;
            }

            if (!transport) {
                LOG(ERROR) << "Failed to install Ascend transport";
                return ErrorCode::INTERNAL_ERROR;
            }
        } else {
            LOG(ERROR) << "unsupported_protocol protocol=" << protocol;
            return ErrorCode::INVALID_PARAMS;
        }
    }

    return ErrorCode::OK;
}

void Client::InitTransferSubmitter() {
    // Initialize TransferSubmitter after transfer engine is ready
    // Keep using logical local_hostname for name-based behaviors; endpoint is
    // used separately where needed.
    int numa_socket_id =
        GetConfiguredNumaSocketId().value_or(GetCurrentNumaSocketId());
    transfer_submitter_ = std::make_unique<TransferSubmitter>(
        *transfer_engine_, storage_backend_,
        metrics_ ? &metrics_->transfer_metric : nullptr, numa_socket_id);
}

std::optional<std::shared_ptr<Client>> Client::Create(
    const std::string& local_hostname, const std::string& metadata_connstring,
    const std::string& protocol, const std::optional<std::string>& device_names,
    const std::string& master_server_entry,
    const std::shared_ptr<TransferEngine>& transfer_engine,
    std::map<std::string, std::string> labels) {
    auto client = std::shared_ptr<Client>(
        new Client(local_hostname, metadata_connstring, labels));

    ErrorCode err = client->ConnectToMaster(master_server_entry);
    if (err != ErrorCode::OK) {
        return std::nullopt;
    }

    // Initialize storage backend if storage_root_dir is valid
    auto config_response = client->master_client_.GetStorageConfig();
    if (!config_response) {
        LOG(ERROR) << "Failed to get storage config from master";
        // Fallback to GetFsdir for backward compatibility
        auto response = client->master_client_.GetFsdir();
        if (!response) {
            LOG(ERROR) << "Failed to get fsdir from master";
        } else if (response.value().empty()) {
            LOG(INFO)
                << "Storage root directory is not set. persisting data is "
                   "disabled.";
        } else {
            auto dir_string = response.value();
            size_t pos = dir_string.find_last_of('/');
            if (pos != std::string::npos) {
                std::string storage_root_dir = dir_string.substr(0, pos);
                std::string fs_subdir = dir_string.substr(pos + 1);
                LOG(INFO) << "Storage root directory is: " << storage_root_dir;
                LOG(INFO) << "Fs subdir is: " << fs_subdir;
                // Initialize storage backend with default eviction settings
                client->PrepareStorageBackend(storage_root_dir, fs_subdir, true,
                                              0);
            } else {
                LOG(ERROR) << "Invalid fsdir format: " << dir_string;
            }
        }
    } else {
        auto config = config_response.value();
        if (config.fsdir.empty()) {
            LOG(INFO)
                << "Storage root directory is not set. persisting data is "
                   "disabled.";
        } else {
            size_t pos = config.fsdir.find_last_of('/');
            if (pos != std::string::npos) {
                std::string storage_root_dir = config.fsdir.substr(0, pos);
                std::string fs_subdir = config.fsdir.substr(pos + 1);
                LOG(INFO) << "Storage root directory is: " << storage_root_dir;
                LOG(INFO) << "Fs subdir is: " << fs_subdir;
                LOG(INFO) << "Disk eviction enabled: "
                          << config.enable_disk_eviction;
                LOG(INFO) << "Quota bytes: " << config.quota_bytes;
                // Initialize storage backend with config from master
                client->PrepareStorageBackend(storage_root_dir, fs_subdir,
                                              config.enable_disk_eviction,
                                              config.quota_bytes);
            } else {
                LOG(ERROR) << "Invalid fsdir format: " << config.fsdir;
            }
        }
    }

    // Initialize transfer engine
    if (transfer_engine == nullptr) {
        client->transfer_engine_ = std::make_shared<TransferEngine>();
        err = client->InitTransferEngine(local_hostname, metadata_connstring,
                                         protocol, device_names);
        if (err != ErrorCode::OK) {
            LOG(ERROR) << "Failed to initialize transfer engine";
            return std::nullopt;
        }
    } else {
        client->transfer_engine_ = transfer_engine;
        LOG(INFO) << "Use existing transfer engine instance. Skip its "
                     "initialization.";
    }

    client->InitTransferSubmitter();

    return client;
}

tl::expected<void, ErrorCode> Client::Get(const std::string& object_key,
                                          std::vector<Slice>& slices) {
    auto query_result = Query(object_key);
    if (!query_result) {
        return tl::unexpected(query_result.error());
    }
    return Get(object_key, query_result.value(), slices);
}

std::vector<tl::expected<void, ErrorCode>> Client::BatchGet(
    const std::vector<std::string>& object_keys,
    std::unordered_map<std::string, std::vector<Slice>>& slices) {
    auto batched_query_results = BatchQuery(object_keys);

    // If any queries failed, return error results immediately for failed
    // queries
    std::vector<tl::expected<void, ErrorCode>> results;
    results.reserve(object_keys.size());

    std::vector<QueryResult> valid_query_results;
    std::vector<size_t> valid_indices;
    std::vector<std::string> valid_keys;

    for (size_t i = 0; i < batched_query_results.size(); ++i) {
        if (batched_query_results[i]) {
            valid_query_results.emplace_back(batched_query_results[i].value());
            valid_indices.emplace_back(i);
            valid_keys.emplace_back(object_keys[i]);
            results.emplace_back();  // placeholder for successful results
        } else {
            results.emplace_back(
                tl::unexpected(batched_query_results[i].error()));
        }
    }

    // If we have any valid queries, process them
    if (!valid_keys.empty()) {
        std::unordered_map<std::string, std::vector<Slice>> valid_slices;
        for (const auto& key : valid_keys) {
            auto it = slices.find(key);
            if (it != slices.end()) {
                valid_slices[key] = it->second;
            }
        }

        auto valid_results =
            BatchGet(valid_keys, valid_query_results, valid_slices);

        // Merge results back
        for (size_t i = 0; i < valid_indices.size(); ++i) {
            results[valid_indices[i]] = valid_results[i];
        }
    }

    return results;
}

tl::expected<
    std::unordered_map<UUID, std::vector<std::string>, boost::hash<UUID>>,
    ErrorCode>
Client::BatchQueryIp(const std::vector<UUID>& client_ids) {
    auto result = master_client_.BatchQueryIp(client_ids);
    return result;
}

tl::expected<std::unordered_map<std::string, std::vector<Replica::Descriptor>>,
             ErrorCode>
Client::QueryByRegex(const std::string& str) {
    auto result = master_client_.GetReplicaListByRegex(str);
    return result;
}

tl::expected<QueryResult, ErrorCode> Client::Query(
    const std::string& object_key) {
    std::chrono::steady_clock::time_point start_time =
        std::chrono::steady_clock::now();
    auto result = master_client_.GetReplicaList(object_key);
    if (!result) {
        return tl::unexpected(result.error());
    }
    return QueryResult(
        std::move(result.value().replicas),
        start_time + std::chrono::milliseconds(result.value().lease_ttl_ms));
}

std::vector<tl::expected<QueryResult, ErrorCode>> Client::BatchQuery(
    const std::vector<std::string>& object_keys) {
    std::chrono::steady_clock::time_point start_time =
        std::chrono::steady_clock::now();
    auto response = master_client_.BatchGetReplicaList(object_keys);

    // Check if we got the expected number of responses
    if (response.size() != object_keys.size()) {
        LOG(ERROR) << "BatchQuery response size mismatch. Expected: "
                   << object_keys.size() << ", Got: " << response.size();
        // Return vector of RPC_FAIL errors
        std::vector<tl::expected<QueryResult, ErrorCode>> results;
        results.reserve(object_keys.size());
        for (size_t i = 0; i < object_keys.size(); ++i) {
            results.emplace_back(tl::unexpected(ErrorCode::RPC_FAIL));
        }
        return results;
    }
    std::vector<tl::expected<QueryResult, ErrorCode>> results;
    results.reserve(response.size());
    for (size_t i = 0; i < response.size(); ++i) {
        if (response[i]) {
            results.emplace_back(QueryResult(
                std::move(response[i].value().replicas),
                start_time + std::chrono::milliseconds(
                                 response[i].value().lease_ttl_ms)));
        } else {
            results.emplace_back(tl::unexpected(response[i].error()));
        }
    }
    return results;
}

tl::expected<std::vector<std::string>, ErrorCode> Client::BatchReplicaClear(
    const std::vector<std::string>& object_keys, const UUID& client_id,
    const std::string& segment_name) {
    auto result =
        master_client_.BatchReplicaClear(object_keys, client_id, segment_name);
    return result;
}

tl::expected<void, ErrorCode> Client::Get(const std::string& object_key,
                                          const QueryResult& query_result,
                                          std::vector<Slice>& slices) {
    // Find the first complete replica
    Replica::Descriptor replica;
    ErrorCode err = FindFirstCompleteReplica(query_result.replicas, replica);
    if (err != ErrorCode::OK) {
        if (err == ErrorCode::INVALID_REPLICA) {
            LOG(ERROR) << "no_complete_replicas_found key=" << object_key;
        }
        return tl::unexpected(err);
    }

    auto t0_get = std::chrono::steady_clock::now();
    err = TransferRead(replica, slices);
    auto us_get = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0_get)
                      .count();
    if (metrics_) {
        metrics_->transfer_metric.get_latency_us.observe(us_get);
    }

    if (err != ErrorCode::OK) {
        LOG(ERROR) << "transfer_read_failed key=" << object_key;
        return tl::unexpected(err);
    }
    if (query_result.IsLeaseExpired()) {
        LOG(WARNING) << "lease_expired_before_data_transfer_completed key="
                     << object_key;
        return tl::unexpected(ErrorCode::LEASE_EXPIRED);
    }
    return {};
}

struct BatchGetOperation {
    std::vector<Replica::Descriptor> replicas;
    std::vector<std::vector<Slice>> batched_slices;
    std::vector<size_t> key_indexes;
    std::vector<TransferFuture> futures;
};

std::vector<tl::expected<void, ErrorCode>> Client::BatchGetWhenPreferSameNode(
    const std::vector<std::string>& object_keys,
    const std::vector<QueryResult>& query_results,
    std::unordered_map<std::string, std::vector<Slice>>& slices) {
    std::vector<tl::expected<void, ErrorCode>> results;
    results.resize(object_keys.size());

    std::unordered_map<std::string, BatchGetOperation> seg_to_op_map{};
    for (size_t i = 0; i < object_keys.size(); ++i) {
        const auto& key = object_keys[i];
        const auto& replica_list = query_results[i].replicas;
        auto slices_it = slices.find(key);
        if (slices_it == slices.end()) {
            LOG(ERROR) << "Slices not found for key: " << key;
            results[i] = tl::unexpected(ErrorCode::INVALID_PARAMS);
            continue;
        }
        Replica::Descriptor replica;
        ErrorCode err = FindFirstCompleteReplica(replica_list, replica);
        if (err != ErrorCode::OK) {
            if (err == ErrorCode::INVALID_REPLICA) {
                LOG(ERROR) << "no_complete_replicas_found key=" << key;
            }
            results[i] = tl::unexpected(err);
            continue;
        }
        if (!replica.is_memory_replica()) {
            results[i] = tl::unexpected(ErrorCode::INVALID_REPLICA);
            continue;
        }
        auto& memory_descriptor = replica.get_memory_descriptor();
        if (memory_descriptor.buffer_descriptor.size_ == 0) {
            results[i] = tl::unexpected(ErrorCode::INVALID_REPLICA);
            continue;
        }
        auto& buffer_descriptor = memory_descriptor.buffer_descriptor;
        auto seg = buffer_descriptor.transport_endpoint_;
        auto& op = seg_to_op_map[seg];
        op.replicas.emplace_back(replica);
        op.batched_slices.emplace_back(slices_it->second);
        op.key_indexes.emplace_back(i);
    }
    for (auto& seg_to_op : seg_to_op_map) {
        auto& op = seg_to_op.second;
        auto future = transfer_submitter_->submit_batch(
            op.replicas, op.batched_slices, TransferRequest::READ);
        if (!future) {
            for (auto index : op.key_indexes) {
                results[index] = tl::unexpected(ErrorCode::TRANSFER_FAIL);
                LOG(ERROR) << "Failed to submit transfer operation for key: "
                           << object_keys[index];
            }
            continue;
        }
        op.futures.emplace_back(std::move(*future));
    }
    for (auto& seg_to_op : seg_to_op_map) {
        auto& op = seg_to_op.second;
        if (op.futures.empty()) {
            continue;
        }
        ErrorCode result = op.futures[0].get();
        if (result != ErrorCode::OK) {
            for (auto index : op.key_indexes) {
                results[index] = tl::unexpected(ErrorCode::TRANSFER_FAIL);
                LOG(ERROR) << "Failed to submit transfer operation for key: "
                           << object_keys[index];
            }
        } else {
            for (auto index : op.key_indexes) {
                VLOG(1) << "Transfer completed successfully for key: "
                        << object_keys[index];
                results[index] = {};
            }
        }
    }
    return results;
}

std::vector<tl::expected<void, ErrorCode>> Client::BatchGet(
    const std::vector<std::string>& object_keys,
    const std::vector<QueryResult>& query_results,
    std::unordered_map<std::string, std::vector<Slice>>& slices,
    bool prefer_alloc_in_same_node) {
    if (!transfer_submitter_) {
        LOG(ERROR) << "TransferSubmitter not initialized";
        std::vector<tl::expected<void, ErrorCode>> results;
        results.reserve(object_keys.size());
        for (size_t i = 0; i < object_keys.size(); ++i) {
            results.emplace_back(tl::unexpected(ErrorCode::INVALID_PARAMS));
        }
        return results;
    }

    // Validate input size consistency
    if (query_results.size() != object_keys.size()) {
        LOG(ERROR) << "Query results size (" << query_results.size()
                   << ") doesn't match object keys size (" << object_keys.size()
                   << ")";
        std::vector<tl::expected<void, ErrorCode>> results;
        results.reserve(object_keys.size());
        for (size_t i = 0; i < object_keys.size(); ++i) {
            results.emplace_back(tl::unexpected(ErrorCode::INVALID_PARAMS));
        }
        return results;
    }
    if (prefer_alloc_in_same_node) {
        return BatchGetWhenPreferSameNode(object_keys, query_results, slices);
    }

    // Collect all transfer operations for parallel execution
    std::vector<std::tuple<size_t, std::string, TransferFuture>>
        pending_transfers;
    std::vector<tl::expected<void, ErrorCode>> results(object_keys.size());
    // Record batch get transfer latency (Submit + Wait)
    auto t0_batch_get = std::chrono::steady_clock::now();

    // Submit all transfers in parallel
    for (size_t i = 0; i < object_keys.size(); ++i) {
        const auto& key = object_keys[i];
        const auto& query_result = query_results[i];

        auto slices_it = slices.find(key);
        if (slices_it == slices.end()) {
            LOG(ERROR) << "Slices not found for key: " << key;
            results[i] = tl::unexpected(ErrorCode::INVALID_PARAMS);
            continue;
        }

        // Find the first complete replica for this key
        Replica::Descriptor replica;
        ErrorCode err =
            FindFirstCompleteReplica(query_result.replicas, replica);
        if (err != ErrorCode::OK) {
            if (err == ErrorCode::INVALID_REPLICA) {
                LOG(ERROR) << "no_complete_replicas_found key=" << key;
            }
            results[i] = tl::unexpected(err);
            continue;
        }

        const auto &slice = slices_it->second;
        void *ptr = slice[0].ptr;
        size_t len = 0;
        for (const auto &x: slice) {
            len += x.size;
        }

        // Submit transfer operation asynchronously
        auto future = transfer_submitter_->submit(replica, slices_it->second,
                                                  TransferRequest::READ, ptr, len);
        if (!future) {
            LOG(ERROR) << "Failed to submit transfer operation for key: "
                       << key;
            results[i] = tl::unexpected(ErrorCode::TRANSFER_FAIL);
            continue;
        }

        VLOG(1) << "Submitted transfer for key " << key
                << " using strategy: " << static_cast<int>(future->strategy());

        pending_transfers.emplace_back(i, key, std::move(*future));
    }

    // Wait for all transfers to complete
    for (auto& [index, key, future] : pending_transfers) {
        ErrorCode result = future.get();
        if (result != ErrorCode::OK) {
            LOG(ERROR) << "Transfer failed for key: " << key
                       << " with error: " << static_cast<int>(result);
            results[index] = tl::unexpected(result);
        } else {
            VLOG(1) << "Transfer completed successfully for key: " << key;
            results[index] = {};
        }
    }

    // As lease expired is a rare case, we check all the results with the same
    // time_point to avoid too many syscalls
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    for (size_t i = 0; i < object_keys.size(); ++i) {
        if (results[i].has_value() && query_results[i].IsLeaseExpired(now)) {
            LOG(WARNING) << "lease_expired_before_data_transfer_completed key="
                         << object_keys[i];
            results[i] = tl::unexpected(ErrorCode::LEASE_EXPIRED);
        }
    }

    auto us_batch_get = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0_batch_get)
                            .count();
    if (metrics_) {
        metrics_->transfer_metric.batch_get_latency_us.observe(us_batch_get);
    }

    VLOG(1) << "BatchGet completed for " << object_keys.size() << " keys";
    return results;
}

tl::expected<void, ErrorCode> Client::Put(const ObjectKey& key,
                                          std::vector<Slice>& slices,
                                          const ReplicateConfig& config) {
    // Prepare slice lengths
    std::vector<size_t> slice_lengths;
    for (size_t i = 0; i < slices.size(); ++i) {
        slice_lengths.emplace_back(slices[i].size);
    }

    // Start put operation
    auto start_result = master_client_.PutStart(key, slice_lengths, config);
    if (!start_result) {
        ErrorCode err = start_result.error();
        if (err == ErrorCode::OBJECT_ALREADY_EXISTS) {
            VLOG(1) << "object_already_exists key=" << key;
            return {};
        }
        if (err == ErrorCode::NO_AVAILABLE_HANDLE) {
            LOG(WARNING) << "Failed to start put operation for key=" << key
                         << PUT_NO_SPACE_HELPER_STR;
        } else {
            LOG(ERROR) << "Failed to start put operation for key=" << key
                       << ": " << toString(err);
        }
        return tl::unexpected(err);
    }

    ReplicaTransferSummary transfer_summary;
    for (const auto& replica : start_result.value()) {
        transfer_summary.RecordAllocatedReplica(replica);
    }

    // Record Put transfer latency (all replicas)
    auto t0_put = std::chrono::steady_clock::now();

    // We must deal with disk replica first, then the disk putrevoke/putend can
    // be called surely
    if (storage_backend_) {
        for (auto it = start_result.value().rbegin();
             it != start_result.value().rend(); ++it) {
            const auto& replica = *it;
            if (replica.is_disk_replica()) {
                // Store to local file if storage backend is available
                auto disk_descriptor = replica.get_disk_descriptor();
                PutToLocalFile(key, slices, disk_descriptor);
                break;  // Only one disk replica is needed
            }
        }
    }

    for (const auto& replica : start_result.value()) {
        if (replica.is_memory_replica() || replica.is_nof_replica()) {
            // Transfer data using allocated handles from all replicas
            const auto replica_type = replica.is_memory_replica()
                                          ? ReplicaType::MEMORY
                                          : ReplicaType::NOF_SSD;
            ErrorCode transfer_err = TransferWrite(replica, slices);
            if (transfer_err != ErrorCode::OK) {
                transfer_summary.RecordFailure(replica_type, transfer_err);
                continue;
            }
            transfer_summary.RecordSuccess(replica_type);
        }
    }

    auto us_put = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0_put)
                      .count();
    if (metrics_) {
        metrics_->transfer_metric.put_latency_us.observe(us_put);
    }

    const auto finalize_decision =
        DetermineFinalizeDecision(config, transfer_summary);

    if (finalize_decision.end_type.has_value()) {
        auto end_result = master_client_.PutEnd(key, *finalize_decision.end_type);
        if (!end_result) {
            ErrorCode err = end_result.error();
            LOG(ERROR) << "Failed to end put operation: " << err;
            return tl::unexpected(err);
        }
    }

    if (finalize_decision.revoke_type.has_value()) {
        auto revoke_result =
            master_client_.PutRevoke(key, *finalize_decision.revoke_type);
        if (!revoke_result) {
            LOG(ERROR) << "Failed to revoke put operation";
            return tl::unexpected(revoke_result.error());
        }
    }

    if (!finalize_decision.success) {
        return tl::unexpected(finalize_decision.error);
    }

    return {};
}

// TODO: `client.cpp` is too long, consider split it into multiple files
enum class PutOperationState {
    PENDING,
    MASTER_FAILED,
    TRANSFER_FAILED,
    FINALIZE_FAILED,
    SUCCESS
};

class PutOperation {
   public:
    struct PendingTransferRecord {
        ReplicaType replica_type;
        TransferFuture future;

        PendingTransferRecord(ReplicaType type, TransferFuture&& transfer_future)
            : replica_type(type), future(std::move(transfer_future)) {}
    };

    PutOperation(std::string_view k, const std::vector<Slice>& s)
        : key(k), slices(s) {
        value_length = CalculateSliceSize(slices);
        ptr = ((!s.empty()) ? slices[0].ptr : nullptr);
        // Initialize with a pending error state to ensure result is always set
        result = tl::unexpected(ErrorCode::INTERNAL_ERROR);
    }

    std::string key;
    std::vector<Slice> slices;
    size_t value_length;
    void *ptr;
    std::vector<std::vector<Slice>> batched_slices;

    // Enhanced state tracking
    PutOperationState state = PutOperationState::PENDING;
    tl::expected<void, ErrorCode> result;
    std::vector<Replica::Descriptor> replicas;
    std::vector<PendingTransferRecord> pending_transfers;

    size_t requested_memory_replicas = 0;
    size_t requested_nof_replicas = 0;
    ReplicaTransferSummary transfer_summary;

    // Error context for debugging
    std::optional<std::string> failure_context;

    // Helper methods for robust state management
    void SetSuccess() {
        state = PutOperationState::SUCCESS;
        result = {};
        failure_context.reset();
    }

    void SetTerminalError(ErrorCode error, PutOperationState terminal_state,
                          const std::string& context = "") {
        state = terminal_state;
        result = tl::unexpected(error);
        if (!context.empty()) {
            failure_context = context;
        }
    }

    void SetError(ErrorCode error, const std::string& context = "") {
        result = tl::unexpected(error);
        if (!context.empty()) {
            failure_context = toString(error) + ": " + context + "; " +
                              failure_context.value_or("");
        }

        // Update state based on current processing stage
        if (replicas.empty()) {
            state = PutOperationState::MASTER_FAILED;
        } else if (pending_transfers.empty()) {
            state = PutOperationState::TRANSFER_FAILED;
        } else {
            state = PutOperationState::FINALIZE_FAILED;
        }
    }

    void InitializeRequestedReplicas(const ReplicateConfig& config) {
        requested_memory_replicas = config.replica_num;
        requested_nof_replicas = config.nof_replica_num;
    }

    ReplicateConfig ToReplicateConfig() const {
        ReplicateConfig config;
        config.replica_num = requested_memory_replicas;
        config.nof_replica_num = requested_nof_replicas;
        return config;
    }

    void RecordAllocatedReplicas() {
        transfer_summary = ReplicaTransferSummary{};
        for (const auto& replica : replicas) {
            transfer_summary.RecordAllocatedReplica(replica);
        }
    }

    bool IsResolved() const { return state != PutOperationState::PENDING; }

    bool IsSuccessful() const {
        return state == PutOperationState::SUCCESS && result.has_value();
    }
};

std::vector<PutOperation> Client::CreatePutOperations(
    const std::vector<ObjectKey>& keys,
    const std::vector<std::vector<Slice>>& batched_slices) {
    std::vector<PutOperation> ops;
    ops.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        ops.emplace_back(keys[i], batched_slices[i]);
    }
    return ops;
}

void Client::StartBatchPut(std::vector<PutOperation>& ops,
                           const ReplicateConfig& config) {
    std::vector<std::string> keys;
    std::vector<std::vector<uint64_t>> slice_lengths;

    keys.reserve(ops.size());
    slice_lengths.reserve(ops.size());

    for (const auto& op : ops) {
        keys.emplace_back(op.key);

        std::vector<uint64_t> slice_sizes;
        slice_sizes.reserve(op.slices.size());
        for (const auto& slice : op.slices) {
            slice_sizes.emplace_back(slice.size);
        }
        slice_lengths.emplace_back(std::move(slice_sizes));
    }

    auto start_responses =
        master_client_.BatchPutStart(keys, slice_lengths, config);

    // Ensure response size matches request size
    if (start_responses.size() != ops.size()) {
        LOG(ERROR) << "BatchPutStart response size mismatch: expected "
                   << ops.size() << ", got " << start_responses.size();
        for (auto& op : ops) {
            op.SetError(ErrorCode::RPC_FAIL,
                        "BatchPutStart response size mismatch");
        }
        return;
    }

    // Process individual responses with robust error handling
    for (size_t i = 0; i < ops.size(); ++i) {
        ops[i].InitializeRequestedReplicas(config);
        if (!start_responses[i]) {
            ops[i].SetTerminalError(start_responses[i].error(),
                                    PutOperationState::MASTER_FAILED,
                                    "Master failed to start put operation");
        } else {
            ops[i].replicas = start_responses[i].value();
            ops[i].RecordAllocatedReplicas();
            if (!HasExpectedReplicaAllocation(config, ops[i].transfer_summary)) {
                ops[i].SetTerminalError(
                    ErrorCode::NO_AVAILABLE_HANDLE,
                    PutOperationState::MASTER_FAILED,
                    "Allocated replicas do not satisfy requested replica policy");
                continue;
            }
            // Operation continues to next stage - result remains INTERNAL_ERROR
            // until fully successful
            VLOG(1) << "Successfully started put for key " << ops[i].key
                    << " with " << ops[i].replicas.size() << " replicas";
        }
    }
}

void Client::SubmitTransfers(std::vector<PutOperation>& ops) {
    if (!transfer_submitter_) {
        LOG(ERROR) << "TransferSubmitter not initialized";
        for (auto& op : ops) {
            op.SetTerminalError(ErrorCode::INVALID_PARAMS,
                                PutOperationState::TRANSFER_FAILED,
                                "TransferSubmitter not initialized");
        }
        return;
    }

    for (auto& op : ops) {
        // Skip operations that already failed in previous stages
        if (op.IsResolved()) {
            continue;
        }

        // Skip operations that don't have replicas (failed in StartBatchPut)
        if (op.replicas.empty()) {
            op.SetTerminalError(ErrorCode::INTERNAL_ERROR,
                                PutOperationState::MASTER_FAILED,
                                "No replicas available for transfer");
            continue;
        }

        // We must deal with disk replica first, then the disk putrevoke/putend
        // can be called surely
        if (storage_backend_) {
            for (auto it = op.replicas.rbegin(); it != op.replicas.rend();
                 ++it) {
                const auto& replica = *it;
                if (replica.is_disk_replica()) {
                    auto disk_descriptor = replica.get_disk_descriptor();
                    PutToLocalFile(op.key, op.slices, disk_descriptor);
                    break;  // Only one disk replica is needed
                }
            }
        }

        for (size_t replica_idx = 0; replica_idx < op.replicas.size();
             ++replica_idx) {
            const auto& replica = op.replicas[replica_idx];
            if (replica.is_memory_replica() || replica.is_nof_replica()) {
                const auto replica_type = replica.is_memory_replica()
                                              ? ReplicaType::MEMORY
                                              : ReplicaType::NOF_SSD;
                auto submit_result = transfer_submitter_->submit(
                    replica, op.slices, TransferRequest::WRITE, op.ptr, op.value_length);

                if (!submit_result) {
                    std::string failure_context =
                        "Failed to submit transfer for replica " +
                        std::to_string(replica_idx);
                    op.transfer_summary.RecordFailure(
                        replica_type, ErrorCode::TRANSFER_FAIL);
                    if (!op.failure_context.has_value()) {
                        op.failure_context = failure_context;
                    } else {
                        op.failure_context =
                            *op.failure_context + "; " + failure_context;
                    }
                    continue;
                }

                op.pending_transfers.emplace_back(replica_type,
                                                  std::move(submit_result.value()));
            }
        }

        VLOG(1) << "Submitted " << op.pending_transfers.size()
                << " transfers for key " << op.key;
    }
}

void Client::WaitForTransfers(std::vector<PutOperation>& ops) {
    for (auto& op : ops) {
        // Skip operations that already failed or completed
        if (op.IsResolved()) {
            continue;
        }

        for (size_t i = 0; i < op.pending_transfers.size(); ++i) {
            auto& pending_transfer = op.pending_transfers[i];
            ErrorCode transfer_result = pending_transfer.future.get();
            if (transfer_result != ErrorCode::OK) {
                op.transfer_summary.RecordFailure(
                    pending_transfer.replica_type, transfer_result);
                std::string error_context =
                    "Transfer " + std::to_string(i) + " failed";
                if (!op.failure_context.has_value()) {
                    op.failure_context = error_context;
                } else {
                    op.failure_context =
                        *op.failure_context + "; " + error_context;
                }
            } else {
                op.transfer_summary.RecordSuccess(pending_transfer.replica_type);
            }
        }

        VLOG(1) << "Transfers finished for key " << op.key
                << ", success(mem=" << op.transfer_summary.successful_memory_transfers
                << ", nof=" << op.transfer_summary.successful_nof_transfers
                << "), fail(mem=" << op.transfer_summary.failed_memory_transfers
                << ", nof=" << op.transfer_summary.failed_nof_transfers << ")";
    }
}

void Client::FinalizeBatchPut(std::vector<PutOperation>& ops) {
    struct BatchFinalizeGroup {
        std::vector<std::string> keys;
        std::vector<size_t> indices;
    };

    BatchFinalizeGroup end_all_group;
    BatchFinalizeGroup end_memory_group;
    BatchFinalizeGroup end_nof_group;
    BatchFinalizeGroup revoke_all_group;
    BatchFinalizeGroup revoke_memory_group;
    BatchFinalizeGroup revoke_nof_group;

    std::vector<size_t> pending_finalize_actions(ops.size(), 0);
    std::vector<bool> should_succeed(ops.size(), false);
    std::vector<ErrorCode> terminal_errors(ops.size(), ErrorCode::OK);
    std::vector<std::optional<ErrorCode>> finalize_rpc_errors(ops.size());

    auto add_group_entry = [](BatchFinalizeGroup& group, const std::string& key,
                              size_t index) {
        group.keys.emplace_back(key);
        group.indices.emplace_back(index);
    };

    auto add_finalize_action =
        [&](const std::optional<ReplicaType>& replica_type, bool is_end,
            const std::string& key, size_t index) {
            if (!replica_type.has_value()) {
                return;
            }
            ++pending_finalize_actions[index];
            switch (*replica_type) {
                case ReplicaType::ALL:
                    add_group_entry(is_end ? end_all_group : revoke_all_group,
                                    key, index);
                    break;
                case ReplicaType::MEMORY:
                    add_group_entry(
                        is_end ? end_memory_group : revoke_memory_group, key,
                        index);
                    break;
                case ReplicaType::NOF_SSD:
                    add_group_entry(is_end ? end_nof_group : revoke_nof_group,
                                    key, index);
                    break;
                default:
                    LOG(ERROR) << "Unexpected replica type in batch finalize: "
                               << *replica_type;
                    finalize_rpc_errors[index] = ErrorCode::INVALID_PARAMS;
                    break;
            }
        };

    for (size_t i = 0; i < ops.size(); ++i) {
        auto& op = ops[i];
        if (op.IsResolved()) {
            continue;
        }
        if (op.replicas.empty()) {
            op.SetTerminalError(ErrorCode::INTERNAL_ERROR,
                                PutOperationState::MASTER_FAILED,
                                "Operation has no replicas to finalize");
            continue;
        }

        const auto finalize_decision =
            DetermineFinalizeDecision(op.ToReplicateConfig(), op.transfer_summary);
        should_succeed[i] = finalize_decision.success;
        terminal_errors[i] = finalize_decision.error;
        add_finalize_action(finalize_decision.end_type, true, op.key, i);
        add_finalize_action(finalize_decision.revoke_type, false, op.key, i);
    }

    auto process_end_group = [&](BatchFinalizeGroup& group,
                                 ReplicaType replica_type) {
        if (group.keys.empty()) {
            return;
        }
        auto responses = master_client_.BatchPutEnd(group.keys, replica_type);
        if (responses.size() != group.keys.size()) {
            for (size_t idx : group.indices) {
                finalize_rpc_errors[idx] = ErrorCode::RPC_FAIL;
            }
            return;
        }
        for (size_t i = 0; i < responses.size(); ++i) {
            const size_t op_idx = group.indices[i];
            if (!responses[i]) {
                finalize_rpc_errors[op_idx] = responses[i].error();
                LOG(ERROR) << "Failed to BatchPutEnd key " << group.keys[i]
                           << ": " << toString(responses[i].error());
                continue;
            }
            if (pending_finalize_actions[op_idx] > 0) {
                --pending_finalize_actions[op_idx];
            }
        }
    };

    auto process_revoke_group = [&](BatchFinalizeGroup& group,
                                    ReplicaType replica_type) {
        if (group.keys.empty()) {
            return;
        }
        auto responses = master_client_.BatchPutRevoke(group.keys, replica_type);
        if (responses.size() != group.keys.size()) {
            for (size_t idx : group.indices) {
                finalize_rpc_errors[idx] = ErrorCode::RPC_FAIL;
            }
            return;
        }
        for (size_t i = 0; i < responses.size(); ++i) {
            const size_t op_idx = group.indices[i];
            if (!responses[i]) {
                finalize_rpc_errors[op_idx] = responses[i].error();
                LOG(ERROR) << "Failed to BatchPutRevoke key " << group.keys[i]
                           << ": " << toString(responses[i].error());
                continue;
            }
            if (pending_finalize_actions[op_idx] > 0) {
                --pending_finalize_actions[op_idx];
            }
        }
    };

    process_end_group(end_all_group, ReplicaType::ALL);
    process_end_group(end_memory_group, ReplicaType::MEMORY);
    process_end_group(end_nof_group, ReplicaType::NOF_SSD);
    process_revoke_group(revoke_all_group, ReplicaType::ALL);
    process_revoke_group(revoke_memory_group, ReplicaType::MEMORY);
    process_revoke_group(revoke_nof_group, ReplicaType::NOF_SSD);

    for (size_t i = 0; i < ops.size(); ++i) {
        auto& op = ops[i];
        if (op.IsResolved()) {
            continue;
        }
        if (finalize_rpc_errors[i].has_value()) {
            op.SetTerminalError(*finalize_rpc_errors[i],
                                PutOperationState::FINALIZE_FAILED,
                                "Batch finalization RPC failed");
            continue;
        }
        if (pending_finalize_actions[i] != 0) {
            op.SetTerminalError(ErrorCode::INTERNAL_ERROR,
                                PutOperationState::FINALIZE_FAILED,
                                "Operation has unfinished finalize actions");
            continue;
        }
        if (should_succeed[i]) {
            op.SetSuccess();
            continue;
        }
        op.SetTerminalError(terminal_errors[i], PutOperationState::TRANSFER_FAILED,
                            op.failure_context.value_or(
                                "Replica transfer failed before finalize"));
    }
}

std::vector<tl::expected<void, ErrorCode>> Client::CollectResults(
    const std::vector<PutOperation>& ops) {
    std::vector<tl::expected<void, ErrorCode>> results;
    results.reserve(ops.size());

    int no_available_handle_count = 0;
    for (const auto& op : ops) {
        // With the new structure, result is always set (never nullopt)
        results.emplace_back(op.result);

        // Additional validation and logging for debugging
        if (!op.result.has_value()) {
            // if error == object already exist, consider as ok
            if (op.result.error() == ErrorCode::OBJECT_ALREADY_EXISTS) {
                results.back() = {};
                continue;
            }
            if (op.result.error() == ErrorCode::NO_AVAILABLE_HANDLE) {
                no_available_handle_count++;
            } else {
                LOG(ERROR) << "Operation for key " << op.key
                           << " failed: " << toString(op.result.error())
                           << (op.failure_context
                                   ? (" (" + *op.failure_context + ")")
                                   : "");
            }
        } else {
            VLOG(1) << "Operation for key " << op.key
                    << " completed successfully";
        }
    }
    if (no_available_handle_count > 0) {
        LOG(WARNING) << "BatchPut failed for " << no_available_handle_count
                     << " keys" << PUT_NO_SPACE_HELPER_STR;
    }

    return results;
}

std::vector<tl::expected<void, ErrorCode>> Client::BatchPutWhenPreferSameNode(
    std::vector<PutOperation>& ops) {
    auto t0 = std::chrono::steady_clock::now();
    std::unordered_map<std::string, PutOperation> seg_to_ops{};
    for (auto& op : ops) {
        if (op.IsResolved()) {
            continue;
        }
        if (op.replicas.empty()) {
            op.SetError(ErrorCode::INTERNAL_ERROR,
                        "No replicas available for transfer");
            continue;
        }
        auto replica = op.replicas[0];
        if (!replica.is_memory_replica()) {
            op.SetError(ErrorCode::INVALID_PARAMS, "only memory is supported.");
            continue;
        }
        auto& memory_descriptor = replica.get_memory_descriptor();
        if (memory_descriptor.buffer_descriptor.size_ == 0) {
            op.SetError(ErrorCode::INVALID_PARAMS, "buffer size is 0.");
            continue;
        }
        auto& buffer_descriptor = memory_descriptor.buffer_descriptor;
        auto seg = buffer_descriptor.transport_endpoint_;
        if (seg_to_ops.find(seg) == seg_to_ops.end()) {
            seg_to_ops.emplace(seg, PutOperation(op.key, op.slices));
        }
        // multiple replica-slices
        seg_to_ops.at(seg).batched_slices.emplace_back(op.slices);
        seg_to_ops.at(seg).replicas.emplace_back(replica);
    }
    std::vector<PutOperation> merged_ops;
    merged_ops.reserve(seg_to_ops.size());
    for (auto& seg_to_op : seg_to_ops) {
        auto& op = seg_to_op.second;
        bool all_transfers_submitted = true;
        std::string failure_context;
        merged_ops.emplace_back(op.key, op.slices);
        auto& merged_op = merged_ops.back();
        merged_op.replicas = op.replicas;
        merged_op.transfer_summary.allocated_memory_replicas = 1;
        auto submit_result = transfer_submitter_->submit_batch(
            op.replicas, op.batched_slices, TransferRequest::WRITE);
        if (!submit_result) {
            failure_context = "Failed to submit batch transfer";
            all_transfers_submitted = false;
        } else {
            merged_op.pending_transfers.emplace_back(
                ReplicaType::MEMORY, std::move(submit_result.value()));
        }
        if (!all_transfers_submitted) {
            LOG(ERROR) << "Transfer submission failed for key " << op.key
                       << ": " << failure_context;
            merged_op.transfer_summary.RecordFailure(ReplicaType::MEMORY,
                                                     ErrorCode::TRANSFER_FAIL);
            merged_op.failure_context = failure_context;
            merged_op.pending_transfers.clear();
        } else {
            VLOG(1) << "Successfully submitted "
                    << merged_op.pending_transfers.size()
                    << " transfers for key " << merged_ops.back().key;
        }
    }
    WaitForTransfers(merged_ops);
    for (auto& op : merged_ops) {
        auto& memory_descriptor = op.replicas[0].get_memory_descriptor();
        auto& buffer_descriptor = memory_descriptor.buffer_descriptor;
        auto seg = buffer_descriptor.transport_endpoint_;
        seg_to_ops.at(seg).transfer_summary = op.transfer_summary;
        seg_to_ops.at(seg).failure_context = op.failure_context;
    }
    for (auto& op : ops) {
        if (op.IsResolved()) {
            continue;
        }
        auto& memory_descriptor = op.replicas[0].get_memory_descriptor();
        auto& buffer_descriptor = memory_descriptor.buffer_descriptor;
        auto seg = buffer_descriptor.transport_endpoint_;
        op.transfer_summary.successful_memory_transfers =
            seg_to_ops.at(seg).transfer_summary.successful_memory_transfers > 0
                ? 1
                : 0;
        op.transfer_summary.failed_memory_transfers =
            seg_to_ops.at(seg).transfer_summary.failed_memory_transfers > 0 ? 1
                                                                            : 0;
        op.transfer_summary.first_error =
            seg_to_ops.at(seg).transfer_summary.first_error;
        op.failure_context = seg_to_ops.at(seg).failure_context;
    }
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    if (metrics_) {
        metrics_->transfer_metric.batch_put_latency_us.observe(us);
    }
    FinalizeBatchPut(ops);
    return CollectResults(ops);
}

std::vector<tl::expected<void, ErrorCode>> Client::BatchPut(
    const std::vector<ObjectKey>& keys,
    std::vector<std::vector<Slice>>& batched_slices,
    const ReplicateConfig& config) {
    std::vector<PutOperation> ops = CreatePutOperations(keys, batched_slices);
    if (config.prefer_alloc_in_same_node) {
        if (config.nof_replica_num != 0) {
            LOG(ERROR) << "prefer_alloc_in_same_node is not supported with "
                          "nof_replica_num != 0";
            return std::vector<tl::expected<void, ErrorCode>>(
                keys.size(), tl::unexpected(ErrorCode::INVALID_PARAMS));
        }
        if (config.replica_num != 1) {
            LOG(ERROR) << "prefer_alloc_in_same_node is not supported with "
                          "replica_num != 1";
            return std::vector<tl::expected<void, ErrorCode>>(
                keys.size(), tl::unexpected(ErrorCode::INVALID_PARAMS));
        }
        StartBatchPut(ops, config);
        return BatchPutWhenPreferSameNode(ops);
    }
    StartBatchPut(ops, config);

    auto t0 = std::chrono::steady_clock::now();
    SubmitTransfers(ops);
    WaitForTransfers(ops);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    if (metrics_) {
        metrics_->transfer_metric.batch_put_latency_us.observe(us);
    }

    FinalizeBatchPut(ops);
    return CollectResults(ops);
}

tl::expected<void, ErrorCode> Client::Remove(const ObjectKey& key) {
    auto result = master_client_.Remove(key);
    // if (storage_backend_) {
    //     storage_backend_->RemoveFile(key);
    // }
    if (!result) {
        return tl::unexpected(result.error());
    }
    return {};
}

tl::expected<long, ErrorCode> Client::RemoveByRegex(const ObjectKey& str) {
    auto result = master_client_.RemoveByRegex(str);
    // if (storage_backend_) {
    //     storage_backend_->RemoveByRegex(str);
    // }
    if (!result) {
        return tl::unexpected(result.error());
    }
    return result.value();
}

tl::expected<long, ErrorCode> Client::RemoveAll() {
    // if (storage_backend_) {
    //     storage_backend_->RemoveAll();
    // }
    return master_client_.RemoveAll();
}

tl::expected<void, ErrorCode> Client::MountSegment(const void* buffer,
                                                   size_t size) {
    auto check_result = CheckRegisterMemoryParams(buffer, size);
    if (!check_result) {
        return tl::unexpected(check_result.error());
    }

    std::lock_guard<std::mutex> lock(mounted_segments_mutex_);

    // Check if the segment overlaps with any existing segment
    for (auto& it : mounted_segments_) {
        auto& mtseg = it.second;
        uintptr_t l1 = reinterpret_cast<uintptr_t>(mtseg.base);
        uintptr_t r1 = reinterpret_cast<uintptr_t>(mtseg.size) + l1;
        uintptr_t l2 = reinterpret_cast<uintptr_t>(buffer);
        uintptr_t r2 = reinterpret_cast<uintptr_t>(size) + l2;
        if (std::max(l1, l2) < std::min(r1, r2)) {
            LOG(ERROR) << "segment_overlaps base1=" << mtseg.base
                       << " size1=" << mtseg.size << " base2=" << buffer
                       << " size2=" << size;
            return tl::unexpected(ErrorCode::INVALID_PARAMS);
        }
    }

    int rc = transfer_engine_->registerLocalMemory(
        (void*)buffer, size, kWildcardLocation, true, true);
    if (rc != 0) {
        LOG(ERROR) << "register_local_memory_failed base=" << buffer
                   << " size=" << size << ", error=" << rc;
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }

    // Build segment with logical name; attach TE endpoint for transport
    Segment segment;
    segment.id = generate_uuid();
    segment.name = local_hostname_;
    segment.base = reinterpret_cast<uintptr_t>(buffer);
    segment.size = size;
    // For P2P handshake mode, publish the actual transport endpoint that was
    // negotiated by the transfer engine. Otherwise, keep the logical hostname
    // so metadata backends (HTTP/etcd/redis) can resolve the segment by name.
    if (metadata_connstring_ == P2PHANDSHAKE) {
        segment.te_endpoint = transfer_engine_->getLocalIpAndPort();
    } else {
        segment.te_endpoint = local_hostname_;
    }

    auto mount_result = master_client_.MountSegment(segment);
    if (!mount_result) {
        ErrorCode err = mount_result.error();
        LOG(ERROR) << "mount_segment_to_master_failed base=" << buffer
                   << " size=" << size << ", error=" << err;
        return tl::unexpected(err);
    }

    mounted_segments_[segment.id] = segment;
    return {};
}

tl::expected<void, ErrorCode> Client::UnmountSegment(const void* buffer,
                                                     size_t size) {
    std::lock_guard<std::mutex> lock(mounted_segments_mutex_);
    auto segment = mounted_segments_.end();

    for (auto it = mounted_segments_.begin(); it != mounted_segments_.end();
         ++it) {
        if (it->second.base == reinterpret_cast<uintptr_t>(buffer) &&
            it->second.size == size) {
            segment = it;
            break;
        }
    }
    if (segment == mounted_segments_.end()) {
        LOG(ERROR) << "segment_not_found base=" << buffer << " size=" << size;
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }

    auto unmount_result = master_client_.UnmountSegment(segment->second.id);
    if (!unmount_result) {
        ErrorCode err = unmount_result.error();
        LOG(ERROR) << "Failed to unmount segment from master: "
                   << toString(err);
        return tl::unexpected(err);
    }

    int rc = transfer_engine_->unregisterLocalMemory(
        reinterpret_cast<void*>(segment->second.base));
    if (rc != 0) {
        LOG(ERROR) << "Failed to unregister transfer buffer with transfer "
                      "engine ret is "
                   << rc;
        if (rc != ERR_ADDRESS_NOT_REGISTERED) {
            return tl::unexpected(ErrorCode::INTERNAL_ERROR);
        }
        // Otherwise, the segment is already unregistered from transfer
        // engine, we can continue
    }

    mounted_segments_.erase(segment);
    return {};
}

tl::expected<void, ErrorCode> Client::RegisterLocalMemory(
    void* addr, size_t length, const std::string& location,
    bool remote_accessible, bool update_metadata) {
    auto check_result = CheckRegisterMemoryParams(addr, length);
    if (!check_result) {
        return tl::unexpected(check_result.error());
    }
    if (this->transfer_engine_->registerLocalMemory(
            addr, length, location, remote_accessible, update_metadata) != 0) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    return {};
}

tl::expected<void, ErrorCode> Client::unregisterLocalMemory(
    void* addr, bool update_metadata) {
    if (this->transfer_engine_->unregisterLocalMemory(addr, update_metadata) !=
        0) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    return {};
}

tl::expected<bool, ErrorCode> Client::IsExist(const std::string& key) {
    auto result = master_client_.ExistKey(key);
    return result;
}

std::vector<tl::expected<bool, ErrorCode>> Client::BatchIsExist(
    const std::vector<std::string>& keys) {
    auto response = master_client_.BatchExistKey(keys);

    // Check if we got the expected number of responses
    if (response.size() != keys.size()) {
        LOG(ERROR) << "BatchExistKey response size mismatch. Expected: "
                   << keys.size() << ", Got: " << response.size();
        // Return vector of RPC_FAIL errors
        std::vector<tl::expected<bool, ErrorCode>> results;
        results.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            results.emplace_back(tl::unexpected(ErrorCode::RPC_FAIL));
        }
        return results;
    }

    // Return the response directly as it's already in the correct
    // format
    return response;
}

tl::expected<void, ErrorCode> Client::MountLocalDiskSegment(
    bool enable_offloading) {
    auto response =
        master_client_.MountLocalDiskSegment(client_id_, enable_offloading);

    if (!response) {
        LOG(ERROR) << "MountLocalDiskSegment failed, error code is "
                   << response.error();
    }
    return response;
}

tl::expected<void, ErrorCode> Client::OffloadObjectHeartbeat(
    bool enable_offloading,
    std::unordered_map<std::string, int64_t>& offloading_objects) {
    auto response =
        master_client_.OffloadObjectHeartbeat(client_id_, enable_offloading);
    if (!response) {
        LOG(ERROR) << "OffloadObjectHeartbeat failed, error code is "
                   << response.error();
        return tl::make_unexpected(response.error());
    }
    offloading_objects = std::move(response.value());
    return {};
}

tl::expected<void, ErrorCode> Client::BatchPutOffloadObject(
    const std::string& transfer_engine_addr,
    const std::vector<std::string>& keys,
    const std::vector<uintptr_t>& pointers,
    const std::unordered_map<std::string, Slice>& batched_slices) {
    return {};
}

tl::expected<void, ErrorCode> Client::NotifyOffloadSuccess(
    const std::vector<std::string>& keys,
    const std::vector<StorageObjectMetadata>& metadatas) {
    auto response =
        master_client_.NotifyOffloadSuccess(client_id_, keys, metadatas);
    return response;
}

void Client::PrepareStorageBackend(const std::string& storage_root_dir,
                                   const std::string& fsdir,
                                   bool enable_eviction, uint64_t quota_bytes) {
    // Initialize storage backend
    storage_backend_ =
        StorageBackend::Create(storage_root_dir, fsdir, enable_eviction);
    if (!storage_backend_) {
        LOG(INFO) << "Failed to initialize storage backend";
    }
    auto init_result = storage_backend_->Init(quota_bytes);
    if (!init_result) {
        LOG(ERROR) << "Failed to initialize StorageBackend. Error: "
                   << init_result.error() << ". The backend will be unusable.";
    }
}

void Client::PutToLocalFile(const std::string& key,
                            const std::vector<Slice>& slices,
                            const DiskDescriptor& disk_descriptor) {
    if (!storage_backend_) return;

    size_t total_size = 0;
    for (const auto& slice : slices) {
        total_size += slice.size;
    }

    std::string path = disk_descriptor.file_path;
    // Currently, persistence is achieved through asynchronous writes, but
    // before asynchronous writing in 3FS, significant performance degradation
    // may occur due to data copying. Profiling reveals that the number of page
    // faults triggered in this scenario is nearly double the normal count.
    // Future plans include introducing a reuse buffer list to address this
    // performance degradation issue.

    std::string value;
    value.reserve(total_size);
    for (const auto& slice : slices) {
        value.append(static_cast<char*>(slice.ptr), slice.size);
    }

    write_thread_pool_.enqueue([this, backend = storage_backend_, key,
                                value = std::move(value), path] {
        // Store the object
        auto store_result = backend->StoreObject(path, value);
        ReplicaType replica_type = ReplicaType::DISK;

        if (!store_result) {
            // If storage failed, revoke the put operation
            LOG(ERROR) << "Failed to store object for key: " << key;
            auto revoke_result = master_client_.PutRevoke(key, replica_type);
            if (!revoke_result) {
                LOG(ERROR) << "Failed to revoke put operation for key: " << key;
            }
            return;
        }

        // If storage succeeded, end the put operation
        auto end_result = master_client_.PutEnd(key, replica_type);
        if (!end_result) {
            LOG(ERROR) << "Failed to end put operation for key: " << key;
        }
    });
}

ErrorCode Client::TransferData(const Replica::Descriptor& replica_descriptor,
                               std::vector<Slice>& slices,
                               TransferRequest::OpCode op_code) {
    if (!transfer_submitter_) {
        LOG(ERROR) << "TransferSubmitter not initialized";
        return ErrorCode::INVALID_PARAMS;
    }

    std::optional<TransferFuture> future;
    if (replica_descriptor.is_nof_replica()) {
        size_t total_size = CalculateSliceSize(slices);
        if (slices.empty() || slices[0].ptr == nullptr || total_size == 0) {
            LOG(ERROR) << "Invalid slices for NoF transfer";
            return ErrorCode::INVALID_PARAMS;
        }
        future = transfer_submitter_->submit(replica_descriptor, slices,
                                             op_code, slices[0].ptr, total_size);
    } else {
        future = transfer_submitter_->submit(replica_descriptor, slices, op_code);
    }
    if (!future) {
        LOG(ERROR) << "Failed to submit transfer operation";
        return ErrorCode::TRANSFER_FAIL;
    }

    VLOG(1) << "Using transfer strategy: " << future->strategy();

    return future->get();
}

ErrorCode Client::TransferWrite(const Replica::Descriptor& replica_descriptor,
                                std::vector<Slice>& slices) {
    return TransferData(replica_descriptor, slices, TransferRequest::WRITE);
}

ErrorCode Client::TransferRead(const Replica::Descriptor& replica_descriptor,
                               std::vector<Slice>& slices) {
    size_t total_size = 0;
    if (replica_descriptor.is_memory_replica()) {
        auto& mem_desc = replica_descriptor.get_memory_descriptor();
        total_size = mem_desc.buffer_descriptor.size_;
    } else if (replica_descriptor.is_nof_replica()) {
        auto &nof_desc = replica_descriptor.get_nof_descriptor();
        total_size = nof_desc.buffer_descriptor.size_;
    } else if (replica_descriptor.is_disk_replica()) {
        auto& disk_desc = replica_descriptor.get_disk_descriptor();
        total_size = disk_desc.object_size;
    } else if (replica_descriptor.is_local_disk_replica()) {
        auto& disk_desc = replica_descriptor.get_local_disk_descriptor();
        total_size = disk_desc.object_size;
    }

    size_t slices_size = CalculateSliceSize(slices);
    if (slices_size < total_size) {
        LOG(ERROR) << "Slice size " << slices_size << " is smaller than total "
                   << "size " << total_size;
        return ErrorCode::INVALID_PARAMS;
    }

    return TransferData(replica_descriptor, slices, TransferRequest::READ);
}

void Client::PingThreadMain(bool is_ha_mode,
                            std::string current_master_address) {
    // How many failed pings before getting latest master view from etcd
    const int max_ping_fail_count = 3;
    // How long to wait for next ping after success
    const int success_ping_interval_ms = 1000;
    // How long to wait for next ping after failure
    const int fail_ping_interval_ms = 1000;
    // Increment after a ping failure, reset after a ping success
    int ping_fail_count = 0;

    auto remount_segment = [this]() {
        // This lock must be held until the remount rpc is finished,
        // otherwise there will be corner cases, e.g., a segment is
        // unmounted successfully first, and then remounted again in
        // this thread.
        std::lock_guard<std::mutex> lock(mounted_segments_mutex_);
        std::vector<Segment> segments;
        for (auto it : mounted_segments_) {
            auto& segment = it.second;
            segments.emplace_back(segment);
        }
        auto remount_result = master_client_.ReMountSegment(segments);
        if (!remount_result) {
            ErrorCode err = remount_result.error();
            LOG(ERROR) << "Failed to remount segments: " << err;
        }
    };
    // Use another thread to remount segments to avoid blocking the ping
    // thread
    std::future<void> remount_segment_future;

    while (ping_running_) {
        // Join the remount segment thread if it is ready
        if (remount_segment_future.valid() &&
            remount_segment_future.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            remount_segment_future = std::future<void>();
        }

        // Ping master
        auto ping_result = master_client_.Ping();
        if (ping_result) {
            // Reset ping failure count
            ping_fail_count = 0;
            auto& ping_response = ping_result.value();
            if (ping_response.client_status == ClientStatus::NEED_REMOUNT &&
                !remount_segment_future.valid()) {
                // Ensure at most one remount segment thread is running
                remount_segment_future =
                    std::async(std::launch::async, remount_segment);
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(success_ping_interval_ms));
            continue;
        }

        ping_fail_count++;
        if (ping_fail_count < max_ping_fail_count) {
            LOG(ERROR) << "Failed to ping master";
            std::this_thread::sleep_for(
                std::chrono::milliseconds(fail_ping_interval_ms));
            continue;
        }

        // Exceeded ping failure threshold. Reconnect based on mode.
        if (is_ha_mode) {
            LOG(ERROR)
                << "Failed to ping master for " << ping_fail_count
                << " times; fetching latest master view and reconnecting";
            std::string master_address;
            ViewVersionId next_version = 0;
            auto err =
                master_view_helper_.GetMasterView(master_address, next_version);
            if (err != ErrorCode::OK) {
                LOG(ERROR) << "Failed to get new master view: "
                           << toString(err);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(fail_ping_interval_ms));
                continue;
            }

            err = master_client_.Connect(master_address);
            if (err != ErrorCode::OK) {
                LOG(ERROR) << "Failed to connect to master " << master_address
                           << ": " << toString(err);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(fail_ping_interval_ms));
                continue;
            }

            current_master_address = master_address;
            LOG(INFO) << "Reconnected to master " << master_address;
            ping_fail_count = 0;
        } else {
            LOG(ERROR) << "Failed to ping master for " << ping_fail_count
                       << " times (non-HA); reconnecting to "
                       << current_master_address;
            auto err = master_client_.Connect(current_master_address);
            if (err != ErrorCode::OK) {
                LOG(ERROR) << "Reconnect failed to " << current_master_address
                           << ": " << toString(err);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(fail_ping_interval_ms));
                continue;
            }
            LOG(INFO) << "Reconnected to master " << current_master_address;
            ping_fail_count = 0;
        }
    }
    // Explicitly wait for the remount segment thread to finish
    if (remount_segment_future.valid()) {
        remount_segment_future.wait();
    }
}

ErrorCode Client::FindFirstCompleteReplica(
    const std::vector<Replica::Descriptor>& replica_list,
    Replica::Descriptor& replica) {
    // Find the first complete replica
    for (size_t i = 0; i < replica_list.size(); ++i) {
        if (replica_list[i].status == ReplicaStatus::COMPLETE) {
            replica = replica_list[i];
            return ErrorCode::OK;
        }
    }

    // No complete replica found
    return ErrorCode::INVALID_REPLICA;
}

tl::expected<Replica::Descriptor, ErrorCode> Client::GetPreferredReplica(
    const std::vector<Replica::Descriptor>& replica_list) {
    if (replica_list.empty()) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }
    if (mounted_segments_.empty() || replica_list.size() == 1) {
        return replica_list[0];
    }

    std::unordered_set<std::string> local_endpoints;
    local_endpoints.reserve(mounted_segments_.size());
    for (const auto& segment : mounted_segments_) {
        local_endpoints.insert(segment.second.te_endpoint);
    }

    for (const auto& rep : replica_list) {
        if (rep.is_memory_replica()) {
            const auto& mem_desc = rep.get_memory_descriptor();
            const std::string& endpoint =
                mem_desc.buffer_descriptor.transport_endpoint_;
            if (local_endpoints.count(endpoint)) {
                return rep;
            }
        }
    }

    return replica_list[0];
}

}  // namespace mooncake
