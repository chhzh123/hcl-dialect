/*
 * Copyright HeteroCL authors. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

//===----------------------------------------------------------------------===//
// MemRefDCE Pass
// This pass removes memrefs that are not loaded from.
// We only look at memrefs allocated in functions.
// Global memrefs and memrefs in function args are not removed.
//===----------------------------------------------------------------------===//
#include "PassDetail.h"
#include "hcl/Transforms/Passes.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"

using namespace mlir;
using namespace hcl;

namespace mlir {
namespace hcl {

void cleanUpUnusedOps(func::FuncOp &func) {
  func.walk([&](Operation *op) {
    if (op->getNumResults() != 0 && op->use_empty()) {
      op->erase();
    }
  });
}

void removeNeverLoadedMemRef(func::FuncOp &func) {
  SmallVector<Operation *, 8> memRefAllocOps;
  func.walk([&](Operation *op) {
    if (auto memRefAllocOp = dyn_cast<memref::AllocOp>(op)) {
      memRefAllocOps.push_back(memRefAllocOp);
    }
  });
  std::reverse(memRefAllocOps.begin(), memRefAllocOps.end());
  for (auto op : memRefAllocOps) {
    auto v = op->getResult(0);
    bool loaded_from = false;
    for (auto u : v.getUsers()) {
      if (isa<memref::LoadOp>(u)) {
        loaded_from = true;
        break;
      } else if (isa<AffineLoadOp>(u)) {
        loaded_from = true;
        break;
      } else if (isa<func::ReturnOp>(u)) {
        loaded_from = true;
        break;
      } else if (isa<func::CallOp>(u)) {
        loaded_from = true;
        break;
      }
    }
    if (!loaded_from) {
      // erase op and all known uses
      for (auto u : v.getUsers()) {
        u->erase();
      }
      op->erase();
    }
  }
}

/// Pass entry point
bool applyMemRefDCE(ModuleOp &mod) {
  for (auto func : mod.getOps<func::FuncOp>()) {
    removeNeverLoadedMemRef(func);
    cleanUpUnusedOps(func);
  }
  return true;
}
} // namespace hcl
} // namespace mlir

namespace {
struct HCLMemRefDCETransformation
    : public MemRefDCEBase<HCLMemRefDCETransformation> {
  void runOnOperation() override {
    auto mod = getOperation();
    if (!applyMemRefDCE(mod)) {
      return signalPassFailure();
    }
  }
};
} // namespace

namespace mlir {
namespace hcl {

std::unique_ptr<OperationPass<ModuleOp>> createMemRefDCEPass() {
  return std::make_unique<HCLMemRefDCETransformation>();
}
} // namespace hcl
} // namespace mlir