#include <torch/csrc/jit/fuser/common/fusion.h>
#include <torch/csrc/jit/fuser/common/transform_replay.h>

// For debug:
/*
#include <torch/csrc/jit/fuser/common/iriostream.h>
*/
namespace torch {
namespace jit {
namespace fuser {

// For debug:
/*
std::ostream& operator<<(std::ostream& os, std::vector<bool> vec) {
  os << "<";
  for (int i = 0; i < vec.size(); i++) {
    if (vec[i])
      os << " t";
    else
      os << " f";
    if (i == vec.size() - 1)
      os << ">";
    else
      os << ",";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, std::vector<int> vec) {
  os << "<";
  for (int i = 0; i < vec.size(); i++) {
    os << vec[i];
    if (i == vec.size() - 1)
      os << ">";
    else
      os << ",";
  }
  return os;
}
*/

/*
 * Functions to backward propagate influence from split/merge/reorder
 */
void TransformReplay::compute_influence(Split* expr) {
  int axis = expr->axis();
  influence[axis] = influence[axis] | influence[axis + 1];
  influence.erase(influence.begin() + axis + 1);
}

void TransformReplay::compute_influence(Merge* expr) {
  int axis = expr->axis();
  influence.insert(influence.begin() + axis + 1, influence[axis]);
}

void TransformReplay::compute_influence(Reorder* expr) {
  // pos2axis[new_pos] = old_pos Generate new axis2pos map
  const std::vector<int>& pos2axis = expr->pos2axis();

  std::vector<bool> reorder_influence(influence.size(), false);
  for (decltype(pos2axis.size()) i = 0; i < pos2axis.size(); i++) {
    int new_pos = i;
    int old_pos = pos2axis[i];
    reorder_influence[old_pos] = influence[new_pos];
  }

  influence = reorder_influence;
}

// Backward influence propagate dispatch
void TransformReplay::compute_influence(Expr* expr) {
  TORCH_CHECK(expr->isExpr());
  switch (*(expr->getExprType())) {
    case (ExprType::Split):
      compute_influence(static_cast<Split*>(expr));
      break;
    case (ExprType::Merge):
      compute_influence(static_cast<Merge*>(expr));
      break;
    case (ExprType::Reorder):
      compute_influence(static_cast<Reorder*>(expr));
      break;
    default:
      throw std::runtime_error(
          "Could not detect expr type in compute_influence.");
  }
}

// Entry for backward influence propagation on td following record
void TransformReplay::compute_influence(TensorDomain* td) {
  influence = std::vector<bool>(td->size(), false);
  for (int i = 0; i < compute_at_axis; i++)
    influence[i] = true;

  for (auto it = record.rbegin(); it < record.rend(); ++it) {
    compute_influence(*it);
  }
}

// Trace back the history of td, record the Expr's that made this td (split,
// merge, reorder)
TensorDomain* TransformReplay::get_root(TensorDomain* td, bool create_record) {
  if (create_record)
    record = std::vector<Expr*>();

  TensorDomain* root = td; // backward running td
  Fusion* fusion = FusionGuard::getCurFusion();

  // Get my origin
  Expr* orig = fusion->origin(root);
  std::set<Expr*> visited_exprs;

  // If I'm not back to the original td
  while (orig != nullptr) {
    if (visited_exprs.find(orig) != visited_exprs.end())
      throw std::runtime_error(
          "TransformReplay::get_root is not traversing a correct history.");

    visited_exprs.emplace(orig);
    TensorDomain* previous_td = nullptr;
    // Check inputs of this operation, make sure there isn't more than one TD
    // I can only record operations that only take this TD as an input.
    for (Val* inp : orig->inputs())
      if (inp->getValType() == ValType::TensorDomain) {
        if (previous_td != nullptr)
          throw std::runtime_error(
              "TransformReplay::get_root could not decifer transform history of a TensorDomain.");

        // Place transform op on top of stack.
        if (create_record)
          record.push_back(orig);

        // Traverse back
        root = static_cast<TensorDomain*>(inp);
        orig = fusion->origin(root);
      }
  }
  if (create_record)
    std::reverse(record.begin(), record.end());

  return root;
}

/*
 * Replay functions, takes a TensorView and steps through the operations in
 * "record" based on influence axes. Will also update influence and propagate
 * it forward.
 */
TensorView* TransformReplay::replay(Split* expr, TensorView* tv) {
  int axis = expr->axis();
  // Forward prop influence
  if (influence[axis]) {
    // Make sure split axis is real.
    int real_axis = axis_map[expr->axis()];
    TORCH_CHECK(real_axis != -1);
    // Replay split
    split(tv, real_axis, *(expr->factor()->value()));
    // Inserted a real axis, push everything in axis_map over to the right
    // after this inserted axis
    for (int i = 0; i < axis_map.size(); i++)
      if (axis_map[i] > real_axis)
        axis_map[i] = axis_map[i] + 1;

    axis_map.insert(
        axis_map.begin() + expr->axis() + 1,
        real_axis + 1); // insert axis at position axis.
  } else {
    // Fake it
    axis_map.insert(axis_map.begin() + expr->axis() + 1, -1);
  }

  influence.insert(influence.begin() + axis + 1, influence[axis]);

  return tv;
}

TensorView* TransformReplay::replay(Merge* expr, TensorView* tv) {
  int axis = expr->axis();

  if (influence[axis] || influence[axis + 1]) {
    // Make sure both merge axes are real.
    TORCH_CHECK(axis_map[axis] != -1 && axis_map[axis + 1] != -1);
    // Replay merge
    merge(tv, axis_map[axis]);
  } else {
    // If we aren't applying the merge, we won't change any following axis
    // Doesn't matter which axis we propagate for the merge in the axis_map
    assert(axis_map[axis + 1] == -1);
  }
  axis_map.erase(axis_map.begin() + expr->axis() + 1);

  for (decltype(axis_map.size()) i = expr->axis() + 1; i < axis_map.size(); i++)
    if (axis_map[i] != -1)
      axis_map[i]--;

  // Forward prop influence
  influence[axis] = influence[axis] || influence[axis + 1];
  influence.erase(influence.begin() + axis + 1);

  return tv;
}

TensorView* TransformReplay::replay(Reorder* expr, TensorView* tv) {
  // axis2pos[old_pos] = new_pos is sent to reorder, Reorder holds
  // pos2axis[new_pos] = old_pos Generate new axis2pos map
  std::unordered_map<int, int> axis2pos;
  const std::vector<int>& pos2axis = expr->pos2axis();

  std::vector<int> reordered_axis_map(axis_map.size(), -1);
  std::vector<bool> reordered_influence(pos2axis.size(), false);

  // We have
  // axis_map[old_fake_pos] -> old_real_pos
  // pos2axis[new_fake_pos] -> old_fake_pos
  // f2r[new_fake_pos] -> new_real_pos
  //
  // We want:
  // axis2pos[old_real_pos] -> new_real_pos
  // axis_map[new_fake_pos] -> new_real_pos

  std::vector<std::pair<int, int>> needed_real_reorder;
  for (int i = 0; i < pos2axis.size(); i++) {
    int new_fake_axis = i;
    int old_fake_axis = pos2axis[i];
    int old_real_axis = axis_map[old_fake_axis];
    bool is_real_axis = old_real_axis != -1;
    // If a real axis
    if (is_real_axis)
      if (influence[old_fake_axis]) {
        needed_real_reorder.push_back({old_real_axis, new_fake_axis});
      }
  }

  // Sort needed_real_reorder by new_fake_axis.
  std::sort(
      needed_real_reorder.begin(),
      needed_real_reorder.end(),
      [](std::pair<int, int> a, std::pair<int, int> b) -> bool {
        return a.second < b.second;
      });

  // axis2pos[old_real_axis] -> new_real_axis
  int axis = 0;
  for (auto entry : needed_real_reorder) {
    axis2pos[entry.first] = axis++;
  }

  for (int i = 0; i < tv->domain()->size(); i++) {
    if (axis2pos.find(i) == axis2pos.end())
      axis2pos[i] = axis++;
  }

  // replay reorder
  reorder(tv, axis2pos);

  // Fake transform:
  for (decltype(pos2axis.size()) i = 0; i < pos2axis.size(); i++) {
    int new_pos = i;
    int old_pos = pos2axis[i];
    // Forward prop influence
    reordered_influence[new_pos] = influence[old_pos];
    if (axis_map[old_pos] != -1)
      reordered_axis_map[new_pos] = axis2pos[axis_map[old_pos]];
  }
  influence = reordered_influence;
  axis_map = reordered_axis_map;

  return tv;
}

// Dispatch for replay functions
TensorView* TransformReplay::replay(Expr* expr, TensorView* tv) {
  TORCH_CHECK(expr->isExpr());
  switch (*(expr->getExprType())) {
    case (ExprType::Split):
      return replay(static_cast<Split*>(expr), tv);
    case (ExprType::Merge):
      return replay(static_cast<Merge*>(expr), tv);
    case (ExprType::Reorder):
      return replay(static_cast<Reorder*>(expr), tv);
    default:
      throw std::runtime_error("Could not detect expr type in replay.");
  }
}

// Entry point for replay on a TensorView, will relpay all ops from "replay"
TensorView* TransformReplay::replay(TensorView* target) {
  TensorView* tv = target;
  for (auto it = record.begin(); it < record.end(); ++it) {
    tv = replay(*it, tv);
  }
  return tv;
}

/*
 * TODO: When we compare root axes, we should ignore reduction axes in the
 * producer. Reduction axes are owned by a consumer.
 *
 * TODO: We should be able to relax the constraints of replay a bit. Right now
 * it requires that the root domain of the target and replay are completely
 * the same. However, we should only require that the root derived from the
 * axes < compute_at_axis match. We could even go further and look for those
 * matching axes as they don't necessairly need to be in the same order.
 * However, once they're replayed they should be.
 *
 * 1) Take the reference, trace back its domain history to get all the
 * split/merge/reorder calls, as well as its original domain. Get the
 * original domain of the target as well.
 *
 * 2) We only need compute_at_axis and earlier dimensions to match for
 * compute_at. Therefore, we want to find all original axes that must have
 * been modified in order to produce the axes below compute_at_axis. We take a
 * bool vector called influence, and mark axes below compute_at_axis as true,
 * and all others as false. This vector is propagated up through
 * split/merge/reorder if split/merge/reorder output a marked axis, their
 * input will be marked as well. This marks all original axes required to be
 * modified to produce the axes below compute_at_axis.
 *
 * 3) We take the ordered list of split/merge/reorder and the influence vector
 * on the inputs and we apply split/merge/reorder operations on the
 * replay_target. We also forward propagate the influence vector again (as this
 * time it could be different than originally marked), a map from "fake axes"
 * (refrence axes corresponding to the full replay) to real axes (axes produced
 * by running the selected split/merge/reorder operations). Reorder replay's can
 * actually be partial and non-equivelent to the original, as some axes may
 * never have been produced based on split, and we don't want to reorder axes
 * outside of compute_at_axis.
 *
 */
TensorView* TransformReplay::runReplay(
    TensorView* replay_ref,
    TensorView* replay_target,
    int compute_at_axis) {
  this->compute_at_axis = compute_at_axis;
  /* STEP 1 */
  // Trace back to the root TensorDomain's of ref and target
  TensorDomain* target_root = get_root(replay_target->domain());

  // Reset the tensor domain of the target, this is the only way we can be
  // certain That we can actually replay the ops of ref.

  replay_target->setDomain(target_root);
  // As we trace the ref, record the operations to go from replay_ref ->
  // ref_root, save in "record"
  TensorDomain* ref_root = get_root(replay_ref->domain(), true);

  /* STEP 2 */
  // Mark compute_at_axis and below as "influenced", trace back through
  // operations, and map these axes to the ref root axis that were modified to
  // produce these axis
  compute_influence(replay_ref->domain());
  // We're going to save a copy of this vector, class member influnce will be
  // used during replay to forward propagate influence.
  std::vector<bool> root_influence_vector = influence;

  auto init_size = replay_target->domain()->size();
  for (decltype(init_size) i = 0; i < init_size; i++)
    if (!replay_target->domain()->axis(i)->isReduction())
      axis_map.push_back(i);

  // Domain sizes must match at root for replay.
  TORCH_CHECK(axis_map.size() == ref_root->size());
  for (decltype(axis_map.size()) i{0}; i < axis_map.size(); i++) {
    TORCH_CHECK(ref_root->axis(i)->same_as(target_root->axis(axis_map[i])));
  }

  /* STEP 3 */
  // Replay operations while forward propagating influence. The resulting
  // influence can be different in forward propagation, than in backward
  // propagation depending on the combination of merge/split/reorder nodes
  // There are multiple things we have to track here. We need to track
  // the propagation of axes for all operations, though we only want to
  // actually execute those based on influence. If we didn't track all
  // axes, we wouldn't know what axis split/merge/reorder are referencing
  // as they're relative to the "full" replay that produced the reference.
  TensorView* replayed = replay(replay_target);

  for (decltype(replayed->domain()->size()) i{0}; i < compute_at_axis; i++)
    if (replayed->domain()->axis(i)->isReduction())
      throw std::runtime_error(
          "Generated a compute_at dependency where a reduction would be used before computed.");

  return replayed;
}

TensorView* TransformReplay::replay(
    TensorView* replay_ref,
    TensorView* replay_target,
    int compute_at_axis) {
  TransformReplay tr;
  tr.runReplay(replay_ref, replay_target, compute_at_axis);
  return replay_target;
}

} // namespace fuser
} // namespace jit
} // namespace torch