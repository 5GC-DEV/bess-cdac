/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2021 Open Networking Foundation
 */

#include "flow_measure.h"

#include <chrono>
#include <thread>

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

  if (arg.leader()) {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    leader_ = true;
    buffer_flag_attr_id_ = AddMetadataAttr(
        arg.flag_attr_name(), sizeof(uint64_t), AccessMode::kWrite);
    current_flag_value_ = Flag::FLAG_VALUE_A;
  } else {
    leader_ = false;
    buffer_flag_attr_id_ = AddMetadataAttr(arg.flag_attr_name(),
                                           sizeof(uint64_t), AccessMode::kRead);
    current_flag_value_ = Flag::FLAG_VALUE_INVALID;
  }

  if (buffer_flag_attr_id_ < 0)
    return CommandFailure(EINVAL, "invalid flag attribute name");

  ts_attr_id_ =
      AddMetadataAttr("timestamp", sizeof(uint64_t), AccessMode::kRead);
  if (ts_attr_id_ < 0)
    return CommandFailure(EINVAL, "invalid metadata declaration");

  fseid_attr_id_ =
      AddMetadataAttr("fseid", sizeof(uint64_t), AccessMode::kRead);
  if (fseid_attr_id_ < 0)
    return CommandFailure(EINVAL, "invalid metadata declaration");

  pdr_attr_id_ = AddMetadataAttr("pdr_id", sizeof(uint32_t), AccessMode::kRead);
  if (pdr_attr_id_ < 0)
    return CommandFailure(EINVAL, "invalid metadata declaration");

  // Initialized dynamic buffers using smart pointers to replace manual DPDK
  // hash creation.
  buf_a_ = std::make_unique<Buffer>();
  buf_b_ = std::make_unique<Buffer>();

  VLOG(1) << name() << ": Tables created successfully.";
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
            << name() << ": encountered invalid flag: " << flag;
        continue;
      }
      const std::lock_guard<std::mutex> lock(flag_mutex_);
      current_flag_value_ = static_cast<Flag>(flag);
      cached_current_flag = current_flag_value_;
    }

    uint64_t ts_ns = get_attr<uint64_t>(this, ts_attr_id_, batch->pkts()[i]);
    uint64_t fseid = get_attr<uint64_t>(this, fseid_attr_id_, batch->pkts()[i]);
    uint32_t pdr = get_attr<uint32_t>(this, pdr_attr_id_, batch->pkts()[i]);

    // Added fallback to current time for missing timestamps to ensure
    // packet/byte counts remain accurate.
    if (ts_ns == 0 || now_ns < ts_ns) {
      ts_ns = now_ns;
    }

    Buffer *buf = nullptr;
    switch (cached_current_flag) {
      case Flag::FLAG_VALUE_A:
        buf = buf_a_.get();
        break;
      case Flag::FLAG_VALUE_B:
        buf = buf_b_.get();
        break;
      default:
        LOG_EVERY_N(ERROR, 100'001)
            << name()
            << ": unknown flag value: " << Flag_Name(cached_current_flag);
        continue;
    }

    TableKey key(fseid, pdr);
    // Replaced index-based lookup with a thread-safe get_or_create pattern for
    // dynamic flow tracking.
    SessionStats *stat = buf->get_or_create(key);

    const std::lock_guard<std::mutex> lock(stat->mutex);
    uint64_t diff_ns = now_ns - ts_ns;
    if (stat->last_latency == 0)
      stat->last_latency = diff_ns;
    uint64_t jitter_ns = absdiff(stat->last_latency, diff_ns);
    stat->last_latency = diff_ns;
    stat->latency_histogram.Insert(diff_ns);
    stat->jitter_histogram.Insert(jitter_ns);
    stat->pkt_count += 1;
    stat->byte_count += batch->pkts()[i]->total_len();
  }

  RunNextModule(ctx, batch);
}

/*----------------------------------------------------------------------------------*/
CommandResponse FlowMeasure::CommandReadStats(
    const bess::pb::FlowMeasureCommandReadArg &arg) {
  Flag flag_to_read = static_cast<Flag>(arg.flag_to_read());

  if (!Flag_IsValid(flag_to_read) && flag_to_read != Flag::FLAG_VALUE_INVALID) {
    return CommandFailure(EINVAL, "invalid flag value");
  }

  Flag cached_current_flag;
  {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    cached_current_flag = current_flag_value_;
  }

  VLOG(1) << name() << ": " << (leader_ ? "leader" : "follower")
          << " last saw buffer flag " << Flag_Name(cached_current_flag)
          << ", now reading from " << Flag_Name(flag_to_read);

  VLOG_IF(1, cached_current_flag == flag_to_read &&
                 flag_to_read != Flag::FLAG_VALUE_INVALID)
      << name() << ": reading from active buffer — either no traffic yet"
      << " or controller is performing invalid requests.";

  if (flag_to_read == Flag::FLAG_VALUE_INVALID) {
    return CommandSuccess(bess::pb::FlowMeasureReadResponse{});
  }

  Buffer *buf =
      (flag_to_read == Flag::FLAG_VALUE_A) ? buf_a_.get() : buf_b_.get();
  if (!buf)
    return CommandFailure(EINVAL, "buffer not initialized");

  bess::pb::FlowMeasureReadResponse resp;
  auto t_start = std::chrono::high_resolution_clock::now();

  const std::vector<double> lat_percs(arg.latency_percentiles().begin(),
                                      arg.latency_percentiles().end());
  const std::vector<double> jitter_percs(arg.jitter_percentiles().begin(),
                                         arg.jitter_percentiles().end());

  {
    // Used shared_lock to allow concurrent dataplane lookups while the control
    // plane iterates through the map.
    std::shared_lock<std::shared_mutex> map_lk(buf->map_mutex);

    for (auto &[key, stat_ptr] : buf->map) {
      if (!stat_ptr)
        continue;

      const std::lock_guard<std::mutex> stat_lk(stat_ptr->mutex);
      if (stat_ptr->pkt_count == 0)
        continue;

      const auto lat_summary = stat_ptr->latency_histogram.Summarize(lat_percs);
      const auto jitter_summary =
          stat_ptr->jitter_histogram.Summarize(jitter_percs);

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
    }
  }  // shared_lock released before clear

  if (arg.clear()) {
    // clear() acquires unique_lock internally.
    // Safe: ProcessBatch is writing to the other buffer after the flag flip.
    buf->clear();
  }

  if (VLOG_IS_ON(1)) {
    auto elapsed = std::chrono::duration<double>(
                       std::chrono::high_resolution_clock::now() - t_start)
                       .count();
    VLOG(1) << name() << ": CommandReadStats took " << elapsed << "s.";
  }

  return CommandSuccess(resp);
}

/*----------------------------------------------------------------------------------*/
CommandResponse FlowMeasure::CommandFlipFlag(
    const bess::pb::FlowMeasureCommandFlipArg &) {
  if (!leader_)
    return CommandFailure(EINVAL, "only leaders can flip the flag");

  Flag cached_old_flag, cached_current_flag;
  {
    const std::lock_guard<std::mutex> lock(flag_mutex_);
    cached_old_flag = current_flag_value_;
    current_flag_value_ = (current_flag_value_ == Flag::FLAG_VALUE_A)
                              ? Flag::FLAG_VALUE_B
                              : Flag::FLAG_VALUE_A;
    cached_current_flag = current_flag_value_;
  }

  VLOG(1) << name() << ": leader flipped the buffer flag to "
          << Flag_Name(cached_current_flag);

  bess::pb::FlowMeasureFlipResponse resp;
  resp.set_old_flag(static_cast<uint64_t>(cached_old_flag));

  // Allow pipeline to flush packets stamped with the old flag value.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  return CommandSuccess(resp);
}

/*----------------------------------------------------------------------------------*/
void FlowMeasure::DeInit() {
  buf_a_.reset();
  buf_b_.reset();
}

/*----------------------------------------------------------------------------------*/
ADD_MODULE(FlowMeasure, "qos_measure", "Measures QoS metrics")