"""Exercise production per-object NoF completion without SPDK or device libraries.

The harness compiles the current BatchGet bodies and transfer-state definitions;
only the submitter, cache, metrics, and expected<void> dependency are stand-ins.
Actual SPDK DMA and H2D copies still require the hardware integration tests.
"""

from nof_test_support import CppTestCase, REPO, definition, main


def extracted_code():
    include = REPO / "mooncake-store/include"
    client_header = (include / "client_service.h").read_text(encoding="utf-8")
    transfer_header = (include / "transfer_task.h").read_text(encoding="utf-8")
    types = (include / "types.h").read_text(encoding="utf-8")
    client = (REPO / "mooncake-store/src/client_service.cpp").read_text(
        encoding="utf-8"
    )
    transfer = (REPO / "mooncake-store/src/transfer_task.cpp").read_text(
        encoding="utf-8"
    )
    batch_start = client.index(
        "std::vector<tl::expected<void, ErrorCode>> Client::BatchGet(\n"
        "    const std::vector<std::string>& object_keys,\n"
        "    const std::vector<QueryResult>& query_results,"
    )
    batch_end = client.index("bool Client::RedirectToHotCache(", batch_start)
    batch = client[batch_start:batch_end]
    if batch.count("Client::BatchGet(") != 2:
        raise AssertionError("expected legacy and restorer BatchGet overloads")
    future_start = transfer.index("TransferFuture::TransferFuture(")
    future_end = transfer.index("// TransferSubmitter Implementation", future_start)
    future_end = transfer.rfind("// =====", future_start, future_end)
    real_client = (REPO / "mooncake-store/src/real_client.cpp").read_text(
        encoding="utf-8"
    )
    filter_start = real_client.index(
        "    // NoF namespace discovery and pool allocation can take time."
    )
    filter_end = real_client.index(
        "    // Early return if no valid operations", filter_start
    )
    return {
        "ERROR": definition(types, "enum class ErrorCode"),
        "STRATEGY": definition(transfer_header, "enum class TransferStrategy"),
        "QUERY": definition(client_header, "class QueryResult {"),
        "STATE": definition(transfer_header, "class OperationState {"),
        "NOF_STATE": definition(transfer_header, "class NofOperationState"),
        "MEM_STATE": definition(transfer_header, "class MemcpyOperationState"),
        "FUTURE": definition(transfer_header, "class TransferFuture {"),
        "FUTURE_IMPL": transfer[future_start:future_end],
        "BATCH_GET": batch,
        "FILTER": real_client[filter_start:filter_end],
        "INFO": definition(
            real_client[
                real_client.rfind("    struct ValidKeyInfo {", 0, filter_start) :
            ],
            "struct ValidKeyInfo {",
        ),
    }


HARNESS = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include "nof_read_completion.h"

struct NullLog {
    template <class T> NullLog& operator<<(const T&) { return *this; }
};
#define LOG(level) NullLog()
#define VLOG(level) NullLog()

namespace tl {
template <class E> struct unexpected {
    E value;
    explicit unexpected(E error) : value(error) {}
};
template <class T, class E> class expected;
template <class E> class expected<void, E> {
 public:
    expected() = default;
    expected(unexpected<E> error) : error_(error.value) {}
    bool has_value() const { return !error_; }
    explicit operator bool() const { return has_value(); }
    E error() const { return error_.value(); }
 private:
    std::optional<E> error_;
};
}

namespace mooncake {
using namespace std::chrono_literals;
@@ERROR@@
std::string toString(ErrorCode code) { return std::to_string(static_cast<int>(code)); }
@@STRATEGY@@
struct DfsDescriptor { size_t offset = 0; size_t object_size = 1024; };
struct Replica {
    struct Descriptor {
        size_t index = 0;
        bool nof = true;
        bool is_memory_replica() const { return !nof; }
        bool is_nof_replica() const { return nof; }
        bool is_dfs_replica() const { return false; }
        DfsDescriptor get_dfs_descriptor() const { return {}; }
    };
};
@@QUERY@@
struct Slice { void* ptr; size_t size; };
struct TransferRequest { enum OpCode { READ }; };
enum class TransferIntent { kForegroundGet };
@@STATE@@
@@NOF_STATE@@
@@MEM_STATE@@
@@FUTURE@@
@@FUTURE_IMPL@@

void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    void Open() {
        { std::lock_guard<std::mutex> lock(mutex); ready = true; }
        cv.notify_all();
    }
    bool Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 3s, [this] { return ready; });
    }
};
struct Submitter {
    std::vector<std::shared_ptr<OperationState>> states;
    std::function<void(size_t)> before_return;
    size_t submitted = 0;
    Gate all_submitted;
    std::optional<TransferFuture> submit(
        const Replica::Descriptor& replica, std::vector<Slice>&,
        TransferRequest::OpCode, void*, size_t, TransferIntent) {
        auto state = states.at(replica.index);
        if (++submitted == states.size()) all_submitted.Open();
        if (!state) return std::nullopt;
        if (before_return) before_return(replica.index);
        return TransferFuture(std::move(state));
    }
};
struct Cache { void ReleaseHotKey(const std::string&) {} };
struct Metric { void observe(int64_t) {} };
struct Metrics { struct { Metric batch_get_latency_us; } transfer_metric; };
using Results = std::vector<tl::expected<void, ErrorCode>>;
using Slices = std::unordered_map<std::string, std::vector<Slice>>;
struct DfsReadRequest { std::string key; DfsDescriptor descriptor; std::vector<Slice> slices; };
struct DfsBackend {
    Results BatchRead(const std::vector<DfsReadRequest>& requests) {
        return Results(requests.size());
    }
};
struct DfsMetric {
    void RecordReadErrors(const std::string&, int64_t = 1) {}
    void ObserveRead(int64_t, int64_t, int64_t) {}
};
size_t calculate_total_size(const Replica::Descriptor&) { return 1024; }
int64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}
class Client {
 public:
    std::shared_ptr<DfsBackend> dfs_storage_backend_;
    DfsMetric* GetDfsMetricPtr() { return nullptr; }
    tl::expected<void, ErrorCode> VerifyObjectChecksum(
        const std::string&, const std::vector<Slice>&, size_t,
        const std::optional<uint64_t>&) { return {}; }
    auto MakeReadReplicaSelector() {
        return [this](const std::vector<Replica::Descriptor>& replicas,
                      const std::vector<Slice>&, Replica::Descriptor& out) {
            return FindFirstCompleteReplica(replicas, out);
        };
    }
    using NofReadBufferRestorer =
        std::function<tl::expected<void, ErrorCode>(size_t)>;
    std::shared_ptr<Submitter> transfer_submitter_ = std::make_shared<Submitter>();
    std::shared_ptr<Cache> hot_cache_;
    std::shared_ptr<Metrics> metrics_;
    bool same_node_called = false;
    Results BatchGet(const std::vector<std::string>&,
                     const std::vector<QueryResult>&, Slices&, bool);
    Results BatchGet(const std::vector<std::string>&,
                     const std::vector<QueryResult>&, Slices&, bool,
                     const NofReadBufferRestorer&);
    Results BatchGetWhenPreferSameNode(const std::vector<std::string>& keys,
                                      const std::vector<QueryResult>&, Slices&) {
        same_node_called = true;
        return Results(keys.size());
    }
    ErrorCode FindFirstCompleteReplica(
        const std::vector<Replica::Descriptor>& replicas, Replica::Descriptor& out) {
        if (replicas.empty()) return ErrorCode::INVALID_REPLICA;
        out = replicas.front();
        return ErrorCode::OK;
    }
    bool RedirectToHotCache(const std::string&, Replica::Descriptor&) { return false; }
    bool ShouldAdmitToHotCache(const std::string&, bool) { return false; }
    void ProcessSlicesAsync(const std::string&, const std::vector<Slice>&,
                            const Replica::Descriptor&) {}
};
@@BATCH_GET@@

struct Batch {
    Client client;
    std::vector<std::string> keys;
    std::vector<QueryResult> queries;
    Slices slices;
    std::vector<std::shared_ptr<NofOperationState>> nof;
    explicit Batch(size_t count) {
        for (size_t i = 0; i < count; ++i) {
            auto key = "key-" + std::to_string(i);
            keys.push_back(key);
            queries.emplace_back(std::vector<Replica::Descriptor>{{i, true}},
                                 std::chrono::steady_clock::now() + 1h);
            slices[key] = {{nullptr, 1024}};
            nof.push_back(std::make_shared<NofOperationState>());
            client.transfer_submitter_->states.push_back(nof.back());
        }
    }
    void SetLease(size_t index, std::chrono::steady_clock::time_point deadline) {
        // QueryResult members are const; replace the vector entry through a new vector.
        std::vector<QueryResult> replacement;
        for (size_t i = 0; i < queries.size(); ++i) {
            replacement.emplace_back(std::vector<Replica::Descriptor>{{i, true}},
                                     i == index ? deadline : queries[i].lease_timeout);
        }
        queries = std::move(replacement);
    }
    Results Get(const Client::NofReadBufferRestorer& restorer) {
        return client.BatchGet(keys, queries, slices, false, restorer);
    }
};

void CompletionOrderAndLease() {
    Batch b(3);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    b.SetLease(1, deadline); b.SetLease(2, deadline);
    Gate first_h2d, second_read, second_h2d;
    const auto caller = std::this_thread::get_id();
    std::vector<size_t> order;
    std::thread worker([&] {
        Check(b.client.transfer_submitter_->all_submitted.Wait(), "batch not submitted");
        b.nof[1]->set_completed(ErrorCode::OK);
        Check(first_h2d.Wait(), "ready key waited for the whole batch");
        Check(std::chrono::steady_clock::now() < deadline, "test missed lease");
        b.nof[2]->set_completed(ErrorCode::OK);
        second_read.Open();
        Check(second_h2d.Wait(), "second ready key waited for the slow first key");
        b.nof[0]->set_completed(ErrorCode::OK);
    });
    auto results = b.Get([&](size_t index) -> tl::expected<void, ErrorCode> {
        Check(std::this_thread::get_id() == caller, "H2D ran on an I/O thread");
        order.push_back(index);
        if (index == 1) {
            first_h2d.Open();
            Check(second_read.Wait(), "next read did not overlap H2D");
            std::this_thread::sleep_until(deadline + 20ms);
        }
        if (index == 2) second_h2d.Open();
        return {};
    });
    worker.join();
    Check(order == std::vector<size_t>({1, 2, 0}), "H2D ignored completion order");
    Check(results[0].has_value() && results[1].has_value() && results[2].has_value(),
          "H2D latency incorrectly consumed completed reads' leases");
}

void FailuresKeepOriginalIndices() {
    Batch b(6);
    b.slices.erase(b.keys[0]);
    b.client.transfer_submitter_->states[1].reset();
    b.SetLease(3, std::chrono::steady_clock::now() - 1s);
    for (size_t i = 2; i < 6; ++i)
        b.nof[i]->set_completed(i == 2 ? ErrorCode::TRANSFER_FAIL : ErrorCode::OK);
    std::vector<size_t> restored;
    auto results = b.Get([&](size_t index) -> tl::expected<void, ErrorCode> {
        restored.push_back(index);
        if (index == 4) throw std::runtime_error("H2D failed");
        return {};
    });
    Check(restored == std::vector<size_t>({4, 5}), "failed/expired read was restored or indices changed");
    const ErrorCode errors[] = {ErrorCode::INVALID_PARAMS, ErrorCode::TRANSFER_FAIL,
        ErrorCode::TRANSFER_FAIL, ErrorCode::LEASE_EXPIRED, ErrorCode::INTERNAL_ERROR};
    for (size_t i = 0; i < 5; ++i)
        Check(!results[i] && results[i].error() == errors[i], "per-key error lost");
    Check(results[5].has_value(), "failed key aborted the rest of the batch");
}

void EarlyCompletionKeepsTimestamp() {
    // Finish inside submit(), then delay its return until the lease expires.
    // Queue registration must retain the actual I/O completion timestamp.
    Batch early(1);
    auto registration_deadline = std::chrono::steady_clock::now() + 100ms;
    early.SetLease(0, registration_deadline);
    early.client.transfer_submitter_->before_return = [&](size_t index) {
        Check(std::chrono::steady_clock::now() < registration_deadline,
              "test missed early completion lease");
        early.nof[index]->set_completed(ErrorCode::OK);
        std::this_thread::sleep_until(registration_deadline + 20ms);
    };
    bool called = false;
    auto early_result = early.Get([&](size_t) -> tl::expected<void, ErrorCode> {
        called = true;
        return {};
    });
    Check(called && early_result[0].has_value(),
          "late notification registration discarded the actual completion timestamp");
}

void MixedMemoryLeasesUseObservedCompletion() {
    Batch b(3);
    const auto deadline = std::chrono::steady_clock::now() + 200ms;
    b.queries.clear();
    for (size_t i = 0; i < b.keys.size(); ++i) {
        b.queries.emplace_back(std::vector<Replica::Descriptor>{{i, i == 0}},
                               deadline);
    }
    auto early = std::make_shared<MemcpyOperationState>();
    auto late = std::make_shared<MemcpyOperationState>();
    early->set_completed(ErrorCode::OK);
    b.client.transfer_submitter_->states[1] = early;
    b.client.transfer_submitter_->states[2] = late;
    b.nof[0]->set_completed(ErrorCode::OK);
    std::thread worker([&] {
        Check(b.client.transfer_submitter_->all_submitted.Wait(), "batch not submitted");
        std::this_thread::sleep_until(deadline + 20ms);
        late->set_completed(ErrorCode::OK);
    });
    bool restored = false;
    auto results = b.Get([&](size_t index) -> tl::expected<void, ErrorCode> {
        Check(index == 0, "non-NoF read was restored");
        // Mixed batches deliberately finish other transports before H2D;
        // only the late transfer, not the already validated one, must expire.
        Check(late->is_completed(), "H2D blocked observation of MEMORY completion");
        restored = true;
        std::this_thread::sleep_until(deadline + 50ms);
        return {};
    });
    worker.join();
    Check(restored && results[0].has_value() && results[1].has_value(),
          "waiting for late MEMORY discarded an earlier completed result");
    Check(!results[2] && results[2].error() == ErrorCode::LEASE_EXPIRED,
          "MEMORY transfer completed after its lease but was accepted");
}

// Reuse QueryResult and ErrorCode to check the production pre-submit filter.
struct NofStagedTransfer { std::shared_ptr<int> owner; };
@@INFO@@
void Filter(std::vector<ValidKeyInfo>& valid_operations, Results& results) {
@@FILTER@@
}
void PreSubmitLeaseFilter() {
    auto expired = std::make_shared<int>(1), live = std::make_shared<int>(2);
    std::weak_ptr<int> expired_owner = expired, live_owner = live;
    std::vector<ValidKeyInfo> ops;
    const auto now = std::chrono::steady_clock::now();
    ops.push_back({"memory", 3, QueryResult({}, now-1h), {}, 1, std::nullopt});
    ops.push_back({"expired", 2, QueryResult({}, now-1h), {}, 1,
                   NofStagedTransfer{std::move(expired)}});
    ops.push_back({"live", 0, QueryResult({}, now+1h), {}, 1,
                   NofStagedTransfer{std::move(live)}});
    Results results(4);
    Filter(ops, results);
    Check(ops.size() == 2 && ops[0].original_index == 3 && ops[1].original_index == 0,
          "pre-submit filtering changed original key indices");
    Check(!results[2] && results[2].error() == ErrorCode::LEASE_EXPIRED &&
          results[0].has_value() && results[3].has_value(), "filter changed unrelated results");
    Check(expired_owner.expired() && !live_owner.expired(), "staging owner released at wrong time");
    ops.clear();
    Check(live_owner.expired(), "surviving staging owner leaked");
}
}

int main() {
    using namespace mooncake;
    CompletionOrderAndLease();
    FailuresKeepOriginalIndices();
    EarlyCompletionKeepsTimestamp();
    MixedMemoryLeasesUseObservedCompletion();
    PreSubmitLeaseFilter();
}
"""


class BatchGetNofCompletionTest(CppTestCase):
    def test_completion_and_lease_regressions(self):
        source = HARNESS
        for marker, code in extracted_code().items():
            source = source.replace(f"@@{marker}@@", code)
        self.run_case(self.build(source))


if __name__ == "__main__":
    main()
