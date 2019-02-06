// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fuchsia/overnet/protocol/cpp/fidl.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "garnet/lib/overnet/environment/timer.h"
#include "garnet/lib/overnet/environment/trace.h"
#include "garnet/lib/overnet/labels/node_id.h"
#include "garnet/lib/overnet/vocabulary/bandwidth.h"
#include "garnet/lib/overnet/vocabulary/internal_list.h"
#include "garnet/lib/overnet/vocabulary/optional.h"
#include "garnet/lib/overnet/vocabulary/slice.h"
#include "lib/fidl/cpp/clone.h"

#include <iostream>

namespace overnet {
namespace routing_table_impl {

struct FullLinkLabel {
  NodeId from;
  NodeId to;
  uint64_t link_label;
};

inline bool operator==(const FullLinkLabel& a, const FullLinkLabel& b) {
  return a.from == b.from && a.to == b.to && a.link_label == b.link_label;
}

}  // namespace routing_table_impl
}  // namespace overnet

namespace std {
template <>
struct hash<overnet::routing_table_impl::FullLinkLabel> {
  size_t operator()(
      const overnet::routing_table_impl::FullLinkLabel& id) const {
    return id.from.Hash() ^ id.to.Hash() ^ id.link_label;
  }
};
}  // namespace std

namespace overnet {

class RoutingTable {
 public:
  RoutingTable(NodeId root_node, Timer* timer, bool allow_threading);
  ~RoutingTable();
  RoutingTable(const RoutingTable&) = delete;
  RoutingTable& operator=(const RoutingTable&) = delete;

  static constexpr TimeDelta EntryExpiry() { return TimeDelta::FromMinutes(5); }

  struct SelectedLink {
    uint64_t link_id;
    uint32_t route_mss;

    bool operator==(SelectedLink other) const {
      return link_id == other.link_id && route_mss == other.route_mss;
    }
  };
  using SelectedLinks = std::unordered_map<NodeId, SelectedLink>;

  void ProcessUpdate(
      std::vector<fuchsia::overnet::protocol::NodeMetrics> node_metrics,
      std::vector<fuchsia::overnet::protocol::LinkMetrics> link_metrics,
      bool flush_old_nodes);

  // Returns true if this update concludes any changes begun by all prior
  // Update() calls.
  template <class F>
  bool PollLinkUpdates(F f) {
    if (!mu_.try_lock()) {
      return false;
    }
    if (selected_links_version_ != published_links_version_) {
      published_links_version_ = selected_links_version_;
      f(selected_links_);
    }
    const bool done = !processing_changes_.has_value();
    mu_.unlock();
    return done;
  }

  void BlockUntilNoBackgroundUpdatesProcessing() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this]() -> bool { return !processing_changes_; });
  }

  uint64_t gossip_version() const {
    std::lock_guard<std::mutex> lock(shared_table_mu_);
    return gossip_version_;
  }

  struct Update {
    uint64_t version;
    fuchsia::overnet::protocol::RoutingTableUpdate data;
  };

  Update GenerateUpdate(Optional<NodeId> exclude_node) const;

  template <class F>
  void ForEachNodeMetric(F visitor) const {
    std::vector<fuchsia::overnet::protocol::NodeMetrics> nodes_copy;
    {
      std::lock_guard lock{shared_table_mu_};
      for (const auto& m : shared_node_metrics_) {
        nodes_copy.emplace_back(fidl::Clone(m));
      }
    }
    for (const auto& m : nodes_copy) {
      visitor(m);
    }
  }

  Status ValidateIncomingUpdate(
      const std::vector<fuchsia::overnet::protocol::NodeMetrics>& nodes,
      const std::vector<fuchsia::overnet::protocol::LinkMetrics>& links) const;

 private:
  const NodeId root_node_;
  Timer* const timer_;

  struct Metrics {
    std::vector<fuchsia::overnet::protocol::NodeMetrics> node_metrics;
    std::vector<fuchsia::overnet::protocol::LinkMetrics> link_metrics;
    bool Empty() const { return node_metrics.empty() && link_metrics.empty(); }
    void Clear() {
      node_metrics.clear();
      link_metrics.clear();
    }
  };
  Metrics change_log_;
  const bool allow_threading_;
  bool flush_requested_ = false;

  void ApplyChanges(TimeStamp now, const Metrics& changes, bool flush);
  SelectedLinks BuildForwardingTable();

  TimeStamp last_update_{TimeStamp::Epoch()};
  uint64_t path_finding_run_ = 0;

  std::mutex mu_;
  std::condition_variable cv_;
  Optional<std::thread> processing_changes_;

  struct Node;

  struct Link {
    Link(TimeStamp now, fuchsia::overnet::protocol::LinkMetrics initial_metrics,
         Node* to)
        : metrics(std::move(initial_metrics)), last_updated(now), to_node(to) {}
    fuchsia::overnet::protocol::LinkMetrics metrics;
    TimeStamp last_updated;
    InternalListNode<Link> outgoing_link;
    Node* const to_node;
  };

  struct Node {
    Node(TimeStamp now, fuchsia::overnet::protocol::NodeMetrics initial_metrics)
        : metrics(std::move(initial_metrics)), last_updated(now) {}
    fuchsia::overnet::protocol::NodeMetrics metrics;
    TimeStamp last_updated;
    InternalList<Link, &Link::outgoing_link> outgoing_links;

    // Path finding temporary state.
    uint64_t last_path_finding_run = 0;
    TimeDelta best_rtt{TimeDelta::Zero()};
    Node* best_from;
    Link* best_link;
    uint32_t mss;
    bool queued = false;
    InternalListNode<Node> path_finding_node;
  };

  void RemoveOutgoingLinks(Node& node);

  std::unordered_map<NodeId, Node> node_metrics_;
  std::unordered_map<routing_table_impl::FullLinkLabel, Link> link_metrics_;

  mutable std::mutex shared_table_mu_;
  uint64_t gossip_version_ = 0;
  std::vector<fuchsia::overnet::protocol::NodeMetrics> shared_node_metrics_;
  std::vector<fuchsia::overnet::protocol::LinkMetrics> shared_link_metrics_;

  uint64_t selected_links_version_ = 0;
  SelectedLinks selected_links_;
  uint64_t published_links_version_ = 0;
};

}  // namespace overnet
