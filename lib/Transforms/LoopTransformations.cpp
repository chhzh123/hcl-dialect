//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The HCL-MLIR Authors.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"

#include "hcl/Dialect/HeteroCLDialect.h"
#include "hcl/Dialect/HeteroCLOps.h"
#include "hcl/Support/Utils.h"
#include "hcl/Transforms/Passes.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/LoopFusionUtils.h"
#include "mlir/Dialect/Affine/LoopUtils.h"
#include "mlir/Dialect/Affine/Utils.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/RegionUtils.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

using namespace mlir;
using namespace hcl;

using AffineLoopBand = SmallVector<AffineForOp, 6>;

//===----------------------------------------------------------------------===//
// Loop transformation
//===----------------------------------------------------------------------===//

namespace mlir {
namespace hcl {

struct ExprCompare {
  int findConstantExpr(const AffineExpr &exp) const {
    int value = -1;
    // TODO: only support one constant now
    exp.walk([&](AffineExpr inner) {
      if (inner.isa<AffineConstantExpr>())
        value = inner.cast<AffineConstantExpr>().getValue();
    });
    return value;
  }
  bool operator()(const AffineExpr &exp1, const AffineExpr &exp2) const {
    int val1 = findConstantExpr(exp1);
    int val2 = findConstantExpr(exp2);
    return val1 < val2;
  }
};

Attribute createZeroAttr(OpBuilder &builder, mlir::Type elementType) {
  if (elementType.isa<FloatType>())
    return builder.getFloatAttr(elementType, 0.0);
  if (elementType.isa<IntegerType>())
    return builder.getIntegerAttr(elementType, 0);
  return {};
}

LogicalResult runSplitting(FuncOp &f, SplitOp &splitOp) {
  // 1) Get the schedule
  unsigned int factor = splitOp.factor();
  const auto loop_name =
      dyn_cast<CreateLoopHandleOp>(splitOp.loop().getDefiningOp()).loop_name();
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(splitOp.stage().getDefiningOp())
          .stage_name();

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Find the requested loop
  bool isOuterMost = false;
  AffineLoopBand band;
  rootForOp->walk([&](AffineForOp forOp) {
    if (band.size() == 0 && loop_name == getLoopName(forOp)) {
      band.push_back(forOp);
      if (forOp->hasAttr("stage_name"))
        isOuterMost = true;
    }
  });
  // handle exception
  if (band.size() == 0) {
    splitOp.emitError("Cannot find Loop ")
        << loop_name.str() << " in Stage " << stage_name.str();
    return failure();
  }
  if (factor >= band[0].getConstantUpperBound()) {
    splitOp.emitError("The requested tiling factor (")
        << factor << ") is larger than the upper bound ("
        << band[0].getConstantUpperBound() << ") of the loop";
    return failure();
  }

  // 4) Split the loop
  SmallVector<unsigned, 6> tileSizes;
  tileSizes.push_back(factor);
  AffineLoopBand tiledNest;
  if (failed(tilePerfectlyNested(band, tileSizes, &tiledNest)))
    return failure();
  if (isOuterMost)
    rootForOp = tiledNest[0];

  // 5) Loop normalization
  // Note: 5) & 6) are used for making the loop bound constants
  //       Otherwise, loops are not perfectly nested
  normalizeAffineFor(tiledNest[0]);
  normalizeAffineFor(tiledNest[1]);
  auto ub = tiledNest[1].getUpperBound();
  auto ubMap = ub.getMap();
  if (ubMap.isConstant()) {
    // Exception case that cannot change loop bound:
    // #map1 = affine_map<(d0, d1) -> (7, -d0 + 1024)>
    // %5 = affine.apply #map0(%arg3)
    // affine.for %arg4 = 0 to min #map1(%5, %5)
    auto cstUb = ubMap.getResult(0).dyn_cast<AffineConstantExpr>().getValue();
    OpBuilder opBuilder(tiledNest[1]);
    tiledNest[1].setUpperBound({}, opBuilder.getConstantAffineMap(cstUb));
  } else {
    auto addMap =
        AffineMap::get(/*numDims=*/1, /*numSymbols=*/0, ubMap.getResult(1));
    auto applyOp = dyn_cast<AffineApplyOp>(
        tiledNest[1].getUpperBoundOperands()[0].getDefiningOp());
    auto outerIV = applyOp.getOperand(0);
    auto mulMap = applyOp.getAffineMap();
    auto composedMap = addMap.compose(mulMap);
    SmallVector<AffineExpr> newExprs{ubMap.getResult(0),
                                     composedMap.getResult(0)};
    auto finalMinMap = AffineMap::get(/*numDims=*/1, /*numSymbols=*/0, newExprs,
                                      tiledNest[1].getContext());
    tiledNest[1].setUpperBound(outerIV, finalMinMap);
  }

  // 6) Sink AffineApply Operations
  auto fstApply = *(tiledNest[0].getOps<AffineApplyOp>().begin());
  auto sndApply = *(tiledNest[1].getOps<AffineApplyOp>().begin());
  WalkResult result = rootForOp->walk(
      [&](AffineForOp forOp) -> WalkResult { // from the innermost
        sndApply->moveBefore(&(*forOp.getBody()->getOperations().begin()));
        // definition should come before reference
        bool isDominance = true;
        for (auto user : sndApply->getUsers()) {
          DominanceInfo domInfo;
          if (!domInfo.properlyDominates(sndApply->getResult(0), user)) {
            isDominance = false;
            break;
          }
        }
        if (isDominance)
          return WalkResult::interrupt();
        return WalkResult::advance();
      });
  if (result.wasInterrupted())
    fstApply->moveBefore(sndApply);

  // 7) Add names to new loops
  SmallVector<std::string, 6> newNameArr;
  newNameArr.push_back(loop_name.str() + ".outer");
  newNameArr.push_back(loop_name.str() + ".inner");
  setLoopNames(tiledNest, newNameArr);
  if (isOuterMost)
    setStageName(tiledNest[0], stage_name);

  // 8) Create new loop handles
  auto firstOp = *(f.getOps<AffineForOp>().begin());
  OpBuilder builder(firstOp);
  auto outer = builder.create<CreateLoopHandleOp>(
      firstOp->getLoc(), LoopHandleType::get(firstOp->getContext()),
      StringAttr::get(firstOp->getContext(), newNameArr[0]));
  auto inner = builder.create<CreateLoopHandleOp>(
      firstOp->getLoc(), LoopHandleType::get(firstOp->getContext()),
      StringAttr::get(firstOp->getContext(), newNameArr[1]));

  // 9) Link the loop handles with SSA values
  splitOp.getResult(0).replaceAllUsesWith(outer);
  splitOp.getResult(1).replaceAllUsesWith(inner);

  return success();
}

LogicalResult runTiling(FuncOp &f, TileOp &tileOp) {
  // 1) Get the schedule
  unsigned int x_factor = tileOp.x_factor();
  unsigned int y_factor = tileOp.y_factor();
  const auto x_loop =
      dyn_cast<CreateLoopHandleOp>(tileOp.x_loop().getDefiningOp()).loop_name();
  const auto y_loop =
      dyn_cast<CreateLoopHandleOp>(tileOp.y_loop().getDefiningOp()).loop_name();
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(tileOp.stage().getDefiningOp())
          .stage_name();

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Find the requested loops
  bool isOuterMost = false;
  SmallVector<StringRef, 6> nameArr;
  nameArr.push_back(x_loop);
  nameArr.push_back(y_loop);
  AffineLoopBand band;
  WalkResult result = rootForOp.walk([&](AffineForOp forOp) -> WalkResult {
    if (findContiguousNestedLoops(forOp, band, nameArr))
      return WalkResult::interrupt();
    return WalkResult::advance();
  });
  // handle exception
  if (!result.wasInterrupted()) {
    tileOp.emitError("Cannot find contiguous nested loops starting from Loop ")
        << x_loop.str();
    return failure();
  }
  if (x_factor >= band[0].getConstantUpperBound()) {
    tileOp.emitError("The requested tiling factor (")
        << x_factor << ") is larger than the upper bound ("
        << band[0].getConstantUpperBound() << ") of the loop";
    return failure();
  }
  if (y_factor >= band[1].getConstantUpperBound()) {
    tileOp.emitError("The requested tiling factor (")
        << y_factor << ") is larger than the upper bound ("
        << band[1].getConstantUpperBound() << ") of the loop";
    return failure();
  }
  if (band[0]->hasAttr("stage_name"))
    isOuterMost = true;

  // 4) Tile the loops
  SmallVector<unsigned, 6> tileSizes;
  tileSizes.push_back(x_factor);
  tileSizes.push_back(y_factor);
  AffineLoopBand tiledNest;
  if (failed(tilePerfectlyNested(band, tileSizes, &tiledNest)))
    return failure();
  if (isOuterMost)
    rootForOp = tiledNest[0];

  // 5) Loop normalization
  // Note: 5) & 6) are used for making the loop bound constants
  //       Otherwise, loops are not perfectly nested
  for (int i = 0; i < 4; ++i)
    normalizeAffineFor(tiledNest[i]);
  // the tiled factor loops are the inner two
  for (int i = 2; i < 4; ++i) {
    auto ub = tiledNest[i].getUpperBound();
    auto ubMap = ub.getMap();
    if (ubMap.isConstant()) {
      auto cstUb = ubMap.getResult(0).dyn_cast<AffineConstantExpr>().getValue();
      OpBuilder opBuilder(tiledNest[i]);
      tiledNest[i].setUpperBound({}, opBuilder.getConstantAffineMap(cstUb));
    } else {
      auto addMap =
          AffineMap::get(/*numDims=*/1, /*numSymbols=*/0, ubMap.getResult(1));
      auto applyOp = dyn_cast<AffineApplyOp>(
          tiledNest[i].getUpperBoundOperands()[0].getDefiningOp());
      auto outerIV = applyOp.getOperand(0);
      auto mulMap = applyOp.getAffineMap();
      auto composedMap = addMap.compose(mulMap);
      SmallVector<AffineExpr> newExprs{ubMap.getResult(0),
                                       composedMap.getResult(0)};
      auto finalMinMap = AffineMap::get(/*numDims=*/1, /*numSymbols=*/0,
                                        newExprs, tiledNest[i].getContext());
      tiledNest[i].setUpperBound(outerIV, finalMinMap);
    }
  }

  // 6) Sink AffineApply Operations
  for (int i = 1; i >= 0; --i) { // from inner to outer
    auto fstApply = *(tiledNest[i].getOps<AffineApplyOp>().begin());
    auto sndApply = *(tiledNest[i + 2].getOps<AffineApplyOp>().begin());
    WalkResult result = rootForOp->walk(
        [&](AffineForOp forOp) -> WalkResult { // from the innermost
          sndApply->moveBefore(&(*forOp.getBody()->getOperations().begin()));
          // definition should come before reference
          bool isDominance = true;
          for (auto user : sndApply->getUsers()) {
            DominanceInfo domInfo;
            if (!domInfo.properlyDominates(sndApply->getResult(0), user)) {
              isDominance = false;
              break;
            }
          }
          if (isDominance)
            return WalkResult::interrupt();
          return WalkResult::advance();
        });
    if (result.wasInterrupted())
      fstApply->moveBefore(sndApply);
  }

  // 7) Add names to new loops
  SmallVector<std::string, 6> newNameArr;
  newNameArr.push_back(x_loop.str() + ".outer");
  newNameArr.push_back(x_loop.str() + ".inner");
  newNameArr.push_back(y_loop.str() + ".outer");
  newNameArr.push_back(y_loop.str() + ".inner");
  setLoopNames(tiledNest, newNameArr);
  if (isOuterMost)
    setStageName(tiledNest[0], stage_name);

  // 8) Create new loop handles &
  //    Link the loop handles with SSA values
  auto firstOp = *(f.getOps<AffineForOp>().begin());
  OpBuilder builder(firstOp);
  for (int i = 0; i < 4; ++i) {
    auto handle = builder.create<CreateLoopHandleOp>(
        firstOp->getLoc(), LoopHandleType::get(firstOp->getContext()),
        StringAttr::get(firstOp->getContext(), newNameArr[i]));
    tileOp.getResult(i).replaceAllUsesWith(handle);
  }

  return success();
}

LogicalResult runReordering(FuncOp &f, ReorderOp &reorderOp) {
  // 1) Get the schedule
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(reorderOp.stage().getDefiningOp())
          .stage_name();
  const auto loopsToReorder = reorderOp.loops(); // operand_range
  if (loopsToReorder.size() < 2) {
    reorderOp.emitError("Should at least input 2 loops to be reordered");
    return failure();
  }

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Get the maximal perfect nest
  //    This should be done first to resolve imperfect loops
  AffineLoopBand nest;
  getPerfectlyNestedLoops(nest, rootForOp);

  // 4) Traverse all the loops in the stage
  //    Get a mapping from loop name to id
  std::map<std::string, unsigned> oldName2ID;
  SmallVector<std::string> oldLoopNames;
  unsigned int curr_depth = 0;
  for (AffineForOp forOp : nest) {
    std::string loop_name = getLoopName(forOp).str();
    oldName2ID[loop_name] = curr_depth;
    oldLoopNames.push_back(loop_name);
    curr_depth++;
  }

  // 5) Traverse all the input arguments that need to be reordered and
  // construct permMap
  // Possible inputs:
  // a) # arguments = # loops: (i,j,k)->(k,j,i)
  // b) # arguments != # loops: input (k,i), but should be the same as a)

  // 5.1) Map input arguments to the corresponding loop names
  SmallVector<std::string> nameOfLoopsToReorder;
  for (auto loop : loopsToReorder) {
    nameOfLoopsToReorder.push_back(loop.getDefiningOp()
                                       ->getAttr("loop_name")
                                       .cast<StringAttr>()
                                       .getValue()
                                       .str());
  }

  // 5.2) Make Case b) to Case a)
  //      i.e. fill in all the missing loops in Case b)
  SmallVector<std::string> nameOfAllLoopsWithNewOrder;
  unsigned int cntInArgs = 0;
  for (unsigned int i = 0, e = oldLoopNames.size(); i < e; ++i) {
    auto name = oldLoopNames[i];
    auto iterator = std::find(nameOfLoopsToReorder.begin(),
                              nameOfLoopsToReorder.end(), name);
    if (iterator != nameOfLoopsToReorder.end()) { // name in the arguments
      nameOfAllLoopsWithNewOrder.push_back(nameOfLoopsToReorder[cntInArgs++]);
    } else { // not in
      nameOfAllLoopsWithNewOrder.push_back(name);
    }
  }

  // 5.3) Traverse the original loop nests and create a new order (permMap) for
  // the loops, where permMap[i] means the ith loop in the original nests will
  // become the permMap[i]-th loop
  unsigned int outerMostIdx = 0;
  SmallVector<unsigned, 6> permMap;
  for (unsigned int i = 0, e = oldLoopNames.size(); i < e; ++i) {
    auto name = oldLoopNames[i];
    auto iterator = std::find(nameOfAllLoopsWithNewOrder.begin(),
                              nameOfAllLoopsWithNewOrder.end(), name);
    unsigned int idx = iterator - nameOfAllLoopsWithNewOrder.begin();
    permMap.push_back(idx);
    if (idx == 0) {
      outerMostIdx = i;
    }
  }

  // 6) Permute the loops
  // TODO: imperfect loops
  // Permute if the nest's size is consistent with the specified
  // permutation
  if (nest.size() >= 2 && nest.size() == permMap.size()) {
    if (outerMostIdx != 0)
      nest[0]->removeAttr("stage_name");
    permuteLoops(nest, permMap);
  } else {
    reorderOp.emitError("Cannot permute the loops because the size of the "
                        "perfectly nested loop band (")
        << nest.size() << ") "
        << "is not consistent with the size of permutation mapping ("
        << permMap.size() << ")";
    return failure();
  }

  // 7) Rename the stage if the outermost loop moves inward
  if (outerMostIdx != 0) {
    nest[outerMostIdx]->setAttr(
        "stage_name",
        StringAttr::get(nest[outerMostIdx]->getContext(), stage_name));
  }

  return success();
}

LogicalResult runUnrolling(FuncOp &f, UnrollOp &unrollOp) {
  // 1) Get the schedule
  auto optional_factor = unrollOp.factor();
  unsigned int factor;
  if (optional_factor.hasValue()) {
    factor = optional_factor.getValue();
  } else {
    factor = 0; // fully unroll
  }
  const auto loop_name =
      dyn_cast<CreateLoopHandleOp>(unrollOp.loop().getDefiningOp()).loop_name();
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(unrollOp.stage().getDefiningOp())
          .stage_name();

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Find the requested loop and attach attribute
  WalkResult result = rootForOp.walk([&](AffineForOp forOp) -> WalkResult {
    if (loop_name == getLoopName(forOp)) {
      AffineLoopBand band{forOp};
      SmallVector<int, 6> attr_arr{(int)factor};
      setIntAttr(band, attr_arr, "unroll");
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  // handle exception
  if (!result.wasInterrupted()) {
    unrollOp.emitError("Cannot find Loop ") << loop_name.str();
    return failure();
  }

  return success();
}

LogicalResult runParallel(FuncOp &f, ParallelOp &parallelOp) {
  // 1) Get the schedule
  const auto loop_name =
      dyn_cast<CreateLoopHandleOp>(parallelOp.loop().getDefiningOp())
          .loop_name();
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(parallelOp.stage().getDefiningOp())
          .stage_name();

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Find the requested loop and attach attribute
  WalkResult result = rootForOp.walk([&](AffineForOp forOp) -> WalkResult {
    if (loop_name == getLoopName(forOp)) {
      AffineLoopBand band{forOp};
      SmallVector<int, 6> attr_arr{1};
      setIntAttr(band, attr_arr, "parallel");
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  // handle exception
  if (!result.wasInterrupted()) {
    parallelOp.emitError("Cannot find Loop ") << loop_name.str();
    return failure();
  }

  return success();
}

LogicalResult runPipelining(FuncOp &f, PipelineOp &pipelineOp) {
  // 1) Get the schedule
  auto optional_ii = pipelineOp.ii();
  unsigned int ii;
  if (optional_ii.hasValue()) {
    ii = optional_ii.getValue();
  } else {
    ii = 1;
  }
  const auto loop_name =
      dyn_cast<CreateLoopHandleOp>(pipelineOp.loop().getDefiningOp())
          .loop_name();
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(pipelineOp.stage().getDefiningOp())
          .stage_name();

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Find the requested loop and attach attribute
  WalkResult result = rootForOp.walk([&](AffineForOp forOp) -> WalkResult {
    if (loop_name == getLoopName(forOp)) {
      AffineLoopBand band{forOp};
      SmallVector<int, 6> attr_arr{(int)ii};
      setIntAttr(band, attr_arr, "pipeline_ii");
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  // handle exception
  if (!result.wasInterrupted()) {
    pipelineOp.emitError("Cannot find Loop ") << loop_name.str();
    return failure();
  }
  return success();
}

LogicalResult runThreadBind(FuncOp &f, ThreadBindOp &threadBindOp) {
  // 1) Get the schedule
  auto target_dim = threadBindOp.dim();
  const auto loop_name =
      dyn_cast<CreateLoopHandleOp>(threadBindOp.loop().getDefiningOp())
          .loop_name();
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(threadBindOp.stage().getDefiningOp())
          .stage_name();

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Find the requested loop and attach attribute
  WalkResult result = rootForOp.walk([&](AffineForOp forOp) -> WalkResult {
    if (loop_name == getLoopName(forOp)) {
      AffineLoopBand band{forOp};
      SmallVector<int, 6> attr_arr{(int)target_dim};
      setIntAttr(band, attr_arr, "thread_axis");
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  // handle exception
  if (!result.wasInterrupted()) {
    threadBindOp.emitError("Cannot find Loop ") << loop_name.str();
    return failure();
  }
  return success();
}

// modified from lib/Transforms/Utils/LoopUtils.cpp
LogicalResult coalesceLoops(MutableArrayRef<AffineForOp> loops,
                            AffineForOp stageLoop) {
  if (loops.size() < 2)
    return failure();

  AffineForOp innermost = loops.back();
  AffineForOp outermost = loops.front();
  AffineBound ub = outermost.getUpperBound();
  Location loc = outermost.getLoc();
  OpBuilder builder(outermost);
  for (AffineForOp loop : loops) {
    // We only work on normalized loops.
    if (loop.getStep() != 1 || !loop.hasConstantLowerBound() ||
        loop.getConstantLowerBound() != 0)
      return failure();
    // TODO: support AffineMap loop bounds
    if (!loop.hasConstantUpperBound())
      return failure();
  }
  SmallVector<Value, 4> upperBoundSymbols;
  SmallVector<Value, 4> ubOperands(ub.getOperands().begin(),
                                   ub.getOperands().end());

  // 1. Store the upper bound of the outermost loop in a variable.
  // 2. Emit code computing the upper bound of the coalesced loop as product of
  // the number of iterations of all loops.
  int64_t prod = 1;
  for (AffineForOp loop : loops) {
    auto cstUb = loop.getConstantUpperBound();
    prod *= cstUb;
    auto cstOp = builder.create<arith::ConstantIndexOp>(loc, cstUb);
    upperBoundSymbols.push_back(cstOp);
    // hoist to the outermost
    cstOp->moveBefore(stageLoop);
  }
  outermost.setConstantUpperBound(prod);

  builder.setInsertionPointToStart(outermost.getBody());

  // 3. Remap induction variables. For each original loop, the value of the
  // induction variable can be obtained by dividing the induction variable of
  // the linearized loop by the total number of iterations of the loops nested
  // in it modulo the number of iterations in this loop (remove the values
  // related to the outer loops):
  //   iv_i = floordiv(iv_linear, product-of-loop-ranges-until-i) mod range_i.
  // Compute these iteratively from the innermost loop by creating a "running
  // quotient" of division by the range.
  Value previous = outermost.getInductionVar();
  SmallVector<Operation *> opToSink;
  for (unsigned idx = loops.size(); idx > 0; --idx) {
    if (idx != loops.size()) {
      SmallVector<Value, 4> operands;
      operands.push_back(previous);
      operands.push_back(upperBoundSymbols[idx]);
      previous = builder.create<AffineApplyOp>(
          loc,
          AffineMap::get(
              /*numDims=*/1, /*numSymbols=*/1,
              builder.getAffineDimExpr(0).floorDiv(
                  builder.getAffineSymbolExpr(0))),
          operands);
      opToSink.push_back(previous.getDefiningOp());
    }
    // Modified value of the induction variables of the nested loops after
    // coalescing.
    Value inductionVariable;
    if (idx == 1) {
      inductionVariable = previous;
    } else {
      SmallVector<Value, 4> applyOperands;
      applyOperands.push_back(previous);
      applyOperands.push_back(upperBoundSymbols[idx - 1]);
      inductionVariable = builder.create<AffineApplyOp>(
          loc,
          AffineMap::get(
              /*numDims=*/1, /*numSymbols=*/1,
              builder.getAffineDimExpr(0) % builder.getAffineSymbolExpr(0)),
          applyOperands);
      opToSink.push_back(inductionVariable.getDefiningOp());
    }
    replaceAllUsesInRegionWith(loops[idx - 1].getInductionVar(),
                               inductionVariable, loops.back().region());
  }

  // 4. Move the operations from the innermost just above the second-outermost
  // loop, delete the extra terminator and the second-outermost loop.
  AffineForOp secondOutermostLoop = loops[1];
  innermost.getBody()->back().erase();
  outermost.getBody()->getOperations().splice(
      Block::iterator(secondOutermostLoop.getOperation()),
      innermost.getBody()->getOperations());
  secondOutermostLoop.erase();

  // 5. Sink AffineApply operations
  std::reverse(opToSink.begin(), opToSink.end());
  loops[0]->walk([&](AffineForOp forOp) -> WalkResult { // from the innermost
    bool isDominance = true;
    for (auto applyOp : opToSink) {
      applyOp->moveBefore(&(*forOp.getBody()->getOperations().begin()));
      // definition should come before reference
      for (auto user : applyOp->getUsers()) {
        DominanceInfo domInfo;
        if (!domInfo.properlyDominates(applyOp->getResult(0), user)) {
          isDominance = false;
          break;
        }
      }
    }
    if (isDominance)
      return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return success();
}

// Notice hcl.fuse (fuses nested loops) is different from affine.fuse,
// which fuses contiguous loops. This is actually the case of hcl.compute_at.
LogicalResult runFusing(FuncOp &f, FuseOp &fuseOp) {
  // 1) Get the schedule
  const auto loopsToFuse = fuseOp.loops(); // operand_range
  unsigned int sizeOfFusedLoops = loopsToFuse.size();
  if (sizeOfFusedLoops < 2) {
    fuseOp.emitError("Should at least input 2 loops to be fused");
    return failure();
  }
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(fuseOp.stage().getDefiningOp())
          .stage_name();
  SmallVector<StringRef, 6> nameArr;
  for (auto loop : loopsToFuse) {
    nameArr.push_back(
        dyn_cast<CreateLoopHandleOp>(loop.getDefiningOp()).loop_name());
  }

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Find the requested loops
  bool isOuterMost = false;
  AffineLoopBand band;
  WalkResult result = rootForOp.walk([&](AffineForOp forOp) -> WalkResult {
    if (findContiguousNestedLoops(forOp, band, nameArr))
      return WalkResult::interrupt();
    return WalkResult::advance();
  });
  // handle exception
  if (!result.wasInterrupted()) {
    fuseOp.emitError("Cannot find contiguous nested loops starting from Loop ")
        << nameArr[0].str()
        << ". Please specify the loop to be fused from outermost to innermost.";
    return failure();
  }
  if (band[0]->hasAttr("stage_name"))
    isOuterMost = true;

  // 4) Construct new loop
  MutableArrayRef<AffineForOp> fusedLoops =
      llvm::makeMutableArrayRef(band.data(), sizeOfFusedLoops);
  if (failed(coalesceLoops(fusedLoops, rootForOp)))
    return failure();
  if (isOuterMost)
    rootForOp = fusedLoops[0];

  // 5) Constant propagation into the affine map
  SmallVector<Operation *> opToRemove;
  rootForOp.walk([&](AffineApplyOp applyOp) {
    auto applyMap = applyOp.getAffineMap();
    if (applyMap.getNumSymbols() == 0)
      return;
    if (auto cst = dyn_cast<arith::ConstantOp>(
            applyOp.getOperand(1).getDefiningOp())) { // get symbolic operand
      int cstVal = cst.getValue().cast<IntegerAttr>().getInt();
      auto builder = OpBuilder(applyOp);
      SmallVector<AffineExpr> newDims{builder.getAffineDimExpr(0)};
      SmallVector<AffineExpr> newSymbols{builder.getAffineConstantExpr(cstVal)};
      auto newMap = applyMap.replaceDimsAndSymbols(newDims, newSymbols, 1, 0);
      auto newApplyOp = builder.create<AffineApplyOp>(
          applyOp.getLoc(), newMap, llvm::makeArrayRef(applyOp.getOperand(0)));
      applyOp.getResult().replaceAllUsesWith(newApplyOp);
      opToRemove.push_back(applyOp);
    }
  });
  for (Operation *op : opToRemove) {
    op->erase();
  }

  // 6) Add name to the new loop
  std::string new_name;
  for (auto name : nameArr) {
    new_name += name.str() + "_";
  }
  new_name += "fused";
  setLoopName(fusedLoops[0], new_name);
  if (isOuterMost)
    setStageName(fusedLoops[0], stage_name);

  // 7) Create new loop handles &
  //    Link the loop handles with SSA values
  auto firstOp = *(f.getOps<AffineForOp>().begin());
  OpBuilder builder(firstOp);
  auto fused = builder.create<CreateLoopHandleOp>(
      firstOp->getLoc(), LoopHandleType::get(firstOp->getContext()),
      StringAttr::get(firstOp->getContext(), new_name));
  fuseOp.getResult().replaceAllUsesWith(fused);

  return success();
}

LogicalResult runComputeAt(FuncOp &f, ComputeAtOp &computeAtOp) {
  // 1) Get the schedule
  const auto loop_name =
      dyn_cast<CreateLoopHandleOp>(computeAtOp.axis().getDefiningOp())
          .loop_name();
  const auto producer_name =
      dyn_cast<CreateStageHandleOp>(computeAtOp.producer().getDefiningOp())
          .stage_name();
  const auto consumer_name =
      dyn_cast<CreateStageHandleOp>(computeAtOp.consumer().getDefiningOp())
          .stage_name();

  // 2) Traverse all the outer-most loops and find the requested one
  AffineForOp producerFor;
  AffineForOp consumerFor;
  std::pair<bool, bool> isFound{false, false};
  for (auto rootForOp : f.getOps<AffineForOp>()) {
    auto curr_name =
        rootForOp->getAttr("stage_name").cast<StringAttr>().getValue();
    if (producer_name == curr_name) {
      producerFor = rootForOp;
      isFound.first = true;
    } else if (consumer_name == curr_name) {
      consumerFor = rootForOp;
      isFound.second = true;
    }
  }
  if (!isFound.first || !isFound.second) {
    computeAtOp.emitError("Cannot find corresponding producer and consumer");
    return failure();
  }

  // 3) Find the requested loops
  int cnt_depth = 0;
  int requested_depth = 0;
  SmallVector<Value> consumerIVs;
  SmallVector<Value> producerIVs;
  consumerFor.walk([&](AffineForOp forOp) {
    cnt_depth++;
    Attribute attr = forOp->getAttr("loop_name");
    if (loop_name == attr.cast<StringAttr>().getValue()) {
      requested_depth = cnt_depth;
    }
    consumerIVs.push_back(forOp.getInductionVar());
  });
  producerFor.walk([&](AffineForOp forOp) {
    producerIVs.push_back(forOp.getInductionVar());
  });
  std::reverse(consumerIVs.begin(), consumerIVs.end());
  std::reverse(producerIVs.begin(), producerIVs.end());
  requested_depth = cnt_depth - requested_depth + 1;

  // 4) Try to merge two loops
  // TODO: bug: 1) cannot support tensor type
  //            2) doesn't support memref.load, memref.store
  SmallVector<Dependency, 4> dependency;
  if (!analyzeDependency(producerFor, consumerFor, dependency)) {
    std::string err_msg =
        "Does not support compute_at of stage with if operation.";
    computeAtOp.emitError("analyzeDependency Failed: ") << err_msg;
  }

  FusionStrategy strategy(FusionStrategy::Generic);
  if (dependency.size() > 0) {
    if (std::find(dependency.begin(), dependency.end(), Dependency::RAW) !=
        dependency.end()) {
      strategy = FusionStrategy::ProducerConsumer;
    } else {
      strategy = FusionStrategy::Generic;
    }
    // use existing MLIR pass
    ComputationSliceState sliceUnion;
    FusionResult result = canFuseLoops(producerFor, consumerFor,
                                       requested_depth, &sliceUnion, strategy);
    std::string err_msg;
    if (result.value == FusionResult::Success) {
      fuseLoops(producerFor, consumerFor, sliceUnion);
      producerFor.erase();
    } else if (result.value == FusionResult::FailPrecondition) {
      err_msg = "failed precondition for fusion (e.g. same block)";
    } else if (result.value == FusionResult::FailBlockDependence) {
      err_msg = "fusion would violate another dependence in block";
    } else if (result.value == FusionResult::FailFusionDependence) {
      err_msg = "fusion would reverse dependences between loops";
    } else if (result.value == FusionResult::FailComputationSlice) {
      err_msg = "unable to compute src loop computation slice";
    } else if (result.value == FusionResult::FailIncorrectSlice) {
      err_msg = "slice is computed, but it is incorrect";
    }
    if (result.value != FusionResult::Success) {
      computeAtOp.emitError("Cannot merge these two loops because ") << err_msg;
      return failure();
    }
  } else {
    // strategy = FusionStrategy::Sibling;
    computeAtOp.emitWarning(
        "MLIR loop fusion pass failed. Attempt using HCL's loop fusion pass.");
    // get inner loops
    AffineForOp secondForOp = consumerFor;
    getLoop(secondForOp, loop_name);
    int curr_depth = 0;
    AffineForOp firstForOp;
    producerFor.walk([&](AffineForOp forOp) {
      if (curr_depth++ == cnt_depth - requested_depth) {
        firstForOp = forOp;
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    auto &firstBody = firstForOp.getBody()->getOperations();
    auto &secondBody = secondForOp.getBody()->getOperations();
    // do not need affine.yield op, so that's why using std::prev
    secondBody.splice(secondBody.begin(), firstBody, firstBody.begin(),
                      std::prev(firstBody.end()));
    // update references
    for (int i = 0; i < requested_depth; ++i)
      producerIVs[i].replaceAllUsesWith(consumerIVs[i]);
    producerFor.erase();
    return success();
  }

  // 5) remove intermediate buffers & loads/stores
  SmallVector<Operation *, 10> opToRemove;
  memref::AllocOp alloc;
  AffineStoreOp targetStore;
  consumerFor.walk([&](AffineStoreOp store) {
    if (!store.getOperand(1).getDefiningOp())
      return WalkResult::advance();
    auto buf = dyn_cast<memref::AllocOp>(store.getOperand(1).getDefiningOp());
    if (buf->hasAttr("name") &&
        buf->getAttr("name").cast<StringAttr>().getValue().str() ==
            producer_name) {
      alloc = buf;
      targetStore = store;
      opToRemove.push_back(store);
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  consumerFor.walk([&](AffineLoadOp load) {
    if (load->hasAttr("from") &&
        load->getAttr("from").cast<StringAttr>().getValue().str() ==
            producer_name) {
      load.getResult().replaceAllUsesWith(targetStore.getOperand(0));
      opToRemove.push_back(load);
    }
    return WalkResult::advance();
  });
  if (alloc && alloc.getResult().use_empty()) {
    opToRemove.push_back(alloc);
  }
  for (Operation *op : opToRemove) {
    op->erase();
  }

  return success();
}

bool findArray(FuncOp &f, const Value &target, Value &ret_array) {
  if (!target.getDefiningOp()) { // in func args
    for (auto arg : f.getArguments()) {
      if (target == arg) { // found the corresponding array
        ret_array = arg;
        return true;
      }
    }
    return false;
  } else {
    ret_array = target;
    return true;
  }
}

// https://github.com/hanchenye/scalehls/blob/master/lib/Transforms/Directive/ArrayPartition.cpp
LogicalResult runPartition(FuncOp &f, PartitionOp &partitionOp, Value &array) {
  // 1) Get the schedule
  // auto memref = partitionOp.target(); // return a Value type
  auto kind = partitionOp.partition_kind();
  unsigned int target_dim = partitionOp.dim();
  auto optional_factor = partitionOp.factor();
  int factor = 0;
  if (optional_factor.hasValue()) {
    factor = optional_factor.getValue();
  } else {
    factor = -1;
    if (kind != PartitionKindEnum::CompletePartition) {
      partitionOp.emitError("Should pass in `factor' for array partition");
      return failure();
    }
  }

  // 2) Find the requested array
  // has been done in findArray

  // 3) Construct new memory layout map
  auto builder = Builder(array.getContext());
  auto arrayType = array.getType().dyn_cast<MemRefType>();
  auto layout = arrayType.getLayout().getAffineMap();

  // Walk through each dimension of the current memory
  SmallVector<AffineExpr, 4> partitionIndices;
  SmallVector<AffineExpr, 4> addressIndices;

  // first N: partition index
  // last N : physical index
  unsigned rank = arrayType.getRank();
  if (layout.getNumResults() != rank) {
    partitionOp.emitWarning("Partition on the array partitioned before. "
                            "The original layout map will be rewritten!");
  }
  for (int64_t dim = 0; dim < rank; ++dim) {
    if (target_dim == 0 || (target_dim > 0 && dim == target_dim - 1)) {
      if (kind == PartitionKindEnum::CyclicPartition) {
        // original index:  0, 1, 2, 3
        // bank (factor 2): 0, 1, 0, 1
        partitionIndices.push_back(builder.getAffineDimExpr(dim) % factor);
        addressIndices.push_back(
            builder.getAffineDimExpr(dim).floorDiv(factor));
      } else if (kind == PartitionKindEnum::BlockPartition) {
        // * block factor N means partition into N blocks
        //   each block has shape[dim] / factor elements
        //   (not N elements in each block!)
        // original index:  0, 1, 2, 3
        // bank (factor 2): 0, 0, 1, 1
        auto blockFactor =
            (arrayType.getShape()[dim] + factor - 1) / factor; // ceil
        partitionIndices.push_back(
            builder.getAffineDimExpr(dim).floorDiv(blockFactor));
        addressIndices.push_back(builder.getAffineDimExpr(dim) % blockFactor);
      } else if (kind == PartitionKindEnum::CompletePartition) {
        // original index:  0, 1, 2, 3
        // bank (factor 2): 0, 1, 2, 3
        partitionIndices.push_back(builder.getAffineDimExpr(dim));
        addressIndices.push_back(builder.getAffineConstantExpr(0));
      } else {
        partitionOp.emitError("No this partition kind");
        return failure();
      }
    } else {
      if (layout.getNumResults() == rank) {
        partitionIndices.push_back(builder.getAffineConstantExpr(0));
        addressIndices.push_back(builder.getAffineDimExpr(dim));
      } else { // already had one layout map before
        partitionIndices.push_back(layout.getResult(dim));
        addressIndices.push_back(layout.getResult(dim));
      }
    }
  }

  // Construct new layout map
  partitionIndices.append(addressIndices.begin(), addressIndices.end());
  auto layoutMap = AffineMap::get(arrayType.getRank(), 0, partitionIndices,
                                  builder.getContext());

  // Construct new array type
  auto newType =
      MemRefType::get(arrayType.getShape(), arrayType.getElementType(),
                      layoutMap, arrayType.getMemorySpace());

  // Set new type
  array.setType(newType);

  // 4) update function signature
  auto resultTypes = f.front().getTerminator()->getOperandTypes();
  auto inputTypes = f.front().getArgumentTypes();
  f.setType(builder.getFunctionType(inputTypes, resultTypes));

  return success();
}

LogicalResult runReuseAt(FuncOp &f, ReuseAtOp &reuseAtOp) {
  // 1) Get the schedule
  auto target = reuseAtOp.target(); // return a Value type
  const auto loop_name =
      dyn_cast<CreateLoopHandleOp>(reuseAtOp.axis().getDefiningOp())
          .loop_name();
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(reuseAtOp.stage().getDefiningOp())
          .stage_name();
  auto arrayType = target.getType().dyn_cast<MemRefType>();
  unsigned int rank = arrayType.getRank();

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 3) Find the requested loop and get the axis id
  AffineForOp reuseLoop = rootForOp;
  int loopAxis = getLoop(reuseLoop, loop_name);
  if (loopAxis == -1) {
    f.emitError("Cannot find Loop ") << loop_name.str();
    return failure();
  }

  // 4) Find (non-)reduction loops
  AffineLoopBand nonReductionLoops;
  AffineLoopBand previousShiftLoops;
  // InductionVar -> Loop upper bound
  DenseMap<Value, int> reductionVars;
  WalkResult result = rootForOp.walk([&](AffineForOp forOp) {
    if (forOp.getStep() != 1 || !forOp.hasConstantLowerBound() ||
        forOp.getConstantLowerBound() != 0 || !forOp.hasConstantUpperBound()) {
      reuseAtOp.emitError("Loop ")
          << getLoopName(forOp).str()
          << " must have (1) constant bounds (2) constant step (3) zero "
             "lower bound";
      return WalkResult::interrupt();
    }
    if (!forOp->hasAttr("reduction") && !forOp->hasAttr("spatial") &&
        !forOp->hasAttr("buffer")) {
      nonReductionLoops.push_back(forOp);
    } else if (forOp->hasAttr("spatial")) {
      previousShiftLoops.push_back(forOp);
    } else if (forOp->hasAttr("reduction")) {
      reductionVars[forOp.getInductionVar()] = forOp.getConstantUpperBound();
    }
    return WalkResult::advance();
  });
  if (result.wasInterrupted())
    return failure();
  std::reverse(nonReductionLoops.begin(), nonReductionLoops.end());
  AffineForOp innerMostForOp = nonReductionLoops[nonReductionLoops.size() - 1];

  // 5) Get span of each dimension
  //    e.g. d0, d0+1, d0+2, span is 2
  //         d0+d1, d1\in[0,2], span is 2
  SmallVector<SmallVector<AffineExpr>> originalLoadExprs;
  for (int i = 0; i < (int)rank; ++i) {
    SmallVector<AffineExpr> tmp;
    originalLoadExprs.push_back(tmp);
  }
  int cntLoad = 0;
  DenseMap<AffineExpr, Value> dim2iv; // dim -> induction var
  reuseLoop.walk([&](AffineLoadOp loadOp) {
    if (loadOp.getOperand(0) != target)
      return WalkResult::advance();
    cntLoad++;
    for (int i = 0; i < (int)rank; ++i)
      originalLoadExprs[i].push_back(loadOp.getAffineMap().getResult(i));
    OpBuilder builder(loadOp);
    for (auto operandItem : llvm::enumerate(loadOp.getMapOperands())) {
      dim2iv[builder.getAffineDimExpr(operandItem.index())] =
          operandItem.value();
    }
    return WalkResult::advance();
  });
  SmallVector<int> spans;
  for (int i = 0; i < (int)rank; ++i) {
    int span = 0;
    // TODO: require strict load order
    AffineExpr baseExpr = originalLoadExprs[i][0];
    int baseCst = 0;
    if (baseExpr.isa<AffineDimExpr>()) {
      bool allAffineDimExpr = true;
      for (int j = 0; j < cntLoad; ++j) {
        auto diff = originalLoadExprs[i][j] - baseExpr;
        if (!originalLoadExprs[i][j].isa<AffineDimExpr>())
          allAffineDimExpr = false;
        if (diff.isa<AffineConstantExpr>()) {
          span = std::max(span,
                          (int)diff.cast<AffineConstantExpr>().getValue() + 1);
        } else {
          assert(1 == 0 && "Load order is not strict");
        }
      }
      if (allAffineDimExpr &&
          reductionVars.count(dim2iv[baseExpr.cast<AffineDimExpr>()]) > 0) {
        span = reductionVars[dim2iv[baseExpr.cast<AffineDimExpr>()]];
      }
    } else if (baseExpr.isa<AffineConstantExpr>()) {
      for (int j = 0; j < cntLoad; ++j) {
        auto diff = originalLoadExprs[i][j] - baseExpr;
        if (diff.isa<AffineConstantExpr>()) {
          span = std::max(span,
                          (int)diff.cast<AffineConstantExpr>().getValue() + 1);
        } else {
          assert(1 == 0 && "Load order is not strict");
        }
      }
    } else { // AffineBinaryOpExpr, reduction
      auto binaryExpr = baseExpr.cast<AffineBinaryOpExpr>();
      int cntDim = 0;
      binaryExpr.walk([&](AffineExpr expr) {
        // d0 + d1, d1 is the reduction variable
        if (expr.isa<AffineDimExpr>()) {
          auto dimExpr = expr.cast<AffineDimExpr>();
          if (cntDim == 1) {
            if (reductionVars.count(dim2iv[dimExpr]) > 0) {
              span = reductionVars[dim2iv[dimExpr]];
            }
          }
        } else if (expr.isa<AffineConstantExpr>()) {
          int cst = expr.cast<AffineConstantExpr>().getValue();
          if (baseCst == 0)
            baseCst = cst;
          span = std::max(span, cst - baseCst + 1);
        }
        cntDim++;
        return WalkResult::advance();
      });
    }
    assert(span != 0 && "Span should not be 0");
    spans.push_back(span);
  }

  // 6) Obtain AffineMaps of load instructions
  // if i-th axis has reduction var before the reuse axis
  //  reductionLoopBound[i] should be the dimension size
  // if i-th axis has reduction var after the reuse axis
  //  target.shape[i] should be the dimension size
  std::set<AffineExpr, ExprCompare> requestedVars;
  SmallVector<AffineLoadOp> allLoadOps;
  std::map<int, int> dimBounds; // dim expr->reduction bound
  int axis = -1;
  int distance = -1;
  int numLoadOp = 0;
  // TODO: eliminate order in inputs
  reuseAtOp.emitWarning("Need to guarantee the loads have orders");
  reuseLoop.walk([&](AffineLoadOp loadOp) {
    if (loadOp.getOperand(0) != target)
      return WalkResult::advance();
    numLoadOp++;
    auto loadMap = loadOp.getAffineMap();
    int numDims = loadMap.getNumDims();
    auto operands = loadOp.getMapOperands();
    int rDim = -1;
    int operandIdx = 0;
    for (int j = 0; j < (int)loadMap.getNumResults(); ++j) {
      AffineExpr expr = loadMap.getResult(j);
      if (axis == -1) {
        if (expr.isa<AffineDimExpr>()) {
          if (operands[operandIdx++] ==
              nonReductionLoops[loopAxis].getInductionVar()) {
            axis = j;
          }
        } else if (expr.isa<AffineBinaryOpExpr>()) {
          if (operands[operandIdx++] ==
              nonReductionLoops[loopAxis].getInductionVar())
            axis = j;
          int cntDim = 0;
          for (int i = 0; i < numDims; ++i)
            if (expr.isFunctionOfDim(i))
              cntDim++;
          if (cntDim > 1)
            if (operands[operandIdx++] ==
                nonReductionLoops[loopAxis].getInductionVar())
              axis = j;
        }
      }
      for (int i = 0; i < numDims; ++i) {
        if (expr.isFunctionOfDim(i) && reductionVars.count(operands[i]) > 0) {
          dimBounds[i] = reductionVars[operands[i]];
          if (j == axis) // target reuse axis
            rDim = i;
        }
      }
    }
    assert(axis != -1);
    OpBuilder builder(loadOp);
    AffineExpr expr = loadMap.getResult(axis);
    auto insertLoadOp = [&](AffineLoadOp loadOp) {
      int size = allLoadOps.size();
      auto exp1 = loadOp.getAffineMap().getResult(axis);
      ExprCompare cmp;
      for (int i = 0; i < size; ++i) {
        int val1 = cmp.findConstantExpr(exp1);
        auto exp2 = allLoadOps[i].getAffineMap().getResult(axis);
        int val2 = cmp.findConstantExpr(exp2);
        if (val1 < val2) {
          allLoadOps.insert(allLoadOps.begin() + i, loadOp);
          return;
        }
      }
      allLoadOps.push_back(loadOp);
    };
    insertLoadOp(loadOp);
    if (rDim != -1) {
      int ub = reductionVars[operands[rDim]];
      distance = ub - 1;
      for (int j = 0; j < ub; j++) {
        auto ubCstExpr = builder.getAffineConstantExpr(j);
        auto newExpr = expr.replace(builder.getAffineDimExpr(rDim), ubCstExpr);
        requestedVars.insert(newExpr);
      }
    } else {
      requestedVars.insert(expr);
      auto var = expr - *(requestedVars.begin());
      distance = std::max(distance,
                          (int)(var.dyn_cast<AffineConstantExpr>().getValue()));
    }
    return WalkResult::advance();
  });
  assert(distance > -1);

  // 7) Try to find reuse pattern
  //    TODO: support more reuse patterns
  bool canReuse = false;
  auto baseVar = *(requestedVars.begin());
  for (auto var : requestedVars) {
    if (std::find(requestedVars.begin(), requestedVars.end(), var + 1) !=
        requestedVars.end()) {
      canReuse = true;
      break;
    }
  }
  if (!canReuse) {
    reuseAtOp.emitError("Cannot find reuse pattern on axis ")
        << std::to_string(loopAxis)
        << ". Only support stride 1 reuse pattern now";
    return failure();
  }

  // 8) Obtain indices and strides in load instructions
  SmallVector<AffineMap> allLoadAffineMaps;
  SmallVector<SmallVector<Value>> allLoadOperands;
  SmallVector<int> preRDim;
  SmallVector<int> preRDimAxis;
  int rDim = -1;
  AffineLoadOp originalLoadOp;
  bool resultFlag = true;
  for (auto loadOp : allLoadOps) {
    auto loadMap = loadOp.getAffineMap();
    // e.g. d0 d0+2, diff=2
    //      d0 d0+d1, diff=d1
    auto var = loadMap.getResult(axis);
    auto diff = var - baseVar;

    // find reduction dimension
    auto getReductionDim = [&](AffineExpr expr) {
      for (auto item : dimBounds)
        if (expr.isFunctionOfDim(item.first))
          return item.first;
      return -1;
    };
    rDim = getReductionDim(diff);

    // obtain load expressions
    OpBuilder builder(loadOp);
    if (rDim != -1) { // is reduction
      int ub = dimBounds[rDim];
      auto operands = loadOp.getMapOperands();
      originalLoadOp = loadOp;
      // expand the reduction axis
      for (int j = 0; j < ub; j++) {
        SmallVector<AffineExpr> singleLoadAffineExpr;
        SmallVector<Value> memAffineIndices;
        int loadRank = 0; // loadOp.getMapOperands().size();
        int operandIdx = 0;
        // TODO: better mapping machanism for high-dimensional tensors
        // i < axis
        for (int i = 0; i < axis; ++i) {
          auto expr = loadMap.getResult(i);
          // TODO: only suppose the expr is in the format of d0+d1
          int d = getReductionDim(expr);
          if (d != -1) {
            // reduction axis before reuse axis
            if (std::find(preRDim.begin(), preRDim.end(), d) == preRDim.end()) {
              preRDim.push_back(d);
              preRDimAxis.push_back(i);
            }
            singleLoadAffineExpr.push_back(
                builder.getAffineDimExpr(loadRank++));
            operandIdx++;
            memAffineIndices.push_back(operands[operandIdx++]);
          } else if (spans[i] > 1) { // AffineConstantExpr
            singleLoadAffineExpr.push_back(expr);
          }
        }
        // i = axis
        // TODO: suppose the expr is d0+d1
        singleLoadAffineExpr.push_back(builder.getAffineConstantExpr(j));
        operandIdx++;
        // i > axis
        for (unsigned int i = axis + 1; i < rank; ++i) {
          auto expr = loadMap.getResult(i);
          if (expr.isa<AffineBinaryOpExpr>()) {
            singleLoadAffineExpr.push_back(
                builder.getAffineDimExpr(loadRank++));
            memAffineIndices.push_back(operands[operandIdx++]);
            operandIdx++;
          } else if (expr.isa<AffineDimExpr>()) {
            singleLoadAffineExpr.push_back(
                builder.getAffineDimExpr(loadRank++));
            memAffineIndices.push_back(operands[operandIdx++]);
          } else { // AffineConstantExpr
            singleLoadAffineExpr.push_back(expr);
          }
        }
        auto affineMap = AffineMap::get(
            loadRank /*rank*/, 0, singleLoadAffineExpr, builder.getContext());
        if (std::find(allLoadAffineMaps.begin(), allLoadAffineMaps.end(),
                      affineMap) == allLoadAffineMaps.end()) {
          allLoadAffineMaps.push_back(affineMap);
          allLoadOperands.push_back(memAffineIndices);
        }
      }
    } else {
      originalLoadOp = loadOp;
      int loadRank = 0;
      int operandIdx = 0;
      auto operands = loadOp.getMapOperands();
      SmallVector<Value> memAffineIndices;
      SmallVector<AffineExpr> singleLoadAffineExpr;
      // i < axis
      for (int i = 0; i < axis; ++i) {
        if (spans[i] > 1) {
          // placeholder
          singleLoadAffineExpr.push_back(builder.getAffineDimExpr(loadRank++));
          memAffineIndices.push_back(operands[operandIdx]);
        }
      }
      // i = axis
      if (diff.isa<AffineConstantExpr>()) {
        singleLoadAffineExpr.push_back(diff);
      } else {
        reuseAtOp.emitError("Cannot support non-constant stride");
        resultFlag = false;
        break;
      }
      // i > axis
      for (unsigned int i = axis + 1; i < rank; ++i) {
        singleLoadAffineExpr.push_back(builder.getAffineDimExpr(loadRank++));
        memAffineIndices.push_back(operands[operandIdx++]);
      }
      auto affineMap = AffineMap::get(loadRank, 0, singleLoadAffineExpr,
                                      builder.getContext());
      if (std::find(allLoadAffineMaps.begin(), allLoadAffineMaps.end(),
                    affineMap) == allLoadAffineMaps.end()) {
        allLoadAffineMaps.push_back(affineMap);
        allLoadOperands.push_back(memAffineIndices);
      }
    }
  }
  if (!resultFlag)
    return failure();

  // 9) Create reuse buffer
  //    e.g., %1 = memref.alloc() : memref<3xi32>
  SmallVector<int64_t> shape;
  // i < axis
  for (int i = 0; i < axis; ++i)
    if (spans[i] > 1)
      shape.push_back(spans[i]);
  // i = axis
  shape.push_back(distance + 1);
  // i > axis
  for (unsigned int i = axis + 1; i < rank; ++i)
    shape.push_back(arrayType.getShape()[i]);
  OpBuilder out_builder(rootForOp); // outside the stage
  auto buf = out_builder.create<memref::AllocOp>(
      rootForOp.getLoc(),
      MemRefType::get(
          shape, target.getType().dyn_cast<MemRefType>().getElementType()));
  buf->setAttr("name", StringAttr::get(buf->getContext(),
                                       StringRef(stage_name.str() + "_reuse_" +
                                                 std::to_string(loopAxis))));

  // 10) link the result SSA with the buffer
  reuseAtOp.getResult().replaceAllUsesWith(buf);

  // 11) Update loop bound
  // TODO: support non-constant bound
  nonReductionLoops[loopAxis].setConstantUpperBound(
      target.getType().dyn_cast<MemRefType>().getShape()[axis]);

  // 12) Update store index, since some load/store will be created later, this
  // step is done in advance reduction case:
  //   skip the first store (to reduction variable)
  //     affine.store %0, %1[%c0] {to = "sum_rv"} : memref<1xi32>
  //   update the outer store
  //     affine.store %6, %3[%arg1, %arg2] : memref<10x8xi32>
  // non-reduction case:
  //   affine.store %9, %0[%arg1, %arg2] : memref<10x8xi32>
  // * index should be changed to [%arg1, %arg2 - 2]
  SmallVector<Operation *> opToRemove;
  reuseLoop.walk([&](AffineStoreOp op) {
    // skip reduction variable store
    auto arrayType = op.getOperand(1).getType().dyn_cast<MemRefType>();
    if (arrayType.getRank() == 1 && arrayType.getShape()[0] == 1) {
      return WalkResult::advance();
    }
    // update the store to output tensor
    OpBuilder rewriter(op);
    SmallVector<AffineExpr> memAffineIndices;
    auto oldAffineMap = op.getAffineMap();
    for (unsigned int i = 0, e = oldAffineMap.getResults().size(); i < e; ++i) {
      AffineExpr idx;
      if ((int)i == loopAxis)
        // the iteration space now is related to the input tensor
        idx = oldAffineMap.getResult(i) - distance;
      else
        idx = oldAffineMap.getResult(i);
      memAffineIndices.push_back(idx);
    }
    auto affineMap = AffineMap::get(arrayType.getRank() /*rank*/, 0,
                                    memAffineIndices, rewriter.getContext());
    rewriter.create<AffineStoreOp>(
        op->getLoc(), op.getOperand(0) /*valueToStore*/,
        op.getOperand(1) /*memref*/, affineMap, op.indices());
    opToRemove.push_back(op);
    return WalkResult::advance();
  });

  // 13) Rewrite original memref to load from buffer
  // reduction case:
  //   skip the first load (from reduction variable)
  //     %1 = affine.load %0[%c0] {from = "sum_rv"} : memref<1xi32>
  //   update the non-reduction load
  //     %7 = affine.load %arg0[%arg1, %arg2 + %arg3] : memref<10x10xi32>
  // * load should be changed to %buf[%arg3]
  // non-reduction case:
  //   %4 = affine.load %arg0[%arg1, %arg2 + 0,1,2] : memref<10x10xi32>
  // * load should be changed to %buf[0,1,2]
  // * buffer shifting will be done later
  for (auto op : allLoadOps) {
    OpBuilder rewriter(op);
    SmallVector<AffineExpr> loadAffineExpr;
    SmallVector<Value> memAffineIndices;
    SmallVector<Value> operands = op.getMapOperands();
    auto loadMap = op.getAffineMap();

    // obtain load expressions
    AffineLoadOp newLoad;
    if (rDim == -1) { // reuse the found rDim value
      auto diff = loadMap.getResult(axis) - baseVar;
      loadAffineExpr.push_back(diff);
      int loadRank = 0;
      int operandIdx = 0;
      // i < axis
      for (int i = 0; i < axis; ++i) {
        if (spans[i] > 1) {
          loadAffineExpr.push_back(loadMap.getResult(i));
        }
      }
      // i > axis
      SmallVector<AffineExpr> dims;
      for (int i = 0; i < axis + 1; ++i) {
        auto expr = loadMap.getResult(i);
        if (!expr.isa<AffineConstantExpr>()) {
          operandIdx++;
          dims.push_back(rewriter.getAffineDimExpr(0)); // placeholder
        }
      }
      for (unsigned int i = axis + 1; i < rank; ++i) {
        dims.push_back(rewriter.getAffineDimExpr(loadRank++));
      }
      for (unsigned int i = axis + 1; i < rank; ++i) {
        auto expr = loadMap.getResult(i);
        auto new_expr = expr.replaceDims(dims);
        loadAffineExpr.push_back(new_expr);
        memAffineIndices.push_back(operands[operandIdx++]);
      }
      auto affineMap = AffineMap::get(loadRank /*rank*/, 0, loadAffineExpr,
                                      rewriter.getContext());
      newLoad = rewriter.create<AffineLoadOp>(op->getLoc(), buf, affineMap,
                                              memAffineIndices);
    } else { // reduction
      int loadRank = 0;
      int operandIdx = 0;
      for (int i = 0; i < (int)rank; ++i) {
        auto expr = loadMap.getResult(i);
        // TODO: only suppose the expr is in the format of d0+d1, and d1 is
        // reduction axis
        if (i < axis) {
          if (spans[i] > 1) {
            if (expr.isa<AffineBinaryOpExpr>()) {
              loadAffineExpr.push_back(rewriter.getAffineDimExpr(loadRank++));
              operandIdx++;
            } else if (expr.isa<AffineDimExpr>()) {
              loadAffineExpr.push_back(rewriter.getAffineDimExpr(loadRank++));
            } else { // expr is a constant
              loadAffineExpr.push_back(expr);
            }
            memAffineIndices.push_back(operands[operandIdx++]);
          } else {
            // TODO: suppose no other reduction axis before `axis`
            operandIdx++;
          }
        } else if (i == axis) {
          loadAffineExpr.push_back(rewriter.getAffineDimExpr(loadRank++));
          if (expr.isa<AffineBinaryOpExpr>()) // put reduction dim
            operandIdx++;
          memAffineIndices.push_back(operands[operandIdx++]);
        } else { // i > axis
          if (expr.isa<AffineBinaryOpExpr>()) {
            auto dim0 = rewriter.getAffineDimExpr(loadRank++);
            auto dim1 = rewriter.getAffineDimExpr(loadRank++);
            loadAffineExpr.push_back(dim0 + dim1);
            memAffineIndices.push_back(operands[operandIdx++]);
            memAffineIndices.push_back(operands[operandIdx++]);
          } else if (expr.isa<AffineDimExpr>()) {
            loadAffineExpr.push_back(rewriter.getAffineDimExpr(loadRank++));
            memAffineIndices.push_back(operands[operandIdx++]);
          } else { // AffineConstantExpr
            loadAffineExpr.push_back(expr);
          }
        }
      }
      auto affineMap = AffineMap::get(loadRank /*rank*/, 0, loadAffineExpr,
                                      rewriter.getContext());
      newLoad = rewriter.create<AffineLoadOp>(op->getLoc(), buf, affineMap,
                                              memAffineIndices);
    }
    op->replaceAllUsesWith(newLoad);
    opToRemove.push_back(op);
  }

  // 14) Create if structure
  //     only if the indices are inside the output tensor iteration space,
  //     results will be computed and written to output
  int cntIf = 0;
  nonReductionLoops[0].walk([&](AffineIfOp ifOp) { cntIf++; });
  nonReductionLoops[nonReductionLoops.size() - 1].walk(
      [&](AffineIfOp ifOp) { cntIf--; });
  AffineIfOp ifOp;
  if (!llvm::isa<AffineIfOp>(
          nonReductionLoops[loopAxis].getBody()->getOperations().front())) {
    OpBuilder builder(
        &(nonReductionLoops[loopAxis].getBody()->getOperations().front()));
    auto loc = nonReductionLoops[loopAxis]
                   .getBody()
                   ->getOperations()
                   .begin()
                   ->getLoc();
    // e.g. #set = affine_set<(d0, d1)[s0]: (d0 - 10 >= 0, s0 - d0 - 9 >= 0,
    //                                d1 - 10 >= 0, s0 - d1 - 9 >= 0)>
    SmallVector<AffineExpr> constraints{builder.getAffineDimExpr(0) - distance};
    SmallVector<bool> eqFlags{false};
    auto ifCondSet = IntegerSet::get(
        1 /*dimCount*/, 0 /*symbolCount*/,
        constraints /*ArrayRef<AffineExpr> constraints*/, eqFlags);
    SmallVector<Value, 4> setOperands{
        nonReductionLoops[loopAxis].getInductionVar()};
    ifOp = builder.create<AffineIfOp>(loc, ifCondSet, setOperands,
                                      /*withElseRegion=*/false);
    auto &innerMostBody =
        nonReductionLoops[loopAxis].getBody()->getOperations();
    auto &ifThenBody = ifOp.getThenBlock()->getOperations();
    ifThenBody.splice(ifThenBody.begin(), innerMostBody,
                      std::next(innerMostBody.begin()),
                      std::prev(innerMostBody.end()));
  } else {
    auto outerIfOp = llvm::cast<AffineIfOp>(
        innerMostForOp.getBody()->getOperations().front());
    // skip the first if statement
    OpBuilder builder(&(*(outerIfOp.getThenBlock()->getOperations().begin())));
    auto loc = outerIfOp.getThenBlock()->getOperations().begin()->getLoc();
    SmallVector<AffineExpr> constraints{builder.getAffineDimExpr(0) - distance};
    SmallVector<bool> eqFlags{false};
    auto ifCondSet = IntegerSet::get(
        1 /*dimCount*/, 0 /*symbolCount*/,
        constraints /*ArrayRef<AffineExpr> constraints*/, eqFlags);
    SmallVector<Value, 4> setOperands{
        nonReductionLoops[loopAxis].getInductionVar()};
    ifOp = builder.create<AffineIfOp>(loc, ifCondSet, setOperands,
                                      /*withElseRegion=*/false);
    auto &innerMostBody = outerIfOp.getThenBlock()->getOperations();
    auto &ifThenBody = ifOp.getThenBlock()->getOperations();
    ifThenBody.splice(ifThenBody.begin(), innerMostBody,
                      std::next(innerMostBody.begin()),
                      std::prev(innerMostBody.end()));
    ifOp = outerIfOp;
  }

  // 15) shift buffer elements & load from memory to buffer
  // reduction case:
  // non-reduction case:
  //   %2 = affine.load %1[1] : memref<3xi32>
  //   affine.store %2, %1[0] : memref<3xi32>
  //   %3 = affine.load %1[2] : memref<3xi32>
  //   affine.store %3, %1[1] : memref<3xi32>
  //   %4 = affine.load %arg0[%arg1, %arg2] : memref<10x10xi32>
  //   affine.store %4, %1[2] : memref<3xi32>
  OpBuilder builder(ifOp);
  Location loc = ifOp.getLoc();
  if (!llvm::isa<AffineIfOp>(ifOp.getThenBlock()->getOperations().front())) {
    loc = nonReductionLoops[loopAxis]
              .getBody()
              ->getOperations()
              .begin()
              ->getLoc();
    builder = OpBuilder(
        &(*(nonReductionLoops[loopAxis].getBody()->getOperations().begin())));
  } else {
    ifOp = llvm::cast<AffineIfOp>(
        innerMostForOp.getBody()->getOperations().front());
    loc = ifOp.getThenBlock()->getOperations().begin()->getLoc();
    builder = OpBuilder(&(*(ifOp.getThenBlock()->getOperations().begin())));
  }
  AffineLoopBand shiftForOps; // after reuse `axis`
  for (unsigned int i = loopAxis + 1; i < nonReductionLoops.size(); ++i) {
    auto ub =
        target.getType().dyn_cast<MemRefType>().getShape()[i - loopAxis + axis];
    shiftForOps.push_back(builder.create<AffineForOp>(loc, 0, ub));
    shiftForOps.back()->setAttr("spatial", builder.getUnitAttr());
    builder =
        OpBuilder(&(*(shiftForOps.back().getBody()->getOperations().begin())));
    loc = shiftForOps.back().getBody()->getOperations().begin()->getLoc();
  }
  AffineLoopBand reductionForOps; // before reuse `axis`
  for (int i = 0; i < axis; ++i) {
    if (spans[i] > 1) {
      reductionForOps.push_back(builder.create<AffineForOp>(loc, 0, spans[i]));
      reductionForOps.back()->setAttr("spatial", builder.getUnitAttr());
      builder = OpBuilder(
          &(*(reductionForOps.back().getBody()->getOperations().begin())));
      loc = reductionForOps.back().getBody()->getOperations().begin()->getLoc();
    }
  }

  std::size_t numLoad = allLoadAffineMaps.size();
  for (std::size_t loadCnt = 0; loadCnt < numLoad; ++loadCnt) {
    AffineLoadOp load;
    if (loadCnt < numLoad - 1) { // load from buffer
      if (allLoadOperands[loadCnt + 1].size() > 0)
        for (unsigned int j = 0; j < reductionForOps.size(); ++j) {
          allLoadOperands[loadCnt + 1][j] =
              reductionForOps[j].getInductionVar();
        }
      std::size_t size = allLoadOperands[loadCnt + 1].size();
      for (unsigned int j = size - shiftForOps.size(); j < size; ++j) {
        allLoadOperands[loadCnt + 1][j] =
            shiftForOps[j - size + shiftForOps.size()].getInductionVar();
      }
      load =
          builder.create<AffineLoadOp>(loc, buf, allLoadAffineMaps[loadCnt + 1],
                                       allLoadOperands[loadCnt + 1]);
    } else { // load from memory
      if (reductionForOps.size() > 0) {
        SmallVector<AffineExpr> loadAffineExpr;
        SmallVector<Value> memAffineIndices;
        auto operands = originalLoadOp.getMapOperands();
        auto loadMap = originalLoadOp.getAffineMap();
        int operandIdx = 0;
        int loadRank = 0;
        int RLCnt = 0; // reduction loop count
        int SLCnt = 0; // shift loop count
        for (int i = 0; i < (int)rank; ++i) {
          auto expr = loadMap.getResult(i);
          if (i < axis) {
            if (spans[i] > 1) {
              if (expr.isa<AffineBinaryOpExpr>()) {
                auto dim0 = builder.getAffineDimExpr(loadRank++);
                auto dim1 = builder.getAffineDimExpr(loadRank++);
                loadAffineExpr.push_back(dim0 + dim1);
                memAffineIndices.push_back(
                    nonReductionLoops[i].getInductionVar());
                memAffineIndices.push_back(
                    reductionForOps[RLCnt++].getInductionVar());
                operandIdx++;
                operandIdx++;
              } else if (expr.isa<AffineDimExpr>()) { // single reduction
                loadAffineExpr.push_back(builder.getAffineDimExpr(loadRank++));
                memAffineIndices.push_back(
                    reductionForOps[RLCnt++].getInductionVar());
                operandIdx++;
              } else { // AffineConstantExpr
                loadAffineExpr.push_back(builder.getAffineDimExpr(loadRank++));
                memAffineIndices.push_back(
                    reductionForOps[RLCnt++].getInductionVar());
              }
            } else {
              loadAffineExpr.push_back(builder.getAffineDimExpr(loadRank++));
              memAffineIndices.push_back(operands[operandIdx++]);
            }
          } else if (i == axis) {
            loadAffineExpr.push_back(builder.getAffineDimExpr(loadRank++));
            memAffineIndices.push_back(operands[operandIdx++]);
            if (expr.isa<AffineBinaryOpExpr>())
              operandIdx++;
          } else {
            if (expr.isa<AffineBinaryOpExpr>()) {
              loadAffineExpr.push_back(builder.getAffineDimExpr(loadRank++));
              operandIdx++;
              memAffineIndices.push_back(
                  shiftForOps[SLCnt++].getInductionVar());
              operandIdx++;
            } else if (expr.isa<AffineDimExpr>()) {
              loadAffineExpr.push_back(builder.getAffineDimExpr(loadRank++));
              memAffineIndices.push_back(
                  shiftForOps[SLCnt++].getInductionVar());
              operandIdx++;
            } else { // AffineConstantExpr
              loadAffineExpr.push_back(expr);
            }
          }
        }
        auto affineMap =
            AffineMap::get(loadRank, 0, loadAffineExpr, builder.getContext());
        load = builder.create<AffineLoadOp>(loc, target, affineMap,
                                            memAffineIndices);
      } else {
        SmallVector<Value> memAffineIndices;
        for (auto forOp : nonReductionLoops)
          memAffineIndices.push_back(forOp.getInductionVar());
        std::size_t size = memAffineIndices.size();
        for (unsigned int j = size - shiftForOps.size(); j < size; ++j) {
          memAffineIndices[j] =
              shiftForOps[j - size + shiftForOps.size()].getInductionVar();
        }
        load = builder.create<AffineLoadOp>(loc, target, memAffineIndices);
      }
    }

    // store the load result to buffer
    if (reductionForOps.size() > 0 && allLoadOperands[loadCnt].size() > 0)
      for (unsigned int j = 0; j < reductionForOps.size(); ++j) {
        allLoadOperands[loadCnt][j] = reductionForOps[j].getInductionVar();
      }
    std::size_t size = allLoadOperands[loadCnt].size();
    for (unsigned int j = size - shiftForOps.size(); j < size; ++j) {
      allLoadOperands[loadCnt][j] =
          shiftForOps[j - size + shiftForOps.size()].getInductionVar();
    }
    builder.create<AffineStoreOp>(loc, load, buf, allLoadAffineMaps[loadCnt],
                                  allLoadOperands[loadCnt]);
  }

  // 16) Remove all the useless operations
  for (Operation *op : opToRemove) {
    op->erase();
  }

  // 17) Merge loops with the same bound
  if (previousShiftLoops.size() > 0 && cntIf < 2) {
    // TODO: only support one shift loop now
    AffineForOp firstLoop = previousShiftLoops.back();
    AffineForOp secondLoop = nonReductionLoops[loopAxis];
    if (firstLoop.getConstantUpperBound() ==
        secondLoop.getConstantUpperBound()) {
      auto &firstBody = firstLoop.getBody()->getOperations();
      auto &secondBody = secondLoop.getBody()->getOperations();
      auto firstOpInSecondLoop = secondBody.begin();
      // do not need affine.yield op, so that's why using std::prev
      secondBody.splice(secondBody.begin(), firstBody, firstBody.begin(),
                        std::prev(firstBody.end()));
      firstLoop.getInductionVar().replaceAllUsesWith(
          secondLoop.getInductionVar());
      firstLoop.erase();
      auto parent = secondLoop->getParentOp();
      if (llvm::isa<AffineIfOp>(parent)) {
        auto ifOp = llvm::cast<AffineIfOp>(parent);
        auto &ifBody = ifOp.getThenBlock()->getOperations();
        auto &parentBody =
            nonReductionLoops[loopAxis - 1].getBody()->getOperations();
        parentBody.splice(parentBody.begin(), ifBody, ifBody.begin(),
                          std::prev(ifBody.end()));
        // skip the previous reuse part
        ifOp->moveBefore(&(*firstOpInSecondLoop));
        // move the rest into the if body
        auto &secondBody = secondLoop.getBody()->getOperations();
        ifBody.splice(ifBody.begin(), secondBody, firstOpInSecondLoop,
                      std::prev(secondBody.end()));
      }
    }
  }

  return success();
}

LogicalResult runBufferAt(FuncOp &f, BufferAtOp &bufferAtOp) {
  // 1) Get the schedule
  auto target = bufferAtOp.target(); // return a Value type
  const auto loop_name =
      dyn_cast<CreateLoopHandleOp>(bufferAtOp.axis().getDefiningOp())
          .loop_name();
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(bufferAtOp.stage().getDefiningOp())
          .stage_name();

  // 2) Find the requested stage
  AffineForOp rootForOp;
  if (failed(getStage(f, rootForOp, stage_name))) {
    f.emitError("Cannot find Stage ") << stage_name.str();
    return failure();
  }

  // 2.1) Find the requested loop and get the axis id
  AffineForOp bufferLoop = rootForOp;
  int axis = getLoop(bufferLoop, loop_name);
  if (axis == -1) {
    f.emitError("Cannot find Loop ") << loop_name.str();
    return failure();
  }

  // 3) Obtain non-reduction loops and reduction loops
  AffineLoopBand band;
  SmallVector<StringRef, 6> nameArr;
  // TODO: test if the requested loop has the target tensor
  bool isFound = findContiguousNestedLoops(rootForOp, band, nameArr);
  if (!isFound) {
    bufferAtOp.emitError("Cannot find nested loops for buffer_at");
    return failure();
  }
  SmallVector<AffineForOp, 6> nonReductionForOps;
  SmallVector<StringRef, 6> nonReductionNameArr;
  int firstReductionIdx = -1;
  for (std::size_t i = 0, e = band.size(); i != e; ++i) {
    if (!band[i]->hasAttr("reduction")) {
      nonReductionForOps.push_back(band[i]);
      nonReductionNameArr.push_back(getLoopName(band[i]));
    } else {
      if (firstReductionIdx == -1)
        firstReductionIdx = i;
    }
  }
  if (firstReductionIdx == -1)
    firstReductionIdx = band.size() - 1;
  // handle exception
  if (axis >= 0 && ((std::size_t)(axis + 1) >= band.size())) {
    bufferAtOp.emitError("Cannot buffer at the inner-most loop: axis=")
        << std::to_string(axis)
        << " inner-most axis=" << std::to_string(band.size() - 1);
    return failure();
  }
  if (axis >= 0 && axis >= firstReductionIdx) {
    bufferAtOp.emitError("Cannot buffer inside the reduction loops: axis=")
        << std::to_string(axis)
        << ", first reduction axis=" << std::to_string(firstReductionIdx);
    return failure();
  }

  // 4) Create write buffer
  // e.g.:
  // without reordering: (0, 1, 2r)
  //   buf_at 0: 1;(1,2r);1 insert at all[axis+1] but take non-red[axis+1]
  //   var buf_at 1: c;2r;c inner-most non-red buf_at 2: x cannot buffer
  //   at the inner-most
  // with reordering: (0, 1r, 2)
  //   buf_at 0: 2;(1r,2);2 non-red[axis+1]
  //   buf_at 1: x cannot buffer inside reduction loop
  //   buf_at 2: x
  if (axis == firstReductionIdx - 1 &&
      (std::size_t)firstReductionIdx ==
          nonReductionForOps.size()) { // inner-most non-reduction loop &&
                                       // no non-reduction loops inside
    OpBuilder builder(band[firstReductionIdx]);
    Location loc_front = band[firstReductionIdx].getLoc();
    mlir::Type elementType =
        target.getType().dyn_cast<MemRefType>().getElementType();
    SmallVector<Value, 4> memIndices;
    // a) Initialization
    // buffer only has one element
    auto buf = builder.create<memref::AllocOp>(
        loc_front, MemRefType::get({1}, elementType));
    auto zero = builder.create<arith::ConstantOp>(
        loc_front, elementType, createZeroAttr(builder, elementType));
    // no need to create an explicit loop
    auto idx = builder.create<arith::ConstantIndexOp>(loc_front, 0);
    memIndices.push_back(idx);
    builder.create<AffineStoreOp>(loc_front, zero, buf, memIndices);

    // link the result SSA with the buffer
    bufferAtOp.getResult().replaceAllUsesWith(buf);

    // b) Rewrite the original buffer
    // TODO: possible bug: replace uses before an untraversed op
    SmallVector<Operation *, 10> opToRemove;
    for (Operation &op : band[firstReductionIdx].getBody()->getOperations()) {
      memIndices.clear();
      if (auto load = dyn_cast<AffineLoadOp>(op)) {
        if (load.getOperand(0) != target)
          continue;
        OpBuilder mid_builder(&op);
        memIndices.push_back(idx);
        auto new_load =
            mid_builder.create<AffineLoadOp>(op.getLoc(), buf, memIndices);
        op.replaceAllUsesWith(new_load);
        opToRemove.push_back(&op);
      } else if (auto store = dyn_cast<AffineStoreOp>(op)) {
        if (store.getOperand(1) != target)
          continue;
        OpBuilder mid_builder(&op);
        memIndices.push_back(idx);
        mid_builder.create<AffineStoreOp>(op.getLoc(), op.getOperand(0), buf,
                                          memIndices);
        opToRemove.push_back(&op);
      }
    }
    for (Operation *op : opToRemove) {
      op->erase();
    }

    // c) Write back
    //    no need to create an explicit loop
    memIndices.clear();
    memIndices.push_back(idx);
    auto load_from_buf =
        builder.create<AffineLoadOp>(loc_front, buf, memIndices);
    memIndices.clear();
    for (int i = 0; i < firstReductionIdx; ++i) {
      memIndices.push_back(band[i].getInductionVar());
    }
    builder.create<AffineStoreOp>(loc_front, load_from_buf, target, memIndices);

    // d) move the original loop in the middle
    band[firstReductionIdx]->moveBefore(load_from_buf);

  } else { // not the inner-most non-reduction axis
    OpBuilder builder(band[axis + 1]);
    Location loc_front = band[axis + 1].getLoc();
    SmallVector<int64_t> ubs;
    for (unsigned int i = axis + 1, e = nonReductionForOps.size(); i < e; ++i) {
      ubs.push_back(nonReductionForOps[axis + 1].getConstantUpperBound());
    }
    // TODO: support more data types
    mlir::Type elementType =
        target.getType().dyn_cast<MemRefType>().getElementType();
    SmallVector<Value, 4> memIndices;
    // a) Initialization
    // a.1) Allocate buffer
    auto buf = builder.create<memref::AllocOp>(
        loc_front, MemRefType::get(ubs, elementType));
    auto zero = builder.create<arith::ConstantOp>(
        loc_front, elementType, createZeroAttr(builder, elementType));

    // a.2) Create initialization loop
    //      need to create an explicit loop
    SmallVector<AffineForOp> initLoops;
    initLoops.push_back(builder.create<AffineForOp>(loc_front, 0, ubs[0]));
    AffineForOp forOp = initLoops[0];
    for (unsigned int i = axis + 2, e = nonReductionForOps.size(); i < e; ++i) {
      OpBuilder init_builder(&(*(forOp.getBody()->getOperations().begin())));
      forOp = init_builder.create<AffineForOp>(
          forOp.getBody()->getOperations().begin()->getLoc(), 0,
          ubs[i - axis - 1]);
      initLoops.push_back(forOp);
    }

    // a.3) Do the initialization
    OpBuilder init_builder(&(
        *(initLoops[initLoops.size() - 1].getBody()->getOperations().begin())));
    for (auto forOp : initLoops) {
      memIndices.push_back(forOp.getInductionVar());
    }
    init_builder.create<AffineStoreOp>(initLoops[initLoops.size() - 1].getLoc(),
                                       zero, buf, memIndices);

    // b) Rewrite the original buffer
    SmallVector<Operation *, 10> opToRemove;
    band[axis + 1].walk([&](Operation *op) {
      memIndices.clear();
      if (auto load = dyn_cast<AffineLoadOp>(op)) {
        if (load.getOperand(0) != target)
          return;
        OpBuilder mid_builder(op);
        for (unsigned int i = axis + 1, e = nonReductionForOps.size(); i < e;
             ++i) {
          memIndices.push_back(nonReductionForOps[i].getInductionVar());
        }
        auto new_load =
            mid_builder.create<AffineLoadOp>(op->getLoc(), buf, memIndices);
        op->replaceAllUsesWith(new_load);
        opToRemove.push_back(op);
      } else if (auto store = dyn_cast<AffineStoreOp>(op)) {
        if (store.getOperand(1) != target)
          return;
        OpBuilder mid_builder(op);
        for (unsigned int i = axis + 1, e = nonReductionForOps.size(); i < e;
             ++i) {
          memIndices.push_back(nonReductionForOps[i].getInductionVar());
        }
        mid_builder.create<AffineStoreOp>(op->getLoc(), op->getOperand(0), buf,
                                          memIndices);
        opToRemove.push_back(op);
      }
    });
    for (Operation *op : opToRemove) {
      op->erase();
    }

    // c) Write back
    // c.1) Create write back loop
    Location loc_back =
        std::prev(band[axis + 1].getBody()->getOperations().end())->getLoc();
    SmallVector<AffineForOp> writeBackLoops;
    writeBackLoops.push_back(builder.create<AffineForOp>(loc_back, 0, ubs[0]));
    forOp = writeBackLoops[0];
    for (unsigned int i = axis + 2, e = nonReductionForOps.size(); i < e; ++i) {
      OpBuilder back_builder(&(*(forOp.getBody()->getOperations().begin())));
      forOp = back_builder.create<AffineForOp>(
          forOp.getBody()->getOperations().begin()->getLoc(), 0,
          ubs[i - axis - 1]);
      writeBackLoops.push_back(forOp);
    }

    // c.2) Load from intermediate results
    OpBuilder back_builder(&(*(writeBackLoops[writeBackLoops.size() - 1]
                                   .getBody()
                                   ->getOperations()
                                   .begin())));
    memIndices.clear();
    for (auto forOp : writeBackLoops) {
      memIndices.push_back(forOp.getInductionVar());
    }
    auto load_from_buf = back_builder.create<AffineLoadOp>(
        writeBackLoops[writeBackLoops.size() - 1].getLoc(), buf, memIndices);

    // c.3) Store the results back to memory
    memIndices.clear();
    for (int i = 0; i < axis + 1; ++i) {
      memIndices.push_back(nonReductionForOps[i].getInductionVar());
    }
    for (auto forOp : writeBackLoops) {
      memIndices.push_back(forOp.getInductionVar());
    }
    back_builder.create<AffineStoreOp>(
        writeBackLoops[writeBackLoops.size() - 1].getLoc(), load_from_buf,
        target, memIndices);

    // d) Move the original loop between the two loops
    band[axis + 1]->moveBefore(writeBackLoops[0]);

    // e) Add names to loops
    SmallVector<std::string, 6> newNameArr;
    newNameArr.push_back(nonReductionNameArr[axis + 1].str() + "_init");
    newNameArr.push_back(nonReductionNameArr[axis + 1].str() + "_back");
    SmallVector<AffineForOp, 6> newLoops{initLoops[0], writeBackLoops[0]};
    setLoopNames(newLoops, newNameArr);
    initLoops[0]->setAttr("buffer", init_builder.getUnitAttr());
    writeBackLoops[0]->setAttr("buffer", back_builder.getUnitAttr());

    // f) Automatic pipelining
    SmallVector<AffineForOp, 6> twoLoops{
        initLoops[initLoops.size() - 1],
        writeBackLoops[writeBackLoops.size() - 1]};
    SmallVector<int, 6> II{1, 1};
    setIntAttr(twoLoops, II, "pipeline_ii");
  }

  return success();
}

LogicalResult runReshape(FuncOp &f, ReshapeOp &reshapeOp, Value &array) {
  // 1) Get the schedule
  auto oldType = array.getType().dyn_cast<MemRefType>();
  auto newType = reshapeOp.output().getType().dyn_cast<MemRefType>();
  int oldRank = oldType.getRank();
  int newRank = newType.getRank();
  auto oldShape = oldType.getShape();
  auto newShape = newType.getShape();
  SmallVector<int64_t> prodOldShape;
  prodOldShape.push_back(1);
  for (int i = oldRank - 1; i >= 0; --i)
    prodOldShape.push_back(oldShape[i] * prodOldShape[oldRank - 1 - i]);

  // 2) Set new type
  array.setType(newType);

  // 3) Update memory access
  SmallVector<Operation *> opToRemove;
  for (auto user : array.getUsers()) {
    if (auto op = dyn_cast<AffineStoreOp>(user)) {
      OpBuilder rewriter(op);
      SmallVector<AffineExpr> memAffineIndices;
      memAffineIndices.clear();
      auto oldAffineMap = op.getAffineMap();
      auto linear_addr = rewriter.getAffineConstantExpr(0);
      for (int i = oldRank - 1; i >= 0; --i) {
        AffineExpr idx = oldAffineMap.getResult(i);
        linear_addr = idx * prodOldShape[oldRank - i - 1] + linear_addr;
      }
      for (int i = 1; i < newRank; ++i) {
        memAffineIndices.push_back(linear_addr % newShape[newRank - i]);
        linear_addr = linear_addr.floorDiv(newShape[newRank - i]);
      }
      memAffineIndices.push_back(linear_addr);
      std::reverse(memAffineIndices.begin(), memAffineIndices.end());
      auto affineMap = AffineMap::get(oldRank, 0 /* symbols */,
                                      memAffineIndices, rewriter.getContext());
      rewriter.create<AffineStoreOp>(
          op->getLoc(), op.getOperand(0) /*valueToStore*/,
          op.getOperand(1) /*memref*/, affineMap, op.indices());
      // remove original op
      opToRemove.push_back(op);
    } else if (auto op = dyn_cast<AffineLoadOp>(user)) {
      OpBuilder rewriter(op);
      SmallVector<AffineExpr> memAffineIndices;
      memAffineIndices.clear();
      auto oldAffineMap = op.getAffineMap();
      auto linear_addr = rewriter.getAffineConstantExpr(0);
      for (int i = oldRank - 1; i >= 0; --i) {
        AffineExpr idx = oldAffineMap.getResult(i);
        linear_addr = idx * prodOldShape[oldRank - i - 1] + linear_addr;
      }
      for (int i = 1; i < newRank; ++i) {
        memAffineIndices.push_back(linear_addr % newShape[newRank - i]);
        linear_addr = linear_addr.floorDiv(newShape[newRank - i]);
      }
      memAffineIndices.push_back(linear_addr);
      std::reverse(memAffineIndices.begin(), memAffineIndices.end());
      auto affineMap = AffineMap::get(oldRank, 0 /* symbols */,
                                      memAffineIndices, rewriter.getContext());
      auto load = rewriter.create<AffineLoadOp>(
          op->getLoc(), op.getOperand(0) /*memref*/, affineMap, op.indices());
      // remove original op
      op.getResult().replaceAllUsesWith(load);
      opToRemove.push_back(op);
    }
  }

  // 4) update function signature
  auto builder = Builder(array.getContext());
  auto resultTypes = f.front().getTerminator()->getOperandTypes();
  auto inputTypes = f.front().getArgumentTypes();
  f.setType(builder.getFunctionType(inputTypes, resultTypes));

  // 5) Remove all the useless operations
  for (Operation *op : opToRemove) {
    op->erase();
  }
  return success();
}

LogicalResult
runInterKernelDataPlacement(std::map<std::string, FuncOp> &funcMap,
                            Value &arrayToStream, int fifo_depth = -1) {
  // Construct new array type (add stream attribute)
  auto arrayType = arrayToStream.getType().dyn_cast<MemRefType>();
  auto shape = arrayType.getShape();
  if (fifo_depth == -1) {
    // a conversative estimation
    fifo_depth = 1;
    for (auto size : shape)
      fifo_depth *= size;
  }
  auto newType = MemRefType::get(
      arrayType.getShape(), arrayType.getElementType(), arrayType.getLayout(),
      StringAttr::get(arrayToStream.getDefiningOp()->getContext(),
                      "stream:" + std::to_string(fifo_depth)));

  // Set new type in the top function
  arrayToStream.setType(newType);

  // Set new types in stage functions
  for (auto user : arrayToStream.getUsers()) {
    // first locate the CallOp
    if (auto callOp = dyn_cast<CallOp>(user)) {
      // get stage function
      auto stage = funcMap[callOp.getCallee().str().substr(6)];
      for (unsigned argIdx = 0, e = user->getNumOperands(); argIdx < e;
           ++argIdx) {
        // find the corresponding array
        if (callOp.getArgOperands()[argIdx] == arrayToStream) {
          // first change argument type
          stage.getArgument(argIdx).setType(newType);
          // get new function input types
          llvm::SmallVector<mlir::Type> inputTypes;
          for (auto indexedArg :
               llvm::enumerate(stage.front().getArgumentTypes())) {
            if (indexedArg.index() != argIdx) {
              inputTypes.push_back(indexedArg.value());
            } else {
              inputTypes.push_back(newType);
            }
          }
          auto resultTypes = stage.front().getTerminator()->getOperandTypes();
          // update function signature
          stage.setType(
              FunctionType::get(stage.getContext(), inputTypes, resultTypes));
          break;
        }
      }
    }
  }
  return success();
}

LogicalResult runInterKernelDataPlacementSingleFunction(Value &arrayToStream,
                                                        int fifo_depth = -1) {
  // Construct new array type (add stream attribute)
  auto arrayType = arrayToStream.getType().dyn_cast<MemRefType>();
  auto shape = arrayType.getShape();
  if (fifo_depth == -1) {
    // a conversative estimation
    fifo_depth = 1;
    for (auto size : shape)
      fifo_depth *= size;
  }
  auto newType = MemRefType::get(
      arrayType.getShape(), arrayType.getElementType(), arrayType.getLayout(),
      StringAttr::get(arrayToStream.getDefiningOp()->getContext(),
                      "stream:" + std::to_string(fifo_depth)));

  // Set new type
  arrayToStream.setType(newType);
  return success();
}

template <class T, int opId>
void getInputMemRefs(AffineForOp stage, SmallVector<Value> &allMemrefs) {
  stage.walk([&](T op) {
    auto target = op.getOperand(opId);
    if (std::find(allMemrefs.begin(), allMemrefs.end(), target) ==
        allMemrefs.end())
      allMemrefs.push_back(target);
  });
}

template <class T, int opId>
void getOutputMemRefs(AffineForOp stage, SmallVector<Value> &allMemrefs,
                      std::set<memref::AllocOp> &allocToMove) {
  SmallVector<Value> memrefToRemove;
  const auto stage_name =
      stage->getAttr("stage_name").cast<StringAttr>().getValue().str();
  stage.walk([&](T op) {
    auto target = op.getOperand(opId);
    if (std::find(allMemrefs.begin(), allMemrefs.end(), target) ==
        allMemrefs.end()) { // need to prevent adding the same memref again
      allMemrefs.push_back(target);
    } else {
      if (allMemrefs.size() == 1)
        return WalkResult::advance();
      if (target.getDefiningOp()) {
        memrefToRemove.push_back(target);
        allocToMove.insert(dyn_cast<memref::AllocOp>(target.getDefiningOp()));
      }
    }
    return WalkResult::advance();
  });
  for (auto target : memrefToRemove) {
    allMemrefs.erase(std::remove(allMemrefs.begin(), allMemrefs.end(), target),
                     allMemrefs.end());
  }
}

LogicalResult runOutline(ModuleOp &mod, FuncOp &f, OutlineOp &outlineOp) {
  // 1) Get the schedule
  auto stages = outlineOp.stages();
  SmallVector<AffineForOp> rootForOps;
  SmallVector<Value> allMemrefs;
  std::vector<std::string> stageNames;
  for (auto stage : stages) {
    const auto stage_name =
        dyn_cast<CreateStageHandleOp>(stage.getDefiningOp()).stage_name();
    stageNames.push_back(stage_name.str());

    // 2) Find the requested stages
    AffineForOp rootForOp;
    if (failed(getStage(f, rootForOp, stage_name))) {
      f.emitError("Cannot find Stage ") << stage_name.str();
      return failure();
    }
    rootForOps.push_back(rootForOp);

    // 3) Find all load memrefs (inputs)
    getInputMemRefs<AffineLoadOp, 0>(rootForOp, allMemrefs);
    getInputMemRefs<memref::LoadOp, 0>(rootForOp, allMemrefs);
  }

  // 4) Find all store memrefs (outputs)
  std::set<memref::AllocOp> allocToMove;
  for (auto rootForOp : rootForOps) {
    getOutputMemRefs<AffineStoreOp, 1>(rootForOp, allMemrefs, allocToMove);
    getOutputMemRefs<memref::StoreOp, 1>(rootForOp, allMemrefs, allocToMove);
  }
  SmallVector<Value> newMemrefs(allMemrefs);

  // 5) Create a new function
  auto builder = OpBuilder::atBlockBegin(mod.getBody());
  TypeRange argTypes = ValueRange(newMemrefs).getTypes();
  FunctionType funcType = builder.getFunctionType(argTypes, llvm::None);
  std::string func_name = "Stage";
  for (auto stage_name : stageNames) {
    func_name += "_" + stage_name;
  }
  auto func =
      builder.create<FuncOp>(mod.getLoc(), StringRef(func_name), funcType);
  func.setPrivate();
  Block *entryBlock = func.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  auto ret = builder.create<ReturnOp>(func->getLoc());

  // 6) Create callop in the main function
  OpBuilder call_builder(rootForOps[rootForOps.size() - 1]);
  call_builder.create<CallOp>(rootForOps[rootForOps.size() - 1].getLoc(), func,
                              allMemrefs);

  // 7) Move original stage to the new function
  for (auto rootForOp : rootForOps) {
    rootForOp->moveBefore(ret);
  }
  for (auto alloc : allocToMove) {
    alloc->moveBefore(rootForOps[0]);
  }

  // 8) Update memrefs
  for (auto item : llvm::enumerate(newMemrefs)) {
    auto newMemref = func.getArgument(item.index());
    auto oldMemref = item.value();
    for (auto rootForOp : rootForOps)
      replaceAllUsesInRegionWith(oldMemref, newMemref, rootForOp.region());
  }

  return success();
}

template <class T>
void updateMemrefAccess(Operation *&user, SmallVector<AffineExpr> &dimExprs) {
  if (auto op = dyn_cast<T>(user)) {
    auto oldAffineMap = op.getAffineMap();
    SmallVector<AffineExpr> memAffineIndices;
    for (auto dim : dimExprs) {
      auto pos = dim.cast<AffineDimExpr>().getPosition();
      memAffineIndices.push_back(oldAffineMap.getResult(pos));
    }
    auto newAffineMap =
        AffineMap::get(oldAffineMap.getNumDims(), 0 /* symbols */,
                       memAffineIndices, op->getContext());
    op->setAttr("map", AffineMapAttr::get(newAffineMap));
  }
}

LogicalResult runLayout(FuncOp &f, LayoutOp &layoutOp, Value &array) {
  // 1) Get the schedule
  auto oldType = array.getType().dyn_cast<MemRefType>();
  auto oldShape = oldType.getShape();
  auto layoutMap =
      layoutOp->getAttr("layout").template cast<AffineMapAttr>().getValue();

  // 2) Get new shape
  SmallVector<int64_t> newShape;
  SmallVector<AffineExpr> dimExprs;
  for (auto dim : layoutMap.getResults()) {
    newShape.push_back(oldShape[dim.cast<AffineDimExpr>().getPosition()]);
    dimExprs.push_back(dim);
  }

  // 3) Set new type
  mlir::Type elementType = oldType.getElementType();
  auto newType = MemRefType::get(newShape, elementType);
  array.setType(newType);

  // 4) Update memory access
  for (auto user : array.getUsers()) {
    updateMemrefAccess<AffineLoadOp>(user, dimExprs);
    updateMemrefAccess<AffineStoreOp>(user, dimExprs);
  }

  // 5) update function signature
  auto builder = Builder(array.getContext());
  auto resultTypes = f.front().getTerminator()->getOperandTypes();
  auto inputTypes = f.front().getArgumentTypes();
  f.setType(builder.getFunctionType(inputTypes, resultTypes));

  return success();
}

bool isHCLOp(Operation &op) {
  return llvm::isa<SplitOp, TileOp, ReorderOp, UnrollOp, PipelineOp, ParallelOp,
                   FuseOp, ComputeAtOp, PartitionOp, ReuseAtOp, BufferAtOp,
                   OutlineOp, ReshapeOp, LayoutOp, ThreadBindOp,
                   InterKernelToOp>(op);
}

template <class HCLOp>
bool runSchedule(
    std::map<std::string, FuncOp> &funcMap, HCLOp &op,
    std::function<LogicalResult(FuncOp &, HCLOp &)> schedule_func) {
  const auto stage_name =
      dyn_cast<CreateStageHandleOp>(op.stage().getDefiningOp())
          .stage_name()
          .str();
  if (funcMap.count(stage_name) > 0) {
    if (!failed(schedule_func(funcMap[stage_name], op)))
      return true;
  }
  return false;
}

void eraseScheduleOp(FuncOp &f, SmallVector<Operation *, 10> &opToRemove) {
  std::reverse(opToRemove.begin(), opToRemove.end());
  for (Operation &op : f.getOps()) {
    if (llvm::isa<hcl::CreateLoopHandleOp, hcl::CreateStageHandleOp>(op))
      opToRemove.push_back(&op);
  }
  for (Operation *op : opToRemove) {
    op->erase();
  }
}

bool applyLoopTransformationOnSingleFunction(ModuleOp &mod, FuncOp &f) {
  SmallVector<Operation *, 10> opToRemove;
  // schedule should preverse orders, thus traverse one by one
  // the following shows the dispatching logic
  for (Operation &op : f.getOps()) {
    if (isHCLOp(op)) {
      if (auto new_op = dyn_cast<SplitOp>(op)) {
        if (failed(runSplitting(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<TileOp>(op)) {
        if (failed(runTiling(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<ReorderOp>(op)) {
        if (failed(runReordering(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<UnrollOp>(op)) {
        if (failed(runUnrolling(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<PipelineOp>(op)) {
        if (failed(runPipelining(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<ThreadBindOp>(op)) {
        if (failed(runThreadBind(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<ParallelOp>(op)) {
        if (failed(runParallel(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<FuseOp>(op)) {
        if (failed(runFusing(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<ComputeAtOp>(op)) {
        if (failed(runComputeAt(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<PartitionOp>(op)) {
        Value array;
        if (findArray(f, new_op.target(), array)) {
          if (failed(runPartition(f, new_op, array)))
            return false;
        } else {
          return false;
        }
      } else if (auto new_op = dyn_cast<ReuseAtOp>(op)) {
        if (failed(runReuseAt(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<BufferAtOp>(op)) {
        if (failed(runBufferAt(f, new_op)))
          return false;
      } else if (auto new_op = dyn_cast<ReshapeOp>(op)) {
        Value array;
        if (findArray(f, new_op.target(), array)) {
          if (failed(runReshape(f, new_op, array)))
            return false;
        } else {
          return false;
        }
      } else if (auto new_op = dyn_cast<LayoutOp>(op)) {
        Value array;
        if (findArray(f, new_op.target(), array)) {
          if (failed(runLayout(f, new_op, array)))
            return false;
        } else {
          return false;
        }
      } else if (auto new_op = dyn_cast<InterKernelToOp>(op)) {
        Value array;
        auto optional_fifo_depth = new_op.fifo_depth();
        unsigned int fifo_depth;
        if (optional_fifo_depth.hasValue()) {
          fifo_depth = optional_fifo_depth.getValue();
        } else {
          fifo_depth = -1; // conservative assumption
        }
        if (findArray(f, new_op.target(), array)) {
          if (failed(
                  runInterKernelDataPlacementSingleFunction(array, fifo_depth)))
            return false;
        } else {
          return false;
        }
      } else if (auto new_op = dyn_cast<OutlineOp>(op)) {
        if (failed(runOutline(mod, f, new_op)))
          return false;
      }
      opToRemove.push_back(&op);
    }
  }
  // remove schedule operations (from back to front) & legacy loop handles
  eraseScheduleOp(f, opToRemove);
  return true;
}

bool applyLoopTransformation(ModuleOp &mod) {
  bool isFoundTopFunc = false;
  std::map<std::string, FuncOp> funcMap;
  // create name->function mapping
  for (FuncOp func : mod.getOps<FuncOp>()) {
    if (func->hasAttr("top")) {
      isFoundTopFunc = true;
      funcMap["top"] = func;
      break;
    }
  }

  // apply schedule
  if (!isFoundTopFunc || !funcMap["top"]->hasAttr("top")) { // fallback
    for (FuncOp f : mod.getOps<FuncOp>()) {
      applyLoopTransformationOnSingleFunction(mod, f);
    }
  } else {
    for (FuncOp func : mod.getOps<FuncOp>()) {
      if (!func->hasAttr("top"))
        funcMap[func.getName().str().substr(6)] = func; // Stage_xxx
    }
    FuncOp top_func = funcMap["top"];
    SmallVector<Operation *, 10> opToRemove;
    for (Operation &op : top_func.getOps()) {
      if (isHCLOp(op)) {
        if (auto new_op = dyn_cast<SplitOp>(op)) {
          runSchedule<SplitOp>(funcMap, new_op, &runSplitting);
        } else if (auto new_op = dyn_cast<TileOp>(op)) {
          runSchedule<TileOp>(funcMap, new_op, &runTiling);
        } else if (auto new_op = dyn_cast<ReorderOp>(op)) {
          runSchedule<ReorderOp>(funcMap, new_op, &runReordering);
        } else if (auto new_op = dyn_cast<UnrollOp>(op)) {
          runSchedule<UnrollOp>(funcMap, new_op, &runUnrolling);
        } else if (auto new_op = dyn_cast<PipelineOp>(op)) {
          runSchedule<PipelineOp>(funcMap, new_op, &runPipelining);
        } else if (auto new_op = dyn_cast<ThreadBindOp>(op)) {
          runSchedule<ThreadBindOp>(funcMap, new_op, &runThreadBind);
        } else if (auto new_op = dyn_cast<ParallelOp>(op)) {
          runSchedule<ParallelOp>(funcMap, new_op, &runParallel);
        } else if (auto new_op = dyn_cast<FuseOp>(op)) {
          runSchedule<FuseOp>(funcMap, new_op, &runFusing);
        } else if (auto new_op = dyn_cast<ComputeAtOp>(op)) {
          // runSchedule<ComputeAtOp>(funcMap, new_op, &runComputeAt);
        } else if (auto new_op = dyn_cast<PartitionOp>(op)) {
          Value array;
          bool isDone = false;
          for (FuncOp f : mod.getOps<FuncOp>()) {
            if (findArray(f, new_op.target(), array)) {
              if (failed(runPartition(f, new_op, array))) {
                return false;
              } else {
                isDone = true;
                break;
              }
            }
          }
          if (!isDone)
            return false;
        } else if (auto new_op = dyn_cast<ReuseAtOp>(op)) {
          runSchedule<ReuseAtOp>(funcMap, new_op, &runReuseAt);
        } else if (auto new_op = dyn_cast<BufferAtOp>(op)) {
          runSchedule<BufferAtOp>(funcMap, new_op, &runBufferAt);
        } else if (auto new_op = dyn_cast<ReshapeOp>(op)) {
          Value array;
          bool isDone = false;
          for (FuncOp f : mod.getOps<FuncOp>()) {
            if (findArray(f, new_op.target(), array)) {
              if (failed(runReshape(f, new_op, array))) {
                return false;
              } else {
                isDone = true;
                break;
              }
            }
          }
          if (!isDone)
            return false;
        } else if (auto new_op = dyn_cast<InterKernelToOp>(op)) {
          Value array;
          auto optional_fifo_depth = new_op.fifo_depth();
          unsigned int fifo_depth;
          if (optional_fifo_depth.hasValue()) {
            fifo_depth = optional_fifo_depth.getValue();
          } else {
            fifo_depth = -1; // conservative assumption
          }
          if (!findArray(top_func, new_op.target(), array) ||
              failed(runInterKernelDataPlacement(funcMap, array, fifo_depth)))
            return false;
        }
        opToRemove.push_back(&op);
      }
    }
    eraseScheduleOp(top_func, opToRemove);
    // move forward stage functions to avoid backward definition
    for (auto item : funcMap) {
      if (item.first != "top") {
        item.second->moveBefore(top_func);
      }
    }
  }
  return true;
}

} // namespace hcl
} // namespace mlir

namespace {

struct HCLLoopTransformation
    : public LoopTransformationBase<HCLLoopTransformation> {

  void runOnOperation() override {
    auto mod = getOperation();
    if (!applyLoopTransformation(mod))
      return signalPassFailure();
  }
};

} // namespace

namespace mlir {
namespace hcl {

// Create A Loop Transformation Pass
std::unique_ptr<OperationPass<ModuleOp>> createLoopTransformationPass() {
  return std::make_unique<HCLLoopTransformation>();
}

} // namespace hcl
} // namespace mlir
