# Multi-Tenant Deployment

## Configure the Master

### File Connector

Tenant quota admission is disabled by default. Enable strict multi-tenant mode on the master when you want memory writes admitted against connector-managed per-tenant quota:

```bash
mooncake_master \
  --enable_multi_tenants=true \
  --tenant_quota_connector_type=file \
  --tenant_quota_connector_uri=/etc/mooncake/tenant_quotas.yaml
```

### etcd Connector

You can also store the same YAML policy in etcd when Mooncake Store is built with `STORE_USE_ETCD=ON`:

```bash
mooncake_master \
  --enable_multi_tenants=true \
  --cluster_id=mooncake_cluster \
  --tenant_quota_connector_type=etcd \
  --tenant_quota_connector_uri=127.0.0.1:2379
```

The etcd connector stores the policy at `mooncake-store/<cluster_id>/tenant_quota_policy`. If the key does not exist, the master starts with an empty policy so the first tenant policy can be created through the admin API. It shares the process-wide store etcd client used by HA/oplog, so if HA or oplog also uses etcd, `tenant_quota_connector_uri` must match those etcd endpoints.

## Define the Tenant Policy

The policy uses schema version `2`. Version `1` policies remain accepted and are
read as memory-quota-only. Tenant names must be non-empty, unique, must not start
with `_`, and must not contain NUL or control characters; quotas must be positive
integers with optional `B`, `KB`, `MB`, `GB`, or `TB` units:

```yaml
version: 2

tenants:
  - name: tenant-a
    quota: 200GB
    nof_quota: 1TB

  - name: tenant-b
    quota: 500GB
```

Per tenant, `name` is the tenant identifier, `quota` is the memory quota enforced
by the MEMORY tier, and `nof_quota` is the optional NoF (NVMe-oF SSD) quota. Omit
`nof_quota` to leave that tenant's NoF tier tracking-only: NoF writes are admitted
while their usage is still accounted. The value is stored either way but only has
an effect when the master runs a NoF pool (`-DUSE_NOF=ON` with mounted NoF
segments).

When strict multi-tenant mode is enabled, write requests must include a registered tenant. The `default` tenant is not special unless it is explicitly registered in the connector policy.

## Manage Tenant Quotas

The same HTTP port used for metrics exposes the tenant quota admin API. Each snapshot carries the MEMORY tier and, in a `USE_NOF` build, the NoF tier:

```bash
# List tenant quota snapshots
curl -s http://<master_host>:9003/api/v1/tenant_quotas

# Query one tenant
curl -s "http://<master_host>:9003/api/v1/tenant_quotas?tenant_id=tenant-a"

# Upsert an explicit policy. requested_quota_bytes (memory) must be between
# 1 byte and 2^63 - 1 bytes. nof_quota_bytes is optional and must be in
# [0, 2^63 - 1]; 0 leaves the tenant's NoF tier tracking-only.
curl -s -X PUT "http://<master_host>:9003/api/v1/tenant_quotas?tenant_id=tenant-a" \
  -H 'Content-Type: application/json' \
  -d '{"requested_quota_bytes":2147483648,"nof_quota_bytes":107374182400}'

# Delete an explicit policy. The tenant must not own objects or quota usage.
curl -s -X DELETE "http://<master_host>:9003/api/v1/tenant_quotas?tenant_id=tenant-a"
```

Each tenant quota snapshot returns:

```json
{
  "success": true,
  "data": {
    "tenant_id": "tenant-a",
    "requested_quota_bytes": 2147483648,
    "effective_quota_bytes": 2147483648,
    "charged_bytes": 0,
    "admission_closed": false,
    "over_quota": false,
    "has_explicit_policy": true,
    "nof_requested_quota_bytes": 107374182400,
    "nof_effective_quota_bytes": 107374182400,
    "nof_charged_bytes": 0,
    "nof_admission_closed": true,
    "nof_over_quota": false
  }
}
```

`charged_bytes` includes completed MEMORY replicas and in-flight MEMORY allocations. Put, Copy, Move, and promotion charge quota when admission starts; failed, revoked, partially completed, or expired operations refund the unused charge. `admission_closed` is `true` when the account rejects new writes, including after its explicit policy is removed.

In HA mode, quota admin requests are served only by the active master service. Standby, candidate, or inactive services return HTTP 503. If strict multi-tenant mode is disabled, the quota admin API returns HTTP 409 with `UNAVAILABLE_IN_CURRENT_MODE`. Deleting a non-empty tenant returns HTTP 409 with `TENANT_NOT_EMPTY`. The NoF tier is subject to the same rules.

### NoF Tier

The same policy carries a second, independent quota for the NoF (NVMe-oF SSD)
pool. It requires a master built with `-DUSE_NOF=ON` that also runs with
`--enable_multi_tenants=true`:

```bash
mooncake_master \
  --enable_multi_tenants=true \
  --tenant_quota_connector_type=file \
  --tenant_quota_connector_uri=/etc/mooncake/tenant_quotas.yaml \
  --nof_tenant_eviction_high_watermark_ratio=0.9
```

NoF quota is opt-in per tenant. A tenant's NoF tier starts tracking-only: NoF
writes are admitted and their usage is accounted, but nothing is rejected, until
the tenant gets a positive `nof_quota` in the policy file (or `nof_quota_bytes`
through the admin API, where `0` means unconfigured). This keeps deployments
without NoF quotas unchanged.

Once set, NoF admission mirrors the memory tier: a NoF write that would exceed the
tenant's NoF effective quota is rejected with `TENANT_QUOTA_EXCEEDED`, and the
memory pre-charge taken for the same request is rolled back. NoF usage is
reclaimed by two passes:

- pool-wide NoF eviction, controlled by `--nof_eviction_ratio` and
  `--nof_eviction_high_watermark_ratio`, reclaims any tenant's NoF replicas when
  the whole NoF pool is full; and
- a per-tenant NoF watermark pass, controlled by
  `--nof_tenant_eviction_high_watermark_ratio` (default `0.9`, `0` disables it),
  reclaims only the tenants whose own NoF usage crosses that fraction of their
  NoF effective quota, down to the NoF low watermark.

The NoF tier is reported as the `nof_*` fields in the admin snapshot above and as
the `mooncake_tenant_quota_nof_requested_bytes`,
`mooncake_tenant_quota_nof_effective_bytes`, `mooncake_tenant_quota_nof_charged_bytes`,
`mooncake_tenant_quota_nof_admission_closed`, `mooncake_tenant_quota_nof_over_quota`,
and `mooncake_tenant_quota_nof_explicit_policy` metrics.

## SGLang

When Mooncake is used as the HiCache storage backend, set `tenant_id` in the
Mooncake backend configuration:

```bash
--hicache-storage-backend mooncake \
--hicache-storage-backend-extra-config \
  '{"master_server_address":"127.0.0.1:50051","tenant_id":"tenant-a"}'
```

Alternatively, add `tenant_id` to the JSON file selected by
`SGLANG_HICACHE_MOONCAKE_CONFIG_PATH`, or use `MOONCAKE_TENANT_ID` when loading
the Mooncake configuration from environment variables. SGLang forwards the
resolved value to the Mooncake client.

All prefill, decode, and replica instances that should share KV cache entries
must use the same `tenant_id` and compatible model and release namespaces.

## vLLM

Add `tenant_id` to the Mooncake client JSON configuration:

```json
"tenant_id": "tenant-a"
```

Point `MOONCAKE_CONFIG_PATH` at that file and enable
`MooncakeStoreConnector` through `--kv-transfer-config`:

```bash
MOONCAKE_CONFIG_PATH=/path/to/mooncake_config.json \
vllm serve <model> \
  --kv-transfer-config \
  '{"kv_connector":"MooncakeStoreConnector","kv_role":"kv_both"}'
```

`MooncakeStoreConnector` reads the JSON during initialization and passes its
tenant ID to the Mooncake client.

All prefill, decode, and replica instances that should share KV cache entries
must use the same `tenant_id` and compatible model and release namespaces.
