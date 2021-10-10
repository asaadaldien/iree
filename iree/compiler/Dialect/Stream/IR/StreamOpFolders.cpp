// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <numeric>

#include "iree/compiler/Dialect/Shape/IR/ShapeOps.h"
#include "iree/compiler/Dialect/Stream/IR/StreamDialect.h"
#include "iree/compiler/Dialect/Stream/IR/StreamOps.h"
#include "iree/compiler/Dialect/Util/IR/ClosureOpUtils.h"
#include "iree/compiler/Dialect/Util/IR/UtilTypes.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/StandardOps/Utils/Utils.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/LogicalResult.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Stream {

//===----------------------------------------------------------------------===//
// Utilities shared across patterns
//===----------------------------------------------------------------------===//

// Finds the insertion point before |targetOp| and after |earliestOp| that would
// not oscillate if an op was moved there. Probably.
static Block::iterator findInsertionPointBefore(Operation *earliestOp,
                                                Operation *targetOp) {
  // Check if ops between this and the target are all used by the target.
  // If they are, we skip sinking so that we don't get stuck in an infinite loop
  // if there are two splats used by the same op (or another pattern sinking).
  if (earliestOp->getBlock() == targetOp->getBlock()) {
    SmallPtrSet<Operation *, 4> producerOps;
    for (auto operand : targetOp->getOperands()) {
      if (operand.getDefiningOp()) {
        producerOps.insert(operand.getDefiningOp());
      }
    }
    bool allUsed = true;
    for (auto it = Block::iterator(earliestOp); it != Block::iterator(targetOp);
         ++it) {
      if (!producerOps.contains(&*it)) {
        allUsed = false;
        break;
      }
    }
    if (allUsed) return Block::iterator(earliestOp);
  }
  return Block::iterator(targetOp);
}

// Sinks |op| down to |targetOp|, ensuring that we don't oscillate.
// Returns success if the op was sunk and failure if sinking was not needed.
static LogicalResult sinkOp(Operation *op, Operation *targetOp) {
  auto ip = findInsertionPointBefore(op, targetOp);
  if (ip == Block::iterator(op)) return failure();
  op->moveBefore(targetOp);
  return success();
}

// Sets |rewriter| to point immediately before the parent execution region.
// Example:
//   %0 =
//   <-- insertion point set to here -->
//   stream.async.execute ... {
//     %1 = op
//   }
static void setInsertionPointToParentExecutionScope(Operation *op,
                                                    PatternRewriter &rewriter) {
  if (auto parentOp = op->getParentOfType<AsyncExecuteOp>()) {
    rewriter.setInsertionPoint(parentOp);
  } else if (auto parentOp = op->getParentOfType<CmdExecuteOp>()) {
    rewriter.setInsertionPoint(parentOp);
  } else {
    llvm_unreachable("must be nested within an execution region");
  }
}

namespace {

// Erases an op if it has no uses.
// This is to support ops that are "pure" but can't be marked as such because
// the MLIR CSE pass would deduplicate them.
template <typename Op>
struct ElideUnusedOp : public OpRewritePattern<Op> {
  using OpRewritePattern<Op>::OpRewritePattern;
  LogicalResult matchAndRewrite(Op op,
                                PatternRewriter &rewriter) const override {
    if (!op.use_empty()) return failure();
    rewriter.eraseOp(op);
    return success();
  }
};

// Materialize copy-on-write (🐄) ops where required.
// This models what a runtime normally does with copy-on-write but uses the
// information we have in the SSA use-def chain to identify ties that write and
// covering reads.
template <typename Op>
struct MaterializeCOW : public OpRewritePattern<Op> {
  using OpRewritePattern<Op>::OpRewritePattern;
  LogicalResult matchAndRewrite(Op op,
                                PatternRewriter &rewriter) const override {
    bool didChange = false;
    for (auto result : op->getResults()) {
      auto resultType =
          result.getType().template dyn_cast<IREE::Stream::ResourceType>();
      if (!resultType) continue;

      // If our result is a constant then we need to ensure that we aren't
      // tied to a constant operand. If we are we need to clone to a
      // non-constant value.
      bool forceClone =
          resultType.getLifetime() == IREE::Stream::Lifetime::Constant;

      // Identify if we need to insert a copy-on-write clone.
      // We do this per use as a single consuming op may use the result of this
      // multiple times - some tied and some not - and if it has it tied several
      // times each will need its own clone.
      struct TiedUse {
        Operation *user;
        unsigned operandIndex;
        Value value;
      };
      SmallVector<TiedUse> tiedUses;
      unsigned untiedUses = 0;
      for (auto &use : result.getUses()) {
        if (isa<IREE::Stream::TimepointAwaitOp>(use.getOwner())) continue;
        auto tiedOp = dyn_cast<IREE::Util::TiedOpInterface>(use.getOwner());
        bool isTied = tiedOp && tiedOp.isOperandTied(use.getOperandNumber());
        if (isTied) {
          tiedUses.push_back({use.getOwner(), use.getOperandNumber(), result});
        } else {
          ++untiedUses;
        }
      }
      if (tiedUses.empty()) {
        // All uses are as normal capturing SSA values.
        continue;
      } else if (tiedUses.size() == 1 && untiedUses == 0 && !forceClone) {
        // Only one use and it's tied - we've already reserved our results for
        // it.
        continue;
      }
      didChange = true;

      // Mixed/multiple tied uses. Clone for each tied use but leave the untied
      // ones referencing us.
      IREE::Stream::AffinityAttr sourceAffinity;
      if (auto affinityOp = dyn_cast<IREE::Stream::AffinityOpInterface>(
              static_cast<Operation *>(op))) {
        sourceAffinity = affinityOp.getAffinity();
      }
      for (auto &tiedUse : tiedUses) {
        auto cloneLoc = FusedLoc::get(op.getContext(),
                                      {op.getLoc(), tiedUse.user->getLoc()});

        rewriter.setInsertionPoint(tiedUse.user);

        auto sizeAwareType =
            tiedUse.value.getType()
                .template cast<IREE::Util::SizeAwareTypeInterface>();
        auto targetSize =
            sizeAwareType.queryValueSize(cloneLoc, tiedUse.value, rewriter);

        IREE::Stream::AffinityAttr targetAffinity;
        if (auto affinityOp =
                dyn_cast<IREE::Stream::AffinityOpInterface>(tiedUse.user)) {
          targetAffinity = affinityOp.getAffinity();
        }

        auto unknownType =
            IREE::Stream::ResourceType::get(rewriter.getContext());
        auto cloneOp = rewriter.create<IREE::Stream::AsyncCloneOp>(
            cloneLoc, unknownType, tiedUse.value, targetSize, targetSize,
            targetAffinity ? targetAffinity : sourceAffinity);
        tiedUse.user->setOperand(tiedUse.operandIndex, cloneOp.result());
      }
    }
    return didChange ? success() : failure();
  }
};

// Ties the results of execution region to their operands when the region
// operations are tied throughout the entire body.
template <typename Op>
struct TieRegionResults : public OpRewritePattern<Op> {
  using OpRewritePattern<Op>::OpRewritePattern;
  LogicalResult matchAndRewrite(Op op,
                                PatternRewriter &rewriter) const override {
    assert(op.getRegion().getBlocks().size() == 1 &&
           "only one stream block supported");
    bool didModify = false;
    for (auto yieldOp : op.template getOps<IREE::Stream::YieldOp>()) {
      for (auto result : llvm::enumerate(yieldOp.operands())) {
        if (op.getTiedResultOperandIndex(result.index()).hasValue()) {
          continue;  // Already tied.
        }
        auto baseValue =
            IREE::Util::TiedOpInterface::findTiedBaseValue(result.value());
        if (auto blockArg = baseValue.template dyn_cast<BlockArgument>()) {
          unsigned operandIndex = blockArg.getArgNumber();
          op.setTiedResultOperandIndex(result.index(), operandIndex);
          didModify = true;
        }
      }
    }
    return didModify ? success() : failure();
  }
};

}  // namespace

//===----------------------------------------------------------------------===//
// stream.resource.alloc
//===----------------------------------------------------------------------===//

void ResourceAllocOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): sink to first user.
}

//===----------------------------------------------------------------------===//
// stream.resource.alloca
//===----------------------------------------------------------------------===//

void ResourceAllocaOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): sink to first user.
  // TODO(benvanik): elide if only user is dealloc.
}

//===----------------------------------------------------------------------===//
// stream.resource.dealloca
//===----------------------------------------------------------------------===//

void ResourceDeallocaOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): move up to producer of timepoint.
}

//===----------------------------------------------------------------------===//
// stream.resource.size
//===----------------------------------------------------------------------===//

OpFoldResult ResourceSizeOp::fold(ArrayRef<Attribute> operands) {
  auto sizeAwareType =
      operand().getType().cast<IREE::Util::SizeAwareTypeInterface>();
  return sizeAwareType.findSizeValue(operand(), *this);
}

//===----------------------------------------------------------------------===//
// stream.resource.map
//===----------------------------------------------------------------------===//

void ResourceMapOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): fold subviews up into maps to limit range.
  results.insert<ElideUnusedOp<ResourceMapOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.resource.try_map
//===----------------------------------------------------------------------===//

void ResourceTryMapOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): fold subviews up into maps to limit range.
  // TODO(benvanik): if mapping for staging then turn into a map?
  results.insert<ElideUnusedOp<ResourceTryMapOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.resource.load
//===----------------------------------------------------------------------===//

namespace {

struct FoldSubviewIntoLoadOp : public OpRewritePattern<ResourceLoadOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ResourceLoadOp op,
                                PatternRewriter &rewriter) const override {
    auto subviewOp = ResourceSubviewOp::findSubviewOp(op.source());
    if (!subviewOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({subviewOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
        fusedLoc, subviewOp.source_offset(), op.source_offset());
    rewriter.updateRootInPlace(op, [&]() {
      op.sourceMutable().assign(subviewOp.source());
      op.source_sizeMutable().assign(subviewOp.source_size());
      op.source_offsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void ResourceLoadOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): if staging resource comes from splat (through transfers)
  //                 then pull splat value.
  // TODO(benvanik): combine multiple loads from the same target if contiguous.
  // TODO(benvanik): value->transfer->load -> value->slice->transfer->load?
  results.insert<FoldSubviewIntoLoadOp>(context);
  results.insert<ElideUnusedOp<ResourceLoadOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.resource.store
//===----------------------------------------------------------------------===//

namespace {

struct FoldSubviewIntoStoreOp : public OpRewritePattern<ResourceStoreOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ResourceStoreOp op,
                                PatternRewriter &rewriter) const override {
    auto subviewOp = ResourceSubviewOp::findSubviewOp(op.target());
    if (!subviewOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({subviewOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
        fusedLoc, subviewOp.source_offset(), op.target_offset());
    rewriter.updateRootInPlace(op, [&]() {
      op.targetMutable().assign(subviewOp.source());
      op.target_sizeMutable().assign(subviewOp.source_size());
      op.target_offsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void ResourceStoreOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): combine multiple stores to the same target if contiguous.
  // TODO(benvanik): if value is a constant splat then turn into fill?
  results.insert<FoldSubviewIntoStoreOp>(context);
  results.insert<ElideUnusedOp<ResourceStoreOp>>(context);
  results.insert<MaterializeCOW<ResourceStoreOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.resource.pack
//===----------------------------------------------------------------------===//

LogicalResult ResourcePackOp::fold(ArrayRef<Attribute> operands,
                                   SmallVectorImpl<OpFoldResult> &results) {
  Builder builder(getContext());

  // If there are no slices then the entire pack results in a zero-length slab.
  if (packed_offsets().empty()) {
    results.push_back(builder.getZeroAttr(builder.getIndexType()));
    return success();
  }

  // If there's a single slice then we just use that as there is no packing to
  // perform.
  if (packed_offsets().size() == 1) {
    // Total length is the slice size and offset is always either 0 or the
    // provided optional base offset.
    results.push_back(dynamic_slice_sizes()[0]);
    if (offset()) {
      results.push_back(offset());
    } else {
      results.push_back(builder.getZeroAttr(builder.getIndexType()));
    }
    return success();
  }

  return failure();
}

namespace {

/// Propagates base offsets on a pack op to its results.
/// This allows for better folding of the results after packing has completed.
/// The offset value is just a convenience for when splitting pack ops and has
/// no impact on the actual packing operation.
struct PropagateResourcePackBaseOffset
    : public OpRewritePattern<ResourcePackOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ResourcePackOp op,
                                PatternRewriter &rewriter) const override {
    // Offset is optional.
    auto baseOffset = op.offset();
    if (!baseOffset) return failure();

    // We always strip the offset here.
    rewriter.updateRootInPlace(op, [&]() { op.offsetMutable().clear(); });

    // Zero offsets don't do anything and can just be removed so we can avoid
    // inserting a bunch of additional IR.
    if (auto constantOp =
            dyn_cast_or_null<ConstantIndexOp>(baseOffset.getDefiningOp())) {
      if (constantOp.getValue() == 0) {
        return success();
      }
    }

    // Propagate the offset to all returned slice offsets.
    rewriter.setInsertionPointAfter(op);
    for (auto sliceOffset : op.packed_offsets()) {
      auto addOp =
          rewriter.create<mlir::AddIOp>(op.getLoc(), baseOffset, sliceOffset);
      SmallPtrSet<Operation *, 1> exclusions;
      exclusions.insert(addOp);
      sliceOffset.replaceAllUsesExcept(addOp.result(), exclusions);
    }

    return success();
  }
};

/// Sorts and compacts the slice intervals into a dense ascending order set.
/// This is not required by the packing algorithm but yields more
/// consistent-looking IR and makes the range overlaps easier to see for us
/// meatbags.
struct CanonicalizeResourcePackIntervals
    : public OpRewritePattern<ResourcePackOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ResourcePackOp op,
                                PatternRewriter &rewriter) const override {
    // Get the slices in a possibly unsorted order and sort.
    auto slices = op.getSlices();
    std::stable_sort(slices.begin(), slices.end());

    // See if the sorted order is different than how they are stored in the op.
    bool orderChanged = false;
    for (auto it : llvm::zip(slices, op.packed_offsets())) {
      if (std::get<0>(it).packedOffset != std::get<1>(it)) {
        orderChanged = true;
        break;
      }
    }
    if (!orderChanged) return failure();

    // TODO(benvanik): compact the slice ranges.

    // Rebuild the op with the sorted values.
    SmallVector<int64_t> lifetimeIntervals(slices.size() * 2);
    SmallVector<Value> dynamicSliceSizes(slices.size());
    for (size_t i = 0; i < slices.size(); ++i) {
      const auto &slice = slices[i];
      lifetimeIntervals[2 * i + 0] = slice.lifetimeStart;
      lifetimeIntervals[2 * i + 1] = slice.lifetimeEnd;
      dynamicSliceSizes[i] = slice.dynamicSize;
    }
    SmallVector<Type> packedOffsetTypes(slices.size(), rewriter.getIndexType());
    auto newOp = rewriter.create<ResourcePackOp>(
        op.getLoc(), op.total_length().getType(), packedOffsetTypes,
        op.offset(), rewriter.getIndexArrayAttr(lifetimeIntervals),
        dynamicSliceSizes, op.affinityAttr());

    // Remap existing values to the new values.
    op.total_length().replaceAllUsesWith(newOp.total_length());
    for (size_t i = 0; i < newOp.packed_offsets().size(); ++i) {
      slices[i].packedOffset.replaceAllUsesWith(newOp.packed_offsets()[i]);
    }

    rewriter.eraseOp(op);
    return success();
  }
};

}  // namespace

void ResourcePackOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<PropagateResourcePackBaseOffset>(context);
  results.insert<CanonicalizeResourcePackIntervals>(context);
}

//===----------------------------------------------------------------------===//
// stream.resource.pack
//===----------------------------------------------------------------------===//

OpFoldResult ResourceSubviewOp::fold(ArrayRef<Attribute> operands) {
  if (source_size() == result_size()) {
    // Entire range is covered; return it all.
    return source();
  }
  return {};
}

namespace {

// Folds subview -> subview to point at the original source resource with an
// updated range.
struct FoldResourceSubviewOps : public OpRewritePattern<ResourceSubviewOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ResourceSubviewOp op,
                                PatternRewriter &rewriter) const override {
    auto parentOp = ResourceSubviewOp::findSubviewOp(op.source());
    if (!parentOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({parentOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
        fusedLoc, parentOp.source_offset(), op.source_offset());
    auto newOp = rewriter.create<ResourceSubviewOp>(
        fusedLoc, parentOp.source(), parentOp.source_size(), newOffset,
        op.result_size());
    rewriter.replaceOp(op, newOp.result());
    return success();
  }
};

}  // namespace

void ResourceSubviewOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<FoldResourceSubviewOps>(context);
}

//===----------------------------------------------------------------------===//
// stream.tensor.import
//===----------------------------------------------------------------------===//

OpFoldResult TensorImportOp::fold(ArrayRef<Attribute> operands) {
  // TODO(benvanik): if operand comes from export then fold.
  return {};
}

void TensorImportOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): check operand and dedupe imports.
  results.insert<MaterializeCOW<TensorImportOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.tensor.export
//===----------------------------------------------------------------------===//

OpFoldResult TensorExportOp::fold(ArrayRef<Attribute> operands) {
  // TODO(benvanik): if operand comes from import then fold.
  return {};
}

void TensorExportOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): check operand and dedupe exports.
}

//===----------------------------------------------------------------------===//
// stream.tensor.sizeof
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// stream.tensor.constant
//===----------------------------------------------------------------------===//

namespace {

struct TensorConstantToSplat : public OpRewritePattern<TensorConstantOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TensorConstantOp constantOp,
                                PatternRewriter &rewriter) const override {
    auto splatAttr = constantOp.value().dyn_cast<SplatElementsAttr>();
    if (!splatAttr || !splatAttr.isSplat()) {
      return rewriter.notifyMatchFailure(
          constantOp,
          "only constant splat attrs can be converted to splat ops");
    }

    auto splatElementAttr = splatAttr.getSplatValue();
    auto splatValue = rewriter.create<mlir::ConstantOp>(
        constantOp.getLoc(), splatElementAttr.getType(), splatElementAttr);
    auto resultType = IREE::Stream::ResourceType::get(constantOp.getContext());
    auto resultSize = rewriter.createOrFold<IREE::Stream::TensorSizeOfOp>(
        constantOp.getLoc(), rewriter.getIndexType(),
        TypeAttr::get(constantOp.result_encoding()),
        constantOp.result_encoding_dims(), /*affinity=*/nullptr);
    auto splatOp = rewriter.create<TensorSplatOp>(
        constantOp.getLoc(), resultType, splatValue,
        constantOp.result_encoding(), constantOp.result_encoding_dims(),
        resultSize,
        /*affinity=*/nullptr);
    rewriter.replaceOpWithNewOp<AsyncTransferOp>(
        constantOp, constantOp.result().getType(), splatOp.result(), resultSize,
        resultSize, /*source_affinity=*/nullptr,
        /*result_affinity=*/nullptr);
    return success();
  }
};

}  // namespace

void TensorConstantOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): if value is _mostly_ a splat, turn into splat + updates.
  results.insert<TensorConstantToSplat>(context);
}

//===----------------------------------------------------------------------===//
// stream.tensor.splat
//===----------------------------------------------------------------------===//

void TensorSplatOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<ElideUnusedOp<TensorSplatOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.tensor.clone
//===----------------------------------------------------------------------===//

OpFoldResult TensorCloneOp::fold(ArrayRef<Attribute> operands) {
  auto users = result().getUsers();
  if (!users.empty() && std::next(users.begin()) == users.end()) {
    return source();
  }
  return {};
}

namespace {

// Elides clones that don't do anything meaningful (like setting up a tie).
struct ElideUnneededTensorClones : public OpRewritePattern<TensorCloneOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TensorCloneOp cloneOp,
                                PatternRewriter &rewriter) const override {
    if (!IREE::Util::TiedOpInterface::hasAnyTiedUses(cloneOp.result())) {
      rewriter.replaceOp(cloneOp, cloneOp.source());
      return success();
    }
    return failure();
  }
};

}  // namespace

void TensorCloneOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): splat -> clone duplicates splat.
  // TODO(benvanik): some way to reduce deep clone->clone->clone chains.
  // TODO(benvanik): clone + slice => slice.
  // TODO(benvanik): if both operand and result are used once then elide.
  //                 (if not tied block/fn arguments)
  results.insert<ElideUnneededTensorClones>(context);
}

//===----------------------------------------------------------------------===//
// stream.tensor.slice
//===----------------------------------------------------------------------===//

OpFoldResult TensorSliceOp::fold(ArrayRef<Attribute> operands) {
  // TODO(benvanik): fold if source_size == result_size and affinity/lifetime.
  return {};
}

void TensorSliceOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): turn into a transfer if target_size == update_size and
  //                 affinity/lifetime differ.
  // TODO(benvanik): splat->slice -> splat.
  // TODO(benvanik): clone->slice -> slice.
}

//===----------------------------------------------------------------------===//
// stream.tensor.fill
//===----------------------------------------------------------------------===//

void TensorFillOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): if target_size == sizeof(value) turn into splat.
}

//===----------------------------------------------------------------------===//
// stream.tensor.update
//===----------------------------------------------------------------------===//

OpFoldResult TensorUpdateOp::fold(ArrayRef<Attribute> operands) {
  // TODO(benvanik): fold if target_size == update_size and affinity/lifetime.
  return {};
}

void TensorUpdateOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): turn into a transfer if target_size == update_size and
  //                 affinity/lifetime differ.
  // TODO(benvanik): turn into fill if source is a splat.
}

//===----------------------------------------------------------------------===//
// stream.tensor.load
//===----------------------------------------------------------------------===//

void TensorLoadOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): splat + load -> splat value.
  // TODO(benvanik): clone + ex load -> slice (ranged) + load.
  // TODO(benvanik): slice + ex load -> slice (ranged) + load.
  // TODO(benvanik): value->transfer->load -> value->slice->transfer->load?
  // TODO(benvanik): combine multiple loads from the same target if contiguous.
}

//===----------------------------------------------------------------------===//
// stream.tensor.store
//===----------------------------------------------------------------------===//

void TensorStoreOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): if value is a constant splat then turn into fill.
  // TODO(benvanik): combine multiple stores to the same target if contiguous.
}

//===----------------------------------------------------------------------===//
// stream.async.alloca
//===----------------------------------------------------------------------===//

void AsyncAllocaOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): alloca (staging) -> non-staging change to target.
  // TODO(benvanik): alloca (non-staging) -> staging change to target.
  // TODO(benvanik): sink to first user.
  results.insert<MaterializeCOW<AsyncAllocaOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.constant
//===----------------------------------------------------------------------===//

void AsyncConstantOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): if value is a splat turn into splat.
  // TODO(benvanik): if value is _mostly_ a splat, turn into splat + updates.
  results.insert<MaterializeCOW<AsyncConstantOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.splat
//===----------------------------------------------------------------------===//

namespace {

// Sinks splat ops down to its consumers to avoid cases where we splat and then
// keep that live/copy-on-write it.
struct SinkSplatsToConsumers : public OpRewritePattern<AsyncSplatOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncSplatOp splatOp,
                                PatternRewriter &rewriter) const override {
    auto users = llvm::to_vector<4>(splatOp->getUsers());
    if (users.size() == 0) return failure();

    // If we have a single user then we can sink right to it.
    if (users.size() == 1) {
      return sinkOp(splatOp, users.front());
    }

    // If we only have users in the same block then we can safely move to the
    // first (as no change to cross-block SSA dominance can happen).
    if (!splatOp.result().isUsedOutsideOfBlock(splatOp->getBlock())) {
      Operation *targetOp = nullptr;
      for (auto user : users) {
        if (!targetOp || user->isBeforeInBlock(targetOp)) {
          targetOp = user;
        }
      }
      assert(targetOp);
      return sinkOp(splatOp, targetOp);
    }

    // Redundant computation here, but only in cases where we have multiple
    // users that may live outside the block the op is in.
    DominanceInfo domInfo(splatOp->getParentOp());

    // Find the common dominator block across all uses. This may be the
    // entry block itself.
    Block *commonDominator = users.front()->getBlock();
    for (auto user : users) {
      commonDominator =
          domInfo.findNearestCommonDominator(commonDominator, user->getBlock());
    }

    // Find the first use within the dominator block (if any) so that we
    // can sink down to it.
    Operation *firstUserInDominator = commonDominator->getTerminator();
    for (auto user : users) {
      if (user->getBlock() == commonDominator) {
        if (user->isBeforeInBlock(firstUserInDominator)) {
          firstUserInDominator = user;
        }
      }
    }

    // Sink to the common dominator - which may not even use the op but will
    // at least prevent us from doing extra work.
    return sinkOp(splatOp, firstUserInDominator);
  }
};

}  // namespace

void AsyncSplatOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(#6972): find splat+update-from and turn into fill.
  // TODO(#6972): find splat+copy-from and turn into fill.
  // TODO(#6972): find splat+update-into and turn into alloca+fill+update.
  // TODO(#6972): find splat+copy-into and turn into alloca+fill+copy.
  results.insert<SinkSplatsToConsumers>(context);
  results.insert<ElideUnusedOp<AsyncSplatOp>>(context);
  results.insert<MaterializeCOW<AsyncSplatOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.clone
//===----------------------------------------------------------------------===//

OpFoldResult AsyncCloneOp::fold(ArrayRef<Attribute> operands) {
  // TODO(benvanik): trivial elides when there are no tied users/one user.
  return {};
}

namespace {

// Clones ops that prefer to be cloned directly.
// This prevents us from splatting out a value and then cloning that (keeping
// the memory live/etc) instead of just splatting it again on-demand.
struct PropagateClonableOps : public OpRewritePattern<AsyncCloneOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncCloneOp cloneOp,
                                PatternRewriter &rewriter) const override {
    if (cloneOp.use_empty()) return failure();
    auto sourceOp = dyn_cast_or_null<IREE::Stream::StreamableOpInterface>(
        cloneOp.source().getDefiningOp());
    if (!sourceOp || !sourceOp.preferCloneToConsumers()) return failure();
    for (auto &use : llvm::make_early_inc_range(cloneOp.result().getUses())) {
      rewriter.setInsertionPoint(use.getOwner());
      auto clonedOp = rewriter.clone(*sourceOp);
      use.set(clonedOp->getResult(0));
    }
    if (cloneOp.use_empty()) {
      rewriter.eraseOp(cloneOp);
    }
    return success();
  }
};

// Propagates slices through clones (slice->clone):
//  %0 = stream.async.slice %arg0
//  %1 = stream.async.clone %0
// ->
//  %0 = stream.async.slice %arg0 (maybe dead)
//  %1 = stream.async.slice %arg0
//
// This prevents the data hazard through the clone when we could instead go
// right to the source.
struct PropagateClonedSlices : public OpRewritePattern<AsyncCloneOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncCloneOp cloneOp,
                                PatternRewriter &rewriter) const override {
    auto sliceOp = dyn_cast_or_null<IREE::Stream::AsyncSliceOp>(
        cloneOp.source().getDefiningOp());
    if (!sliceOp) return failure();

    // Sourced from a slice; slice from the origin directly.
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncSliceOp>(
        cloneOp, cloneOp.result().getType(), sliceOp.source(),
        sliceOp.source_size(), sliceOp.source_offset(), sliceOp.source_end(),
        sliceOp.result_size(), cloneOp.affinityAttr());
    return success();
  }
};

// Elides clones that don't do anything meaningful (like setting up a tie).
struct ElideUnneededAsyncClones : public OpRewritePattern<AsyncCloneOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncCloneOp cloneOp,
                                PatternRewriter &rewriter) const override {
    if (!IREE::Util::TiedOpInterface::hasAnyTiedUses(cloneOp.result()) &&
        !IREE::Util::TiedOpInterface::hasAnyTiedUses(cloneOp.source())) {
      rewriter.replaceOp(cloneOp, cloneOp.source());
      return success();
    }
    return failure();
  }
};

}  // namespace

void AsyncCloneOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): some way to reduce deep clone->clone->clone chains.
  results.insert<PropagateClonableOps>(context);
  results.insert<PropagateClonedSlices>(context);
  results.insert<ElideUnneededAsyncClones>(context);
  results.insert<ElideUnusedOp<AsyncCloneOp>>(context);
  results.insert<MaterializeCOW<AsyncCloneOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.slice
//===----------------------------------------------------------------------===//

OpFoldResult AsyncSliceOp::fold(ArrayRef<Attribute> operands) {
  if (source_size() == result_size()) {
    // Slicing entire source - just reroute to source.
    // Note that this breaks copy-on-write semantics but will be fixed up during
    // canonicalization if needed.
    return source();
  }
  return {};
}

namespace {

// Propagates slices through clones (clone->slice):
//  %0 = stream.async.clone %arg0
//  %1 = stream.async.slice %0
// ->
//  %0 = stream.async.clone %arg0 (maybe dead)
//  %1 = stream.async.slice %arg0
//
// This prevents us from potentially cloning a large resource to then slice out
// a small bit.
struct PropagateSliceClones : public OpRewritePattern<AsyncSliceOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    auto cloneOp = dyn_cast_or_null<IREE::Stream::AsyncCloneOp>(
        sliceOp.source().getDefiningOp());
    if (cloneOp) {
      // Sourced from a slice; slice from the origin.
      rewriter.replaceOpWithNewOp<IREE::Stream::AsyncSliceOp>(
          sliceOp, sliceOp.result().getType(), cloneOp.source(),
          cloneOp.source_size(), sliceOp.source_offset(), sliceOp.source_end(),
          sliceOp.result_size(), sliceOp.affinityAttr());
      return success();
    }
    return failure();
  }
};

// Clones a splat op through a slice as a splat+slice is just a smaller splat.
struct PropagateSplatsThroughSlices : public OpRewritePattern<AsyncSliceOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    auto splatOp = dyn_cast_or_null<IREE::Stream::AsyncSplatOp>(
        sliceOp.source().getDefiningOp());
    if (!splatOp) return failure();
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncSplatOp>(
        sliceOp, sliceOp.result().getType(), splatOp.value(),
        sliceOp.result_size(), sliceOp.affinityAttr());
    return success();
  }
};

}  // namespace

void AsyncSliceOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): turn into a transfer if target_size == update_size and
  //                 affinity/lifetime differ.
  results.insert<PropagateSliceClones>(context);
  results.insert<PropagateSplatsThroughSlices>(context);
  results.insert<ElideUnusedOp<AsyncSliceOp>>(context);
  results.insert<MaterializeCOW<AsyncSliceOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.fill
//===----------------------------------------------------------------------===//

namespace {

// Turns fills that cover an entire target resource into splats.
// This acts as a discard as it indicates we don't care about the previous
// resource contents.
struct FlattenFullFillToSplat : public OpRewritePattern<AsyncFillOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncFillOp fillOp,
                                PatternRewriter &rewriter) const override {
    if (fillOp.target_length() == fillOp.target_size()) {
      rewriter.replaceOpWithNewOp<IREE::Stream::AsyncSplatOp>(
          fillOp, fillOp.result().getType(), fillOp.value(),
          fillOp.target_size(), fillOp.affinityAttr());
      return success();
    }
    return failure();
  }
};

}  // namespace

void AsyncFillOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.insert<FlattenFullFillToSplat>(context);
  results.insert<ElideUnusedOp<AsyncFillOp>>(context);
  results.insert<MaterializeCOW<AsyncFillOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.update
//===----------------------------------------------------------------------===//

OpFoldResult AsyncUpdateOp::fold(ArrayRef<Attribute> operands) {
  if (update_size() == target_size()) {
    // If updating the entire target then just replace with the update.
    // Note that this breaks copy-on-write semantics but will be fixed up during
    // canonicalization if needed.
    return update();
  }
  return {};
}

namespace {

// Turns a splat+update-from into a fill.
struct CombineSplatUpdateFromToFill : public OpRewritePattern<AsyncUpdateOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncUpdateOp updateOp,
                                PatternRewriter &rewriter) const override {
    auto splatOp = dyn_cast_or_null<IREE::Stream::AsyncSplatOp>(
        updateOp.update().getDefiningOp());
    if (!splatOp) return failure();
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncFillOp>(
        updateOp, updateOp.result().getType(), updateOp.target(),
        updateOp.target_size(), updateOp.target_offset(), updateOp.target_end(),
        updateOp.update_size(), splatOp.value(), updateOp.tied_operandsAttr(),
        updateOp.affinityAttr());
    return success();
  }
};

// Turns slice+update-from into a copy.
// This is equivalent behavior at runtime but better to schedule as a single
// operation.
//
// This could pessimize memory consumption if the slice is far from the consumer
// update: it's better to slice away a small part of a resource to retain than
// keeping the whole one around. Because of that we only trigger this pattern
// if the slice is produced after the update target.
struct CombineSliceUpdateFromToCopy : public OpRewritePattern<AsyncUpdateOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncUpdateOp updateOp,
                                PatternRewriter &rewriter) const override {
    auto sliceOp = dyn_cast_or_null<IREE::Stream::AsyncSliceOp>(
        updateOp.update().getDefiningOp());
    if (!sliceOp || sliceOp->getBlock() != updateOp->getBlock()) {
      // Source is not a slice or a slice from out-of-block. We don't want to
      // grow memory usage by sinking the slice here (we may slice into the
      // body of a for loop, for example).
      return failure();
    }
    auto *targetDefOp = updateOp.target().getDefiningOp();
    if (!targetDefOp || targetDefOp->getBlock() != sliceOp->getBlock() ||
        sliceOp->isBeforeInBlock(targetDefOp)) {
      // Target is defined after the slice and we want to avoid keeping the
      // slice source live as the slice may allow us to allocate the target
      // in-place.
      return failure();
    }
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncCopyOp>(
        updateOp, updateOp.result().getType(), updateOp.target(),
        updateOp.target_size(), updateOp.target_offset(), updateOp.target_end(),
        sliceOp.source(), sliceOp.source_size(), sliceOp.source_offset(),
        sliceOp.source_end(), sliceOp.result_size(),
        updateOp.tied_operandsAttr(), updateOp.affinityAttr());
    return success();
  }
};

}  // namespace

void AsyncUpdateOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): turn into a transfer if target_size == update_size and
  //                 affinity/lifetime differ.
  // TODO(#6972): updates into splats could become alloca + fill exclusive
  //              region + update into undefined contents (used in padding).
  results.insert<CombineSplatUpdateFromToFill>(context);
  results.insert<CombineSliceUpdateFromToCopy>(context);
  results.insert<ElideUnusedOp<AsyncUpdateOp>>(context);
  results.insert<MaterializeCOW<AsyncUpdateOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.copy
//===----------------------------------------------------------------------===//

namespace {

// Turns a copy from an entire resource into an update. Updates can be more
// efficient during allocation as we know the producer can write directly into
// the target.
struct AsyncCopyFullSourceToUpdate : public OpRewritePattern<AsyncCopyOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncCopyOp copyOp,
                                PatternRewriter &rewriter) const override {
    if (copyOp.source_end() == copyOp.source_size()) {
      rewriter.replaceOpWithNewOp<IREE::Stream::AsyncUpdateOp>(
          copyOp, copyOp.result().getType(), copyOp.target(),
          copyOp.target_size(), copyOp.target_offset(), copyOp.target_end(),
          copyOp.source(), copyOp.source_size(), copyOp.tied_operandsAttr(),
          copyOp.affinityAttr());
      return success();
    }
    return failure();
  }
};

}  // namespace

void AsyncCopyOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.insert<AsyncCopyFullSourceToUpdate>(context);
  results.insert<ElideUnusedOp<AsyncCopyOp>>(context);
  results.insert<MaterializeCOW<AsyncCopyOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.transfer
//===----------------------------------------------------------------------===//

OpFoldResult AsyncTransferOp::fold(ArrayRef<Attribute> operands) {
  if (auto sourceTransferOp =
          dyn_cast_or_null<AsyncTransferOp>(source().getDefiningOp())) {
    if (sourceTransferOp.source().getType() == result().getType() &&
        sourceTransferOp.source_affinity() == result_affinity()) {
      return sourceTransferOp.source();
    }
  }
  return {};
}

namespace {

struct RedundantTransferElision : public OpRewritePattern<AsyncTransferOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncTransferOp transferOp,
                                PatternRewriter &rewriter) const override {
    if (transferOp.source_affinityAttr() == transferOp.result_affinityAttr() &&
        transferOp.source().getType() == transferOp.result().getType()) {
      // Transfer performs no work, elide.
      rewriter.replaceOp(transferOp, transferOp.source());
      return success();
    }
    return failure();
  }
};

}  // namespace

void AsyncTransferOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): staging propagation (fill of staging -> fill on device).
  results.insert<RedundantTransferElision>(context);
  results.insert<ElideUnusedOp<AsyncTransferOp>>(context);
  results.insert<MaterializeCOW<AsyncTransferOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.dispatch
//===----------------------------------------------------------------------===//

void AsyncDispatchOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): nothing? maybe tied type/lifetime updates?
  results.insert<ElideUnusedOp<AsyncDispatchOp>>(context);
  results.insert<MaterializeCOW<AsyncDispatchOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.execute
//===----------------------------------------------------------------------===//

namespace {

struct ElideImmediateAsyncExecuteWaits
    : public OpRewritePattern<AsyncExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncExecuteOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<unsigned> elidedTimepoints;
    for (auto timepoint : llvm::enumerate(op.await_timepoints())) {
      if (isa_and_nonnull<TimepointImmediateOp>(
              timepoint.value().getDefiningOp())) {
        // Non-immediate (at least that we know of).
        elidedTimepoints.push_back(timepoint.index());
      }
    }
    if (elidedTimepoints.empty()) return failure();
    rewriter.updateRootInPlace(op, [&]() {
      for (unsigned idx : llvm::reverse(elidedTimepoints)) {
        op.await_timepointsMutable().erase(idx);
      }
    });
    return success();
  }
};

// TODO(benvanik): check for covering waits (A->B->C, C just needs B).
struct ElideDuplicateAsyncExecuteWaits
    : public OpRewritePattern<AsyncExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncExecuteOp op,
                                PatternRewriter &rewriter) const override {
    SetVector<Value> uniqueTimepoints;
    uniqueTimepoints.insert(op.await_timepoints().begin(),
                            op.await_timepoints().end());
    if (uniqueTimepoints.size() == op.await_timepoints().size()) {
      return failure();
    }
    rewriter.updateRootInPlace(op, [&]() {
      op.await_timepointsMutable().assign(uniqueTimepoints.takeVector());
    });
    return success();
  }
};

struct ChainAsyncExecuteWaits : public OpRewritePattern<AsyncExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncExecuteOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<std::pair<unsigned, Value>> replacements;
    for (auto operand : llvm::enumerate(op.operands())) {
      if (auto awaitOp = dyn_cast_or_null<TimepointAwaitOp>(
              operand.value().getDefiningOp())) {
        replacements.push_back(std::make_pair(
            operand.index(), awaitOp.getTiedResultOperand(operand.value())));
      }
    }
    if (replacements.empty()) return failure();
    rewriter.updateRootInPlace(op, [&]() {
      for (auto replacement : replacements) {
        op.operandsMutable()
            .slice(replacement.first, 1)
            .assign(replacement.second);
      }
    });
    return success();
  }
};

// If any operands are sourced from subviews clone those subviews into the
// region and rewrite the operands to point at the original resource. This
// allows us to progressively fold the subviews into the ops consuming them.
struct CloneCapturedAsyncExecuteSubviewOps
    : public OpRewritePattern<AsyncExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncExecuteOp op,
                                PatternRewriter &rewriter) const override {
    struct SubviewCapture {
      unsigned operandIdx;
      IREE::Stream::ResourceSubviewOp subviewOp;
    };
    SmallVector<SubviewCapture> captures;
    for (auto operand : llvm::enumerate(op.operands())) {
      auto subviewOp = ResourceSubviewOp::findSubviewOp(operand.value());
      if (!subviewOp) continue;
      captures.push_back(
          SubviewCapture{static_cast<unsigned>(operand.index()), subviewOp});
    }
    if (captures.empty()) return failure();
    rewriter.startRootUpdate(op);

    auto &entryBlock = op.body().front();
    rewriter.setInsertionPointToStart(&entryBlock);
    for (auto &capture : captures) {
      // Replace operand with the source subview resource.
      op.operandsMutable()
          .slice(capture.operandIdx, 1)
          .assign(capture.subviewOp.source());
      op.operand_sizesMutable()
          .slice(capture.operandIdx, 1)
          .assign(capture.subviewOp.source_size());

      // Clone the subview into the region and wire it up to take the same
      // range as the original.
      auto arg = entryBlock.getArgument(capture.operandIdx);
      auto newOp = rewriter.create<ResourceSubviewOp>(
          capture.subviewOp.getLoc(), arg, capture.subviewOp.source_size(),
          capture.subviewOp.source_offset(), capture.subviewOp.result_size());
      arg.replaceAllUsesExcept(newOp.result(), newOp);
    }

    rewriter.finalizeRootUpdate(op);
    return success();
  }
};

// Elides stream.async.execute ops when they have no meaningful work.
// The returned timepoint is replaced with an immediately resolved timepoint.
struct ElideNoOpAsyncExecuteOp : public OpRewritePattern<AsyncExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(AsyncExecuteOp op,
                                PatternRewriter &rewriter) const override {
    auto &entryBlock = op.body().front();
    if (entryBlock.getOperations().size() != 1) {
      // Has non-yield ops.
      return failure();
    }
    auto yieldOp = cast<YieldOp>(entryBlock.getTerminator());
    if (!yieldOp.operands().empty()) {
      return rewriter.notifyMatchFailure(
          op, "no ops in execute region but still passing through operands");
    }
    rewriter.replaceOpWithNewOp<TimepointImmediateOp>(
        op, op.result_timepoint().getType());
    return success();
  }
};

}  // namespace

void AsyncExecuteOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<ElideImmediateAsyncExecuteWaits>(context);
  results.insert<ElideDuplicateAsyncExecuteWaits>(context);
  results.insert<ChainAsyncExecuteWaits>(context);
  results.insert<CloneCapturedAsyncExecuteSubviewOps>(context);
  results.insert<ElideNoOpAsyncExecuteOp>(context);
  results.insert<IREE::Util::ClosureOptimizationPattern<AsyncExecuteOp>>(
      context);
  results.insert<TieRegionResults<AsyncExecuteOp>>(context);
  results.insert<ElideUnusedOp<AsyncExecuteOp>>(context);
  results.insert<MaterializeCOW<AsyncExecuteOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.async.wave
//===----------------------------------------------------------------------===//

void AsyncWaveOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.insert<IREE::Util::ClosureOptimizationPattern<AsyncWaveOp>>(context);
  results.insert<TieRegionResults<AsyncWaveOp>>(context);
  results.insert<ElideUnusedOp<AsyncWaveOp>>(context);
  results.insert<MaterializeCOW<AsyncWaveOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.flush
//===----------------------------------------------------------------------===//

namespace {

struct FoldSubviewsIntoCmdFlushOp : public OpRewritePattern<CmdFlushOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdFlushOp op,
                                PatternRewriter &rewriter) const override {
    auto subviewOp = ResourceSubviewOp::findSubviewOp(op.target());
    if (!subviewOp) return failure();
    setInsertionPointToParentExecutionScope(op, rewriter);
    auto fusedLoc = rewriter.getFusedLoc({subviewOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
        fusedLoc, subviewOp.source_offset(), op.target_offset());
    rewriter.updateRootInPlace(op, [&]() {
      op.targetMutable().assign(subviewOp.source());
      op.target_sizeMutable().assign(subviewOp.source_size());
      op.target_offsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void CmdFlushOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                             MLIRContext *context) {
  results.insert<FoldSubviewsIntoCmdFlushOp>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.invalidate
//===----------------------------------------------------------------------===//

namespace {

struct FoldCmdInSubviewsIntoCmdInvalidateOp
    : public OpRewritePattern<CmdInvalidateOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdInvalidateOp op,
                                PatternRewriter &rewriter) const override {
    auto subviewOp = ResourceSubviewOp::findSubviewOp(op.target());
    if (!subviewOp) return failure();
    setInsertionPointToParentExecutionScope(op, rewriter);
    auto fusedLoc = rewriter.getFusedLoc({subviewOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
        fusedLoc, subviewOp.source_offset(), op.target_offset());
    rewriter.updateRootInPlace(op, [&]() {
      op.targetMutable().assign(subviewOp.source());
      op.target_sizeMutable().assign(subviewOp.source_size());
      op.target_offsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void CmdInvalidateOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<FoldCmdInSubviewsIntoCmdInvalidateOp>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.discard
//===----------------------------------------------------------------------===//

namespace {

struct FoldCmdSubviewsIntoCmdDiscardOp : public OpRewritePattern<CmdDiscardOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdDiscardOp op,
                                PatternRewriter &rewriter) const override {
    auto subviewOp = ResourceSubviewOp::findSubviewOp(op.target());
    if (!subviewOp) return failure();
    setInsertionPointToParentExecutionScope(op, rewriter);
    auto fusedLoc = rewriter.getFusedLoc({subviewOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
        fusedLoc, subviewOp.source_offset(), op.target_offset());
    rewriter.updateRootInPlace(op, [&]() {
      op.targetMutable().assign(subviewOp.source());
      op.target_sizeMutable().assign(subviewOp.source_size());
      op.target_offsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void CmdDiscardOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<FoldCmdSubviewsIntoCmdDiscardOp>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.fill
//===----------------------------------------------------------------------===//

namespace {

struct FoldSubviewsIntoCmdFillOp : public OpRewritePattern<CmdFillOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdFillOp op,
                                PatternRewriter &rewriter) const override {
    auto subviewOp = ResourceSubviewOp::findSubviewOp(op.target());
    if (!subviewOp) return failure();
    setInsertionPointToParentExecutionScope(op, rewriter);
    auto fusedLoc = rewriter.getFusedLoc({subviewOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
        fusedLoc, subviewOp.source_offset(), op.target_offset());
    rewriter.updateRootInPlace(op, [&]() {
      op.targetMutable().assign(subviewOp.source());
      op.target_sizeMutable().assign(subviewOp.source_size());
      op.target_offsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void CmdFillOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  results.insert<FoldSubviewsIntoCmdFillOp>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.copy
//===----------------------------------------------------------------------===//

namespace {

struct FoldSubviewsIntoCmdCopyOp : public OpRewritePattern<CmdCopyOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdCopyOp op,
                                PatternRewriter &rewriter) const override {
    auto sourceSubviewOp = ResourceSubviewOp::findSubviewOp(op.source());
    auto targetSubviewOp = ResourceSubviewOp::findSubviewOp(op.target());
    if (!sourceSubviewOp && !targetSubviewOp) return failure();
    setInsertionPointToParentExecutionScope(op, rewriter);
    if (sourceSubviewOp) {
      auto fusedLoc =
          rewriter.getFusedLoc({sourceSubviewOp.getLoc(), op.getLoc()});
      auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
          fusedLoc, sourceSubviewOp.source_offset(), op.source_offset());
      rewriter.updateRootInPlace(op, [&]() {
        op.sourceMutable().assign(sourceSubviewOp.source());
        op.source_sizeMutable().assign(sourceSubviewOp.source_size());
        op.source_offsetMutable().assign(newOffset);
      });
    }
    if (targetSubviewOp) {
      auto fusedLoc =
          rewriter.getFusedLoc({targetSubviewOp.getLoc(), op.getLoc()});
      auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
          fusedLoc, targetSubviewOp.source_offset(), op.target_offset());
      rewriter.updateRootInPlace(op, [&]() {
        op.targetMutable().assign(targetSubviewOp.source());
        op.target_sizeMutable().assign(targetSubviewOp.source_size());
        op.target_offsetMutable().assign(newOffset);
      });
    }
    return success();
  }
};

}  // namespace

void CmdCopyOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  results.insert<FoldSubviewsIntoCmdCopyOp>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.dispatch
//===----------------------------------------------------------------------===//

namespace {

struct FoldCmdSubviewsIntoCmdDispatchOp
    : public OpRewritePattern<CmdDispatchOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdDispatchOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<ResourceSubviewOp> resourceSubviewOps;
    resourceSubviewOps.reserve(op.resources().size());
    bool anySubviewOps = false;
    for (auto operand : op.resources()) {
      auto subviewOp = ResourceSubviewOp::findSubviewOp(operand);
      if (subviewOp) anySubviewOps = true;
      resourceSubviewOps.push_back(subviewOp);
    }
    if (!anySubviewOps) return failure();
    rewriter.startRootUpdate(op);

    setInsertionPointToParentExecutionScope(op, rewriter);
    for (auto it : llvm::enumerate(resourceSubviewOps)) {
      unsigned resourceIdx = static_cast<unsigned>(it.index());
      auto subviewOp = it.value();
      if (!subviewOp) continue;
      auto fusedLoc = rewriter.getFusedLoc({subviewOp.getLoc(), op.getLoc()});
      auto newOffset = rewriter.createOrFold<mlir::AddIOp>(
          fusedLoc, subviewOp.source_offset(),
          op.resource_offsets()[resourceIdx]);
      op.resourcesMutable().slice(resourceIdx, 1).assign(subviewOp.source());
      op.resource_sizesMutable()
          .slice(resourceIdx, 1)
          .assign(subviewOp.source_size());
      op.resource_offsetsMutable().slice(resourceIdx, 1).assign(newOffset);
    }

    rewriter.finalizeRootUpdate(op);
    return success();
  }
};

}  // namespace

void CmdDispatchOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<FoldCmdSubviewsIntoCmdDispatchOp>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.execute
//===----------------------------------------------------------------------===//

namespace {

struct ElideImmediateCmdExecuteWaits : public OpRewritePattern<CmdExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdExecuteOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<unsigned> elidedTimepoints;
    for (auto timepoint : llvm::enumerate(op.await_timepoints())) {
      if (isa_and_nonnull<TimepointImmediateOp>(
              timepoint.value().getDefiningOp())) {
        // Non-immediate (at least that we know of).
        elidedTimepoints.push_back(timepoint.index());
      }
    }
    if (elidedTimepoints.empty()) return failure();
    rewriter.updateRootInPlace(op, [&]() {
      for (unsigned idx : llvm::reverse(elidedTimepoints)) {
        op.await_timepointsMutable().erase(idx);
      }
    });
    return success();
  }
};

// TODO(benvanik): check for covering waits (A->B->C, C just needs B).
struct ElideDuplicateCmdExecuteWaits : public OpRewritePattern<CmdExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdExecuteOp op,
                                PatternRewriter &rewriter) const override {
    SetVector<Value> uniqueTimepoints;
    uniqueTimepoints.insert(op.await_timepoints().begin(),
                            op.await_timepoints().end());
    if (uniqueTimepoints.size() == op.await_timepoints().size()) {
      return failure();
    }
    rewriter.updateRootInPlace(op, [&]() {
      op.await_timepointsMutable().assign(uniqueTimepoints.takeVector());
    });
    return success();
  }
};

struct ChainCmdExecuteWaits : public OpRewritePattern<CmdExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdExecuteOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<std::pair<unsigned, Value>> replacements;
    for (auto operand : llvm::enumerate(op.operands())) {
      if (auto awaitOp = dyn_cast_or_null<TimepointAwaitOp>(
              operand.value().getDefiningOp())) {
        replacements.push_back(std::make_pair(
            operand.index(), awaitOp.getTiedResultOperand(operand.value())));
      }
    }
    if (replacements.empty()) return failure();
    rewriter.updateRootInPlace(op, [&]() {
      for (auto replacement : replacements) {
        op.operandsMutable()
            .slice(replacement.first, 1)
            .assign(replacement.second);
      }
    });
    return success();
  }
};

// If any operands are sourced from subviews clone those subviews into the
// region and rewrite the operands to point at the original resource. This
// allows us to progressively fold the subviews into the ops consuming them.
struct CloneCapturedCmdExecuteSubviewOps
    : public OpRewritePattern<CmdExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdExecuteOp op,
                                PatternRewriter &rewriter) const override {
    struct SubviewCapture {
      unsigned operandIdx;
      IREE::Stream::ResourceSubviewOp subviewOp;
    };
    SmallVector<SubviewCapture> captures;
    for (auto operand : llvm::enumerate(op.operands())) {
      auto subviewOp = ResourceSubviewOp::findSubviewOp(operand.value());
      if (!subviewOp) continue;
      captures.push_back(
          SubviewCapture{static_cast<unsigned>(operand.index()), subviewOp});
    }
    if (captures.empty()) return failure();
    rewriter.startRootUpdate(op);

    auto &entryBlock = op.body().front();
    rewriter.setInsertionPointToStart(&entryBlock);
    for (auto &capture : captures) {
      // Replace operand with the source subview resource.
      op.operandsMutable()
          .slice(capture.operandIdx, 1)
          .assign(capture.subviewOp.source());
      op.operand_sizesMutable()
          .slice(capture.operandIdx, 1)
          .assign(capture.subviewOp.source_size());

      // Clone the subview into the region and wire it up to take the same
      // range as the original.
      auto arg = entryBlock.getArgument(capture.operandIdx);
      auto newOp = rewriter.create<ResourceSubviewOp>(
          capture.subviewOp.getLoc(), arg, capture.subviewOp.source_size(),
          capture.subviewOp.source_offset(), capture.subviewOp.result_size());
      arg.replaceAllUsesExcept(newOp.result(), newOp);
    }

    rewriter.finalizeRootUpdate(op);
    return success();
  }
};

// Elides stream.cmd.execute ops when they have no meaningful work.
// The returned timepoint is replaced with an immediately resolved timepoint.
struct ElideNoOpCmdExecuteOp : public OpRewritePattern<CmdExecuteOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CmdExecuteOp op,
                                PatternRewriter &rewriter) const override {
    auto &entryBlock = op.body().front();
    if (entryBlock.getOperations().size() != 1) {
      // Has non-yield ops.
      return failure();
    }
    auto yieldOp = cast<YieldOp>(entryBlock.getTerminator());
    if (!yieldOp.operands().empty()) {
      return rewriter.notifyMatchFailure(
          op, "no ops in execute region but still passing through operands");
    }
    rewriter.replaceOpWithNewOp<TimepointImmediateOp>(
        op, op.result_timepoint().getType());
    return success();
  }
};

}  // namespace

void CmdExecuteOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<ElideImmediateCmdExecuteWaits>(context);
  results.insert<ElideDuplicateCmdExecuteWaits>(context);
  results.insert<ChainCmdExecuteWaits>(context);
  results.insert<CloneCapturedCmdExecuteSubviewOps>(context);
  results.insert<ElideNoOpCmdExecuteOp>(context);
  results.insert<IREE::Util::ClosureOptimizationPattern<CmdExecuteOp>>(context);
  results.insert<ElideUnusedOp<CmdExecuteOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.serial
//===----------------------------------------------------------------------===//

namespace {

// Elides a region-carrying op when the region is empty.
// Requires no results that need replacement.
template <typename OpT>
struct ElideEmptyRegionOp : public OpRewritePattern<OpT> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(OpT op,
                                PatternRewriter &rewriter) const override {
    auto &entryBlock = op.body().front();
    if (entryBlock.getOperations().size() != 1) {
      // Has non-yield ops.
      return failure();
    }
    auto yieldOp = cast<YieldOp>(entryBlock.getTerminator());
    if (!yieldOp.operands().empty()) {
      return rewriter.notifyMatchFailure(
          op, "no ops in execution region but still passing through operands");
    }
    rewriter.eraseOp(op);
    return success();
  }
};

}  // namespace

void CmdSerialOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.insert<ElideEmptyRegionOp<CmdSerialOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.cmd.concurrent
//===----------------------------------------------------------------------===//

void CmdConcurrentOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<ElideEmptyRegionOp<CmdConcurrentOp>>(context);
}

//===----------------------------------------------------------------------===//
// stream.timepoint.immediate
//===----------------------------------------------------------------------===//

OpFoldResult TimepointImmediateOp::fold(ArrayRef<Attribute> operands) {
  return IREE::Stream::TimepointAttr::get(getContext(), getResult().getType());
}

//===----------------------------------------------------------------------===//
// stream.timepoint.join
//===----------------------------------------------------------------------===//

OpFoldResult TimepointJoinOp::fold(ArrayRef<Attribute> operands) {
  if (llvm::all_of(operands, [](auto operand) { return operand != nullptr; })) {
    // Immediate wait; fold into immediate.
    return IREE::Stream::TimepointAttr::get(getContext(),
                                            getResult().getType());
  } else if (timepoints().size() == 1) {
    // Join of a single timepoint => that timepoint.
    return timepoints().front();
  }
  return {};
}

namespace {

struct ElideImmediateTimepointJoinOperands
    : public OpRewritePattern<TimepointJoinOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TimepointJoinOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value> newTimepoints;
    newTimepoints.reserve(op.timepoints().size());
    for (auto timepoint : op.timepoints()) {
      if (!isa_and_nonnull<TimepointImmediateOp>(timepoint.getDefiningOp())) {
        newTimepoints.push_back(timepoint);
      }
    }
    if (newTimepoints.size() == op.timepoints().size()) return failure();
    if (newTimepoints.empty()) {
      // Fully immediate; replace entire join with immediate.
      rewriter.replaceOpWithNewOp<TimepointImmediateOp>(op,
                                                        op.result().getType());
    } else {
      rewriter.updateRootInPlace(
          op, [&]() { op.timepointsMutable().assign(newTimepoints); });
    }
    return success();
  }
};

struct FoldDuplicateTimepointJoinOperands
    : public OpRewritePattern<TimepointJoinOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TimepointJoinOp op,
                                PatternRewriter &rewriter) const override {
    SetVector<Value> newTimepoints;
    newTimepoints.insert(op.timepoints().begin(), op.timepoints().end());
    if (newTimepoints.size() == op.timepoints().size()) return failure();
    rewriter.updateRootInPlace(op, [&]() {
      op.timepointsMutable().assign(newTimepoints.takeVector());
    });
    return success();
  }
};

}  // namespace

void TimepointJoinOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): elide operands if timepoint must be satisfied in use-def.
  // TODO(benvanik): sink and pull in other timepoints (join on all needed).
  results.insert<ElideImmediateTimepointJoinOperands>(context);
  results.insert<FoldDuplicateTimepointJoinOperands>(context);
}

//===----------------------------------------------------------------------===//
// stream.timepoint.await
//===----------------------------------------------------------------------===//

LogicalResult TimepointAwaitOp::fold(ArrayRef<Attribute> foldOperands,
                                     SmallVectorImpl<OpFoldResult> &results) {
  if (foldOperands[0]) {
    // Immediate wait; fold to all captured operands.
    results.append(operands().begin(), operands().end());
    return success();
  }
  return failure();
}

namespace {

struct ElideImmediateAwaits : public OpRewritePattern<TimepointAwaitOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TimepointAwaitOp op,
                                PatternRewriter &rewriter) const override {
    if (isa_and_nonnull<TimepointImmediateOp>(op.timepoint().getDefiningOp())) {
      rewriter.replaceOp(op, op.operands());
      return success();
    }
    return failure();
  }
};

// Sinks an await down to the first consumer of any resource. Note that there
// may be multiple resources guarded by the await.
struct SinkAwaitToFirstConsumer : public OpRewritePattern<TimepointAwaitOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TimepointAwaitOp op,
                                PatternRewriter &rewriter) const override {
    // TODO(benvanik): amortize this dominance calculation.
    DominanceInfo domInfo(op->getParentOp());

    // Gather all direct users of the awaited resources and find the common
    // dominator block across all uses. This may be the entry block itself.
    SetVector<Operation *> allUsers;
    Block *commonDominator = nullptr;
    for (auto result : op.results()) {
      for (auto &use : result.getUses()) {
        if (allUsers.insert(use.getOwner())) {
          auto *userBlock = use.getOwner()->getBlock();
          commonDominator = commonDominator
                                ? domInfo.findNearestCommonDominator(
                                      commonDominator, userBlock)
                                : userBlock;
        }
      }
    }
    if (!commonDominator) return failure();

    // Find the first use within the dominator block (if any) so that we
    // can sink down to it.
    Operation *firstUserInDominator = commonDominator->getTerminator();
    for (auto *user : allUsers) {
      if (user->getBlock() == commonDominator) {
        if (user->isBeforeInBlock(firstUserInDominator)) {
          firstUserInDominator = user;
        }
      }
    }

    // Find the earliest point before |user| that is safe to insert into. If it
    // ends up being where we already are then no-op.
    auto ip = findInsertionPointBefore(op, firstUserInDominator);
    if (ip == Block::iterator(op)) return failure();

    rewriter.updateRootInPlace(op,
                               [&]() { op->moveBefore(ip->getBlock(), ip); });
    return success();
  }
};

// Moves stream.resource.subview ops across to results of an await.
// This allows us to pass-through the subviews to consumers that can hopefully
// fold the range.
struct SinkSubviewsAcrossAwaits : public OpRewritePattern<TimepointAwaitOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TimepointAwaitOp op,
                                PatternRewriter &rewriter) const override {
    rewriter.startRootUpdate(op);
    bool didChange = false;
    for (auto operand : llvm::enumerate(op.operands())) {
      auto subviewOp = dyn_cast_or_null<IREE::Stream::ResourceSubviewOp>(
          operand.value().getDefiningOp());
      if (!subviewOp) continue;
      didChange = true;
      unsigned operandIdx = static_cast<unsigned>(operand.index());

      // Create a new subview op matching the original on our result and swap
      // users to it.
      auto result = op.results()[operandIdx];
      auto newOp = rewriter.create<IREE::Stream::ResourceSubviewOp>(
          subviewOp.getLoc(), result, subviewOp.source_size(),
          subviewOp.source_offset(), subviewOp.result_size());
      result.replaceAllUsesExcept(newOp.result(), newOp);

      // Update our bound size to the subview source size (not the subrange).
      op.operand_sizesMutable()
          .slice(operandIdx, 1)
          .assign(subviewOp.source_size());

      // Replace our resource usage with the source of the subview op.
      op.operandsMutable().slice(operandIdx, 1).assign(subviewOp.source());
    }
    if (didChange) {
      rewriter.finalizeRootUpdate(op);
      return success();
    } else {
      rewriter.cancelRootUpdate(op);
      return failure();
    }
  }
};

// Finds timepoint awaits on the same timepoint within the same domination
// paths and groups them together.
//
// Example:
//  %6 = stream.timepoint.await %tp -> %3 : !stream.resource<external>{%c4000}
//  %7 = stream.tensor.export %6 ...
//  %8 = stream.timepoint.await %tp -> %4 : !stream.resource<external>{%c4000}
//  %9 = stream.tensor.export %8 ...
// ->
//  %6:2 = stream.timepoint.await %tp -> %3, %4 :
//      !stream.resource<external>{%c4000}, !stream.resource<external>{%c4000}
//  %7 = stream.tensor.export %6#0 ...
//  %9 = stream.tensor.export %6#1 ...
struct GroupAwaitsByTimepoint : public OpRewritePattern<TimepointAwaitOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TimepointAwaitOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<TimepointAwaitOp> coveredOps;
    for (auto &use : op.timepoint().getUses()) {
      // TODO(benvanik): make this handle joins/ties; today we get blocked
      // there. We rely on other canonicalizers to sink things such that
      // (hopefully) we get them directly accessible here.
      if (use.getOwner() == op) continue;
      if (use.getOwner()->getBlock() != op->getBlock() ||
          use.getOwner()->isBeforeInBlock(op)) {
        // TODO(benvanik): allow dominated blocks.
        continue;
      }
      auto awaitOp = dyn_cast<TimepointAwaitOp>(use.getOwner());
      if (!awaitOp ||
          !AffinityAttr::areCompatible(
              op.affinityAttr().dyn_cast_or_null<AffinityAttr>(),
              awaitOp.affinityAttr().dyn_cast_or_null<AffinityAttr>())) {
        // Can't combine if the affinities differ as the wait semantics are
        // load-bearing. Probably. They really shouldn't be.
        // TODO(benvanik): remove affinity from stream.timepoint.await.
        continue;
      }
      coveredOps.push_back(awaitOp);
    }
    if (coveredOps.empty()) return failure();

    // Combine all awaits into a single one.
    SmallVector<Value> newOperands;
    SmallVector<Value> newOperandSizes;
    llvm::append_range(newOperands, op.operands());
    llvm::append_range(newOperandSizes, op.operand_sizes());
    for (auto coveredOp : coveredOps) {
      llvm::append_range(newOperands, coveredOp.operands());
      llvm::append_range(newOperandSizes, coveredOp.operand_sizes());
    }
    auto newOp = rewriter.create<TimepointAwaitOp>(
        op.getLoc(), newOperands, newOperandSizes, op.timepoint());
    if (op.affinity().hasValue()) {
      newOp.affinityAttr(op.affinityAttr());
    }

    // Replace covered ops with the new results.
    unsigned resultIdx = 0;
    for (auto result : op.results()) {
      result.replaceAllUsesWith(newOp.results()[resultIdx++]);
    }
    for (auto coveredOp : coveredOps) {
      for (auto result : coveredOp.results()) {
        result.replaceAllUsesWith(newOp.results()[resultIdx++]);
      }
      rewriter.eraseOp(coveredOp);
    }

    op.erase();
    return success();
  }
};

// Folds duplicate resources passing through an await op.
//
// Example:
//  %1:4 = stream.timepoint.await %tp -> %1, %1, %2, %2
// ->
//  %1:2 = stream.timepoint.await %tp -> %1, %2
struct FoldDuplicateAwaitResources : public OpRewritePattern<TimepointAwaitOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TimepointAwaitOp op,
                                PatternRewriter &rewriter) const override {
    DenseMap<Value, unsigned> baseMap;
    SmallVector<std::pair<Value, unsigned>> replacements;
    SmallVector<Value> newOperands;
    SmallVector<Value> newOperandSizes;
    for (auto it : llvm::zip(op.operands(), op.operand_sizes(), op.results())) {
      auto operand = std::get<0>(it);
      auto operandSize = std::get<1>(it);
      auto result = std::get<2>(it);
      auto insertion =
          baseMap.insert(std::make_pair(operand, newOperands.size()));
      if (insertion.second) {
        // Inserted as a new unique operand.
        newOperands.push_back(operand);
        newOperandSizes.push_back(operandSize);
      }
      unsigned resultIdx = insertion.first->second;
      replacements.push_back(std::make_pair(result, resultIdx));
    }
    if (newOperands.size() == op.operands().size()) {
      return failure();  // No change.
    }

    // Create replacement op with deduped operands/results.
    auto newOp = rewriter.create<IREE::Stream::TimepointAwaitOp>(
        op.getLoc(), newOperands, newOperandSizes, op.timepoint());
    if (op.affinity().hasValue()) {
      newOp.affinityAttr(op.affinityAttr());
    }

    // Replace all duplicate results with the base results.
    for (auto &replacement : replacements) {
      auto oldResult = replacement.first;
      auto newResult = newOp.results()[replacement.second];
      oldResult.replaceAllUsesWith(newResult);
    }
    rewriter.eraseOp(op);
    return success();
  }
};

}  // namespace

void TimepointAwaitOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // TODO(benvanik): elide waits if timepoint must be satisfied in use-def.
  results.insert<ElideImmediateAwaits>(context);
  results.insert<SinkAwaitToFirstConsumer>(context);
  results.insert<SinkSubviewsAcrossAwaits>(context);
  results.insert<GroupAwaitsByTimepoint>(context);
  results.insert<FoldDuplicateAwaitResources>(context);
  results.insert<ElideUnusedOp<TimepointAwaitOp>>(context);
}

}  // namespace Stream
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir