/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2021 Open Networking Foundation
 *
 * FIX SUMMARY
 * -----------
 * Root cause: rte_hash_iterate() is NOT safe when rte_hash_add_key() runs
 * concurrently on the same table, even with RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY.
 * DPDK's internal bucket walk in rte_hash_iterate dereferences pointers that
 * rte_hash_add_key may be reshuffling simultaneously → SIGSEGV at address 0.
 *
 * Fix: replaced rte_hash + rte_hash_iterate with std::unordered_map protected
 * by std::shared_mutex.  The double-buffer (flag-flip) design is preserved
 * exactly — only the underlying hash table implementation changes.
 *
 * To confirm this binary is running, look for "[v2-FIXED]" in the logs.
 */

// rte_errno / rte_jhash intentionally removed (no rte_hash tables).
#include "flow_measure.h"

#include <chrono>
#include <thread>

#include "../core/utils/common.h"

// Build-stamp: grep for this string in `strings /bin/bessd` to confirm
// that this version compiled into the binary.
static const char *kBuildStamp = "[v2-FIXED] flow_measure built " __DATE__ " " __TIME__;

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

  // This log line confirms the new binary is running.
  // If you see "[v2-FIXED]" here — the fix is compiled in.
  // If you still see the old logs without this prefix — old binary is running.
  LOG(INFO) << name() << ": [v2-FIXED] Init() called."
            << " BUILD=" << kBuildStamp
            << " leader=" << arg.leader()
            << " flag_attr_name=" << arg.flag_attr_name()
            << " entries=" << arg.entries();

  if (arg.leader()) {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    leader_ = true;
    buffer_flag_attr_id_ = AddMetadataAttr(
        arg.flag_attr_name(), sizeof(uint64_t), AccessMode::kWrite);
    current_flag_value_ = Flag::FLAG_VALUE_A;
    LOG(INFO) << name() << ": [v2-FIXED] LEADER init."
              << " initial_flag=FLAG_VALUE_A"
              << " buffer_flag_attr_id=" << buffer_flag_attr_id_;
  } else {
    leader_ = false;
    buffer_flag_attr_id_ = AddMetadataAttr(
        arg.flag_attr_name(), sizeof(uint64_t), AccessMode::kRead);
    // Followers read the flag from packet metadata in ProcessBatch.
    // Start INVALID — ProcessBatch sets it on first packet.
    current_flag_value_ = Flag::FLAG_VALUE_INVALID;
    LOG(INFO) << name() << ": [v2-FIXED] FOLLOWER init."
              << " buffer_flag_attr_id=" << buffer_flag_attr_id_;
  }

  if (buffer_flag_attr_id_ < 0) {
    LOG(ERROR) << name() << ": [v2-FIXED] failed to add flag attr, id="
               << buffer_flag_attr_id_;
    return CommandFailure(EINVAL, "invalid flag attribute name");
  }

  ts_attr_id_ = AddMetadataAttr("timestamp", sizeof(uint64_t),
                                AccessMode::kRead);
  if (ts_attr_id_ < 0)
    return CommandFailure(EINVAL, "invalid metadata declaration");

  fseid_attr_id_ = AddMetadataAttr("fseid", sizeof(uint64_t),
                                   AccessMode::kRead);
  if (fseid_attr_id_ < 0)
    return CommandFailure(EINVAL, "invalid metadata declaration");

  pdr_attr_id_ = AddMetadataAttr("pdr_id", sizeof(uint32_t),
                                 AccessMode::kRead);
  if (pdr_attr_id_ < 0)
    return CommandFailure(EINVAL, "invalid metadata declaration");

  // Allocate the two double-buffers.
  // Previously: rte_hash_create() x2 + vector<SessionStats> x2
  // Now: two heap-allocated Buffer objects (unordered_map + shared_mutex each)
  buf_a_ = std::make_unique<Buffer>();
  buf_b_ = std::make_unique<Buffer>();

  LOG(INFO) << name() << ": [v2-FIXED] Init() complete."
            << " buf_a=" << buf_a_.get()
            << " buf_b=" << buf_b_.get()
            << " (std::unordered_map, NO rte_hash)";

  return CommandSuccess();
}

/*----------------------------------------------------------------------------------*/
void FlowMeasure::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  uint64_t now_ns = tsc_to_ns(rdtsc());

  for (int i = 0; i < batch->cnt(); ++i) {
    Flag cached_current_flag;

    if (leader_) {
      const std::lock_guard<std::mutex> lock(flag_mutex_);
      set_attr<uint64_t>(this, buffer_flag_attr_id_, batch->pkts()[i],
                         static_cast<uint64_t>(current_flag_value_));
      cached_current_flag = current_flag_value_;
    } else {
      uint64_t flag =
          get_attr<uint64_t>(this, buffer_flag_attr_id_, batch->pkts()[i]);
      if (!Flag_IsValid(flag)) {
        LOG_EVERY_N(WARNING, 100'001)
            << name() << ": [v2-FIXED] invalid flag=" << flag << " skipping.";
        continue;
      }
      const std::lock_guard<std::mutex> lock(flag_mutex_);
      current_flag_value_ = static_cast<Flag>(flag);
      cached_current_flag = current_flag_value_;
    }

    uint64_t ts_ns = get_attr<uint64_t>(this, ts_attr_id_,    batch->pkts()[i]);
    uint64_t fseid = get_attr<uint64_t>(this, fseid_attr_id_, batch->pkts()[i]);
    uint32_t pdr   = get_attr<uint32_t>(this, pdr_attr_id_,   batch->pkts()[i]);

    if (!ts_ns || now_ns < ts_ns) continue;

    // Select the active buffer.
    // FIX: buf->get_or_create() uses shared_lock for lookup and unique_lock
    // only for new insertions — safe to call from ProcessBatch concurrently
    // with CommandReadStats iterating the OTHER buffer.
    Buffer *buf = nullptr;
    switch (cached_current_flag) {
      case Flag::FLAG_VALUE_A: buf = buf_a_.get(); break;
      case Flag::FLAG_VALUE_B: buf = buf_b_.get(); break;
      default:
        LOG_EVERY_N(ERROR, 100'001)
            << name() << ": [v2-FIXED] unknown flag="
            << Flag_Name(cached_current_flag);
        continue;
    }

    TableKey key(fseid, pdr);
    SessionStats *stat = buf->get_or_create(key);

    const std::lock_guard<std::mutex> lock(stat->mutex);
    uint64_t diff_ns = now_ns - ts_ns;
    if (stat->last_latency == 0) stat->last_latency = diff_ns;
    uint64_t jitter_ns = absdiff(stat->last_latency, diff_ns);
    stat->last_latency = diff_ns;
    stat->latency_histogram.Insert(diff_ns);
    stat->jitter_histogram.Insert(jitter_ns);
    stat->pkt_count  += 1;
    stat->byte_count += batch->pkts()[i]->total_len();
  }

  RunNextModule(ctx, batch);
}

/*----------------------------------------------------------------------------------*/
CommandResponse FlowMeasure::CommandReadStats(
    const bess::pb::FlowMeasureCommandReadArg &arg) {

  Flag flag_to_read = static_cast<Flag>(arg.flag_to_read());

  // FIX: FLAG_VALUE_INVALID is valid here — means no traffic yet.
  // Original code returned CommandFailure for INVALID; we return empty stats.
  if (!Flag_IsValid(flag_to_read) &&
      flag_to_read != Flag::FLAG_VALUE_INVALID) {
    LOG(ERROR) << name() << ": [v2-FIXED] invalid flag_to_read="
               << arg.flag_to_read();
    return CommandFailure(EINVAL, "invalid flag value");
  }

  Flag cached_current_flag;
  {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    cached_current_flag = current_flag_value_;
  }

  LOG(INFO) << name() << ": [v2-FIXED] CommandReadStats() called."
            << " flag_to_read=" << arg.flag_to_read()
            << " (" << Flag_Name(flag_to_read) << ")"
            << " clear=" << arg.clear()
            << " active_buffer=" << Flag_Name(cached_current_flag)
            << " role=" << (leader_ ? "leader" : "follower");

  if (cached_current_flag == flag_to_read &&
      flag_to_read != Flag::FLAG_VALUE_INVALID) {
    LOG(WARNING) << name() << ": [v2-FIXED] reading from ACTIVE buffer"
                 << " (flag=" << Flag_Name(flag_to_read) << ")."
                 << " CommandFlipFlag may not have been called first."
                 << " Stats may be partial.";
  }

  if (flag_to_read == Flag::FLAG_VALUE_INVALID) {
    LOG(INFO) << name() << ": [v2-FIXED] INVALID flag — no traffic yet,"
              << " returning empty response.";
    return CommandSuccess(bess::pb::FlowMeasureReadResponse{});
  }

  // Select the INACTIVE buffer to read from.
  Buffer *buf = nullptr;
  if (flag_to_read == Flag::FLAG_VALUE_A) {
    buf = buf_a_.get();
    LOG(INFO) << name() << ": [v2-FIXED] reading from unordered_map buffer A"
              << " ptr=" << buf;
  } else {
    buf = buf_b_.get();
    LOG(INFO) << name() << ": [v2-FIXED] reading from unordered_map buffer B"
              << " ptr=" << buf;
  }

  if (!buf) {
    LOG(ERROR) << name() << ": [v2-FIXED] buffer ptr is null — Init() not called?";
    return CommandFailure(EINVAL, "buffer not initialized");
  }

  bess::pb::FlowMeasureReadResponse resp;
  auto t_start = std::chrono::high_resolution_clock::now();
  int64_t entries_seen = 0, entries_exported = 0, entries_empty = 0;

  const std::vector<double> lat_percs(arg.latency_percentiles().begin(),
                                      arg.latency_percentiles().end());
  const std::vector<double> jitter_percs(arg.jitter_percentiles().begin(),
                                         arg.jitter_percentiles().end());

  // FIX: std::shared_lock allows concurrent ProcessBatch lookups on this
  // buffer (via get_or_create fast-path), but ProcessBatch is actually writing
  // to the OTHER buffer right now (flag was flipped). So this buffer has zero
  // writers — iteration is completely safe.
  // Previously: rte_hash_iterate with RW_CONCURRENCY → SIGSEGV when
  // ProcessBatch called rte_hash_add_key concurrently on the same table.
  {
    std::shared_lock<std::shared_mutex> map_lk(buf->map_mutex);

    LOG(INFO) << name() << ": [v2-FIXED] iterating unordered_map size="
              << buf->map.size();

    for (auto &[key, stat_ptr] : buf->map) {
      ++entries_seen;

      if (!stat_ptr) {
        LOG_EVERY_N(WARNING, 101)
            << name() << ": [v2-FIXED] null stat_ptr for key="
            << key.ToString() << " skipping.";
        continue;
      }

      const std::lock_guard<std::mutex> stat_lk(stat_ptr->mutex);

      if (stat_ptr->pkt_count == 0) {
        ++entries_empty;
        continue;
      }

      VLOG(1) << name() << ": [v2-FIXED] exporting"
              << " fseid=" << key.fseid << " pdr=" << key.pdr
              << " pkts=" << stat_ptr->pkt_count
              << " bytes=" << stat_ptr->byte_count;

      const auto lat_summary    = stat_ptr->latency_histogram.Summarize(lat_percs);
      const auto jitter_summary = stat_ptr->jitter_histogram.Summarize(jitter_percs);

      bess::pb::FlowMeasureReadResponse::Statistic stat;
      stat.set_fseid(key.fseid);
      stat.set_pdr(key.pdr);
      for (const auto &v : lat_summary.percentile_values)
        stat.mutable_latency()->add_percentile_values_ns(v);
      for (const auto &v : jitter_summary.percentile_values)
        stat.mutable_jitter()->add_percentile_values_ns(v);
      stat.set_total_packets(stat_ptr->pkt_count);
      stat.set_total_bytes(stat_ptr->byte_count);
      *resp.add_statistics() = stat;
      ++entries_exported;
    }
  }  // shared_lock released here — before clear

  LOG(INFO) << name() << ": [v2-FIXED] iteration complete."
            << " seen=" << entries_seen
            << " exported=" << entries_exported
            << " empty=" << entries_empty;

  if (arg.clear()) {
    // FIX: buf->clear() acquires unique_lock and deletes all entries.
    // Safe because ProcessBatch is writing to the OTHER buffer right now.
    // Previously: rte_hash_reset() — not concurrency-safe, caused SIGSEGV.
    LOG(INFO) << name() << ": [v2-FIXED] clearing inactive buffer...";
    buf->clear();
    LOG(INFO) << name() << ": [v2-FIXED] buffer cleared.";
  }

  auto elapsed = std::chrono::duration<double>(
      std::chrono::high_resolution_clock::now() - t_start).count();

  LOG(INFO) << name() << ": [v2-FIXED] CommandReadStats() done in "
            << elapsed << " s"
            << " stats_returned=" << resp.statistics_size();

  return CommandSuccess(resp);
}

/*----------------------------------------------------------------------------------*/
CommandResponse FlowMeasure::CommandFlipFlag(
    const bess::pb::FlowMeasureCommandFlipArg &) {

  if (!leader_) {
    LOG(ERROR) << name() << ": [v2-FIXED] CommandFlipFlag on non-leader";
    return CommandFailure(EINVAL, "only leaders can flip the flag");
  }

  Flag old_flag, new_flag;
  {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    old_flag = current_flag_value_;
    current_flag_value_ = (current_flag_value_ == Flag::FLAG_VALUE_A)
                              ? Flag::FLAG_VALUE_B
                              : Flag::FLAG_VALUE_A;
    new_flag = current_flag_value_;
  }

  LOG(INFO) << name() << ": [v2-FIXED] CommandFlipFlag()"
            << " old=" << Flag_Name(old_flag)
            << " new=" << Flag_Name(new_flag)
            << " — sleeping 10ms to drain in-flight packets...";

  bess::pb::FlowMeasureFlipResponse resp;
  resp.set_old_flag(static_cast<uint64_t>(old_flag));

  // Allow pipeline to flush packets stamped with the old flag.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  LOG(INFO) << name() << ": [v2-FIXED] flip drain complete."
            << " new active=" << Flag_Name(new_flag);

  return CommandSuccess(resp);
}

/*----------------------------------------------------------------------------------*/
void FlowMeasure::DeInit() {
  LOG(INFO) << name() << ": [v2-FIXED] DeInit() releasing buffers.";
  buf_a_.reset();
  buf_b_.reset();
  LOG(INFO) << name() << ": [v2-FIXED] DeInit() complete.";
}

/*----------------------------------------------------------------------------------*/
ADD_MODULE(FlowMeasure, "qos_measure", "Measures QoS metrics")