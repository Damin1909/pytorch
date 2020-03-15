#pragma once

#include <torch/csrc/jit/ir/ir.h>

/*
 * This file handles compilation and execution of a CudaFusionGroup;
 *
 * A CudaFusionGroup node comes with `attr::Subgraph` containing the computation
 * graph. We compile the graph to generate CUDA function and cache them in a
 * registry. We cache & reuse kernels across nodes sharing identical graph.
 *
 * After compilation, we assign the key to cached kernel as an integer attribute 
 * on the node `attr::cache_id`.
 */

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

// Get fusion_node ready for execution.
// find or compile `CudaKernel` for graph stored in `attr::Subgraph`
// this function assigns `attr::cache_id` to `fusion_node`
TORCH_API void compileCudaFusionGroup(Node* fusion_node);

// Execute fusion_node.
// Current protocol is that the function allocates output tensor append them to
// `stack` after execution.
// TODO: support shape inferencing. Right now we only handles static shape
TORCH_API void runCudaFusionGroup(const Node* const fusion_node, Stack& stack);

}}}} // namespace torch::jit::fuser::cuda