#include "markettwin/common/version.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

void print_usage(std::ostream& output) {
  output << "Usage: markettwin [--version] [--help]\n"
         << "Print the MarketTwin engine version.\n"
         << "This build does not run simulations or accept orders.\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    print_usage(std::cerr);
    return 2;
  }

  const std::string_view argument{argv[1]};
  if (argument == "--version") {
    const std::string_view engine_version = markettwin::version();
    std::cout << "MarketTwin " << std::string{engine_version} << '\n';
    return 0;
  }
  if (argument == "--help" || argument == "-h") {
    print_usage(std::cout);
    return 0;
  }

  print_usage(std::cerr);
  return 2;
}
