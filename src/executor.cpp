#include "ael/executor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace ael {
namespace {

struct IndexedJob {
  Job job;
  std::size_t ordinal{};
  std::int64_t absolute_deadline_ms{};
};

std::array<std::string, 8> parse_workload_csv_record(const std::string& line,
                                                     std::size_t line_number) {
  enum class State { field_boundary, unquoted, quoted, quote_closed };

  std::array<std::string, 8> fields;
  std::size_t field_index = 0;
  State state = State::field_boundary;
  const auto reject = [line_number](std::string_view reason) {
    throw std::invalid_argument("line " + std::to_string(line_number) + ": " +
                                std::string(reason));
  };
  const auto next_field = [&field_index, &state, &reject, &fields]() {
    if (field_index + 1U >= fields.size()) {
      reject("CSV record must have exactly 8 fields");
    }
    ++field_index;
    state = State::field_boundary;
  };

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    switch (state) {
      case State::field_boundary:
        if (ch == '"') {
          state = State::quoted;
        } else if (ch == ',') {
          next_field();
        } else {
          fields[field_index].push_back(ch);
          state = State::unquoted;
        }
        break;
      case State::unquoted:
        if (ch == '"') {
          reject("quote may begin only at a field boundary");
        } else if (ch == ',') {
          next_field();
        } else {
          fields[field_index].push_back(ch);
        }
        break;
      case State::quoted:
        if (ch != '"') {
          fields[field_index].push_back(ch);
        } else if (i + 1U < line.size() && line[i + 1U] == '"') {
          fields[field_index].push_back('"');
          ++i;
        } else {
          state = State::quote_closed;
        }
        break;
      case State::quote_closed:
        if (ch != ',') {
          reject("closing quote must be followed by comma or end");
        }
        next_field();
        break;
    }
  }
  if (state == State::quoted) {
    reject("unterminated quoted field");
  }
  if (field_index + 1U != fields.size()) {
    reject("CSV record must have exactly 8 fields");
  }
  return fields;
}

void normalize_workload_line(std::string& line, std::size_t line_number,
                             bool reached_eof_without_lf) {
  if (!line.empty() && line.back() == '\r') {
    if (reached_eof_without_lf) {
      throw std::invalid_argument("line " + std::to_string(line_number) +
                                  ": bare carriage return is not a line ending");
    }
    line.pop_back();
  }
  if (line.find('\r') != std::string::npos) {
    throw std::invalid_argument("line " + std::to_string(line_number) +
                                ": embedded carriage return is not permitted");
  }
  if (line.find(static_cast<char>(0x1a)) != std::string::npos) {
    throw std::invalid_argument("line " + std::to_string(line_number) +
                                ": CTRL-Z is not permitted in a workload");
  }
}

std::string encode_csv_field(std::string_view value) {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string(value);
  }

  std::string encoded;
  encoded.reserve(value.size() + 2U);
  encoded.push_back('"');
  for (const char ch : value) {
    if (ch == '"') encoded.push_back('"');
    encoded.push_back(ch);
  }
  encoded.push_back('"');
  return encoded;
}

template <typename T>
T parse_integer(const std::string& text, const char* name) {
  T value{};
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw std::invalid_argument(std::string("invalid integer in ") + name + ": " + text);
  }
  return value;
}

std::int64_t checked_add(std::int64_t left, std::int64_t right, const char* calculation) {
  constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  if ((right > 0 && left > maximum - right) || (right < 0 && left < minimum - right)) {
    throw std::overflow_error(std::string(calculation) + " overflow");
  }
  return left + right;
}

std::int64_t checked_subtract(std::int64_t left, std::int64_t right, const char* calculation) {
  constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  if ((right > 0 && left < minimum + right) || (right < 0 && left > maximum + right)) {
    throw std::overflow_error(std::string(calculation) + " overflow");
  }
  return left - right;
}

bool decode_utf8_code_point(std::string_view text, std::size_t& offset,
                            std::uint32_t& code_point) {
  const auto first = static_cast<unsigned char>(text[offset]);
  std::size_t length = 0;
  std::uint32_t minimum = 0;
  if (first <= 0x7fU) {
    length = 1;
    minimum = 0;
    code_point = first;
  } else if (first >= 0xc2U && first <= 0xdfU) {
    length = 2;
    minimum = 0x80U;
    code_point = first & 0x1fU;
  } else if (first >= 0xe0U && first <= 0xefU) {
    length = 3;
    minimum = 0x800U;
    code_point = first & 0x0fU;
  } else if (first >= 0xf0U && first <= 0xf4U) {
    length = 4;
    minimum = 0x10000U;
    code_point = first & 0x07U;
  } else {
    return false;
  }

  if (text.size() - offset < length) return false;
  for (std::size_t i = 1; i < length; ++i) {
    const auto continuation = static_cast<unsigned char>(text[offset + i]);
    if ((continuation & 0xc0U) != 0x80U) return false;
    code_point = (code_point << 6U) | (continuation & 0x3fU);
  }
  if (code_point < minimum || (code_point >= 0xd800U && code_point <= 0xdfffU) ||
      code_point > 0x10ffffU) {
    return false;
  }
  offset += length;
  return true;
}

void validate_stream_identifier(std::string_view stream) {
  std::size_t offset = 0;
  while (offset < stream.size()) {
    std::uint32_t code_point = 0;
    if (!decode_utf8_code_point(stream, offset, code_point)) {
      throw std::invalid_argument("stream must be valid UTF-8");
    }
    if (code_point <= 0x1fU || (code_point >= 0x7fU && code_point <= 0x9fU)) {
      throw std::invalid_argument("stream contains unsupported control character");
    }
  }
}

std::int64_t validate_job(const Job& job) {
  if (job.stream.empty()) {
    throw std::invalid_argument("stream must be non-empty");
  }
  if (job.stream.size() > 256U) {
    throw std::invalid_argument("stream exceeds 256 bytes");
  }
  if (job.stream.find_first_of(",\r\n") != std::string::npos) {
    throw std::invalid_argument("stream contains unsupported CSV characters");
  }
  validate_stream_identifier(job.stream);
  if (job.release_ms < 0 || job.execution_ms <= 0 || job.deadline_ms <= 0 || job.source_ms < 0 ||
      job.max_age_ms < 0) {
    throw std::invalid_argument("times must be non-negative and execution/deadline must be positive");
  }
  if (job.source_ms > job.release_ms) {
    throw std::invalid_argument("source_ms cannot be after release_ms");
  }
  return checked_add(job.release_ms, job.deadline_ms, "absolute deadline");
}

void derive_report_aggregates(Report& report) {
  report.completed = 0;
  report.deadline_misses = 0;
  report.stale_rejections = 0;
  report.superseded = 0;
  report.makespan_ms = 0;
  report.maximum_response_ms = 0;
  report.maximum_age_ms = 0;
  for (const auto& entry : report.trace) {
    switch (entry.outcome) {
      case Outcome::completed:
        ++report.completed;
        break;
      case Outcome::deadline_miss:
        ++report.deadline_misses;
        break;
      case Outcome::rejected_stale:
        ++report.stale_rejections;
        break;
      case Outcome::superseded:
        ++report.superseded;
        break;
    }
    report.makespan_ms = std::max(report.makespan_ms, entry.finish_ms);
    report.maximum_response_ms =
        std::max(report.maximum_response_ms, entry.response_ms);
    report.maximum_age_ms =
        std::max(report.maximum_age_ms, entry.age_at_start_ms);
  }
}

std::size_t choose(const std::vector<IndexedJob>& ready, Policy policy) {
  if (ready.empty()) {
    throw std::logic_error("cannot choose from an empty ready queue");
  }
  auto better = [policy](const IndexedJob& a, const IndexedJob& b) {
    if (policy == Policy::fixed_priority || policy == Policy::latest_only) {
      return std::tie(a.job.priority, a.job.release_ms, a.ordinal) <
             std::tie(b.job.priority, b.job.release_ms, b.ordinal);
    }
    if (policy == Policy::earliest_deadline) {
      return std::tie(a.absolute_deadline_ms, a.job.release_ms, a.ordinal) <
             std::tie(b.absolute_deadline_ms, b.job.release_ms, b.ordinal);
    }
    return std::tie(a.job.release_ms, a.ordinal) < std::tie(b.job.release_ms, b.ordinal);
  };
  std::size_t best = 0;
  for (std::size_t i = 1; i < ready.size(); ++i) {
    if (better(ready[i], ready[best])) {
      best = i;
    }
  }
  return best;
}

std::string escape_json(std::string_view value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20U) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
              << std::dec;
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

constexpr std::array<std::uint32_t, 64> sha256_round_constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t rotate_right(std::uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

std::string sha256_hex(std::vector<std::uint8_t> bytes) {
  if (bytes.size() > std::numeric_limits<std::uint64_t>::max() / 8U) {
    throw std::length_error("trace is too large to fingerprint");
  }
  const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
  bytes.push_back(0x80U);
  while (bytes.size() % 64U != 56U) bytes.push_back(0U);
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
  }

  std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                        0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                        0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t offset = 0; offset < bytes.size(); offset += 64U) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16U; ++i) {
      const std::size_t base = offset + i * 4U;
      words[i] = (static_cast<std::uint32_t>(bytes[base]) << 24U) |
                 (static_cast<std::uint32_t>(bytes[base + 1U]) << 16U) |
                 (static_cast<std::uint32_t>(bytes[base + 2U]) << 8U) |
                 static_cast<std::uint32_t>(bytes[base + 3U]);
    }
    for (std::size_t i = 16U; i < words.size(); ++i) {
      const auto s0 = rotate_right(words[i - 15U], 7U) ^ rotate_right(words[i - 15U], 18U) ^
                      (words[i - 15U] >> 3U);
      const auto s1 = rotate_right(words[i - 2U], 17U) ^ rotate_right(words[i - 2U], 19U) ^
                      (words[i - 2U] >> 10U);
      words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
      const auto sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temporary1 = h + sum1 + choose + sha256_round_constants[i] + words[i];
      const auto sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto word : state) out << std::setw(8) << word;
  return out.str();
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void append_i64(std::vector<std::uint8_t>& bytes, std::int64_t value) {
  append_u64(bytes, static_cast<std::uint64_t>(value));
}

void append_text(std::vector<std::uint8_t>& bytes, std::string_view value) {
  append_u64(bytes, static_cast<std::uint64_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

}  // namespace

std::string trace_fingerprint_v1(const std::vector<TraceEntry>& trace) {
  std::vector<std::uint8_t> canonical;
  constexpr std::string_view domain = "autonomy-executor-lab/trace-fingerprint/v1";
  append_text(canonical, domain);
  append_u64(canonical, static_cast<std::uint64_t>(trace.size()));
  for (const auto& entry : trace) {
    append_text(canonical, entry.job.stream);
    append_u64(canonical, entry.job.sequence);
    append_i64(canonical, entry.job.release_ms);
    append_i64(canonical, entry.job.execution_ms);
    append_i64(canonical, entry.job.deadline_ms);
    append_i64(canonical, entry.job.source_ms);
    append_i64(canonical, entry.job.max_age_ms);
    append_i64(canonical, static_cast<std::int64_t>(entry.job.priority));
    append_i64(canonical, entry.start_ms);
    append_i64(canonical, entry.finish_ms);
    append_i64(canonical, entry.wait_ms);
    append_i64(canonical, entry.response_ms);
    append_i64(canonical, entry.age_at_start_ms);
    append_u64(canonical, static_cast<std::uint64_t>(entry.outcome));
    append_text(canonical, entry.diagnosis);
  }
  return "sha256-v1:" + sha256_hex(std::move(canonical));
}

std::string trace_fingerprint_v2(const std::vector<TraceEntry>& trace) {
  std::vector<std::uint8_t> canonical;
  constexpr std::string_view domain = "autonomy-executor-lab/trace-fingerprint/v2";
  append_text(canonical, domain);
  append_u64(canonical, static_cast<std::uint64_t>(trace.size()));
  for (const auto& entry : trace) {
    append_u64(canonical, entry.trace_ordinal);
    append_text(canonical, entry.job.stream);
    append_u64(canonical, entry.job.sequence);
    append_i64(canonical, entry.job.release_ms);
    append_i64(canonical, entry.job.execution_ms);
    append_i64(canonical, entry.job.deadline_ms);
    append_i64(canonical, entry.job.source_ms);
    append_i64(canonical, entry.job.max_age_ms);
    append_i64(canonical, static_cast<std::int64_t>(entry.job.priority));
    append_i64(canonical, entry.start_ms);
    append_i64(canonical, entry.finish_ms);
    append_i64(canonical, entry.wait_ms);
    append_i64(canonical, entry.response_ms);
    append_i64(canonical, entry.age_at_start_ms);
    append_u64(canonical, static_cast<std::uint64_t>(entry.outcome));
    append_text(canonical, entry.diagnosis);
  }
  return "sha256-v2:" + sha256_hex(std::move(canonical));
}

Policy parse_policy(std::string_view value) {
  if (value == "fifo") return Policy::fifo;
  if (value == "fixed-priority") return Policy::fixed_priority;
  if (value == "edf") return Policy::earliest_deadline;
  if (value == "latest-only") return Policy::latest_only;
  throw std::invalid_argument("unknown policy: " + std::string(value));
}

std::string to_string(Policy value) {
  switch (value) {
    case Policy::fifo: return "fifo";
    case Policy::fixed_priority: return "fixed-priority";
    case Policy::earliest_deadline: return "edf";
    case Policy::latest_only: return "latest-only";
  }
  throw std::logic_error("invalid policy");
}

std::string to_string(Outcome value) {
  switch (value) {
    case Outcome::completed: return "completed";
    case Outcome::deadline_miss: return "deadline_miss";
    case Outcome::rejected_stale: return "rejected_stale";
    case Outcome::superseded: return "superseded";
  }
  throw std::logic_error("invalid outcome");
}

std::vector<Job> read_workload_csv(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open workload: " + path.string());
  }
  std::string line;
  if (!std::getline(input, line)) {
    if (input.bad()) {
      throw std::runtime_error("I/O failure while reading workload: " + path.string());
    }
    throw std::invalid_argument("workload is empty");
  }
  normalize_workload_line(line, 1U, input.eof());
  const std::string expected =
      "stream,sequence,release_ms,execution_ms,deadline_ms,source_ms,max_age_ms,priority";
  if (line != expected) {
    throw std::invalid_argument("unexpected CSV header; expected: " + expected);
  }
  std::vector<Job> jobs;
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    normalize_workload_line(line, line_number, input.eof());
    if (line.empty()) continue;
    const auto fields = parse_workload_csv_record(line, line_number);
    Job job{
        fields[0],
        parse_integer<std::uint64_t>(fields[1], "sequence"),
        parse_integer<std::int64_t>(fields[2], "release_ms"),
        parse_integer<std::int64_t>(fields[3], "execution_ms"),
        parse_integer<std::int64_t>(fields[4], "deadline_ms"),
        parse_integer<std::int64_t>(fields[5], "source_ms"),
        parse_integer<std::int64_t>(fields[6], "max_age_ms"),
        parse_integer<int>(fields[7], "priority")};
    (void)validate_job(job);
    jobs.push_back(std::move(job));
    if (jobs.size() > 1'000'000U) {
      throw std::invalid_argument("workload exceeds one million jobs");
    }
  }
  if (input.bad()) {
    throw std::runtime_error("I/O failure while reading workload: " + path.string());
  }
  if (jobs.empty()) {
    throw std::invalid_argument("workload contains no jobs");
  }
  return jobs;
}

Report simulate(const std::vector<Job>& workload, Policy policy) {
  if (workload.empty()) {
    throw std::invalid_argument("workload contains no jobs");
  }
  if (workload.size() > 1'000'000U) {
    throw std::invalid_argument("workload exceeds one million jobs");
  }
  std::vector<IndexedJob> incoming;
  incoming.reserve(workload.size());
  std::unordered_set<std::string> identities;
  for (std::size_t i = 0; i < workload.size(); ++i) {
    const auto absolute_deadline_ms = validate_job(workload[i]);
    const std::string identity = workload[i].stream + "\x1f" + std::to_string(workload[i].sequence);
    if (!identities.insert(identity).second) {
      throw std::invalid_argument("duplicate stream/sequence identity: " + workload[i].stream + "/" +
                                  std::to_string(workload[i].sequence));
    }
    incoming.push_back({workload[i], i, absolute_deadline_ms});
  }
  std::stable_sort(incoming.begin(), incoming.end(), [](const auto& a, const auto& b) {
    return std::tie(a.job.release_ms, a.ordinal) < std::tie(b.job.release_ms, b.ordinal);
  });
  if (policy == Policy::latest_only) {
    std::unordered_map<std::string, std::uint64_t> last_sequence;
    for (const auto& indexed : incoming) {
      const auto previous = last_sequence.find(indexed.job.stream);
      if (previous != last_sequence.end() && indexed.job.sequence <= previous->second) {
        throw std::invalid_argument(
            "latest-only requires strictly increasing per-stream sequence in release order: " +
            indexed.job.stream);
      }
      last_sequence[indexed.job.stream] = indexed.job.sequence;
    }
  }

  Report report;
  report.policy = policy;
  std::vector<IndexedJob> ready;
  std::size_t next = 0;
  std::int64_t now = 0;
  std::uint64_t next_trace_ordinal = 0;

  const auto supersede_older = [&report, &ready, &next_trace_ordinal](const IndexedJob& newest) {
    auto it = ready.begin();
    while (it != ready.end()) {
      if (it->job.stream == newest.job.stream && it->job.sequence < newest.job.sequence) {
        TraceEntry entry;
        entry.trace_ordinal = next_trace_ordinal++;
        entry.job = it->job;
        entry.start_ms = newest.job.release_ms;
        entry.finish_ms = newest.job.release_ms;
        entry.wait_ms =
            checked_subtract(entry.start_ms, it->job.release_ms, "supersession wait time");
        entry.response_ms =
            checked_subtract(entry.finish_ms, it->job.release_ms, "supersession response time");
        entry.age_at_start_ms =
            checked_subtract(entry.start_ms, it->job.source_ms, "supersession age");
        entry.outcome = Outcome::superseded;
        entry.diagnosis = "newer sample arrived before dispatch";
        report.trace.push_back(std::move(entry));
        it = ready.erase(it);
      } else {
        ++it;
      }
    }
  };

  while (next < incoming.size() || !ready.empty()) {
    if (ready.empty() && next < incoming.size() && now < incoming[next].job.release_ms) {
      now = incoming[next].job.release_ms;
    }
    while (next < incoming.size() && incoming[next].job.release_ms <= now) {
      if (policy == Policy::latest_only) supersede_older(incoming[next]);
      ready.push_back(incoming[next]);
      ++next;
    }
    if (ready.empty()) continue;

    const std::size_t selected = choose(ready, policy);
    IndexedJob indexed = ready[selected];
    ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(selected));
    TraceEntry entry;
    entry.trace_ordinal = next_trace_ordinal++;
    entry.job = indexed.job;
    entry.start_ms = std::max(now, indexed.job.release_ms);
    entry.wait_ms = checked_subtract(entry.start_ms, indexed.job.release_ms, "wait time");
    entry.age_at_start_ms =
        checked_subtract(entry.start_ms, indexed.job.source_ms, "age at start");

    if (entry.age_at_start_ms > indexed.job.max_age_ms) {
      entry.finish_ms = entry.start_ms;
      entry.response_ms =
          checked_subtract(entry.finish_ms, indexed.job.release_ms, "response time");
      entry.outcome = Outcome::rejected_stale;
      entry.diagnosis = "queueing made input older than its freshness contract";
    } else {
      entry.finish_ms = checked_add(entry.start_ms, indexed.job.execution_ms, "simulated time");
      entry.response_ms =
          checked_subtract(entry.finish_ms, indexed.job.release_ms, "response time");
      now = entry.finish_ms;
      if (entry.response_ms > indexed.job.deadline_ms) {
        entry.outcome = Outcome::deadline_miss;
        entry.diagnosis = entry.wait_ms > indexed.job.execution_ms
                              ? "queue interference dominated the miss"
                              : "execution demand exceeded available deadline slack";
      } else {
        entry.outcome = Outcome::completed;
        entry.diagnosis = "within deadline and freshness contracts";
      }
    }
    report.trace.push_back(std::move(entry));
  }

  std::stable_sort(report.trace.begin(), report.trace.end(), [](const auto& a, const auto& b) {
    return std::tie(a.start_ms, a.trace_ordinal) <
           std::tie(b.start_ms, b.trace_ordinal);
  });
  derive_report_aggregates(report);
  report.trace_fingerprint = trace_fingerprint_v2(report.trace);
  return report;
}

void write_trace_csv(const std::filesystem::path& path, const Report& report) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot write trace: " + path.string());
  }
  output << "trace_ordinal,stream,sequence,release_ms,start_ms,finish_ms,wait_ms,response_ms,age_at_start_ms,outcome,diagnosis\n";
  for (const auto& entry : report.trace) {
    output << entry.trace_ordinal << ',' << encode_csv_field(entry.job.stream) << ','
           << entry.job.sequence << ','
           << entry.job.release_ms << ',' << entry.start_ms << ',' << entry.finish_ms << ','
           << entry.wait_ms << ',' << entry.response_ms << ',' << entry.age_at_start_ms << ','
           << encode_csv_field(to_string(entry.outcome)) << ','
           << encode_csv_field(entry.diagnosis) << '\n';
  }
}

std::string report_json(const Report& report) {
  std::ostringstream out;
  out << "{\n"
      << "  \"policy\": \"" << escape_json(to_string(report.policy)) << "\",\n"
      << "  \"completed\": " << report.completed << ",\n"
      << "  \"deadline_misses\": " << report.deadline_misses << ",\n"
      << "  \"stale_rejections\": " << report.stale_rejections << ",\n"
      << "  \"superseded\": " << report.superseded << ",\n"
      << "  \"makespan_ms\": " << report.makespan_ms << ",\n"
      << "  \"maximum_response_ms\": " << report.maximum_response_ms << ",\n"
      << "  \"maximum_age_ms\": " << report.maximum_age_ms << ",\n"
      << "  \"trace_fingerprint\": \"" << report.trace_fingerprint << "\",\n"
      << "  \"clean\": " << (report.clean() ? "true" : "false") << "\n"
      << "}\n";
  return out.str();
}

}  // namespace ael
