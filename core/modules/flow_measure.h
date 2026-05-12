/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2021 Open Networking Foundation
 */
#ifndef BESS_MODULES_QOS_MEASURE_H_
#define BESS_MODULES_QOS_MEASURE_H_

// NOTE: rte_hash intentionally removed — rte_hash_iterate is not safe when
// rte_hash_add_key runs concurrently even with
// RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY. Replaced with std::unordered_map +
// std::shared_mutex.

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../core/utils/histogram.h"
#include "../module.h"

class FlowMeasure final : public Module {
 public:
  FlowMeasure()
      : leader_(false),
        current_flag_value_(Flag::FLAG_VALUE_INVALID),
        buf_a_(nullptr),
        buf_b_(nullptr),
        ts_attr_id_(-1),
        fseid_attr_id_(-1),
        pdr_attr_id_(-1),
        buffer_flag_attr_id_(-1) {
    // Single worker only — double-buffer design serialises per-buffer writes.
    max_allowed_workers_ = 1;
  }

  static const Commands cmds;

  CommandResponse Init(const bess::pb::FlowMeasureArg &arg);
  void DeInit() override;
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  std::string GetDesc() const override { return ""; }

  CommandResponse CommandReadStats(
      const bess::pb::FlowMeasureCommandReadArg &arg);
  CommandResponse CommandFlipFlag(
      const bess::pb::FlowMeasureCommandFlipArg &arg);

 private:
  // -----------------------------------------------------------------------
  // Flag — selects which double-buffer is active.
  // -----------------------------------------------------------------------
  enum class Flag {
    FLAG_VALUE_INVALID = 0,
    FLAG_VALUE_A = 1,
    FLAG_VALUE_B = 2,
    FLAG_VALUE_MAX = FLAG_VALUE_B,
  };

  template <typename T>
  static constexpr bool Flag_IsValid(T value) {
    Flag f = static_cast<Flag>(value);
    return f > Flag::FLAG_VALUE_INVALID && f <= Flag::FLAG_VALUE_MAX;
  }

  static std::string Flag_Name(const Flag &flag) {
    switch (flag) {
      case Flag::FLAG_VALUE_INVALID:
        return "FLAG_VALUE_INVALID";
      case Flag::FLAG_VALUE_A:
        return "FLAG_VALUE_A";
      case Flag::FLAG_VALUE_B:
        return "FLAG_VALUE_B";
      default:
        return "<unknown>";
    }
  }

  // -----------------------------------------------------------------------
  // TableKey — same layout as original, kept identical so no behaviour change.
  // -----------------------------------------------------------------------
  struct __attribute__((packed, aligned(16))) TableKey {
    uint64_t fseid;
    uint64_t pdr;

    TableKey(uint64_t fseid, uint64_t pdr) : fseid(fseid), pdr(pdr) {}
    TableKey() : fseid(0), pdr(0) {}

    bool operator==(const TableKey &o) const {
      return fseid == o.fseid && pdr == o.pdr;
    }

    std::string ToString() const {
      std::stringstream ss;
      ss << "{ fseid: " << fseid << ", pdr: " << pdr << " }";
      return ss.str();
    }
  };

  static_assert(std::is_trivially_copyable<TableKey>::value,
                "TableKey must be trivially copyable.");

  // Hash for TableKey — replaces rte_jhash, same mixing quality.
  struct TableKeyHash {
    std::size_t operator()(const TableKey &k) const noexcept {
      std::size_t h = k.fseid;
      h ^= h >> 33;
      h *= 0xff51afd7ed558ccdULL;
      h ^= h >> 33;
      h *= 0xc4ceb9fe1a85ec53ULL;
      h ^= h >> 33;
      h ^= k.pdr * 0x9e3779b97f4a7c15ULL;
      return h;
    }
  };

  // -----------------------------------------------------------------------
  // SessionStats — identical to original.
  // -----------------------------------------------------------------------
  struct SessionStats {
    uint64_t pkt_count;
    uint64_t byte_count;
    uint64_t last_latency;

    static constexpr uint64_t kBucketWidthNs = 1000;
    static constexpr uint64_t kNumBuckets = 100;

    Histogram<uint64_t> latency_histogram;
    Histogram<uint64_t> jitter_histogram;
    mutable std::mutex mutex;

    SessionStats()
        : pkt_count(0),
          byte_count(0),
          last_latency(0),
          latency_histogram(kNumBuckets, kBucketWidthNs),
          jitter_histogram(kNumBuckets, kBucketWidthNs) {}

    // mutex is not movable — must live on heap, accessed via pointer.
    SessionStats(const SessionStats &) = delete;
    SessionStats &operator=(const SessionStats &) = delete;
    SessionStats(SessionStats &&) = delete;
    SessionStats &operator=(SessionStats &&) = delete;

    void reset() {
      // Caller must hold mutex.
      pkt_count = 0;
      byte_count = 0;
      last_latency = 0;
      latency_histogram.Reset();
      jitter_histogram.Reset();
    }
  };

  // -----------------------------------------------------------------------
  // Buffer — one side of the double-buffer.
  //
  // Replaces: rte_hash* + std::vector<SessionStats>
  //
  // Concurrency model:
  //   ProcessBatch  : shared_lock for existing-entry lookup,
  //                   unique_lock briefly for new-entry insertion.
  //   CommandReadStats : shared_lock for full iteration
  //                      (safe because ProcessBatch is on the OTHER buffer
  //                       after CommandFlipFlag is called).
  //   clear()       : unique_lock; only ever called on the inactive buffer.
  // -----------------------------------------------------------------------
  struct Buffer {
    mutable std::shared_mutex map_mutex;
    std::unordered_map<TableKey, SessionStats *, TableKeyHash> map;

    Buffer() = default;
    ~Buffer() { clear(); }

    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    // Free all heap-allocated SessionStats and empty the map.
    void clear() {
      std::unique_lock<std::shared_mutex> lk(map_mutex);
      for (auto &kv : map)
        delete kv.second;
      map.clear();
    }

    // Return existing SessionStats or create a new one — thread-safe.
    SessionStats *get_or_create(const TableKey &key) {
      // Fast path: shared lock, entry likely exists.
      {
        std::shared_lock<std::shared_mutex> lk(map_mutex);
        auto it = map.find(key);
        if (it != map.end())
          return it->second;
      }
      // Slow path: exclusive lock, insert new entry.
      std::unique_lock<std::shared_mutex> lk(map_mutex);
      // Re-check after acquiring exclusive lock (another thread may have
      // inserted between the two lock acquisitions).
      auto [it, inserted] = map.emplace(key, nullptr);
      if (inserted)
        it->second = new SessionStats();
      return it->second;
    }
  };

  // -----------------------------------------------------------------------
  // Data members
  // -----------------------------------------------------------------------

  // Double-buffers: replaces table_a_/table_b_ + table_data_a_/table_data_b_.
  std::unique_ptr<Buffer> buf_a_;
  std::unique_ptr<Buffer> buf_b_;

  mutable std::mutex flag_mutex_;  // protects current_flag_value_
  Flag current_flag_value_;
  bool leader_;

  int buffer_flag_attr_id_;
  int ts_attr_id_;
  int fseid_attr_id_;
  int pdr_attr_id_;
};

#endif  // BESS_MODULES_QOS_MEASURE_H_