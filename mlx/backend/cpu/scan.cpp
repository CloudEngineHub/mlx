// Copyright © 2023 Apple Inc.

#include <cassert>

#include "mlx/backend/common/utils.h"
#include "mlx/backend/cpu/binary_ops.h"
#include "mlx/backend/cpu/copy.h"
#include "mlx/backend/cpu/encoder.h"
#include "mlx/backend/cpu/simd/simd.h"
#include "mlx/primitives.h"

namespace mlx::core {

namespace {

template <typename T, typename U, typename Op>
void contiguous_scan(
    const T* input,
    U* output,
    int count,
    int stride,
    bool reverse,
    bool inclusive,
    const Op& op,
    U init) {
  if (!reverse) {
    if (inclusive) {
      for (int i = 0; i < count; i++) {
        *output = *input;
        for (int j = 1; j < stride; j++) {
          input++;
          output++;
          *output = op(*(output - 1), *input);
        }
        output++;
        input++;
      }
    } else {
      for (int i = 0; i < count; i++) {
        *output = init;
        for (int j = 1; j < stride; j++) {
          *(output + 1) = op(*output, *input);
          input++;
          output++;
        }
        output++;
        input++;
      }
    }
  } else {
    if (inclusive) {
      for (int i = 0; i < count; i++) {
        output += stride - 1;
        input += stride - 1;
        *output = *input;
        for (int j = 1; j < stride; j++) {
          input--;
          output--;
          *output = op(*(output + 1), *input);
        }
        output += stride;
        input += stride;
      }
    } else {
      for (int i = 0; i < count; i++) {
        output += stride - 1;
        input += stride - 1;
        *output = init;
        for (int j = 1; j < stride; j++) {
          *(output - 1) = op(*output, *input);
          input--;
          output--;
        }
        output += stride;
        input += stride;
      }
    }
  }
};

template <typename T, typename U, typename Op>
void strided_scan(
    const T* input,
    U* output,
    int count,
    int size,
    int stride,
    bool reverse,
    bool inclusive,
    const Op& op,
    U init) {
  // TODO: Vectorize the following naive implementation
  if (!reverse) {
    if (inclusive) {
      for (int i = 0; i < count; i++) {
        std::copy(input, input + stride, output);
        output += stride;
        input += stride;
        for (int j = 1; j < size; j++) {
          for (int k = 0; k < stride; k++) {
            *output = op(*(output - stride), *input);
            output++;
            input++;
          }
        }
      }
    } else {
      for (int i = 0; i < count; i++) {
        std::fill(output, output + stride, init);
        output += stride;
        input += stride;
        for (int j = 1; j < size; j++) {
          for (int k = 0; k < stride; k++) {
            *output = op(*(output - stride), *(input - stride));
            output++;
            input++;
          }
        }
      }
    }
  } else {
    if (inclusive) {
      for (int i = 0; i < count; i++) {
        output += (size - 1) * stride;
        input += (size - 1) * stride;
        std::copy(input, input + stride, output);
        for (int j = 1; j < size; j++) {
          for (int k = 0; k < stride; k++) {
            output--;
            input--;
            *output = op(*(output + stride), *input);
          }
        }
        output += size * stride;
        input += size * stride;
      }
    } else {
      for (int i = 0; i < count; i++) {
        output += (size - 1) * stride;
        input += (size - 1) * stride;
        std::fill(output, output + stride, init);
        for (int j = 1; j < size; j++) {
          for (int k = 0; k < stride; k++) {
            output--;
            input--;
            *output = op(*(output + stride), *(input + stride));
          }
        }
        output += size * stride;
        input += size * stride;
      }
    }
  }
};

template <typename T, typename U, typename Op>
void scan_op(
    const array& in,
    array& out,
    int axis,
    bool reverse,
    bool inclusive,
    const Op& op,
    U init) {
  if (in.flags().row_contiguous) {
    if (in.strides()[axis] == 1) {
      contiguous_scan(
          in.data<T>(),
          out.data<U>(),
          in.size() / in.shape(axis),
          in.shape(axis),
          reverse,
          inclusive,
          op,
          init);
    } else {
      strided_scan(
          in.data<T>(),
          out.data<U>(),
          in.size() / in.shape(axis) / in.strides()[axis],
          in.shape(axis),
          in.strides()[axis],
          reverse,
          inclusive,
          op,
          init);
    }
  } else {
    throw std::runtime_error("Scan op supports only contiguous inputs");
  }
}

template <typename T, typename U>
void scan_dispatch(
    Scan::ReduceType rtype,
    const array& in,
    array& out,
    int axis,
    bool reverse,
    bool inclusive) {
  switch (rtype) {
    case Scan::Sum: {
      auto op = [](U y, T x) { return y + x; };
      auto init = static_cast<U>(0);
      scan_op<T, U>(in, out, axis, reverse, inclusive, op, init);
      break;
    }
    case Scan::Prod: {
      auto op = [](U y, T x) { return y * x; };
      auto init = static_cast<U>(1);
      scan_op<T, U>(in, out, axis, reverse, inclusive, op, init);
      break;
    }
    case Scan::Min: {
      auto op = [](U y, T x) { return x < y ? x : y; };
      auto init = (issubdtype(in.dtype(), floating))
          ? static_cast<U>(std::numeric_limits<float>::infinity())
          : std::numeric_limits<U>::max();
      scan_op<T, U>(in, out, axis, reverse, inclusive, op, init);
      break;
    }
    case Scan::Max: {
      auto op = [](U y, T x) { return x < y ? y : x; };
      auto init = (issubdtype(in.dtype(), floating))
          ? static_cast<U>(-std::numeric_limits<float>::infinity())
          : std::numeric_limits<U>::min();
      scan_op<T, U>(in, out, axis, reverse, inclusive, op, init);
      break;
    }
    case Scan::LogAddExp: {
      auto op = [](U a, T b) {
        return detail::LogAddExp{}(a, static_cast<U>(b));
      };
      auto init = (issubdtype(in.dtype(), floating))
          ? static_cast<U>(-std::numeric_limits<float>::infinity())
          : std::numeric_limits<U>::min();
      scan_op<T, U>(in, out, axis, reverse, inclusive, op, init);
      break;
    }
  }
}

} // namespace

void Scan::eval_cpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 1);

  auto& encoder = cpu::get_command_encoder(stream());

  // Ensure contiguity
  auto in = inputs[0];
  if (!in.flags().row_contiguous) {
    in = contiguous_copy_cpu(in, stream());
    encoder.add_temporary(in);
  }
  out.set_data(allocator::malloc(out.nbytes()));

  encoder.set_input_array(in);
  encoder.set_output_array(out);
  encoder.dispatch([in = array::unsafe_weak_copy(in),
                    out = array::unsafe_weak_copy(out),
                    axis_ = axis_,
                    reduce_type_ = reduce_type_,
                    reverse_ = reverse_,
                    inclusive_ = inclusive_]() mutable {
    switch (in.dtype()) {
      case bool_: {
        // We could do a full dtype x dtype switch but this is the only case
        // where we accumulate in a different type, for now.
        //
        // TODO: If we add the option to accumulate floats in higher precision
        //       floats perhaps we should add the full all-to-all dispatch.
        if (reduce_type_ == Scan::Sum && out.dtype() == int32) {
          scan_dispatch<bool, int32_t>(
              reduce_type_, in, out, axis_, reverse_, inclusive_);
        } else {
          scan_dispatch<bool, bool>(
              reduce_type_, in, out, axis_, reverse_, inclusive_);
        }
        break;
      }
      case uint8:
        scan_dispatch<uint8_t, uint8_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case uint16:
        scan_dispatch<uint16_t, uint16_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case uint32:
        scan_dispatch<uint32_t, uint32_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case uint64:
        scan_dispatch<uint64_t, uint64_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case int8:
        scan_dispatch<int8_t, int8_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case int16:
        scan_dispatch<int16_t, int16_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case int32:
        scan_dispatch<int32_t, int32_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case int64:
        scan_dispatch<int64_t, int64_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case float16:
        scan_dispatch<float16_t, float16_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case float32:
        scan_dispatch<float, float>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case float64:
        scan_dispatch<double, double>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case bfloat16:
        scan_dispatch<bfloat16_t, bfloat16_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
      case complex64:
        scan_dispatch<complex64_t, complex64_t>(
            reduce_type_, in, out, axis_, reverse_, inclusive_);
        break;
    }
  });
}

} // namespace mlx::core
