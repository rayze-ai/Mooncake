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

#include <gtest/gtest.h>

#include <string>

#include "multi_transport_locality.h"

using namespace mooncake;

TEST(MultiTransportLocalityTest, SegmentHostStripsPort) {
    EXPECT_EQ(segmentHost("10.0.0.1:12345"), "10.0.0.1");
    EXPECT_EQ(segmentHost("node-a:8000"), "node-a");
    // No port: whole string is the host.
    EXPECT_EQ(segmentHost("node-a"), "node-a");
    EXPECT_EQ(segmentHost(""), "");
}

TEST(MultiTransportLocalityTest, HipReachableForSameHostDifferentPort) {
    // Two engines co-located on one host share the host, differ only in port.
    EXPECT_TRUE(isHipReachableTarget("10.0.0.1:20000", "10.0.0.1:20001"));
    EXPECT_TRUE(isHipReachableTarget("node-a:8000", "node-a:9000"));
}

TEST(MultiTransportLocalityTest, HipReachableForIdenticalName) {
    EXPECT_TRUE(isHipReachableTarget("node-a:8000", "node-a:8000"));
}

TEST(MultiTransportLocalityTest, HipNotReachableForRemoteHost) {
    // Cross-host target: hip (GPU IPC) is not usable, must fall back to rdma.
    EXPECT_FALSE(isHipReachableTarget("10.0.0.2:20000", "10.0.0.1:20000"));
    EXPECT_FALSE(isHipReachableTarget("node-b:8000", "node-a:8000"));
}

TEST(MultiTransportLocalityTest, HandlesMissingPort) {
    // Locality decision still works when one side has no explicit port.
    EXPECT_TRUE(isHipReachableTarget("node-a", "node-a:8000"));
    EXPECT_FALSE(isHipReachableTarget("node-b", "node-a:8000"));
}

TEST(MultiTransportLocalityTest, SegmentHostParsesIPv6) {
    // Bracketed IPv6 with and without a port.
    EXPECT_EQ(segmentHost("[2001:db8::1]:8000"), "2001:db8::1");
    EXPECT_EQ(segmentHost("[2001:db8::1]"), "2001:db8::1");
    EXPECT_EQ(segmentHost("[::1]:20000"), "::1");
    // Bare IPv6 literal without a port: the whole string is the host.
    EXPECT_EQ(segmentHost("2001:db8::1"), "2001:db8::1");
    EXPECT_EQ(segmentHost("::1"), "::1");
}

TEST(MultiTransportLocalityTest, HipReachableForIPv6) {
    // Same IPv6 host, different ports (bracketed) -> intra-node hip.
    EXPECT_TRUE(
        isHipReachableTarget("[2001:db8::1]:20000", "[2001:db8::1]:20001"));
    // Bracketed-with-port vs bare literal for the same host must still match.
    EXPECT_TRUE(isHipReachableTarget("[2001:db8::1]:20000", "2001:db8::1"));
    // Different IPv6 hosts -> cross-node, fall back to rdma.
    EXPECT_FALSE(
        isHipReachableTarget("[2001:db8::1]:20000", "[2001:db8::2]:20000"));
}

TEST(MultiTransportLocalityTest, HostMatchIsCaseInsensitive) {
    // Hostnames and IPv6 hex literals are case-insensitive.
    EXPECT_TRUE(isHipReachableTarget("Node-A:8000", "node-a:9000"));
    EXPECT_TRUE(
        isHipReachableTarget("[2001:DB8::1]:8000", "[2001:db8::1]:9000"));
}

// --- rack reachability for the nvlink fabric path ---

TEST(MultiTransportLocalityTest, RackReachableWhenRacksMatch) {
    EXPECT_EQ(rackReachabilityForNvlink("node-b:8000", "node-a:8000", "rack0",
                                        "rack0"),
              RackReachability::Reachable);
}

TEST(MultiTransportLocalityTest, RackUnreachableWhenRacksDiffer) {
    EXPECT_EQ(rackReachabilityForNvlink("node-b:8000", "node-a:8000", "rack1",
                                        "rack0"),
              RackReachability::Unreachable);
}

TEST(MultiTransportLocalityTest, RackUnknownWhenEitherSideUnset) {
    // Unset is not a statement that the racks differ, so it must not be
    // reported as Unreachable: the caller lets the request through.
    EXPECT_EQ(
        rackReachabilityForNvlink("node-b:8000", "node-a:8000", "", "rack0"),
        RackReachability::Unknown);
    EXPECT_EQ(
        rackReachabilityForNvlink("node-b:8000", "node-a:8000", "rack1", ""),
        RackReachability::Unknown);
    EXPECT_EQ(rackReachabilityForNvlink("node-b:8000", "node-a:8000", "", ""),
              RackReachability::Unknown);
}

TEST(MultiTransportLocalityTest, SameHostIsReachableRegardlessOfRack) {
    // A same-host target needs no fabric import at all, so a mismatched or
    // missing rack id must not block it. This keeps single-node deployments
    // working without any rack configuration.
    EXPECT_EQ(rackReachabilityForNvlink("node-a:9000", "node-a:8000", "rack1",
                                        "rack0"),
              RackReachability::Reachable);
    EXPECT_EQ(
        rackReachabilityForNvlink("node-a:9000", "node-a:8000", "", "rack0"),
        RackReachability::Reachable);
    EXPECT_EQ(rackReachabilityForNvlink("node-a:9000", "node-a:8000", "", ""),
              RackReachability::Reachable);
}

TEST(MultiTransportLocalityTest, RackIdsAreTrimmed) {
    // A YAML env picks up stray whitespace easily; it must not turn two nodes
    // in the same rack into a cross-rack pair.
    EXPECT_EQ(rackReachabilityForNvlink("node-b:8000", "node-a:8000",
                                        " rack0 ", "rack0\n"),
              RackReachability::Reachable);
    // Whitespace-only is indistinguishable from unset.
    EXPECT_EQ(rackReachabilityForNvlink("node-b:8000", "node-a:8000", "  ",
                                        "rack0"),
              RackReachability::Unknown);
}

TEST(MultiTransportLocalityTest, RackIdComparisonIsCaseSensitive) {
    // Unlike hostnames, rack ids are opaque operator labels: "Rack0" and
    // "rack0" may well be two different racks, so they are not folded.
    EXPECT_EQ(rackReachabilityForNvlink("node-b:8000", "node-a:8000", "Rack0",
                                        "rack0"),
              RackReachability::Unreachable);
}

TEST(MultiTransportLocalityTest, TrimRackIdHandlesEdgeCases) {
    EXPECT_EQ(trimRackId(""), "");
    EXPECT_EQ(trimRackId("   "), "");
    EXPECT_EQ(trimRackId("\t\r\n"), "");
    EXPECT_EQ(trimRackId("rack0"), "rack0");
    EXPECT_EQ(trimRackId("  rack-0  "), "rack-0");
    // Interior whitespace is part of the label, not padding.
    EXPECT_EQ(trimRackId(" rack 0 "), "rack 0");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
