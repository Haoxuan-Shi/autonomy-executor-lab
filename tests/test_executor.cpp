#include "ael/executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ael::Job job(std::string stream, std::uint64_t sequence, std::int64_t release,
             std::int64_t execution, std::int64_t deadline, std::int64_t max_age,
             int priority) {
  return {std::move(stream), sequence, release, execution, deadline, release, max_age, priority};
}

void check_aggregates_match_trace(const ael::Report& report,
                                  const std::string& label) {
  std::size_t completed = 0;
  std::size_t deadline_misses = 0;
  std::size_t stale_rejections = 0;
  std::size_t superseded = 0;
  std::int64_t makespan = 0;
  std::int64_t maximum_response = 0;
  std::int64_t maximum_age = 0;
  for (const auto& entry : report.trace) {
    switch (entry.outcome) {
      case ael::Outcome::completed:
        ++completed;
        break;
      case ael::Outcome::deadline_miss:
        ++deadline_misses;
        break;
      case ael::Outcome::rejected_stale:
        ++stale_rejections;
        break;
      case ael::Outcome::superseded:
        ++superseded;
        break;
    }
    makespan = std::max(makespan, entry.finish_ms);
    maximum_response = std::max(maximum_response, entry.response_ms);
    maximum_age = std::max(maximum_age, entry.age_at_start_ms);
  }
  check(report.completed == completed, label + " completed count must derive from trace");
  check(report.deadline_misses == deadline_misses,
        label + " deadline-miss count must derive from trace");
  check(report.stale_rejections == stale_rejections,
        label + " stale count must derive from trace");
  check(report.superseded == superseded,
        label + " superseded count must derive from trace");
  check(report.makespan_ms == makespan, label + " makespan must derive from trace");
  check(report.maximum_response_ms == maximum_response,
        label + " maximum response must derive from trace");
  check(report.maximum_age_ms == maximum_age,
        label + " maximum age must derive from trace");
}

std::string raw_bytes(std::initializer_list<unsigned int> values) {
  std::string result;
  result.reserve(values.size());
  for (const auto value : values) result.push_back(static_cast<char>(value));
  return result;
}

class TemporaryFile {
 public:
  explicit TemporaryFile(std::string_view contents) {
    static std::uint64_t sequence = 0;
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("ael-executor-test-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence++) + ".csv");
    std::ofstream output(path_, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create temporary test file");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) throw std::runtime_error("cannot write temporary test file");
  }

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  ~TemporaryFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

std::string workload_csv(std::string_view stream) {
  std::string contents =
      "stream,sequence,release_ms,execution_ms,deadline_ms,source_ms,max_age_ms,priority\n";
  contents.append(stream);
  contents += ",0,0,1,10,0,10,1\n";
  return contents;
}

std::vector<std::string> decode_csv_record_for_test(std::string_view record) {
  std::vector<std::string> fields;
  std::size_t position = 0;
  while (true) {
    std::string field;
    if (position < record.size() && record[position] == '"') {
      ++position;
      while (true) {
        if (position == record.size()) {
          throw std::runtime_error("test CSV decoder found an unclosed field");
        }
        if (record[position] != '"') {
          field.push_back(record[position++]);
        } else if (position + 1U < record.size() && record[position + 1U] == '"') {
          field.push_back('"');
          position += 2U;
        } else {
          ++position;
          break;
        }
      }
      if (position < record.size() && record[position] != ',') {
        throw std::runtime_error("test CSV decoder found text after a closing quote");
      }
    } else {
      while (position < record.size() && record[position] != ',') {
        if (record[position] == '"') {
          throw std::runtime_error("test CSV decoder found a bare quote");
        }
        field.push_back(record[position++]);
      }
    }
    fields.push_back(std::move(field));
    if (position == record.size()) break;
    ++position;
    if (position == record.size()) {
      fields.emplace_back();
      break;
    }
  }
  return fields;
}

void nominal_fifo() {
  const auto report = ael::simulate(
      {job("control", 0, 0, 2, 10, 20, 1), job("sensor", 0, 3, 2, 10, 20, 2)},
      ael::Policy::fifo);
  check(report.clean(), "nominal FIFO should be clean");
  check(report.completed == 2, "nominal FIFO should complete two jobs");
}

void priority_changes_dispatch() {
  const std::vector<ael::Job> jobs = {
      job("slow", 0, 0, 3, 20, 20, 5), job("urgent", 0, 0, 1, 20, 20, 1)};
  const auto fifo = ael::simulate(jobs, ael::Policy::fifo);
  const auto fixed = ael::simulate(jobs, ael::Policy::fixed_priority);
  check(fifo.trace.front().job.stream == "slow", "FIFO should retain arrival order");
  check(fixed.trace.front().job.stream == "urgent", "fixed priority should dispatch urgent job");
}

void simultaneous_zero_duration_outcomes_preserve_dispatch_order() {
  auto first = job("z", 0, 10, 1, 20, 0, 0);
  auto second = job("a", 0, 10, 1, 20, 0, 0);
  first.source_ms = 0;
  second.source_ms = 0;

  const auto report = ael::simulate({first, second}, ael::Policy::fixed_priority);
  check(report.trace.size() == 2, "two stale jobs must produce two trace rows");
  if (report.trace.size() != 2) return;
  check(report.trace[0].job.stream == "z" && report.trace[1].job.stream == "a",
        "equal-time outcomes must retain scheduler dispatch order, not lexical order");
  check(report.trace[0].trace_ordinal == 0 && report.trace[1].trace_ordinal == 1,
        "trace ordinals must record monotonically increasing dispatch order");
  check(report.trace[0].start_ms == 10 && report.trace[1].start_ms == 10,
        "stale zero-duration outcomes must retain their shared event time");
  check(report.trace_fingerprint == ael::trace_fingerprint_v2(report.trace),
        "report fingerprint must use the v2 trace-ordinal binding");

  const TemporaryFile trace("");
  ael::write_trace_csv(trace.path(), report);
  std::ifstream input(trace.path(), std::ios::binary);
  std::string header;
  std::string row0;
  std::string row1;
  std::getline(input, header);
  std::getline(input, row0);
  std::getline(input, row1);
  const auto fields0 = decode_csv_record_for_test(row0);
  const auto fields1 = decode_csv_record_for_test(row1);
  check(header.rfind("trace_ordinal,", 0) == 0,
        "trace CSV must expose its event-order field in the schema");
  check(fields0.size() == 11 && fields1.size() == 11,
        "trace CSV rows must contain the v2 eleven-field schema");
  if (fields0.size() == 11 && fields1.size() == 11) {
    check(fields0[0] == "0" && fields0[1] == "z" &&
              fields1[0] == "1" && fields1[1] == "a",
          "trace CSV must preserve scheduler order for equal-time outcomes");
  }
}

void latest_only_supersedes() {
  const std::vector<ael::Job> jobs = {
      job("blocker", 0, 0, 10, 30, 30, 0), job("camera", 1, 1, 1, 20, 30, 2),
      job("camera", 2, 2, 1, 20, 30, 2)};
  const auto report = ael::simulate(jobs, ael::Policy::latest_only);
  check(report.superseded == 1, "latest-only should supersede one queued sample");
  check(report.trace.size() == 3, "superseded sample should remain visible in trace");
  check_aggregates_match_trace(report, "basic latest-only report");
}

void superseded_samples_contribute_to_aggregates() {
  auto old_camera = job("camera", 1, 1, 1, 200, 1000, 1);
  auto new_camera = job("camera", 2, 100, 1, 200, 1000, 1);
  const auto report = ael::simulate(
      {job("blocker", 0, 0, 100, 200, 1000, 0), old_camera, new_camera},
      ael::Policy::latest_only);
  check(report.superseded == 1,
        "delayed camera sample should be superseded exactly once");
  check(report.maximum_age_ms == 99,
        "superseded age must contribute to maximum age");
  check(report.maximum_response_ms == 100,
        "maximum response must match the complete trace including blocker");
  check_aggregates_match_trace(report, "superseded-age report");
}

void deadline_and_staleness() {
  const auto miss = ael::simulate({job("a", 0, 0, 8, 3, 20, 1)}, ael::Policy::fifo);
  check(miss.deadline_misses == 1, "execution overrun should miss deadline");

  auto stale = job("stale", 0, 10, 1, 20, 5, 1);
  stale.source_ms = 0;
  const auto rejected = ael::simulate({stale}, ael::Policy::fifo);
  check(rejected.stale_rejections == 1, "old source timestamp should reject stale job");
  check(rejected.makespan_ms == 10, "stale job should consume no execution time");
}

void deterministic_fingerprint() {
  const std::vector<ael::Job> jobs = {job("a", 0, 0, 1, 10, 10, 1), job("b", 0, 0, 1, 10, 10, 1)};
  const auto first = ael::simulate(jobs, ael::Policy::earliest_deadline);
  const auto second = ael::simulate(jobs, ael::Policy::earliest_deadline);
  check(first.trace_fingerprint == second.trace_fingerprint, "fingerprint must be deterministic");
  check(first.trace_fingerprint.rfind("sha256-v2:", 0) == 0 && first.trace_fingerprint.size() == 74,
        "fingerprint must be a versioned SHA-256 digest");
}

void rejects_invalid_utf8_stream() {
  bool threw = false;
  try {
    (void)ael::simulate({job(std::string(1, static_cast<char>(0xff)), 0, 0, 1, 10, 10, 1)},
                        ael::Policy::fifo);
  } catch (const std::invalid_argument& error) {
    threw = true;
    check(std::string(error.what()) == "stream must be valid UTF-8",
          "invalid UTF-8 should have a deterministic diagnostic");
  }
  check(threw, "stream containing byte 0xff must be rejected before simulation");
}

void rejects_malformed_utf8_classes() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {raw_bytes({0x80}), "unexpected continuation"},
      {raw_bytes({0xe2, 0x28, 0xa1}), "invalid continuation"},
      {raw_bytes({0xe2, 0x82}), "truncated sequence"},
      {raw_bytes({0xc0, 0xaf}), "two-byte overlong encoding"},
      {raw_bytes({0xe0, 0x80, 0xaf}), "three-byte overlong encoding"},
      {raw_bytes({0xf0, 0x80, 0x80, 0xaf}), "four-byte overlong encoding"},
      {raw_bytes({0xed, 0xa0, 0x80}), "surrogate code point"},
      {raw_bytes({0xf4, 0x90, 0x80, 0x80}), "code point above U+10FFFF"}};
  for (const auto& [stream, label] : cases) {
    bool threw = false;
    try {
      (void)ael::simulate({job(stream, 0, 0, 1, 10, 10, 1)}, ael::Policy::fifo);
    } catch (const std::invalid_argument& error) {
      threw = true;
      check(std::string(error.what()) == "stream must be valid UTF-8",
            label + " should have the invalid UTF-8 diagnostic");
    }
    check(threw, label + " must be rejected before simulation");
  }
}

void csv_reader_rejects_invalid_utf8_stream() {
  const TemporaryFile workload(workload_csv(raw_bytes({0xff})));
  bool threw = false;
  try {
    (void)ael::read_workload_csv(workload.path());
  } catch (const std::invalid_argument& error) {
    threw = true;
    check(std::string(error.what()) == "stream must be valid UTF-8",
          "CSV invalid UTF-8 should have a deterministic diagnostic");
  }
  check(threw, "CSV reader must reject byte 0xff in a stream identifier");
}

void csv_reader_rejects_ctrl_z_without_accepting_a_valid_prefix() {
  std::string contents =
      "stream,sequence,release_ms,execution_ms,deadline_ms,source_ms,max_age_ms,priority\n"
      "camera,0,0,1,10,0,10,1\n";
  contents.push_back(static_cast<char>(0x1a));
  contents += "camera,1,1,1,10,1,10,1\n";
  const TemporaryFile workload(contents);

  bool threw = false;
  try {
    (void)ael::read_workload_csv(workload.path());
  } catch (const std::invalid_argument& error) {
    threw = true;
    check(std::string(error.what()) == "line 3: CTRL-Z is not permitted in a workload",
          "CTRL-Z should have a deterministic binary-input diagnostic");
  }
  check(threw, "CSV reader must reject CTRL-Z and the complete suffix on every platform");
}

void rejects_control_characters_in_stream() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {raw_bytes({0x00}), "NUL"},
      {raw_bytes({0x09}), "ASCII tab"},
      {raw_bytes({0x7f}), "ASCII DEL"},
      {raw_bytes({0xc2, 0x85}), "Unicode C1 control"}};
  for (const auto& [stream, label] : cases) {
    bool threw = false;
    try {
      (void)ael::simulate({job("prefix" + stream, 0, 0, 1, 10, 10, 1)}, ael::Policy::fifo);
    } catch (const std::invalid_argument& error) {
      threw = true;
      check(std::string(error.what()) == "stream contains unsupported control character",
            label + " should have the unsupported-control diagnostic");
    }
    check(threw, label + " must be rejected before simulation");
  }
}

void valid_multibyte_stream_round_trips() {
  const std::string stream =
      raw_bytes({0x63, 0x61, 0x66, 0xc3, 0xa9, 0x2f, 0xe5, 0xa7, 0xbf, 0xe6, 0x80,
                 0x81, 0x2f, 0xf0, 0x9f, 0x9a, 0x81});  // 2-, 3-, and 4-byte characters
  const TemporaryFile workload(workload_csv(stream));
  const auto jobs = ael::read_workload_csv(workload.path());
  check(jobs.size() == 1 && jobs.front().stream == stream,
        "CSV reader must preserve valid multibyte UTF-8 bytes");

  const auto first = ael::simulate(jobs, ael::Policy::fifo);
  const auto second = ael::simulate(jobs, ael::Policy::fifo);
  check(first.trace.front().job.stream == stream,
        "simulation trace must preserve a multibyte stream identifier");
  check(first.trace_fingerprint == second.trace_fingerprint,
        "multibyte stream fingerprinting must remain deterministic");

  const TemporaryFile trace("");
  ael::write_trace_csv(trace.path(), first);
  std::ifstream input(trace.path(), std::ios::binary);
  const std::string trace_bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  check(trace_bytes.find("\n0," + stream + ",0,") != std::string::npos,
        "trace CSV must contain the original multibyte stream bytes");
}

void quote_bearing_stream_round_trips() {
  const std::string stream = "camera \"front\"";
  const TemporaryFile workload(
      "stream,sequence,release_ms,execution_ms,deadline_ms,source_ms,max_age_ms,priority\n"
      "\"camera \"\"front\"\"\",0,0,1,10,0,10,1\n");
  const auto jobs = ael::read_workload_csv(workload.path());
  check(jobs.size() == 1 && jobs.front().stream == stream,
        "CSV reader must decode doubled quotes without changing stream bytes");

  const auto report = ael::simulate(jobs, ael::Policy::fifo);
  const auto direct =
      ael::simulate({job(stream, 0, 0, 1, 10, 10, 1)}, ael::Policy::fifo);
  check(report.trace_fingerprint == direct.trace_fingerprint,
        "CSV quoting must not affect trace fingerprint semantics");

  const TemporaryFile trace("");
  ael::write_trace_csv(trace.path(), report);
  std::ifstream input(trace.path(), std::ios::binary);
  const std::string trace_bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  check(trace_bytes.find("\n0,\"camera \"\"front\"\"\",0,") != std::string::npos,
        "trace CSV must quote and double embedded quotes in stream identifiers");
}

void csv_reader_rejects_malformed_quote_placement() {
  struct Case {
    std::string record;
    std::string diagnostic;
    std::string label;
  };
  const std::vector<Case> cases = {
      {"cam\"\"era,0,0,1,10,0,10,1",
       "line 2: quote may begin only at a field boundary", "bare quotes"},
      {"\"camera\"x,0,0,1,10,0,10,1",
       "line 2: closing quote must be followed by comma or end",
       "text after a closing quote"},
      {"\"camera,0,0,1,10,0,10,1", "line 2: unterminated quoted field",
       "an unclosed quoted field"}};

  const std::string header =
      "stream,sequence,release_ms,execution_ms,deadline_ms,source_ms,max_age_ms,priority\n";
  for (const auto& test_case : cases) {
    const TemporaryFile workload(header + test_case.record + "\n");
    bool threw = false;
    try {
      (void)ael::read_workload_csv(workload.path());
    } catch (const std::invalid_argument& error) {
      threw = true;
      check(std::string(error.what()) == test_case.diagnostic,
            test_case.label + " should have a deterministic CSV diagnostic");
    }
    check(threw, "CSV reader must reject " + test_case.label);
  }
}

void trace_csv_text_fields_are_reparseable() {
  ael::Report report;
  ael::TraceEntry entry;
  entry.trace_ordinal = 19;
  entry.job = job("camera \"front\"", 7, 10, 3, 20, 50, 2);
  entry.start_ms = 11;
  entry.finish_ms = 14;
  entry.wait_ms = 1;
  entry.response_ms = 4;
  entry.age_at_start_ms = 1;
  entry.outcome = ael::Outcome::completed;
  entry.diagnosis = "operator said \"retry, now\"";
  report.trace.push_back(entry);
  report.trace_fingerprint = ael::trace_fingerprint_v2(report.trace);
  const std::string fingerprint = report.trace_fingerprint;

  const TemporaryFile trace("");
  ael::write_trace_csv(trace.path(), report);
  check(report.trace_fingerprint == fingerprint,
        "serializing trace CSV must not alter fingerprint semantics");
  std::ifstream input(trace.path(), std::ios::binary);
  const std::string trace_bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  const std::size_t header_end = trace_bytes.find('\n');
  const std::size_t record_end =
      header_end == std::string::npos ? std::string::npos : trace_bytes.find('\n', header_end + 1U);
  check(header_end != std::string::npos && record_end == trace_bytes.size() - 1U,
        "one trace entry must emit one complete CSV record");
  if (header_end == std::string::npos || record_end == std::string::npos) return;

  const auto fields = decode_csv_record_for_test(
      std::string_view(trace_bytes).substr(header_end + 1U, record_end - header_end - 1U));
  check(fields.size() == 11, "trace CSV record must reparse into 11 fields");
  if (fields.size() == 11) {
    check(fields[0] == "19", "reparsed trace ordinal must preserve event order");
    check(fields[1] == entry.job.stream,
          "reparsed trace stream must preserve embedded quote bytes");
    check(fields[9] == "completed", "reparsed trace outcome must preserve text");
    check(fields[10] == entry.diagnosis,
          "reparsed trace diagnosis must preserve comma and quote bytes");
  }
  check(trace_bytes.find("\"operator said \"\"retry, now\"\"\"") != std::string::npos,
        "trace encoder must quote commas and double embedded quotes");
}

void csv_reader_retains_stream_comma_prohibition() {
  const TemporaryFile workload(
      "stream,sequence,release_ms,execution_ms,deadline_ms,source_ms,max_age_ms,priority\n"
      "\"camera,front\",0,0,1,10,0,10,1\n");
  bool threw = false;
  try {
    (void)ael::read_workload_csv(workload.path());
  } catch (const std::invalid_argument& error) {
    threw = true;
    check(std::string(error.what()) == "stream contains unsupported CSV characters",
          "decoded stream commas should retain the identifier-policy diagnostic");
  }
  check(threw, "quoted CSV syntax must not bypass the stream comma prohibition");
}

template <typename Mutation>
void check_fingerprint_field(const ael::TraceEntry& baseline, const std::string& label,
                             Mutation mutate) {
  auto changed = baseline;
  mutate(changed);
  check(ael::trace_fingerprint_v2({baseline}) != ael::trace_fingerprint_v2({changed}),
        "fingerprint must bind " + label);
}

void fingerprint_binds_every_trace_field() {
  ael::TraceEntry entry;
  entry.trace_ordinal = 9;
  entry.job = job("camera", 7, 10, 3, 20, 50, 2);
  entry.job.source_ms = 8;
  entry.start_ms = 11;
  entry.finish_ms = 14;
  entry.wait_ms = 1;
  entry.response_ms = 4;
  entry.age_at_start_ms = 3;
  entry.outcome = ael::Outcome::completed;
  entry.diagnosis = "within contract";
  check(ael::trace_fingerprint_v2({entry}) ==
            "sha256-v2:2591dc6a4c0f91ef567abb2dd956f7610056eda41eac8eaf8056f6509783ac69",
        "fingerprint must match the independently computed SHA-256 vector");
  check_fingerprint_field(entry, "trace ordinal", [](auto& item) { ++item.trace_ordinal; });
  check_fingerprint_field(entry, "stream", [](auto& item) { item.job.stream += "-2"; });
  check_fingerprint_field(entry, "sequence", [](auto& item) { ++item.job.sequence; });
  check_fingerprint_field(entry, "release", [](auto& item) { ++item.job.release_ms; });
  check_fingerprint_field(entry, "execution", [](auto& item) { ++item.job.execution_ms; });
  check_fingerprint_field(entry, "deadline", [](auto& item) { ++item.job.deadline_ms; });
  check_fingerprint_field(entry, "source", [](auto& item) { ++item.job.source_ms; });
  check_fingerprint_field(entry, "max age", [](auto& item) { ++item.job.max_age_ms; });
  check_fingerprint_field(entry, "priority", [](auto& item) { ++item.job.priority; });
  check_fingerprint_field(entry, "start", [](auto& item) { ++item.start_ms; });
  check_fingerprint_field(entry, "finish", [](auto& item) { ++item.finish_ms; });
  check_fingerprint_field(entry, "wait", [](auto& item) { ++item.wait_ms; });
  check_fingerprint_field(entry, "response", [](auto& item) { ++item.response_ms; });
  check_fingerprint_field(entry, "age", [](auto& item) { ++item.age_at_start_ms; });
  check_fingerprint_field(entry, "outcome", [](auto& item) { item.outcome = ael::Outcome::deadline_miss; });
  check_fingerprint_field(entry, "diagnosis", [](auto& item) { item.diagnosis += " changed"; });
}

void latest_only_sequence_contract() {
  const auto expect_rejected = [](const std::vector<ael::Job>& jobs, const std::string& label) {
    bool threw = false;
    try {
      (void)ael::simulate(jobs, ael::Policy::latest_only);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    check(threw, "latest-only must reject " + label);
  };
  expect_rejected({job("camera", 2, 1, 1, 20, 20, 1),
                   job("camera", 1, 2, 1, 20, 20, 1)},
                  "decreasing per-stream sequence");
  expect_rejected({job("camera", 2, 1, 1, 20, 20, 1),
                   job("camera", 1, 1, 1, 20, 20, 1)},
                  "out-of-order sequence at the same release");
  expect_rejected({job("camera", 1, 1, 1, 20, 20, 1),
                   job("camera", 1, 2, 1, 20, 20, 1)},
                  "repeated per-stream sequence");

  const auto accepted = ael::simulate(
      {job("camera", 2, 2, 1, 20, 20, 1), job("camera", 1, 1, 1, 20, 20, 1)},
      ael::Policy::latest_only);
  check(accepted.completed == 2,
        "latest-only should use release order rather than input file order for monotonicity");
}

void rejects_unrepresentable_absolute_deadline() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::vector<ael::Job> jobs = {
      job("overflow", 0, maximum - 10, 1, 11, maximum, 1),
      job("valid-peer", 0, maximum - 10, 1, 10, maximum, 1)};
  for (const auto policy : {ael::Policy::fifo, ael::Policy::fixed_priority,
                            ael::Policy::earliest_deadline, ael::Policy::latest_only}) {
    bool threw = false;
    try {
      (void)ael::simulate(jobs, policy);
    } catch (const std::overflow_error& error) {
      threw = true;
      check(std::string(error.what()) == "absolute deadline overflow",
            "absolute deadline overflow should have a deterministic diagnostic");
    }
    check(threw, "absolute deadline beyond INT64_MAX must be rejected under " +
                     ael::to_string(policy));
  }
}

void edf_compares_deadlines_near_int64_max() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::vector<ael::Job> jobs = {
      job("deadline-max", 0, maximum - 10, 1, 10, maximum, 1),
      job("deadline-max-minus-one", 0, maximum - 10, 1, 9, maximum, 1)};
  const auto report = ael::simulate(jobs, ael::Policy::earliest_deadline);
  check(report.clean(), "near-boundary EDF workload should be clean");
  check(report.trace.front().job.stream == "deadline-max-minus-one",
        "EDF must compare representable absolute deadlines near INT64_MAX correctly");
}

void rejects_unrepresentable_finish_time() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  bool threw = false;
  try {
    (void)ael::simulate({job("finish-overflow", 0, maximum - 2, 3, 2, maximum, 1)},
                        ael::Policy::fifo);
  } catch (const std::overflow_error& error) {
    threw = true;
    check(std::string(error.what()) == "simulated time overflow",
          "finish overflow should preserve its deterministic diagnostic");
  }
  check(threw, "finish time beyond INT64_MAX must be rejected");
}

void derived_times_reach_int64_max_without_overflow() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::vector<ael::Job> jobs = {
      job("blocker", 0, 0, maximum, maximum, maximum, 0),
      job("stale", 0, 0, 1, maximum, maximum - 1, 1)};
  const auto report = ael::simulate(jobs, ael::Policy::fifo);
  check(report.completed == 1, "job finishing at INT64_MAX should complete");
  check(report.stale_rejections == 1, "maximally delayed stale job should be rejected");
  check(report.trace.back().start_ms == maximum, "dispatch time should reach INT64_MAX exactly");
  check(report.trace.back().finish_ms == maximum, "stale job should finish at its dispatch time");
  check(report.trace.back().wait_ms == maximum, "wait should reach INT64_MAX exactly");
  check(report.trace.back().response_ms == maximum, "response should reach INT64_MAX exactly");
  check(report.trace.back().age_at_start_ms == maximum, "age should reach INT64_MAX exactly");
  check(report.makespan_ms == maximum, "makespan should reach INT64_MAX exactly");
  check(report.maximum_response_ms == maximum, "maximum response should retain INT64_MAX");
  check(report.maximum_age_ms == maximum, "maximum age should retain INT64_MAX");
}

void supersession_times_remain_exact_near_int64_max() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  auto newest = job("camera", 2, maximum - 1, 1, 1, maximum - 2, 2);
  newest.source_ms = 0;
  const std::vector<ael::Job> jobs = {
      job("blocker", 0, 0, maximum - 1, maximum, maximum, 0),
      job("camera", 1, 0, 1, maximum, maximum, 1), newest};
  const auto report = ael::simulate(jobs, ael::Policy::latest_only);
  check(report.superseded == 1, "near-boundary latest-only workload should supersede once");

  const ael::TraceEntry* superseded = nullptr;
  for (const auto& entry : report.trace) {
    if (entry.outcome == ael::Outcome::superseded) {
      superseded = &entry;
      break;
    }
  }
  check(superseded != nullptr, "superseded job should remain in the trace");
  if (superseded != nullptr) {
    check(superseded->start_ms == maximum - 1,
          "supersession should occur at the newer release time");
    check(superseded->finish_ms == maximum - 1,
          "superseded sample should consume no execution time");
    check(superseded->wait_ms == maximum - 1,
          "supersession wait should remain exact near INT64_MAX");
    check(superseded->response_ms == maximum - 1,
          "supersession response should remain exact near INT64_MAX");
    check(superseded->age_at_start_ms == maximum - 1,
          "supersession age should remain exact near INT64_MAX");
  }
}

void invalid_input() {
  bool threw = false;
  try {
    (void)ael::simulate({}, ael::Policy::fifo);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "empty workload must be rejected");

  threw = false;
  try {
    const auto duplicate = job("same", 3, 0, 1, 5, 5, 1);
    (void)ael::simulate({duplicate, duplicate}, ael::Policy::fifo);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "duplicate stream/sequence identity must be rejected");
}

}  // namespace

int main() {
  nominal_fifo();
  priority_changes_dispatch();
  simultaneous_zero_duration_outcomes_preserve_dispatch_order();
  latest_only_supersedes();
  superseded_samples_contribute_to_aggregates();
  deadline_and_staleness();
  deterministic_fingerprint();
  rejects_invalid_utf8_stream();
  rejects_malformed_utf8_classes();
  csv_reader_rejects_invalid_utf8_stream();
  csv_reader_rejects_ctrl_z_without_accepting_a_valid_prefix();
  rejects_control_characters_in_stream();
  valid_multibyte_stream_round_trips();
  quote_bearing_stream_round_trips();
  csv_reader_rejects_malformed_quote_placement();
  trace_csv_text_fields_are_reparseable();
  csv_reader_retains_stream_comma_prohibition();
  fingerprint_binds_every_trace_field();
  latest_only_sequence_contract();
  rejects_unrepresentable_absolute_deadline();
  edf_compares_deadlines_near_int64_max();
  rejects_unrepresentable_finish_time();
  derived_times_reach_int64_max_without_overflow();
  supersession_times_remain_exact_near_int64_max();
  invalid_input();
  if (failures == 0) {
    std::cout << "all executor tests passed\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " executor test(s) failed\n";
  return EXIT_FAILURE;
}
