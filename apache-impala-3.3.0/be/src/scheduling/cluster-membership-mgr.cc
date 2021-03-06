// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "scheduling/cluster-membership-mgr.h"

#include "common/logging.h"
#include "common/names.h"
#include "util/metrics.h"
#include "util/test-info.h"

namespace {
using namespace impala;

/// Looks for an executor group with name 'name' in 'executor_groups' and returns it. If
/// the group doesn't exist yet, it creates a new one and inserts it into
/// 'executor_groups'.
ExecutorGroup* FindOrInsertExecutorGroup(const TExecutorGroupDesc& group,
    ClusterMembershipMgr::ExecutorGroups* executor_groups) {
  auto it = executor_groups->find(group.name);
  if (it != executor_groups->end()) {
    DCHECK_EQ(group.name, it->second.name());
    return &it->second;
  }
  bool inserted;
  tie(it, inserted) = executor_groups->emplace(group.name, ExecutorGroup(group));
  DCHECK(inserted);
  return &it->second;
}

}

namespace impala {

static const string LIVE_EXEC_GROUP_KEY("cluster-membership.executor-groups.total");
static const string HEALTHY_EXEC_GROUP_KEY(
    "cluster-membership.executor-groups.total-healthy");
static const string TOTAL_BACKENDS_KEY("cluster-membership.backends.total");

ClusterMembershipMgr::ClusterMembershipMgr(
    string local_backend_id, StatestoreSubscriber* subscriber, MetricGroup* metrics)
  : current_membership_(std::make_shared<const Snapshot>()),
    statestore_subscriber_(subscriber),
    thrift_serializer_(/* compact= */ false),
    local_backend_id_(move(local_backend_id)) {
  DCHECK(metrics != nullptr);
  MetricGroup* metric_grp = metrics->GetOrCreateChildGroup("cluster-membership");
  total_live_executor_groups_ = metric_grp->AddCounter(LIVE_EXEC_GROUP_KEY, 0);
  total_healthy_executor_groups_ = metric_grp->AddCounter(HEALTHY_EXEC_GROUP_KEY, 0);
  total_backends_ = metric_grp->AddCounter(TOTAL_BACKENDS_KEY, 0);
}

Status ClusterMembershipMgr::Init() {
  LOG(INFO) << "Starting cluster membership manager";
  if (statestore_subscriber_ == nullptr) {
    DCHECK(TestInfo::is_test());
    return Status::OK();
  }
  // Register with the statestore
  StatestoreSubscriber::UpdateCallback cb =
      bind<void>(mem_fn(&ClusterMembershipMgr::UpdateMembership), this, _1, _2);
  Status status = statestore_subscriber_->AddTopic(
      Statestore::IMPALA_MEMBERSHIP_TOPIC, /* is_transient=*/ true,
      /* populate_min_subscriber_topic_version=*/ false,
      /* filter_prefix= */"", cb);
  if (!status.ok()) {
    status.AddDetail("Scheduler failed to register membership topic");
    return status;
  }
  return Status::OK();
}

void ClusterMembershipMgr::SetLocalBeDescFn(BackendDescriptorPtrFn fn) {
  lock_guard<mutex> l(callback_fn_lock_);
  DCHECK(fn);
  DCHECK(!local_be_desc_fn_);
  local_be_desc_fn_ = std::move(fn);
}

void ClusterMembershipMgr::SetUpdateLocalServerFn(UpdateLocalServerFn fn) {
  lock_guard<mutex> l(callback_fn_lock_);
  DCHECK(fn);
  DCHECK(!update_local_server_fn_);
  update_local_server_fn_ = std::move(fn);
}

void ClusterMembershipMgr::SetUpdateFrontendFn(UpdateFrontendFn fn) {
  lock_guard<mutex> l(callback_fn_lock_);
  DCHECK(fn);
  DCHECK(!update_frontend_fn_);
  update_frontend_fn_ = std::move(fn);
}

ClusterMembershipMgr::SnapshotPtr ClusterMembershipMgr::GetSnapshot() const {
  lock_guard<mutex> l(current_membership_lock_);
  DCHECK(current_membership_.get() != nullptr);
  SnapshotPtr state = current_membership_;
  return state;
}

void ClusterMembershipMgr::UpdateMembership(
    const StatestoreSubscriber::TopicDeltaMap& incoming_topic_deltas,
    vector<TTopicDelta>* subscriber_topic_updates) {
  lock_guard<mutex> l(update_membership_lock_);

  // First look to see if the topic we're interested in has an update.
  StatestoreSubscriber::TopicDeltaMap::const_iterator topic =
      incoming_topic_deltas.find(Statestore::IMPALA_MEMBERSHIP_TOPIC);

  // Ignore spurious messages.
  if (topic == incoming_topic_deltas.end()) return;
  const TTopicDelta& update = topic->second;

  // If the update transmitted by the statestore is an empty delta, we don't need to
  // process it.
  bool no_ss_update = update.is_delta && update.topic_entries.empty();

  // Check if the local backend is up and needs updating.
  const Snapshot* base_snapshot = recovering_membership_.get();
  if (base_snapshot == nullptr) base_snapshot = current_membership_.get();
  DCHECK(base_snapshot != nullptr);
  BeDescSharedPtr local_be_desc = GetLocalBackendDescriptor();
  bool needs_local_be_update = NeedsLocalBackendUpdate(*base_snapshot, local_be_desc);

  // We consider the statestore to be recovering from a connection failure until its post
  // recovery grace period has elapsed.
  bool ss_is_recovering = statestore_subscriber_ != nullptr
      && statestore_subscriber_->IsInPostRecoveryGracePeriod();

  // If we are tracking a recovering membership but the statestore is out of recovery, we
  // will need to send the current membership to the impala server.
  bool update_local_server = recovering_membership_.get() != nullptr && !ss_is_recovering;

  // Check if there are any executors that can be removed from the blacklist.
  bool needs_blacklist_maintenance = base_snapshot->executor_blacklist.NeedsMaintenance();

  // If there's no statestore update, the local backend descriptor has no changes, we
  // don't need to update the local server, and the blacklist doesn't need to be updated,
  // then we can skip processing altogether and avoid making a copy of the state.
  if (no_ss_update && !needs_local_be_update && !update_local_server
      && !needs_blacklist_maintenance) {
    return;
  }

  if (!no_ss_update) VLOG(1) << "Processing statestore update";
  if (needs_local_be_update) VLOG(1) << "Local backend membership needs update";
  if (update_local_server) VLOG(1) << "Local impala server needs update";
  if (needs_blacklist_maintenance) VLOG(1) << "Removing executors from the blacklist";
  if (ss_is_recovering) {
    VLOG(1) << "Statestore subscriber is in post-recovery grace period";
  }

  // By now we know that we need to renew the snapshot. Construct a new state based on the
  // type of the update we received.
  std::shared_ptr<Snapshot> new_state;

  if (!update.is_delta) {
    VLOG(1) << "Received full membership update";
    // Full topic transmit, create fresh state.
    new_state = std::make_shared<Snapshot>();
    // A full update could remove backends and therefore we need to send an update to the
    // local server.
    update_local_server = true;
  } else {
    VLOG(1) << "Received delta membership update";
    if (recovering_membership_.get() != nullptr) {
      // The recovering membership is never exposed to clients and therefore requires no
      // copying.
      new_state = recovering_membership_;
    } else {
      // Make a copy of the current membership. This is the only function calling SetState
      // and thus no lock is needed for read access.
      new_state = std::make_shared<Snapshot>(*current_membership_);
    }
  }
  if (local_be_desc.get() != nullptr) new_state->local_be_desc = local_be_desc;
  new_state->version += 1;

  // Process removed, new, and updated entries from the topic update and apply the changes
  // to the new backend map and executor groups.
  BackendIdMap* new_backend_map = &(new_state->current_backends);
  ExecutorGroups* new_executor_groups = &(new_state->executor_groups);
  ExecutorBlacklist* new_blacklist = &(new_state->executor_blacklist);
  for (const TTopicItem& item : update.topic_entries) {
    // Deleted item
    if (item.deleted) {
      if (new_backend_map->find(item.key) != new_backend_map->end()) {
        const TBackendDescriptor& be_desc = (*new_backend_map)[item.key];
        bool blacklisted = new_blacklist->FindAndRemove(be_desc)
            == ExecutorBlacklist::State::BLACKLISTED;
        // If the backend was quiescing or was previously blacklisted, it will already
        // have been removed from 'executor_groups'.
        if (be_desc.is_executor && !be_desc.is_quiescing && !blacklisted) {
          for (const auto& group : be_desc.executor_groups) {
            VLOG(1) << "Removing backend " << item.key << " from group " << group
                    << " (deleted)";
            FindOrInsertExecutorGroup(
                group, new_executor_groups)->RemoveExecutor(be_desc);
          }
        }
        new_backend_map->erase(item.key);
        update_local_server = true;
      }
      continue;
    }

    // New or existing item
    TBackendDescriptor be_desc;
    // Benchmarks have suggested that this method can deserialize ~10m messages per
    // second, so no immediate need to consider optimization.
    uint32_t len = item.value.size();
    Status status = DeserializeThriftMsg(
        reinterpret_cast<const uint8_t*>(item.value.data()), &len, false, &be_desc);
    if (!status.ok()) {
      LOG_EVERY_N(WARNING, 30) << "Error deserializing membership topic item with key: "
          << item.key;
      continue;
    }
    if (be_desc.ip_address.empty()) {
      // Each backend resolves its own IP address and transmits it inside its backend
      // descriptor as part of the statestore update. If it is empty, then either that
      // code has been changed, or someone else is sending malformed packets.
      LOG_EVERY_N(WARNING, 30) << "Ignoring subscription request with empty IP address "
          "from subscriber: " << TNetworkAddressToString(be_desc.address);
      continue;
    }
    if (item.key == local_backend_id_) {
      if (local_be_desc.get() == nullptr) {
        LOG_EVERY_N(WARNING, 30) << "Another host registered itself with the local "
            << "backend id (" << item.key << "), but the local backend has not started "
            "yet. The offending address is: " << TNetworkAddressToString(be_desc.address);
      } else if (be_desc.address != local_be_desc->address) {
        // Someone else has registered this subscriber ID with a different address. We
        // will try to re-register (i.e. overwrite their subscription), but there is
        // likely a configuration problem.
        LOG_EVERY_N(WARNING, 30) << "Duplicate subscriber registration from address: "
            << TNetworkAddressToString(be_desc.address) << " (we are: "
            << TNetworkAddressToString(local_be_desc->address) << ", backend id: "
            << item.key << ")";
      }
      // We will always set the local backend explicitly below, so we ignore it here.
      continue;
    }

    auto it = new_backend_map->find(item.key);
    if (it != new_backend_map->end()) {
      // Update
      TBackendDescriptor& existing = it->second;
      bool blacklisted =
          new_blacklist->FindAndRemove(be_desc) == ExecutorBlacklist::State::BLACKLISTED;
      if (be_desc.is_quiescing && !existing.is_quiescing && existing.is_executor
          && !blacklisted) {
        // Executor needs to be removed from its groups
        for (const auto& group : be_desc.executor_groups) {
          VLOG(1) << "Removing backend " << item.key << " from group " << group
                  << " (quiescing)";
          FindOrInsertExecutorGroup(group, new_executor_groups)->RemoveExecutor(be_desc);
        }
      }
      existing = be_desc;
    } else {
      // Create
      new_backend_map->insert(make_pair(item.key, be_desc));
      if (!be_desc.is_quiescing && be_desc.is_executor) {
        for (const auto& group : be_desc.executor_groups) {
          VLOG(1) << "Adding backend " << item.key << " to group " << group;
          FindOrInsertExecutorGroup(group, new_executor_groups)->AddExecutor(be_desc);
        }
      }
      // Since this backend is new, it cannot already be on the blacklist or probation.
      DCHECK_EQ(new_blacklist->FindAndRemove(be_desc),
          ExecutorBlacklist::State::NOT_BLACKLISTED);
    }
    DCHECK(CheckConsistency(*new_backend_map, *new_executor_groups, *new_blacklist));
  }

  if (needs_blacklist_maintenance) {
    // Add any backends that were removed from the blacklist and put on probation back
    // into 'executor_groups'.
    std::list<TBackendDescriptor> probation_list;
    new_blacklist->Maintenance(&probation_list);
    for (const TBackendDescriptor& be_desc : probation_list) {
      for (const auto& group : be_desc.executor_groups) {
        VLOG(1) << "Adding backend " << TNetworkAddressToString(be_desc.address)
                << " to group " << group << " (passed blacklist timeout)";
        FindOrInsertExecutorGroup(group, new_executor_groups)->AddExecutor(be_desc);
      }
    }
    DCHECK(CheckConsistency(*new_backend_map, *new_executor_groups, *new_blacklist));
  }

  // Update the local backend descriptor if required. We need to re-check new_state here
  // in case it was reset to empty above.
  if (NeedsLocalBackendUpdate(*new_state, local_be_desc)) {
    // We need to update both the new membership state and the statestore
    (*new_backend_map)[local_backend_id_] = *local_be_desc;
    for (const auto& group : local_be_desc->executor_groups) {
      if (local_be_desc->is_quiescing) {
        VLOG(1) << "Removing local backend from group " << group;
        FindOrInsertExecutorGroup(
            group, new_executor_groups)->RemoveExecutor(*local_be_desc);
      } else if (local_be_desc->is_executor) {
        VLOG(1) << "Adding local backend to group " << group;
        FindOrInsertExecutorGroup(
            group, new_executor_groups)->AddExecutor(*local_be_desc);
      }
    }
    AddLocalBackendToStatestore(*local_be_desc, subscriber_topic_updates);
    DCHECK(CheckConsistency(*new_backend_map, *new_executor_groups, *new_blacklist));
  }

  UpdateMetrics(*new_backend_map, *new_executor_groups);

  // Don't send updates or update the current membership if the statestore is in its
  // post-recovery grace period.
  if (ss_is_recovering) {
    recovering_membership_ = new_state;
    return;
  }

  // Send notifications to local ImpalaServer and Frontend through registered callbacks.
  if (update_local_server) NotifyLocalServerForDeletedBackend(*new_backend_map);
  UpdateFrontendExecutorMembership(*new_backend_map, *new_executor_groups);

  // Atomically update the respective membership snapshot.
  SetState(new_state);
  recovering_membership_.reset();
}

void ClusterMembershipMgr::BlacklistExecutor(const TBackendDescriptor& be_desc) {
  if (!ExecutorBlacklist::BlacklistingEnabled()) return;
  lock_guard<mutex> l(update_membership_lock_);
  // Don't blacklist the local executor. Some queries may have root fragments that must be
  // scheduled on the coordinator and will always fail if its blacklisted.
  if (be_desc.ip_address == current_membership_->local_be_desc->ip_address
      && be_desc.address.port == current_membership_->local_be_desc->address.port) {
    return;
  }

  bool recovering = recovering_membership_.get() != nullptr;
  const Snapshot* base_snapshot;
  if (recovering) {
    base_snapshot = recovering_membership_.get();
  } else {
    base_snapshot = current_membership_.get();
  }
  DCHECK(base_snapshot != nullptr);
  // Check the Snapshot that we'll be updating to see if the backend is present, to avoid
  // copying the Snapshot if it isn't.
  bool exists = false;
  for (const auto& group : be_desc.executor_groups) {
    auto it = base_snapshot->executor_groups.find(group.name);
    if (it != base_snapshot->executor_groups.end()
        && it->second.LookUpBackendDesc(be_desc.address) != nullptr) {
      exists = true;
      break;
    }
  }
  if (!exists) {
    // This backend does not exist in 'executor_groups', eg. because it was removed by
    // a statestore update before the coordinator decided to blacklist it or because
    // it is quiescing.
    return;
  }

  std::shared_ptr<Snapshot> new_state;
  if (recovering) {
    // If the statestore is currently recovering, we can apply the blacklisting to
    // 'recovering_membership_', which doesn't need to be copied.
    new_state = recovering_membership_;
  } else {
    new_state = std::make_shared<Snapshot>(*current_membership_);
  }
  ExecutorGroups* new_executor_groups = &(new_state->executor_groups);

  for (const auto& group : be_desc.executor_groups) {
    VLOG(1) << "Removing backend " << TNetworkAddressToString(be_desc.address)
            << " from group " << group << " (blacklisted)";
    FindOrInsertExecutorGroup(group, new_executor_groups)->RemoveExecutor(be_desc);
  }

  ExecutorBlacklist* new_blacklist = &(new_state->executor_blacklist);
  new_blacklist->Blacklist(be_desc);

  // We'll call SetState() with 'recovering_membership_' once the statestore is no longer
  // in recovery.
  if (recovering) return;

  // Note that we don't call the update functions here:
  // - The backend update function is used to cancel queries on backends that are no
  //   longer present in 'current_backends', but we don't remove the executor from
  //   'current_backends' here, since it always reflects the full statestore membership.
  //   This avoids cancelling queries that may still be running successfully, eg. if the
  //   backend is still up but got blacklisted due to a flaky network. If the backend
  //   really is down, we'll still cancel the queries when the statestore removes it from
  //   the membership.
  // - The frontend update function is used to notify the planner, but the executors the
  //   planner schedules things on is just a hint and the Scheduler will still see the
  //   updated membership and choose non-blacklisted executors regardless of what the
  //   planner says, so its fine to wait until the next topic update to notify the fe.
  SetState(new_state);
}

void ClusterMembershipMgr::AddLocalBackendToStatestore(
    const TBackendDescriptor& local_be_desc,
    vector<TTopicDelta>* subscriber_topic_updates) {
  VLOG(1) << "Sending local backend to statestore";

  subscriber_topic_updates->emplace_back(TTopicDelta());
  TTopicDelta& update = subscriber_topic_updates->back();
  update.topic_name = Statestore::IMPALA_MEMBERSHIP_TOPIC;
  update.topic_entries.emplace_back(TTopicItem());
  // Setting this flag allows us to pass the resulting topic update to other
  // ClusterMembershipMgr instances in tests unmodified.
  update.is_delta = true;

  TTopicItem& item = update.topic_entries.back();
  item.key = local_backend_id_;
  Status status = thrift_serializer_.SerializeToString(&local_be_desc, &item.value);
  if (!status.ok()) {
    LOG(FATAL) << "Failed to serialize Impala backend descriptor for statestore topic:"
                << " " << status.GetDetail();
    subscriber_topic_updates->pop_back();
    return;
  }
}

ClusterMembershipMgr::BeDescSharedPtr ClusterMembershipMgr::GetLocalBackendDescriptor() {
  lock_guard<mutex> l(callback_fn_lock_);
  return local_be_desc_fn_ ? local_be_desc_fn_() : nullptr;
}

void ClusterMembershipMgr::NotifyLocalServerForDeletedBackend(
    const BackendIdMap& current_backends) {
  VLOG(3) << "Notifying local server of membership changes";
  lock_guard<mutex> l(callback_fn_lock_);
  if (!update_local_server_fn_) return;
  BackendAddressSet current_backend_set;
  for (const auto& it : current_backends) current_backend_set.insert(it.second.address);
  update_local_server_fn_(current_backend_set);
}

void ClusterMembershipMgr::UpdateFrontendExecutorMembership(
    const BackendIdMap& current_backends, const ExecutorGroups executor_groups) {
  lock_guard<mutex> l(callback_fn_lock_);
  if (!update_frontend_fn_) return;
  TUpdateExecutorMembershipRequest update_req;
  for (const auto& it : current_backends) {
    const TBackendDescriptor& backend = it.second;
    if (backend.is_executor) {
      update_req.hostnames.insert(backend.address.hostname);
      update_req.ip_addresses.insert(backend.ip_address);
      update_req.num_executors++;
    }
  }
  Status status = update_frontend_fn_(update_req);
  if (!status.ok()) {
    LOG(WARNING) << "Error updating frontend membership snapshot: " << status.GetDetail();
  }
}

void ClusterMembershipMgr::SetState(const SnapshotPtr& new_state) {
  lock_guard<mutex> l(current_membership_lock_);
  DCHECK(new_state.get() != nullptr);
  current_membership_ = new_state;
}

bool ClusterMembershipMgr::NeedsLocalBackendUpdate(const Snapshot& state,
    const BeDescSharedPtr& local_be_desc) {
  if (local_be_desc.get() == nullptr) return false;
  if (state.local_be_desc.get() == nullptr) return true;
  auto it = state.current_backends.find(local_backend_id_);
  if (it == state.current_backends.end()) return true;
  return it->second.is_quiescing != local_be_desc->is_quiescing;
}

bool ClusterMembershipMgr::CheckConsistency(const BackendIdMap& current_backends,
    const ExecutorGroups& executor_groups, const ExecutorBlacklist& executor_blacklist) {
  // Build a map of all backend descriptors
  std::unordered_map<TNetworkAddress, TBackendDescriptor> address_to_backend;
  for (const auto& it : current_backends) {
    address_to_backend.emplace(it.second.address, it.second);
  }

  // Check groups against the map
  for (const auto& group_it : executor_groups) {
    const string& group_name = group_it.first;
    const ExecutorGroup& group = group_it.second;
    ExecutorGroup::Executors backends = group.GetAllExecutorDescriptors();
    for (const TBackendDescriptor& group_be : backends) {
      if (!group_be.is_executor) {
        LOG(WARNING) << "Backend " << group_be.address << " in group " << group_name
            << " is not an executor";
        return false;
      }
      if (group_be.is_quiescing) {
        LOG(WARNING) << "Backend " << group_be.address << " in group " << group_name
            << " is quiescing";
        return false;
      }
      auto current_be_it = address_to_backend.find(group_be.address);
      if (current_be_it == address_to_backend.end()) {
        LOG(WARNING) << "Backend " << group_be.address << " is in group " << group_name
            << " but not in current set of backends";
        return false;
      }
      if (current_be_it->second.is_quiescing != group_be.is_quiescing) {
        LOG(WARNING) << "Backend " << group_be.address << " in group " << group_name
            << " differs from backend in current set of backends: is_quiescing ("
            << current_be_it->second.is_quiescing << " != " << group_be.is_quiescing
            << ")";
        return false;
      }
      if (current_be_it->second.is_executor != group_be.is_executor) {
        LOG(WARNING) << "Backend " << group_be.address << " in group " << group_name
            << " differs from backend in current set of backends: is_executor ("
            << current_be_it->second.is_executor << " != " << group_be.is_executor << ")";
        return false;
      }
      if (executor_blacklist.IsBlacklisted(group_be)) {
        LOG(WARNING) << "Backend " << group_be.address << " in group " << group_name
                     << " is blacklisted.";
        return false;
      }
    }
  }
  return true;
}

void ClusterMembershipMgr::UpdateMetrics(
    const BackendIdMap& current_backends, const ExecutorGroups& executor_groups) {
  int total_live_executor_groups = 0;
  int total_healthy_executor_groups = 0;
  for (const auto& group_it : executor_groups) {
    const ExecutorGroup& group = group_it.second;
    if (group.IsHealthy()) {
      total_live_executor_groups++;
      total_healthy_executor_groups++;
    } else if (group.NumHosts() > 0) {
      total_live_executor_groups++;
    }
  }
  DCHECK_GE(total_live_executor_groups, total_healthy_executor_groups);
  total_live_executor_groups_->SetValue(total_live_executor_groups);
  total_healthy_executor_groups_->SetValue(total_healthy_executor_groups);
  total_backends_->SetValue(current_backends.size());
}

} // end namespace impala
