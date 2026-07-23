#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ael {

enum class Policy { fifo, fixed_priority, earliest_deadline, latest_only };

struct Job {
  std::string stream;
  std::uint64_t sequence{};
  std::int64_t release_ms{};
  std::int64_t execution_ms{};
  std::int64_t deadline_ms{};
  std::int64_t source_ms{};
  std::int64_t max_age_ms{};
  int priority{};
};

enum class Outcome { completed, deadline_miss, rejected_stale, superseded };

struct TraceEntry {
  std::uint64_t trace_ordinal{};
  Job job;
  std::int64_t start_ms{};
  std::int64_t finish_ms{};
  std::int64_t wait_ms{};
  std::int64_t response_ms{};
  std::int64_t age_at_start_ms{};
  Outcome outcome{Outcome::completed};
  std::string diagnosis;
};

struct Report {
  Policy policy{Policy::fifo};
  std::vector<TraceEntry> trace;
  std::size_t completed{};
  std::size_t deadline_misses{};
  std::size_t stale_rejections{};
  std::size_t superseded{};
  std::int64_t makespan_ms{};
  std::int64_t maximum_response_ms{};
  std::int64_t maximum_age_ms{};
  std::string trace_fingerprint;

  [[nodiscard]] bool clean() const noexcept {
    return deadline_misses == 0 && stale_rejections == 0;
  }
};

[[nodiscard]] Policy parse_policy(std::string_view value);
[[nodiscard]] std::string to_string(Policy value);
[[nodiscard]] std::string to_string(Outcome value);
[[nodiscard]] std::vector<Job> read_workload_csv(const std::filesystem::path& path);
void write_trace_csv(const std::filesystem::path& path, const Report& report);
[[nodiscard]] std::string trace_fingerprint_v1(const std::vector<TraceEntry>& trace);
[[nodiscard]] std::string trace_fingerprint_v2(const std::vector<TraceEntry>& trace);
[[nodiscard]] Report simulate(const std::vector<Job>& workload, Policy policy);
[[nodiscard]] std::string report_json(const Report& report);

}  // namespace ael
