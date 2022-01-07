//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The HCL-MLIR Authors.
//
//===----------------------------------------------------------------------===//

#include "hcl-c/HCL.h"
#include "mlir/CAPI/Registration.h"
#include "hcl/Dialect/HeteroCLDialect.h"

using namespace mlir;
using namespace hcl;

MLIR_DEFINE_CAPI_DIALECT_REGISTRATION(HCL, hcl, hcl::HeteroCLDialect)