/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <executorch/kernels/optimized/cpu/binary_ops.h>
#include <executorch/kernels/optimized/vec/functional.h>
#include <executorch/kernels/optimized/vec/vec.h>
#include <executorch/kernels/portable/cpu/scalar_utils.h>
#include <executorch/kernels/portable/cpu/util/broadcast_util.h>
#include <executorch/runtime/core/exec_aten/util/tensor_util.h> // IWYU pragma: export
#include <executorch/runtime/kernel/kernel_includes.h>
#include <executorch/runtime/platform/assert.h>

namespace torch {
namespace executor {
namespace native {

using Tensor = executorch::aten::Tensor;
using ScalarType = executorch::aten::ScalarType;

namespace {

template <
    bool can_cast,
    typename CTYPE_A,
    typename CTYPE_B,
    typename CTYPE_IN,
    typename CTYPE_OUT>
struct MulInner;

template <
    typename CTYPE_A,
    typename CTYPE_B,
    typename CTYPE_IN,
    typename CTYPE_OUT>
struct MulInner<true, CTYPE_A, CTYPE_B, CTYPE_IN, CTYPE_OUT> {
  static void run(const Tensor& a, const Tensor& b, Tensor& out) {
    apply_binary_elementwise_fn<CTYPE_A, CTYPE_B, CTYPE_OUT>(
        // NOLINTNEXTLINE(facebook-hte-ConstantArgumentPassByValue)
        [](const CTYPE_A val_a, const CTYPE_B val_b) {
          CTYPE_IN a_casted = static_cast<CTYPE_IN>(val_a);
          CTYPE_IN b_casted = static_cast<CTYPE_IN>(val_b);
          CTYPE_IN value = a_casted * b_casted;

          return static_cast<CTYPE_OUT>(value);
        },
        a,
        b,
        out);
  }
};

struct ReportCanCastBug {
  static void run(const Tensor&, const Tensor&, Tensor&) {
    ET_DCHECK_MSG(false, "BUG: canCast should have been checked above");
  }
};

template <
    typename CTYPE_A,
    typename CTYPE_B,
    typename CTYPE_IN,
    typename CTYPE_OUT>
struct MulInner<false, CTYPE_A, CTYPE_B, CTYPE_IN, CTYPE_OUT>
    : public ReportCanCastBug {};

Tensor& handle_last_dim_broadcast(
    KernelRuntimeContext& ctx,
    const Tensor& a,
    const Tensor& b,
    Tensor& out,
    const ElementwiseOptimizedPath selected_optimized_path) {
  ScalarType out_type = out.scalar_type();
  const Tensor* lhs;
  const Tensor* rhs;
  if (selected_optimized_path ==
      ElementwiseOptimizedPath::kBroadcastLastDimReverseArguments) {
    lhs = &b;
    rhs = &a;
  } else {
    lhs = &a;
    rhs = &b;
  }
  auto error = resize_tensor(out, lhs->sizes());
  ET_KERNEL_CHECK_MSG(
      ctx,
      error == Error::Ok,
      InvalidArgument,
      out,
      "Failed to resize output tensor.");
  const size_t outer_size = getLeadingDims(out, out.dim() - 1);
  const auto broadcast_size = out.size(out.dim() - 1);
  ET_SWITCH_REALB_TYPES(out_type, ctx, "mul.out", CTYPE, [&]() {
    using Vec = executorch::vec::Vectorized<CTYPE>;
    executorch::vec::broadcasting_map_broadcast_last_dim<CTYPE>(
        [](Vec x, Vec y) { return x * y; },
        out.mutable_data_ptr<CTYPE>(),
        lhs->const_data_ptr<CTYPE>(),
        rhs->const_data_ptr<CTYPE>(),
        outer_size,
        broadcast_size);
  });
  return out;
}

Tensor& handle_broadcast_mul(
    KernelRuntimeContext& ctx,
    const Tensor& a,
    const Tensor& b,
    Tensor& out,
    const ElementwiseOptimizedPath selected_optimized_path) {
  if ((selected_optimized_path ==
       ElementwiseOptimizedPath::kBroadcastLastDim) ||
      (selected_optimized_path ==
       ElementwiseOptimizedPath::kBroadcastLastDimReverseArguments)) {
    return handle_last_dim_broadcast(ctx, a, b, out, selected_optimized_path);
  }

  ScalarType out_type = out.scalar_type();
  const Tensor* lhs;
  const Tensor* rhs;
  if ((selected_optimized_path ==
       ElementwiseOptimizedPath::kBroadcast2dBy1dReverseArguments) ||
      (selected_optimized_path ==
       ElementwiseOptimizedPath::kBroadcastNdByNdReverseArguments)) {
    lhs = &b;
    rhs = &a;
  } else {
    // Catch failure to update logic when adding new broadcasting possibility.
    ET_DCHECK(
        (selected_optimized_path ==
         ElementwiseOptimizedPath::kBroadcast2dBy1d) ||
        (selected_optimized_path ==
         ElementwiseOptimizedPath::kBroadcastNdByNd));
    lhs = &a;
    rhs = &b;
  }
  auto error = resize_tensor(out, lhs->sizes());
  ET_KERNEL_CHECK_MSG(
      ctx,
      error == Error::Ok,
      InvalidArgument,
      out,
      "Failed to resize output tensor.");
  int64_t outer_size = 1;
  int64_t broadcast_size;
  int64_t inner_size;
  if ((selected_optimized_path == ElementwiseOptimizedPath::kBroadcastNdByNd) ||
      (selected_optimized_path ==
       ElementwiseOptimizedPath::kBroadcastNdByNdReverseArguments)) {
    int32_t broadcast_dim = internal::get_broadcast_dim(*lhs, *rhs);
    int32_t broadcast_dim_lhs = lhs->dim() + broadcast_dim;
    auto normalized_tensor_size_lhs =
        get_normalized_tensor_size(*lhs, broadcast_dim_lhs);
    outer_size = normalized_tensor_size_lhs[0];
    broadcast_size = normalized_tensor_size_lhs[1];
    inner_size = normalized_tensor_size_lhs[2];
  } else {
    broadcast_size = lhs->sizes()[lhs->dim() - 2];
    inner_size = lhs->sizes()[lhs->dim() - 1];
  }
  ET_SWITCH_REALB_TYPES(out_type, ctx, "mul.out", CTYPE, [&]() {
    using Vec = executorch::vec::Vectorized<CTYPE>;
    executorch::vec::broadcasting_map_3d_and_unsqueezed_3d<CTYPE>(
        [](Vec x, Vec y) { return x * y; },
        out.mutable_data_ptr<CTYPE>(),
        lhs->const_data_ptr<CTYPE>(),
        rhs->const_data_ptr<CTYPE>(),
        outer_size,
        broadcast_size,
        inner_size);
  });
  return out;
}
} // namespace

Tensor& opt_mul_out(
    KernelRuntimeContext& ctx,
    const Tensor& a,
    const Tensor& b,
    Tensor& out) {
  (void)ctx;

  ScalarType a_type = a.scalar_type();
  ScalarType b_type = b.scalar_type();
  ScalarType out_type = out.scalar_type();

  if (b.numel() == 1) {
    if (a_type == b_type && a_type == out_type && a_type != ScalarType::Half &&
        a_type != ScalarType::BFloat16) {
      ET_KERNEL_CHECK(
          ctx,
          resize_to_broadcast_target_size(a, b, out) == Error::Ok,
          InvalidArgument,
          out);

      ET_SWITCH_REALB_TYPES(a_type, ctx, "mul.out", CTYPE, [&]() {
        ET_SWITCH_REALB_TYPES(b_type, ctx, "mul.out", CTYPE_B, [&]() {
          CTYPE_B b_val = *b.const_data_ptr<CTYPE_B>();
          CTYPE b_casted = static_cast<CTYPE>(b_val);

          using Vec = executorch::vec::Vectorized<CTYPE>;
          executorch::vec::map<CTYPE>(
              [b_casted](Vec x) { return x * Vec(b_casted); },
              out.mutable_data_ptr<CTYPE>(),
              a.const_data_ptr<CTYPE>(),
              out.numel());
        });
      });
      return out;
    }
  } else if (a.numel() == 1) {
    return opt_mul_out(ctx, b, a, out);
  }

  auto selected_optimized_path = select_optimized_path(a, b, out);
  if (selected_optimized_path == ElementwiseOptimizedPath::kTreatAs1d) {
    // Resize for dynamic shape
    auto error = resize_tensor(out, a.sizes());
    ET_KERNEL_CHECK_MSG(
        ctx,
        error == Error::Ok,
        InvalidArgument,
        out,
        "Failed to resize output tensor.");

    ET_SWITCH_REALB_TYPES(out_type, ctx, "mul.out", CTYPE, [&]() {
      using Vec = executorch::vec::Vectorized<CTYPE>;
      executorch::vec::map2<CTYPE>(
          [](Vec x, Vec y) { return x * y; },
          out.mutable_data_ptr<CTYPE>(),
          a.const_data_ptr<CTYPE>(),
          b.const_data_ptr<CTYPE>(),
          out.numel());
    });
  } else if (selected_optimized_path != ElementwiseOptimizedPath::kNone) {
    return handle_broadcast_mul(ctx, a, b, out, selected_optimized_path);
  } else {
    ScalarType common_type =
        promoteTypes(a_type, b_type, /*half_to_float*/ true);
    ET_KERNEL_CHECK(ctx, canCast(common_type, out_type), InvalidArgument, out);

    ET_KERNEL_CHECK(
        ctx,
        resize_to_broadcast_target_size(a, b, out) == Error::Ok,
        InvalidArgument,
        out);

    ET_SWITCH_REALHBBF16_TYPES(a_type, ctx, "mul.out", CTYPE_A, [&]() {
      ET_SWITCH_REALHBBF16_TYPES(b_type, ctx, "mul.out", CTYPE_B, [&]() {
        using CTYPE_IN = typename torch::executor::
            promote_types<CTYPE_A, CTYPE_B, /*half_to_float*/ true>::type;
        ET_DCHECK(CppTypeToScalarType<CTYPE_IN>::value == common_type);
        ET_SWITCH_REALHBBF16_TYPES(out_type, ctx, "mul.out", CTYPE_OUT, [&]() {
          apply_binary_elementwise_fn<CTYPE_A, CTYPE_B, CTYPE_OUT>(
              [](const CTYPE_A val_a, const CTYPE_B val_b) {
                CTYPE_IN a_casted = static_cast<CTYPE_IN>(val_a);
                CTYPE_IN b_casted = static_cast<CTYPE_IN>(val_b);
                CTYPE_IN value = a_casted * b_casted;

                return static_cast<CTYPE_OUT>(value);
              },
              a,
              b,
              out);
        });
      });
    });
  }

  return out;
}

Tensor& opt_mul_scalar_out(
    KernelRuntimeContext& ctx,
    const Tensor& a,
    const Scalar& b,
    Tensor& out) {
  (void)ctx;

  ScalarType a_type = a.scalar_type();
  ScalarType b_type = utils::get_scalar_dtype(b);
  ScalarType common_type =
      utils::promote_type_with_scalar(a_type, b, /*half_to_float*/ false);
  ScalarType out_type = out.scalar_type();

  ET_CHECK(common_type == out_type);

  if (common_type == ScalarType::Half || common_type == ScalarType::BFloat16) {
    common_type = ScalarType::Float;
  }

  // Resize for dynamic shape
  auto error = resize_tensor(out, a.sizes());
  ET_CHECK_MSG(error == Error::Ok, "Failed to resize output tensor.");

  if (a_type == common_type && a_type == out_type &&
      a_type != ScalarType::Half && a_type != ScalarType::BFloat16) {
    ET_SWITCH_REALB_TYPES(a_type, ctx, "mul.Scalar_out", CTYPE, [&]() {
      ET_SWITCH_SCALAR_OBJ_TYPES(b_type, ctx, "mul.Scalar_out", CTYPE_B, [&]() {
        CTYPE_B b_val;
        ET_EXTRACT_SCALAR(b, b_val);
        CTYPE b_casted = static_cast<CTYPE>(b_val);

        using Vec = executorch::vec::Vectorized<CTYPE>;
        executorch::vec::map<CTYPE>(
            [b_casted](Vec x) { return x * Vec(b_casted); },
            out.mutable_data_ptr<CTYPE>(),
            a.const_data_ptr<CTYPE>(),
            out.numel());
      });
    });
  } else {
    ET_SWITCH_REALHBBF16_TYPES(a_type, ctx, "mul.Scalar_out", CTYPE_A, [&]() {
      ET_SWITCH_SCALAR_OBJ_TYPES(b_type, ctx, "mul.Scalar_out", CTYPE_B, [&]() {
        ET_SWITCH_REALB_TYPES(
            common_type, ctx, "mul.Scalar_out", CTYPE_IN, [&]() {
              ET_SWITCH_REALHBBF16_TYPES(
                  out_type, ctx, "mul.Scalar_out", CTYPE_OUT, [&]() {
                    CTYPE_B b_val;
                    ET_EXTRACT_SCALAR(b, b_val);
                    CTYPE_IN b_casted = static_cast<CTYPE_IN>(b_val);

                    const size_t n = a.numel();
                    const CTYPE_A* a_data = a.const_data_ptr<CTYPE_A>();
                    CTYPE_OUT* out_data = out.mutable_data_ptr<CTYPE_OUT>();
                    for (auto i = 0; i < n; ++i) {
                      out_data[i] = static_cast<CTYPE_OUT>(
                          static_cast<CTYPE_IN>(a_data[i]) * b_casted);
                    }
                  });
            });
      });
    });
  }

  return out;
}

} // namespace native
} // namespace executor
} // namespace torch
