#include "apitrace/d3d12_proxy.hpp"

namespace apitrace::d3d12 {

ProxyModuleDescriptor proxy_descriptor()
{
  ProxyModuleDescriptor descriptor;
  descriptor.api = trace::ApiKind::D3D12;
  descriptor.dll_name = "d3d12.dll";
  descriptor.bootstrap_symbol = "apitrace_bootstrap_d3d12";
  return descriptor;
}

} // namespace apitrace::d3d12
