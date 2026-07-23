#include <ael/executor.hpp>

int main() {
  const ael::Job job{"control", 0, 0, 1, 5, 0, 5, 0};
  const auto report = ael::simulate({job}, ael::Policy::fifo);
  return report.clean() && report.completed == 1 ? 0 : 1;
}
