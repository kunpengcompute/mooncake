// Copyright 2026 Mooncake Authors
// Licensed under the Apache License, Version 2.0.
#include "transport/rdma_transport/ascend_storage_config.h"
#include <glog/logging.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#if defined(USE_ASCEND_NDR) || defined(USE_ASCEND_STAGING)
#include <acl/acl.h>
#endif
namespace mooncake {
namespace {
std::string ReadSysfsValue(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool InterfaceOwnsIp(const std::string &interface_name,
                     const std::string &configured_ip) {
    struct ifaddrs *addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return false;
    bool found = false;
    for (const struct ifaddrs *it = addresses; it != nullptr;
         it = it->ifa_next) {
        if (it->ifa_addr == nullptr || interface_name != it->ifa_name) continue;
        const int family = it->ifa_addr->sa_family;
        char text[INET6_ADDRSTRLEN] = {};
        const void *raw = nullptr;
        if (family == AF_INET) {
            raw = &reinterpret_cast<const struct sockaddr_in *>(it->ifa_addr)
                       ->sin_addr;
        } else if (family == AF_INET6) {
            raw = &reinterpret_cast<const struct sockaddr_in6 *>(it->ifa_addr)
                       ->sin6_addr;
        }
        if (raw != nullptr &&
            inet_ntop(family, raw, text, sizeof(text)) != nullptr &&
            configured_ip == text) {
            found = true;
            break;
        }
    }
    freeifaddrs(addresses);
    return found;
}

bool GidMatchesIp(const std::string &gid, const std::string &configured_ip) {
    std::array<unsigned char, 16> gid_bytes{};
    if (inet_pton(AF_INET6, gid.c_str(), gid_bytes.data()) != 1) return false;

    std::array<unsigned char, 4> ipv4{};
    if (inet_pton(AF_INET, configured_ip.c_str(), ipv4.data()) == 1) {
        static constexpr std::array<unsigned char, 12> kIpv4MappedPrefix = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        return std::equal(kIpv4MappedPrefix.begin(), kIpv4MappedPrefix.end(),
                          gid_bytes.begin()) &&
               std::equal(ipv4.begin(), ipv4.end(), gid_bytes.begin() + 12);
    }

    std::array<unsigned char, 16> ipv6{};
    return inet_pton(AF_INET6, configured_ip.c_str(), ipv6.data()) == 1 &&
           ipv6 == gid_bytes;
}

int ResolveRdmaRoute(AscendStorageDevice *device) {
    namespace fs = std::filesystem;
    const fs::path device_root =
        fs::path("/sys/class/infiniband") / device->rdma_device;
    std::error_code error;
    if (!fs::is_directory(device_root, error)) {
        LOG(ERROR) << "Configured RDMA device does not exist: "
                   << device->rdma_device;
        return -ENODEV;
    }

    std::set<std::pair<unsigned int, int>> candidates;
    const fs::path ports_root = device_root / "ports";
    for (const auto &port_entry : fs::directory_iterator(ports_root, error)) {
        if (error || !port_entry.is_directory()) continue;
        unsigned long port = 0;
        try {
            port = std::stoul(port_entry.path().filename().string());
        } catch (...) {
            continue;
        }
        if (port == 0 || port > std::numeric_limits<uint8_t>::max() ||
            (device->rdma_port != 0 && device->rdma_port != port)) {
            continue;
        }
        const std::string state = ReadSysfsValue(port_entry.path() / "state");
        if (state.find("ACTIVE") == std::string::npos) continue;

        const fs::path ndev_root = port_entry.path() / "gid_attrs" / "ndevs";
        for (const auto &gid_entry : fs::directory_iterator(ndev_root, error)) {
            if (error || !gid_entry.is_regular_file()) continue;
            int gid_index = -1;
            try {
                gid_index = std::stoi(gid_entry.path().filename().string());
            } catch (...) {
                continue;
            }
            if (gid_index < 0 ||
                (device->gid_index >= 0 && device->gid_index != gid_index)) {
                continue;
            }
            const std::string netdev = ReadSysfsValue(gid_entry.path());
            const std::string gid = ReadSysfsValue(port_entry.path() / "gids" /
                                                   std::to_string(gid_index));
            if (!netdev.empty() && InterfaceOwnsIp(netdev, device->rdma_ip) &&
                GidMatchesIp(gid, device->rdma_ip)) {
                candidates.emplace(static_cast<unsigned int>(port), gid_index);
            }
        }
    }

    if (candidates.size() != 1) {
        LOG(ERROR) << "RDMA device/IP did not resolve to exactly one active "
                      "port/GID: device="
                   << device->rdma_device << ", ip=" << device->rdma_ip
                   << ", candidates=" << candidates.size();
        return candidates.empty() ? -ENOENT : -EEXIST;
    }
    device->rdma_port = static_cast<uint8_t>(candidates.begin()->first);
    device->gid_index = candidates.begin()->second;
    return 0;
}
}  // namespace

int AscendStorageConfig::ParseMode(const Json::Value &root,
                                   AscendTransferMode &mode) {
    mode = AscendTransferMode::Legacy;
    if (!root.isObject()) return -EINVAL;
    if (root.isMember("ascend_transfer")) {
        const auto &value = root["ascend_transfer"];
        if (!value.isObject() || !value["mode"].isString()) return -EINVAL;
        const auto text = value["mode"].asString();
        if (text == "parameter_direct")
            mode = AscendTransferMode::ParameterDirect;
        else if (text == "data_staging")
            mode = AscendTransferMode::DataStaging;
        else
            return -EINVAL;
    } else if (root.isMember("data_plane")) {
        return -EINVAL;
    } else if (root.isMember("parameter_plane")) {
        mode = AscendTransferMode::ParameterDirect;
    }
    if (mode == AscendTransferMode::Legacy) return 0;
    if (!root["protocol"].isString() || root["protocol"].asString() != "rdma")
        return -EINVAL;
    const auto &plane =
        root[mode == AscendTransferMode::DataStaging ? "data_plane"
                                                     : "parameter_plane"];
    if (!plane.isObject() || !plane["schema_version"].isUInt() ||
        plane["schema_version"].asUInt() != 1 || !plane["network"].isString() ||
        plane["network"].asString() != "rdma" ||
        (plane.isMember("strict_route") && !plane["strict_route"].isBool()) ||
        !plane.get("strict_route", true).asBool() ||
        !plane["devices"].isObject())
        return -EINVAL;
    if (mode == AscendTransferMode::ParameterDirect &&
        (!plane["memory_registration"].isString() ||
         plane["memory_registration"].asString() != "ub"))
        return -EINVAL;
    return 0;
}

int AscendStorageConfig::ParseDevice(const Json::Value &root, int physical_id,
                                     AscendTransferMode mode,
                                     AscendStorageDevice &device) {
    AscendTransferMode parsed;
    if (ParseMode(root, parsed) != 0 || parsed != mode ||
        mode == AscendTransferMode::Legacy || physical_id < 0)
        return -EINVAL;
    const auto &plane =
        root[mode == AscendTransferMode::DataStaging ? "data_plane"
                                                     : "parameter_plane"];
    const auto &entry = plane["devices"][std::to_string(physical_id)];
    if (!entry.isObject() || !entry["rdma_device"].isString() ||
        !entry["rdma_ip"].isString())
        return -EINVAL;
    AscendStorageDevice selected;
    selected.physical_device_id = physical_id;
    selected.rdma_device = entry["rdma_device"].asString();
    selected.rdma_ip = entry["rdma_ip"].asString();
    if (selected.rdma_device.empty() ||
        selected.rdma_device.find_first_of("/, ") != std::string::npos)
        return -EINVAL;
    std::array<unsigned char, 16> ip{};
    if (inet_pton(AF_INET, selected.rdma_ip.c_str(), ip.data()) != 1 &&
        inet_pton(AF_INET6, selected.rdma_ip.c_str(), ip.data()) != 1)
        return -EINVAL;
    if (entry.isMember("rdma_port")) {
        if (!entry["rdma_port"].isUInt() || entry["rdma_port"].asUInt() == 0 ||
            entry["rdma_port"].asUInt() > 255)
            return -EINVAL;
        selected.rdma_port = static_cast<uint8_t>(entry["rdma_port"].asUInt());
    }
    if (entry.isMember("gid_index")) {
        if (!entry["gid_index"].isInt() || entry["gid_index"].asInt() < 0)
            return -EINVAL;
        selected.gid_index = entry["gid_index"].asInt();
    }
    if (root.isMember("device_name") &&
        (!root["device_name"].isString() ||
         (!root["device_name"].asString().empty() &&
          root["device_name"].asString() != selected.rdma_device)))
        return -EINVAL;
    device = std::move(selected);
    return 0;
}

AscendStorageConfig &AscendStorageConfig::GetInstance() {
    static AscendStorageConfig config;
    return config;
}
int AscendStorageConfig::InitializeFromEnvironment() {
    std::call_once(once_, [this] { result_ = Initialize(); });
    return result_;
}
int AscendStorageConfig::Initialize() {
    const char *path = std::getenv("MOONCAKE_CONFIG_PATH");
    if (!path || !*path) return 0;
    std::ifstream input(path);
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!input || !Json::parseFromStream(builder, input, &root, &errors)) {
        LOG(ERROR) << "Cannot read Mooncake config: " << path << " " << errors;
        return -EINVAL;
    }
    AscendTransferMode mode;
    if (ParseMode(root, mode) != 0) {
        LOG(ERROR) << "Invalid ascend_transfer/active plane configuration";
        return -EINVAL;
    }
    if (mode == AscendTransferMode::Legacy) return 0;
#ifndef USE_ASCEND_STAGING
    if (mode == AscendTransferMode::DataStaging) {
        LOG(ERROR) << "data_staging requires USE_ASCEND_STAGING";
        return -ENOTSUP;
    }
#endif
#ifndef USE_ASCEND_NDR
    if (mode == AscendTransferMode::ParameterDirect) return -ENOTSUP;
#endif
#if defined(USE_ASCEND_NDR) || defined(USE_ASCEND_STAGING)
    int logical = -1, physical = -1;
    if (aclrtGetDevice(&logical) != ACL_SUCCESS || logical < 0 ||
        aclrtGetPhyDevIdByLogicDevId(logical, &physical) != ACL_SUCCESS ||
        physical < 0)
        return -ENODEV;
    AscendStorageDevice selected;
    int rc = ParseDevice(root, physical, mode, selected);
    if (rc != 0) return rc;
    selected.logical_device_id = logical;
    rc = ResolveRdmaRoute(&selected);
    if (rc != 0) return rc;
    device_ = std::move(selected);
    mode_ = mode;
    LOG(INFO) << "Store route: mode="
              << (mode == AscendTransferMode::DataStaging ? "data_staging"
                                                          : "parameter_direct")
              << " pid=" << getpid() << " logical_npu=" << logical
              << " physical_npu=" << physical
              << " device=" << device_.rdma_device << " ip=" << device_.rdma_ip
              << " port=" << unsigned(device_.rdma_port)
              << " gid=" << device_.gid_index;
    return 0;
#else
    return -ENOTSUP;
#endif
}
}  // namespace mooncake
