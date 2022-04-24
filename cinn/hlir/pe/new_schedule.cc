// Copyright (c) 2021 CINN Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cinn/hlir/pe/new_schedule.h"

#include <absl/container/flat_hash_map.h>
#include <isl/cpp.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <utility>

#include "cinn/common/cas.h"
#include "cinn/hlir/pe/load_x86_params.h"
#include "cinn/hlir/pe/schedule.h"
#include "cinn/optim/ir_simplify.h"
#include "cinn/poly/isl_utils.h"

namespace cinn {
namespace hlir {
namespace pe {

void NewScheduleInjectiveCPU(ir::IRSchedule &ir_sch,
                             const std::vector<int> &output_shape,
                             const common::Target &target,
                             bool vectorizable) {
  auto all_blocks = ir_sch.GetAllBlocks();
  auto loops      = ir_sch.GetLoops(all_blocks[0]);
  int dims        = output_shape.size();
  int factor      = GetBasicFactor(GetTensor(all_blocks[0])->type(), target);
  auto fused      = loops[0];
  if (dims >= 5) {
    fused = ir_sch.Fuse({loops[0], loops[1], loops[2]});
    dims  = dims - 2;
  } else if (dims >= 3) {
    fused = ir_sch.Fuse({loops[0], loops[1]});
    dims  = dims - 1;
  }
  ir_sch.Parallel(fused);

  if (vectorizable) {
    auto all_blocks = ir_sch.GetAllBlocks();
    auto loops      = ir_sch.GetLoops(all_blocks[0]);
    int last_shape  = ir::GetLoopExtent(loops[dims - 1]);
    factor          = GetVectorizeFactor(last_shape, factor);
    auto splited    = ir_sch.Split(loops[dims - 1], {-1, factor});
    ir_sch.Vectorize(splited[1], factor);
    if (dims == 1) {
      ir_sch.Parallel(splited[0]);
    }
  }
}

void NewCudaScheduleInjective(ir::IRSchedule &ir_sch,
                              const std::vector<int> &output_shape,
                              const common::Target &target) {
  auto all_blocks = ir_sch.GetAllBlocks();
  auto loops      = ir_sch.GetLoops(all_blocks[0]);
  auto fused      = ir_sch.Fuse(loops);

  int num_thread        = target.max_num_threads();
  int num_block         = 1024;
  int vector_width      = 1;
  int prod_size         = std::accumulate(output_shape.begin(), output_shape.end(), 1, std::multiplies<int>());
  bool need_block_split = prod_size > num_thread * num_block * vector_width ? true : false;
  if (need_block_split) {
    auto splited = ir_sch.Split(fused, {num_block, num_thread, -1});
    ir_sch.Bind(splited[0], "blockIdx.x");
    ir_sch.Bind(splited[1], "threadIdx.x");
  } else {
    if (prod_size > num_thread) {
      auto splited = ir_sch.Split(fused, {-1, num_thread});
      ir_sch.Bind(splited[0], "blockIdx.x");
      ir_sch.Bind(splited[1], "threadIdx.x");
    } else {
      ir_sch.Bind(fused, "blockIdx.x");
    }
  }
  LOG(INFO) << "After NewCudaScheduleInjective, new ir is : " << ir_sch.GetModule().GetExprs().at(0);
}

void NewCudaScheduleMul(ir::IRSchedule &ir_sch, const std::vector<int> &output_shape, const common::Target &target) {
  auto all_blocks = ir_sch.GetAllBlocks();
  auto loops      = ir_sch.GetLoops(all_blocks.back());
  auto splited    = ir_sch.Split(loops[1], {-1, 2});
  all_blocks      = ir_sch.GetAllBlocks();
  loops           = ir_sch.GetLoops(all_blocks.back());
  ir_sch.Bind(loops[0], "blockIdx.x");
  ir_sch.Bind(loops[1], "threadIdx.x");
}

void NewMulScheduleCPU(ir::IRSchedule &ir_sch,
                       const std::vector<int> &reduce_first_shape,
                       const common::Target &target) {
  ir_sch.MergeExprs();
  auto all_blocks = ir_sch.GetAllBlocks();
  CHECK_EQ(all_blocks.size(), 4U);
  auto loops    = ir_sch.GetLoops(all_blocks[1]);
  int loop_size = loops.size();
  // ir_sch.Reorder({loops[loop_size-1], loops[loop_size-2]});

  if (reduce_first_shape.back() > 1) {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks[3]);
    ir_sch.Unroll(loops.back());
  }
}

void NewCudaSplitSchedule(ir::IRSchedule &ir_sch,
                          const std::vector<std::vector<int>> &output_shapes,
                          int axis,
                          const common::Target &target) {
  ir_sch.MergeExprs();
  int dims = output_shapes[0].size();
  std::vector<int> reorders;
  for (int i = 0; i < dims; i++) {
    reorders.push_back(i);
  }
  reorders.erase(reorders.begin() + axis);
  reorders.push_back(axis);
  auto all_blocks = ir_sch.GetAllBlocks();
  for (auto &block : all_blocks) {
    ir_sch.Reorder(block, reorders);
  }
  std::vector<int> fuse_index;
  for (int i = 0; i < dims - 1; i++) fuse_index.push_back(i);
  all_blocks = ir_sch.GetAllBlocks();
  for (auto &block : all_blocks) {
    ir_sch.Fuse(block, fuse_index);
  }
  int fused_shape = 1;
  for (int i = 0; i < dims; i++) {
    if (i != axis) fused_shape = fused_shape * output_shapes[0][i];
  }

  all_blocks           = ir_sch.GetAllBlocks();
  auto loops           = ir_sch.GetLoops(all_blocks.back());
  int compute_at_level = 0;
  if (target.arch == Target::Arch::NVGPU) {
    if (fused_shape > target.max_num_threads()) {
      ir_sch.Split(loops[0], {-1, target.max_num_threads()});
      all_blocks = ir_sch.GetAllBlocks();
      loops      = ir_sch.GetLoops(all_blocks.back());
      ir_sch.Bind(loops[0], "blockIdx.x");
      ir_sch.Bind(loops[1], "threadIdx.x");
      compute_at_level++;
    } else {
      ir_sch.Bind(loops[0], "threadIdx.x");
    }
    int all_blocks_num = all_blocks.size();
    for (int i = 0; i < all_blocks_num - 1; i++) {
      all_blocks = ir_sch.GetAllBlocks();
      loops      = ir_sch.GetLoops(all_blocks[i]);
      if (fused_shape > target.max_num_threads()) {
        ir_sch.Split(loops[0], {-1, target.max_num_threads()});
        all_blocks = ir_sch.GetAllBlocks();
        loops      = ir_sch.GetLoops(all_blocks.back());
        ir_sch.SimpleComputeAt(all_blocks[i], loops[compute_at_level]);
      }
    }
  } else {
    int all_blocks_num = all_blocks.size();
    for (int i = 0; i < all_blocks_num - 1; i++) {
      all_blocks = ir_sch.GetAllBlocks();
      loops      = ir_sch.GetLoops(all_blocks.back());
      ir_sch.SimpleComputeAt(all_blocks[i], loops[0]);
    }
  }
}

void NewCudaScheduleReduce(ir::IRSchedule &ir_sch,
                           const std::vector<int> &output_shape,
                           int last_dimension_num,
                           const common::Target &target) {
  int parallel_thread_num = 1;
  for (int idx = output_shape.size() - 1; idx >= static_cast<int>(output_shape.size()) - last_dimension_num; --idx) {
    parallel_thread_num *= output_shape[idx];
  }

  int index = output_shape.size() - last_dimension_num;

  auto all_blocks = ir_sch.GetAllBlocks();
  auto loops      = ir_sch.GetLoops(all_blocks.back());

  for (int idx = output_shape.size() - last_dimension_num; idx < static_cast<int>(output_shape.size()) - 1; ++idx) {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks.back());
    ir_sch.Fuse({loops[index], loops[index + 1]});
  }

  int max_block_size = 1024;
  if (parallel_thread_num > max_block_size) {
    all_blocks   = ir_sch.GetAllBlocks();
    loops        = ir_sch.GetLoops(all_blocks.back());
    auto splited = ir_sch.Split(loops[index], {-1, max_block_size});
    ir_sch.Bind(splited[1], "threadIdx.x");
  } else {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks.back());
    ir_sch.Bind(loops[index], "threadIdx.x");
  }

  for (int idx = 0; idx < index - 1; ++idx) {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks.back());
    ir_sch.Fuse({loops[0], loops[1]});
  }

  if (index > 0) {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks.back());
    ir_sch.Bind(loops[0], "blockIdx.x");
  }
}

void NewCudaScheduleBlockReduceInternal(ir::IRSchedule &ir_sch,
                                        ir::Tensor tmp_out,
                                        ir::Tensor out,
                                        const common::Target &target) {
  ir_sch.MergeExprs();
  auto all_blocks = ir_sch.GetAllBlocks();
  auto loops      = ir_sch.GetLoops(all_blocks[0]);
  CHECK_EQ(all_blocks.size(), 2U);

  for (int idx = 0; idx < static_cast<int>(tmp_out->shape.size()) - 2; ++idx) {
    all_blocks = ir_sch.GetAllBlocks();
    ir_sch.Fuse(all_blocks[0], {0, 1});
    ir_sch.Fuse(all_blocks[1], {0, 1});
  }

  if (tmp_out->shape.size() == 1) {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks[0]);
    ir_sch.Bind(loops[0], "threadIdx.x");
    ir_sch.SetBuffer(all_blocks[0], "local");
    loops = ir_sch.GetLoops(all_blocks[1]);
    ir_sch.Bind(loops[0], "threadIdx.x");
  } else {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks[0]);
    ir_sch.Bind(loops[1], "threadIdx.x");
    ir_sch.SetBuffer(all_blocks[0], "local");
    loops = ir_sch.GetLoops(all_blocks[1]);
    ir_sch.Bind(loops[0], "blockIdx.x");
    ir_sch.Bind(loops[1], "threadIdx.x");
    loops = ir_sch.GetLoops(all_blocks[1]);
    ir_sch.SimpleComputeAt(all_blocks[0], loops[0]);
  }
}

void NewCudaScheduleBlockReduce(ir::IRSchedule &ir_sch,
                                ir::Tensor reduce_tmp_out,
                                ir::Tensor tmp_out,
                                ir::Tensor out,
                                const common::Target &target) {
  ir_sch.MergeExprs();
  auto all_blocks = ir_sch.GetAllBlocks();
  auto loops      = ir_sch.GetLoops(all_blocks[0]);
  CHECK_EQ(all_blocks.size(), 3U);

  int output_shape_size_without_reduce = tmp_out->shape.size() - 1;
  // fuse last parallel dimension
  for (int idx = 0; idx < reduce_tmp_out->shape.size() - tmp_out->shape.size(); ++idx) {
    auto all_blocks = ir_sch.GetAllBlocks();
    ir_sch.Fuse(all_blocks[0], {output_shape_size_without_reduce, output_shape_size_without_reduce + 1});
  }

  // fuse parallel dimension
  for (int idx = 0; idx < output_shape_size_without_reduce - 1; ++idx) {
    auto all_blocks = ir_sch.GetAllBlocks();
    ir_sch.Fuse(all_blocks[0], {0, 1});
    ir_sch.Fuse(all_blocks[1], {0, 1});
    ir_sch.Fuse(all_blocks[2], {0, 1});
  }

  if (tmp_out->shape.size() == 1) {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks[0]);
    ir_sch.Bind(loops[0], "threadIdx.x");
    ir_sch.SetBuffer(all_blocks[0], "local");
    loops = ir_sch.GetLoops(all_blocks[1]);
    ir_sch.Bind(loops[0], "threadIdx.x");
    ir_sch.SetBuffer(all_blocks[1], "local");
    loops = ir_sch.GetLoops(all_blocks[2]);
    ir_sch.Bind(loops[0], "threadIdx.x");

  } else {
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks[0]);
    ir_sch.Bind(loops[1], "threadIdx.x");
    ir_sch.SetBuffer(all_blocks[0], "local");
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks[1]);
    ir_sch.Bind(loops[1], "threadIdx.x");
    ir_sch.SetBuffer(all_blocks[1], "local");
    loops = ir_sch.GetLoops(all_blocks[1]);
    ir_sch.SimpleComputeAt(all_blocks[0], loops[0]);
    all_blocks = ir_sch.GetAllBlocks();
    loops      = ir_sch.GetLoops(all_blocks[2]);
    ir_sch.Bind(loops[0], "blockIdx.x");
    ir_sch.Bind(loops[1], "threadIdx.x");
    loops = ir_sch.GetLoops(all_blocks[2]);
    ir_sch.SimpleComputeAt(all_blocks[1], loops[0]);
  }
}

void NewSoftmaxScheduleCPU(ir::IRSchedule &ir_sch, int axis) {
  ir_sch.MergeExprs();
  auto all_blocks = ir_sch.GetAllBlocks();
  CHECK_EQ(all_blocks.size(), 3U);
  auto output = GetTensor(all_blocks[2]);
  if (axis == -1) {
    axis += output->shape.size();
  }
  auto loops = ir_sch.GetLoops(all_blocks[2]);
  // ir_sch.Parallel(loops[0]);
  all_blocks = ir_sch.GetAllBlocks();
  for (int i = 1; i < axis; i++) {
    ir_sch.Fuse(all_blocks[2], {0, 1});
  }
  all_blocks = ir_sch.GetAllBlocks();
  loops      = ir_sch.GetLoops(all_blocks[2]);
  ir_sch.ComputeAt(all_blocks[1], loops[0]);
}

void NewPoolScheduleGPU(ir::IRSchedule &ir_sch, const common::Target &target) {
  auto all_blocks = ir_sch.GetAllBlocks();
  CHECK_EQ(all_blocks.size(), 1U);
  ir_sch.Fuse(all_blocks[0], {0, 1, 2, 3});
  auto loops   = ir_sch.GetLoops(all_blocks[0]);
  auto splited = ir_sch.Split(loops[0], {-1, 1024});
  ir_sch.Bind(splited[0], "blockIdx.x");
  ir_sch.Bind(splited[1], "threadIdx.x");
}

void NewGlobalPoolScheduleGPU(ir::IRSchedule &ir_sch, const common::Target &target) {
  auto all_blocks = ir_sch.GetAllBlocks();
  CHECK_EQ(all_blocks.size(), 2U);
  auto fused   = ir_sch.Fuse(all_blocks[0], {0, 1});
  auto splited = ir_sch.Split(fused, {-1, 32});
  all_blocks   = ir_sch.GetAllBlocks();
  fused        = ir_sch.Fuse(all_blocks[1], {0, 1});
  splited      = ir_sch.Split(fused, {-1, 32});
  ir_sch.Bind(splited[0], "blockIdx.x");
  ir_sch.Bind(splited[1], "threadIdx.y");
  all_blocks = ir_sch.GetAllBlocks();
  ir_sch.SimpleComputeAt(all_blocks[0], splited[1]);
  all_blocks = ir_sch.GetAllBlocks();
  ir_sch.SetBuffer(all_blocks[0], "local");
  auto loops = ir_sch.GetLoops(all_blocks[0]);
  ir_sch.Bind(loops[2], "threadIdx.x");
}

}  // namespace pe
}  // namespace hlir
}  // namespace cinn