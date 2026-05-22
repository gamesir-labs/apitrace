#pragma once

#include "apitrace/api_types.hpp"

#include <string>

namespace apitrace::d3d11 {

struct ProxyModuleDescriptor {
  trace::ApiKind api = trace::ApiKind::Unknown;
  std::string dll_name;
  std::string bootstrap_symbol;
};

ProxyModuleDescriptor proxy_descriptor();

} // namespace apitrace::d3d11
