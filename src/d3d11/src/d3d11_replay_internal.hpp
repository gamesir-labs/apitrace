#pragma once

#include "apitrace/replay_session.hpp"
#include "retrace/src/d3d11_replay_parser.hpp"

#include <string>

namespace apitrace::d3d11::internal {

bool replay_translation_layer_plan(
    const replay::internal::D3D11ReplayPlan &plan,
    replay::ReplayStatistics &statistics,
    std::string &error);

} // namespace apitrace::d3d11::internal
