#include "apitrace/api.hpp"

#include <iostream>
#include <string_view>

namespace {

void print_usage(std::string_view argv0)
{
  std::cerr << "usage: " << argv0 << " <trace-path>\n";
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    print_usage(argc > 0 ? argv[0] : "retrace");
    return 1;
  }

  apitrace::replay::ReplayOptions options;
  options.bundle_root = argv[1];

  apitrace::replay::ReplaySession session(options);
  if (!session.run()) {
    std::cerr << "retrace failed: " << session.last_error() << '\n';
    return 1;
  }

  std::cout << "retrace " << apitrace::version_string() << '\n';
  return 0;
}
