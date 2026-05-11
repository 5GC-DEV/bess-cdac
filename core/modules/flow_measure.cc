/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2021 Open Networking Foundation
 */

#include "flow_measure.h"

#include <rte_errno.h>
#include <rte_jhash.h>

#include "../core/utils/common.h"

/*----------------------------------------------------------------------------------*/
const Commands FlowMeasure::cmds = {
    {"read", "FlowMeasureCommandReadArg",
     MODULE_CMD_FUNC(&FlowMeasure::CommandReadStats), Command::THREAD_SAFE},
    {"flip", "FlowMeasureCommandFlipArg",
     MODULE_CMD_FUNC(&FlowMeasure::CommandFlipFlag), Command::THREAD_SAFE},
};
/*----------------------------------------------------------------------------------*/
CommandResponse FlowMeasure::Init(const bess::pb::FlowMeasureArg &arg) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  LOG(INFO) << name() << ": Init() called."
            << " leader=" << arg.leader()
            << " flag_attr_name=" << arg.flag_attr_name()
            << " entries=" << arg.entries();

  // Leader module decides which buffer side to use.
  if (arg.leader()) {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    leader_ = true;
    buffer_flag_attr_id_ = AddMetadataAttr(
        arg.flag_attr_name(), sizeof(uint64_t), AccessMode::kWrite);
    current_flag_value_ = Flag::FLAG_VALUE_A;
    LOG(INFO) << name() << ": initialized as LEADER."
              << " initial flag=FLAG_VALUE_A"
              << " buffer_flag_attr_id=" << buffer_flag_attr_id_;
  } else {
    leader_ = false;
    buffer_flag_attr_id_ = AddMetadataAttr(arg.flag_attr_name(),
                                           sizeof(uint64_t), AccessMode::kRead);
    LOG(INFO) << name() << ": initialized as FOLLOWER."
              << " buffer_flag_attr_id=" << buffer_flag_attr_id_;
  }

  if (buffer_flag_attr_id_ < 0) {
    LOG(ERROR) << name() << ": failed to add flag metadata attr '"
               << arg.flag_attr_name()
               << "': id=" << buffer_flag_attr_id_;
    return CommandFailure(EINVAL, "invalid flag attribute name");
  }

  ts_attr_id_ =
      AddMetadataAttr("timestamp", sizeof(uint64_t), AccessMode::kRead);
  if (ts_attr_id_ < 0) {
    LOG(ERROR) << name() << ": failed to add 'timestamp' metadata attr:"
               << " id=" << ts_attr_id_;
    return CommandFailure(EINVAL, "invalid metadata declaration");
  }
  LOG(INFO) << name() << ": ts_attr_id=" << ts_attr_id_;

  fseid_attr_id_ =
      AddMetadataAttr("fseid", sizeof(uint64_t), AccessMode::kRead);
  if (fseid_attr_id_ < 0) {
    LOG(ERROR) << name() << ": failed to add 'fseid' metadata attr:"
               << " id=" << fseid_attr_id_;
    return CommandFailure(EINVAL, "invalid metadata declaration");
  }
  LOG(INFO) << name() << ": fseid_attr_id=" << fseid_attr_id_;

  pdr_attr_id_ =
      AddMetadataAttr("pdr_id", sizeof(uint32_t), AccessMode::kRead);
  if (pdr_attr_id_ < 0) {
    LOG(ERROR) << name() << ": failed to add 'pdr_id' metadata attr:"
               << " id=" << pdr_attr_id_;
    return CommandFailure(EINVAL, "invalid metadata declaration");
  }
  LOG(INFO) << name() << ": pdr_attr_id=" << pdr_attr_id_;

  rte_hash_parameters hash_params = {};
  hash_params.entries = kDefaultNumEntries;
  hash_params.key_len = sizeof(TableKey);
  hash_params.hash_func = rte_jhash;
  hash_params.socket_id = static_cast<int>(rte_socket_id());
  // NOTE: RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY allows concurrent reads but does
  // NOT guarantee snapshot isolation during rte_hash_iterate. In af_packet
  // mode, ProcessBatch (worker) and CommandReadStats (gRPC/control thread) run
  // truly concurrently. We rely on per-entry SessionStats::mutex to protect
  // individual stat fields, and defensive bounds checking to guard the index.
  hash_params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY;

  if (arg.entries()) {
    hash_params.entries = arg.entries();
  }

  LOG(INFO) << name() << ": hash table params:"
            << " entries=" << hash_params.entries
            << " key_len=" << hash_params.key_len
            << " socket_id=" << hash_params.socket_id;

  // Create hash table A.
  std::string name_a = name() + "Ta" + std::to_string(hash_params.socket_id);
  LOG(INFO) << name() << ": creating hash table A"
            << " name='" << name_a << "'"
            << " len=" << name_a.length();
  if (name_a.length() > 26 /*RTE_HASH_NAMESIZE - 1*/) {
    LOG(ERROR) << name() << ": hash table A name too long: " << name_a;
    return CommandFailure(EINVAL, "invalid hash name A");
  }
  hash_params.name = name_a.c_str();
  table_a_ = rte_hash_create(&hash_params);
  if (!table_a_) {
    LOG(ERROR) << name() << ": rte_hash_create failed for table A"
               << " rte_errno=" << rte_errno
               << " (" << rte_strerror(rte_errno) << ")";
    return CommandFailure(rte_errno, "could not create hashmap A");
  }
  LOG(INFO) << name() << ": hash table A created successfully.";

  // Create hash table B.
  std::string name_b = name() + "Tb" + std::to_string(hash_params.socket_id);
  LOG(INFO) << name() << ": creating hash table B"
            << " name='" << name_b << "'"
            << " len=" << name_b.length();
  if (name_b.length() > 26 /*RTE_HASH_NAMESIZE - 1*/) {
    LOG(ERROR) << name() << ": hash table B name too long: " << name_b;
    return CommandFailure(EINVAL, "invalid hash name B");
  }
  hash_params.name = name_b.c_str();
  table_b_ = rte_hash_create(&hash_params);
  if (!table_b_) {
    LOG(ERROR) << name() << ": rte_hash_create failed for table B"
               << " rte_errno=" << rte_errno
               << " (" << rte_strerror(rte_errno) << ")";
    return CommandFailure(rte_errno, "could not create hashmap B");
  }
  LOG(INFO) << name() << ": hash table B created successfully.";

  // Allocate backing data vectors.
  // resize() would require a copyable object, so use swap instead.
  std::vector<SessionStats> tmp_a(hash_params.entries);
  std::vector<SessionStats> tmp_b(hash_params.entries);
  table_data_a_.swap(tmp_a);
  table_data_b_.swap(tmp_b);

  LOG(INFO) << name() << ": data vectors allocated."
            << " table_data_a_.size()=" << table_data_a_.size()
            << " table_data_b_.size()=" << table_data_b_.size();

  VLOG(1) << name() << ": Init() complete.";
  return CommandSuccess();
}

/*----------------------------------------------------------------------------------*/
void FlowMeasure::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  uint64_t now_ns = tsc_to_ns(rdtsc());

  VLOG(2) << name() << ": ProcessBatch() cnt=" << batch->cnt()
          << " now_ns=" << now_ns;

  for (int i = 0; i < batch->cnt(); ++i) {
    Flag cached_current_flag;

    if (leader_) {
      const std::lock_guard<std::mutex> lock(flag_mutex_);
      set_attr<uint64_t>(this, buffer_flag_attr_id_, batch->pkts()[i],
                         static_cast<uint64_t>(current_flag_value_));
      cached_current_flag = current_flag_value_;
    } else {
      const std::lock_guard<std::mutex> lock(flag_mutex_);
      uint64_t flag =
          get_attr<uint64_t>(this, buffer_flag_attr_id_, batch->pkts()[i]);
      if (!Flag_IsValid(flag)) {
        LOG_EVERY_N(WARNING, 100'001)
            << name() << ": encountered invalid flag=" << flag
            << " in packet " << i << " — skipping.";
        continue;
      }
      current_flag_value_ = static_cast<Flag>(flag);
      cached_current_flag = current_flag_value_;
    }

    uint64_t ts_ns =
        get_attr<uint64_t>(this, ts_attr_id_, batch->pkts()[i]);
    uint64_t fseid =
        get_attr<uint64_t>(this, fseid_attr_id_, batch->pkts()[i]);
    uint32_t pdr =
        get_attr<uint32_t>(this, pdr_attr_id_, batch->pkts()[i]);

    // Guard against unset (zero) or future timestamps, and absurdly large
    // latency values (> 10 s almost certainly means the metadata was never
    // written, e.g. the Timestamp module is not in the pipeline for this path).
    if (ts_ns == 0) {
      VLOG(2) << name() << ": pkt[" << i << "] ts_ns=0 — skipping"
              << " (Timestamp module may be missing from pipeline).";
      continue;
    }
    if (now_ns < ts_ns) {
      LOG_EVERY_N(WARNING, 100'001)
          << name() << ": pkt[" << i << "] ts_ns=" << ts_ns
          << " > now_ns=" << now_ns << " — clock skew or unset metadata,"
          << " skipping.";
      continue;
    }
    if ((now_ns - ts_ns) > 10ULL * 1'000'000'000ULL) {
      LOG_EVERY_N(WARNING, 100'001)
          << name() << ": pkt[" << i << "] latency="
          << (now_ns - ts_ns) << " ns > 10 s — garbage timestamp, skipping.";
      continue;
    }

    // Select the active buffer side.
    rte_hash *current_hash = nullptr;
    std::vector<SessionStats> *current_data = nullptr;
    switch (cached_current_flag) {
      case Flag::FLAG_VALUE_A:
        current_hash = table_a_;
        current_data = &table_data_a_;
        break;
      case Flag::FLAG_VALUE_B:
        current_hash = table_b_;
        current_data = &table_data_b_;
        break;
      default:
        LOG_EVERY_N(ERROR, 100'001)
            << name() << ": unknown flag value="
            << Flag_Name(cached_current_flag) << " — skipping packet.";
        continue;
    }

    // Find or create the session entry.
    TableKey key(fseid, pdr);
    int32_t ret = rte_hash_lookup(current_hash, &key);
    if (ret == -ENOENT) {
      ret = rte_hash_add_key(current_hash, &key);
      VLOG(1) << name() << ": new session"
              << " fseid=" << fseid << " pdr=" << pdr
              << " assigned index=" << ret;
    }
    if (ret < 0) {
      LOG_EVERY_N(ERROR, 1'001)
          << name() << ": failed to lookup/insert session"
          << " fseid=" << fseid << " pdr=" << pdr
          << " ret=" << ret << " (" << rte_strerror(-ret) << ").";
      continue;
    }
    if (static_cast<size_t>(ret) >= current_data->size()) {
      // This should never happen if hash_params.entries matches vector size,
      // but log it rather than crash.
      LOG_EVERY_N(ERROR, 1'001)
          << name() << ": ProcessBatch: hash index " << ret
          << " out of bounds (size=" << current_data->size() << ")"
          << " fseid=" << fseid << " pdr=" << pdr << ".";
      continue;
    }

    // Update per-session stats under the entry's own mutex.
    SessionStats &stat = current_data->at(ret);
    const std::lock_guard<std::mutex> lock(stat.mutex);

    uint64_t diff_ns = now_ns - ts_ns;
    if (stat.last_latency == 0) {
      stat.last_latency = diff_ns;
    }
    uint64_t jitter_ns = absdiff(stat.last_latency, diff_ns);
    stat.last_latency = diff_ns;
    stat.latency_histogram.Insert(diff_ns);
    stat.jitter_histogram.Insert(jitter_ns);
    stat.pkt_count += 1;
    stat.byte_count += batch->pkts()[i]->total_len();

    VLOG(2) << name() << ": updated stats"
            << " fseid=" << fseid << " pdr=" << pdr
            << " idx=" << ret
            << " diff_ns=" << diff_ns
            << " jitter_ns=" << jitter_ns
            << " pkt_count=" << stat.pkt_count
            << " byte_count=" << stat.byte_count;
  }

  RunNextModule(ctx, batch);
}

/*----------------------------------------------------------------------------------*/
CommandResponse FlowMeasure::CommandReadStats(
    const bess::pb::FlowMeasureCommandReadArg &arg) {

  LOG(INFO) << name() << ": CommandReadStats() called."
            << " flag_to_read=" << arg.flag_to_read()
            << " clear=" << arg.clear()
            << " latency_percentiles=" << arg.latency_percentiles().size()
            << " jitter_percentiles=" << arg.jitter_percentiles().size();

  Flag flag_to_read = static_cast<Flag>(arg.flag_to_read());
  if (!Flag_IsValid(flag_to_read)) {
    LOG(ERROR) << name() << ": CommandReadStats: invalid flag_to_read="
               << arg.flag_to_read();
    return CommandFailure(EINVAL, "invalid flag value");
  }

  // Snapshot the current active flag under the mutex so we hold it as briefly
  // as possible and do not stall ProcessBatch.
  Flag cached_current_flag;
  {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    cached_current_flag = current_flag_value_;
  }

  LOG(INFO) << name() << ": " << (leader_ ? "leader" : "follower")
            << " active buffer=" << Flag_Name(cached_current_flag)
            << " reading from=" << Flag_Name(flag_to_read);

  // Warn if pfcpiface is asking us to read from the buffer that is currently
  // being written to — this means either no traffic or a controller bug.
  if (cached_current_flag == flag_to_read &&
      flag_to_read != Flag::FLAG_VALUE_INVALID) {
    LOG(WARNING) << name() << ": reading from the ACTIVE buffer"
                 << " (flag=" << Flag_Name(flag_to_read) << ")."
                 << " Either there is no traffic yet, or CommandFlipFlag was"
                 << " not called before CommandReadStats."
                 << " Stats may be partial or empty.";
  }

  bess::pb::FlowMeasureReadResponse resp;
  auto t_start = std::chrono::high_resolution_clock::now();

  rte_hash *current_hash = nullptr;
  std::vector<SessionStats> *current_data = nullptr;

  switch (flag_to_read) {
    case Flag::FLAG_VALUE_INVALID:
      // No traffic has been seen yet (flag was never set by leader).
      LOG(INFO) << name() << ": flag_to_read=INVALID — no traffic yet,"
                << " returning empty response.";
      return CommandSuccess(resp);
    case Flag::FLAG_VALUE_A:
      current_hash = table_a_;
      current_data = &table_data_a_;
      LOG(INFO) << name() << ": reading from table A.";
      break;
    case Flag::FLAG_VALUE_B:
      current_hash = table_b_;
      current_data = &table_data_b_;
      LOG(INFO) << name() << ": reading from table B.";
      break;
    default:
      LOG(ERROR) << name() << ": unhandled flag value="
                 << Flag_Name(flag_to_read);
      return CommandFailure(EINVAL, "invalid flag value");
  }

  const size_t data_size = current_data->size();
  LOG(INFO) << name() << ": iterating hash table, data_size=" << data_size;

  const void *key = nullptr;
  void *data = nullptr;
  uint32_t next = 0;
  int32_t ret = 0;
  int64_t entries_seen = 0;
  int64_t entries_skipped_bounds = 0;
  int64_t entries_skipped_empty = 0;
  int64_t entries_exported = 0;

  // SAFETY NOTE (af_packet / RW_CONCURRENCY race):
  //
  // rte_hash_iterate with RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY does NOT
  // guarantee snapshot isolation. ProcessBatch may call rte_hash_add_key
  // concurrently, which can shift entries inside the hash table. The `ret`
  // value returned by rte_hash_iterate is the position index for this
  // specific iteration step; we must:
  //   1. Bounds-check ret against current_data->size() before any access.
  //   2. Acquire the per-entry mutex BEFORE touching any field of the entry,
  //      including pkt_count.
  //   3. Never call rte_hash_lookup inside this loop on the same hash table —
  //      that risks a second lock acquisition on a RW_CONCURRENCY table and
  //      can deadlock or return a stale/different index.
  //
  while (ret = rte_hash_iterate(current_hash, &key, &data, &next), ret >= 0) {
    ++entries_seen;

    // --- Guard 1: bounds check before any vector access ---
    if (static_cast<size_t>(ret) >= data_size) {
      LOG_EVERY_N(WARNING, 101)
          << name() << ": CommandReadStats: iterate returned index " << ret
          << " which is out of bounds (data_size=" << data_size << ")."
          << " This can happen transiently in af_packet mode when"
          << " ProcessBatch is inserting new sessions concurrently. Skipping.";
      ++entries_skipped_bounds;
      continue;
    }

    const TableKey *table_key = reinterpret_cast<const TableKey *>(key);

    // --- Guard 2: acquire mutex BEFORE any field read ---
    // In the original code the lock came AFTER accessing session_stat by
    // reference, which is safe for the reference itself but means pkt_count
    // and other fields are read without protection. Lock first.
    SessionStats &session_stat = current_data->at(ret);
    const std::lock_guard<std::mutex> lock(session_stat.mutex);

    // --- Guard 3: skip entries that have never been written to ---
    if (session_stat.pkt_count == 0) {
      VLOG(1) << name() << ": skipping empty entry"
              << " idx=" << ret
              << " fseid=" << table_key->fseid
              << " pdr=" << table_key->pdr;
      ++entries_skipped_empty;
      continue;
    }

    VLOG(1) << name() << ": exporting stats"
            << " idx=" << ret
            << " fseid=" << table_key->fseid
            << " pdr=" << table_key->pdr
            << " pkt_count=" << session_stat.pkt_count
            << " byte_count=" << session_stat.byte_count;

    const std::vector<double> lat_percs(arg.latency_percentiles().begin(),
                                        arg.latency_percentiles().end());
    const std::vector<double> jitter_percs(arg.jitter_percentiles().begin(),
                                           arg.jitter_percentiles().end());

    const auto lat_summary =
        session_stat.latency_histogram.Summarize(lat_percs);
    const auto jitter_summary =
        session_stat.jitter_histogram.Summarize(jitter_percs);

    bess::pb::FlowMeasureReadResponse::Statistic stat;
    stat.set_fseid(table_key->fseid);
    stat.set_pdr(table_key->pdr);

    for (const auto &v : lat_summary.percentile_values) {
      stat.mutable_latency()->add_percentile_values_ns(v);
    }
    for (const auto &v : jitter_summary.percentile_values) {
      stat.mutable_jitter()->add_percentile_values_ns(v);
    }

    stat.set_total_packets(session_stat.pkt_count);
    stat.set_total_bytes(session_stat.byte_count);
    *resp.add_statistics() = stat;
    ++entries_exported;
  }

  LOG(INFO) << name() << ": iteration complete."
            << " entries_seen=" << entries_seen
            << " exported=" << entries_exported
            << " skipped_bounds=" << entries_skipped_bounds
            << " skipped_empty=" << entries_skipped_empty;

  if (arg.clear()) {
    LOG(INFO) << name() << ": clearing hash table and data vector...";

    // Reset the hash table — this frees all keys. ProcessBatch will re-insert
    // them as new packets arrive. The 10 ms sleep in CommandFlipFlag is meant
    // to drain in-flight packets before we get here; if clear is called without
    // a preceding flip, concurrent ProcessBatch inserts may race with the reset.
    rte_hash_reset(current_hash);
    LOG(INFO) << name() << ": hash table reset done.";

    // Zero out the backing data vector under each entry's mutex.
    for (size_t idx = 0; idx < current_data->size(); ++idx) {
      SessionStats &s = current_data->at(idx);
      const std::lock_guard<std::mutex> lock(s.mutex);
      s.reset();
    }
    LOG(INFO) << name() << ": data vector cleared.";
  }

  auto t_done = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = t_done - t_start;
  LOG(INFO) << name() << ": CommandReadStats() done in "
            << elapsed.count() << " s"
            << " stats_returned=" << resp.statistics_size();

  return CommandSuccess(resp);
}

/*----------------------------------------------------------------------------------*/
CommandResponse FlowMeasure::CommandFlipFlag(
    const bess::pb::FlowMeasureCommandFlipArg &) {

  if (!leader_) {
    LOG(ERROR) << name() << ": CommandFlipFlag called on non-leader module.";
    return CommandFailure(EINVAL, "only leaders can flip the flag");
  }

  Flag cached_old_flag, cached_new_flag;
  {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    cached_old_flag = current_flag_value_;
    current_flag_value_ = (current_flag_value_ == Flag::FLAG_VALUE_A)
                              ? Flag::FLAG_VALUE_B
                              : Flag::FLAG_VALUE_A;
    cached_new_flag = current_flag_value_;
  }

  LOG(INFO) << name() << ": CommandFlipFlag() flipped"
            << " old=" << Flag_Name(cached_old_flag)
            << " new=" << Flag_Name(cached_new_flag)
            << " — sleeping 10 ms to drain in-flight packets...";

  bess::pb::FlowMeasureFlipResponse resp;
  resp.set_old_flag(static_cast<uint64_t>(cached_old_flag));

  // Allow the BESS pipeline to flush any packets that were already stamped with
  // the old flag value before the flip. 10 ms is conservative; one scheduling
  // quantum (typically ~1 ms) would suffice for most deployments.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  LOG(INFO) << name() << ": CommandFlipFlag() drain complete."
            << " old_flag=" << static_cast<uint64_t>(cached_old_flag)
            << " new active=" << Flag_Name(cached_new_flag);

  return CommandSuccess(resp);
}

/*----------------------------------------------------------------------------------*/
void FlowMeasure::DeInit() {
  LOG(INFO) << name() << ": DeInit() freeing hash tables.";
  rte_hash_free(table_a_);
  rte_hash_free(table_b_);
  LOG(INFO) << name() << ": DeInit() complete.";
}

/*----------------------------------------------------------------------------------*/
ADD_MODULE(FlowMeasure, "qos_measure", "Measures QoS metrics")