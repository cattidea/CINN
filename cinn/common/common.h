#pragma once

#include "cinn/common/axis.h"
#include "cinn/common/context.h"
#include "cinn/common/graph_utils.h"
#include "cinn/common/pod_value.h"
#include "cinn/common/shared.h"
#include "cinn/common/type.h"

namespace cinn {

// export some general concepts.
using common::Context;
using common::make_shared;
using common::Object;
using common::ref_count;
using common::Shared;

// Type related.
using common::Float;
using common::Int;
using common::type_of;

}  // namespace cinn