// Copyright 2026 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MULTI_TRANSPORT_LOCALITY_H
#define MULTI_TRANSPORT_LOCALITY_H

#include <cctype>
#include <string>

namespace mooncake {

// Extract the host portion of a segment name. Segment names carry an optional
// ":port" suffix, and the host may be an IPv4 address, a hostname, or an IPv6
// literal. IPv6 literals contain multiple colons, so a naive rfind(':') would
// corrupt them; the following forms are handled explicitly:
//   "10.0.0.1:8000"      -> "10.0.0.1"      (IPv4 / hostname with port)
//   "node-a"             -> "node-a"        (no port)
//   "[2001:db8::1]:8000" -> "2001:db8::1"   (bracketed IPv6 with port)
//   "[2001:db8::1]"      -> "2001:db8::1"   (bracketed IPv6, no port)
//   "2001:db8::1"        -> "2001:db8::1"   (bare IPv6, no port)
inline std::string segmentHost(const std::string& segment_name) {
    if (!segment_name.empty() && segment_name.front() == '[') {
        // Bracketed IPv6 literal: strip the brackets and ignore any ":port".
        auto close = segment_name.find(']');
        if (close != std::string::npos) {
            return segment_name.substr(1, close - 1);
        }
        return segment_name;  // Malformed; return as-is.
    }
    auto first = segment_name.find(':');
    if (first == std::string::npos) {
        return segment_name;  // No port and no colon.
    }
    if (first != segment_name.rfind(':')) {
        // More than one colon and not bracketed: a bare IPv6 literal without a
        // port (an IPv6 host with a port must use brackets). Treat it all as
        // the host.
        return segment_name;
    }
    return segment_name.substr(0, first);  // "host:port".
}

// Case-insensitive equality. Hostnames are case-insensitive per DNS, and IPv6
// hex literals may differ only in letter case, so a plain "==" would spuriously
// classify the same host as remote and lose the intra-node hip fast path.
inline bool hostEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Same-host IPC transports (HIP/MUSA GPU IPC and POSIX SHM) only work between
// processes on the same physical host. A cross-host target must fall back to
// RDMA/TCP. Two engines co-located on one host share the host portion of their
// segment name (they differ only in port).
inline bool isLocalIpcReachableTarget(const std::string& target_segment_name,
                                      const std::string& local_server_name) {
    return hostEquals(segmentHost(target_segment_name),
                      segmentHost(local_server_name));
}

inline bool isGpuIpcReachableTarget(const std::string& target_segment_name,
                                    const std::string& local_server_name) {
    return isLocalIpcReachableTarget(target_segment_name, local_server_name);
}

// Trim ASCII whitespace from both ends. Rack ids reach us from an environment
// variable or a YAML field, where a stray trailing space is easy to introduce
// and would otherwise silently turn a same-rack pair into a cross-rack one.
inline std::string trimRackId(const std::string& rack_id) {
    auto first = rack_id.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = rack_id.find_last_not_of(" \t\r\n");
    return rack_id.substr(first, last - first + 1);
}

// Whether a cross-node NVLink (MNNVL fabric) transport can reach the target.
//
// Unlike GPU IPC, MNNVL spans every node of one NVLink domain, so the gate is
// the domain boundary rather than the host boundary. That boundary is physical
// and software cannot cross it: a fabric handle exported on one rack cannot be
// imported on another, and the import failure surfaces as a transfer error
// rather than a degradation. The decision is therefore made before choosing the
// transport, not after a failed import.
//
// `Unknown` is deliberately distinct from `Unreachable`: an unset rack id means
// the deployment has not told us anything, which is not the same as having told
// us the racks differ. Callers downgrade on both, but only `Unreachable` is a
// statement about the topology and warrants an error when no fallback exists.
enum class RackReachability { Reachable, Unreachable, Unknown };

inline RackReachability rackReachabilityForNvlink(
    const std::string& target_segment_name,
    const std::string& local_server_name, const std::string& target_rack_id,
    const std::string& local_rack_id) {
    // Same host is always inside the local NVLink domain, whatever the rack
    // ids say. Checking this first keeps single-node deployments -- which have
    // no reason to configure a rack id -- working untouched.
    if (isLocalIpcReachableTarget(target_segment_name, local_server_name)) {
        return RackReachability::Reachable;
    }
    const std::string local = trimRackId(local_rack_id);
    const std::string target = trimRackId(target_rack_id);
    if (local.empty() || target.empty()) {
        return RackReachability::Unknown;
    }
    // Rack ids are operator-assigned labels compared for exact equality. They
    // are not hostnames, so the case-insensitive comparison used for segment
    // names does not apply: "Rack0" and "rack0" may well be two racks.
    return local == target ? RackReachability::Reachable
                           : RackReachability::Unreachable;
}

// Compatibility name retained for existing callers/tests.
inline bool isHipReachableTarget(const std::string& target_segment_name,
                                 const std::string& local_server_name) {
    return isGpuIpcReachableTarget(target_segment_name, local_server_name);
}

}  // namespace mooncake

#endif  // MULTI_TRANSPORT_LOCALITY_H
