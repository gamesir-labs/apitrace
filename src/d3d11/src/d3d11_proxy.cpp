#include "apitrace/d3d11_proxy.hpp"

namespace apitrace::d3d11 {

ProxyModuleDescriptor proxy_descriptor()
{
  ProxyModuleDescriptor descriptor;
  descriptor.api = trace::ApiKind::D3D11;
  descriptor.dll_name = "d3d11.dll";
  descriptor.bootstrap_symbol = "apitrace_bootstrap_d3d11";
  return descriptor;
}

} // namespace apitrace::d3d11
