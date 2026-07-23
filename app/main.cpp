#include "ael/executor.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void usage() {
  std::cerr << "usage: ael-sim WORKLOAD.csv --policy fifo|fixed-priority|edf|latest-only "
               "[--trace TRACE.csv] [--strict]\n";
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (ch >= 0x20U) escaped.push_back(static_cast<char>(ch));
    }
  }
  return escaped;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    usage();
    return 2;
  }
  try {
    std::filesystem::path workload = argv[1];
    ael::Policy policy = ael::Policy::fifo;
    std::filesystem::path trace;
    bool strict = false;
    bool policy_set = false;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--policy" && i + 1 < argc) {
        policy = ael::parse_policy(argv[++i]);
        policy_set = true;
      } else if (arg == "--trace" && i + 1 < argc) {
        trace = argv[++i];
      } else if (arg == "--strict") {
        strict = true;
      } else {
        throw std::invalid_argument("unknown or incomplete argument: " + arg);
      }
    }
    if (!policy_set) throw std::invalid_argument("--policy is required");
    const auto report = ael::simulate(ael::read_workload_csv(workload), policy);
    if (!trace.empty()) ael::write_trace_csv(trace, report);
    std::cout << ael::report_json(report);
    return strict && !report.clean() ? 1 : 0;
  } catch (const std::exception& exc) {
    std::cerr << "{\"status\":\"error\",\"message\":\"" << json_escape(exc.what()) << "\"}\n";
    return 2;
  }
}
