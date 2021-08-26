// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/HAL/Conversion/ConversionDialectInterface.h"
#include "iree/compiler/Dialect/HAL/Conversion/ConversionTarget.h"
#include "iree/compiler/Dialect/HAL/Conversion/TypeConverter.h"
#include "iree/compiler/Dialect/HAL/Conversion2/StandardToHAL/ConvertStandardToHAL.h"
#include "iree/compiler/Dialect/HAL/Conversion2/StreamToHAL/ConvertStreamToHAL.h"
#include "iree/compiler/Dialect/HAL/Conversion2/UtilToHAL/ConvertUtilToHAL.h"
#include "iree/compiler/Dialect/HAL/IR/HALDialect.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "iree/compiler/Dialect/HAL/IR/HALTypes.h"
#include "iree/compiler/Dialect/Shape/IR/ShapeOps.h"
#include "iree/compiler/Dialect/Stream/IR/StreamDialect.h"
#include "iree/compiler/Dialect/Stream/IR/StreamOps.h"
#include "iree/compiler/Dialect/Util/Conversion/ConversionPatterns.h"
#include "iree/compiler/Dialect/Util/IR/UtilDialect.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "iree/compiler/Dialect/Util/IR/UtilTypes.h"
#include "iree/compiler/Dialect/Util/Transforms/Passes.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace HAL {
namespace {

// A pass converting the IREE flow dialect into the IREE HAL dialect.
class ConvertToHALPass
    : public PassWrapper<ConvertToHALPass, OperationPass<ModuleOp>> {
 public:
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<mlir::StandardOpsDialect>();
    registry.insert<IREE::HAL::HALDialect>();
    registry.insert<IREE::Stream::StreamDialect>();
    registry.insert<IREE::Util::UtilDialect>();
  }

  StringRef getArgument() const override { return "iree-convert-to-hal2"; }

  StringRef getDescription() const override {
    return "Convert input stream/std/etc dialects to the IREE HAL dialect.";
  }

  void runOnOperation() override {
    auto *context = &getContext();

    // Gather all interfaces from registered dialects.
    // These will perform the tensor->buffer mapping for their ops.
    SmallVector<const HALConversionDialectInterface *, 4> conversionInterfaces;
    for (auto *dialect : context->getLoadedDialects()) {
      if (auto *conversionInterface =
              dialect
                  ->getRegisteredInterface<HALConversionDialectInterface>()) {
        conversionInterfaces.emplace_back(conversionInterface);
      }
    }

    HALTypeConverter typeConverter(conversionInterfaces, true);
    HALConversionTarget conversionTarget(context, typeConverter);

    OwningRewritePatternList patterns(&getContext());

    populateUtilToHALPatterns(context, conversionTarget, typeConverter,
                              patterns);
    populateUtilConversionPatterns(context, conversionTarget, typeConverter,
                                   patterns);
    populateStandardToHALPatterns(context, conversionTarget, typeConverter,
                                  patterns);
    populateStreamToHALPatterns(context, conversionTarget, typeConverter,
                                patterns);

    // Gather all HAL dialect conversion patterns from custom dialects.
    // These will perform the tensor->buffer mapping for their ops.
    for (auto *conversionInterface : conversionInterfaces) {
      conversionInterface->setupConversionTarget(conversionTarget, patterns,
                                                 typeConverter);
    }

    // NOTE: we allow ops that we don't know about to allow custom dialects
    // that don't need anything HAL-specific to pass through. This is handled by
    // the fallback type legality support of the
    if (failed(applyPartialConversion(getOperation(), conversionTarget,
                                      std::move(patterns)))) {
      return signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>> createConvertToHAL2Pass() {
  return std::make_unique<ConvertToHALPass>();
}

static PassRegistration<ConvertToHALPass> pass;

}  // namespace HAL
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
