// Copyright © 2023-2024 Apple Inc.
#include <algorithm>
#include <cassert>
#include <numeric>
#include <sstream>

#include "mlx/backend/common/compiled.h"
#include "mlx/backend/common/slicing.h"
#include "mlx/backend/common/utils.h"
#include "mlx/backend/gpu/copy.h"
#include "mlx/backend/gpu/slicing.h"
#include "mlx/backend/metal/device.h"
#include "mlx/backend/metal/kernels.h"
#include "mlx/backend/metal/utils.h"
#include "mlx/primitives.h"
#include "mlx/scheduler.h"
#include "mlx/utils.h"

namespace mlx::core {

template <typename T>
void arange_set_scalars(T start, T next, metal::CommandEncoder& enc) {
  enc.set_bytes(start, 0);
  T step = next - start;
  enc.set_bytes(step, 1);
}

static array compute_dynamic_offset(
    const array& indices,
    const Strides& strides,
    const std::vector<int>& axes,
    Stream s) {
  auto& d = metal::device(s.device);

  // Kernel to compute offset here.
  array offset({1}, int64, nullptr, {});
  bool donate = indices.is_donatable() &&
      (indices.data_size() * indices.itemsize()) >= offset.itemsize();
  if (donate) {
    offset.copy_shared_buffer(indices);
  } else {
    offset.set_data(allocator::malloc(offset.itemsize()));
  }
  d.add_temporary(offset, s.index);

  auto dtype = indices.dtype();
  std::string lib_name = "compute_dynamic_offset_" + type_to_name(dtype);
  auto lib = d.get_library(lib_name, [dtype]() {
    return fmt::format(
        R"(
        [[kernel]] void compute_dynamic_offset_{0}(
            constant const {1}* indices [[buffer(0)]],
            device int64_t& offset [[buffer(1)]],
            constant const int64_t* strides [[buffer(2)]],
            constant const int* axes [[buffer(3)]],
            constant const int& n_axes [[buffer(4)]],
            uint index [[thread_position_in_grid]]) {{
          int64_t acc = 0;
          for (int i = 0; i < n_axes; ++i) {{
            acc += indices[i] * strides[axes[i]];
          }}
          offset = acc;
        }})",
        type_to_name(dtype),
        get_type_string(dtype));
  });
  auto kernel = d.get_kernel(lib_name, lib);

  auto& compute_encoder = d.get_command_encoder(s.index);
  compute_encoder.set_compute_pipeline_state(kernel);
  compute_encoder.set_input_array(indices, 0);
  compute_encoder.set_output_array(offset, 1);
  compute_encoder.set_vector_bytes(strides, 2);
  compute_encoder.set_vector_bytes(axes, 3);
  int n_axes = axes.size();
  compute_encoder.set_bytes(n_axes, 4);
  MTL::Size dims = MTL::Size(1, 1, 1);
  compute_encoder.dispatch_threads(dims, dims);
  return offset;
}

void Arange::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 0);
  out.set_data(allocator::malloc(out.nbytes()));
  if (out.size() == 0) {
    return;
  }
  auto& s = stream();
  auto& d = metal::device(s.device);
  auto kernel = get_arange_kernel(d, "arange" + type_to_name(out), out);
  size_t nthreads = out.size();
  MTL::Size grid_dims = MTL::Size(nthreads, 1, 1);
  MTL::Size group_dims = MTL::Size(
      std::min(nthreads, kernel->maxTotalThreadsPerThreadgroup()), 1, 1);
  auto& compute_encoder = d.get_command_encoder(s.index);
  compute_encoder.set_compute_pipeline_state(kernel);

  switch (out.dtype()) {
    case bool_: // unsupported
      throw std::runtime_error("[Arange::eval_gpu] Does not support bool");
    case uint8:
      arange_set_scalars<uint8_t>(start_, start_ + step_, compute_encoder);
      break;
    case uint16:
      arange_set_scalars<uint16_t>(start_, start_ + step_, compute_encoder);
      break;
    case uint32:
      arange_set_scalars<uint32_t>(start_, start_ + step_, compute_encoder);
      break;
    case uint64:
      arange_set_scalars<uint64_t>(start_, start_ + step_, compute_encoder);
      break;
    case int8:
      arange_set_scalars<int8_t>(start_, start_ + step_, compute_encoder);
      break;
    case int16:
      arange_set_scalars<int16_t>(start_, start_ + step_, compute_encoder);
      break;
    case int32:
      arange_set_scalars<int32_t>(start_, start_ + step_, compute_encoder);
      break;
    case int64:
      arange_set_scalars<int64_t>(start_, start_ + step_, compute_encoder);
      break;
    case float16:
      arange_set_scalars<float16_t>(start_, start_ + step_, compute_encoder);
      break;
    case float32:
      arange_set_scalars<float>(start_, start_ + step_, compute_encoder);
      break;
    case bfloat16:
      arange_set_scalars<bfloat16_t>(start_, start_ + step_, compute_encoder);
      break;
    default:
      throw std::runtime_error("[Arange::eval_gpu] Does not support type.");
  }

  compute_encoder.set_output_array(out, 2);
  compute_encoder.dispatch_threads(grid_dims, group_dims);
}

void ArgReduce::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 1);
  auto& in = inputs[0];
  out.set_data(allocator::malloc(out.nbytes()));
  auto& s = stream();
  auto& d = metal::device(s.device);
  std::string op_name;
  switch (reduce_type_) {
    case ArgReduce::ArgMin:
      op_name = "argmin_";
      break;
    case ArgReduce::ArgMax:
      op_name = "argmax_";
      break;
  }

  // Prepare the shapes, strides and axis arguments.
  auto in_strides = in.strides();
  auto shape = in.shape();
  auto out_strides = out.strides();
  auto axis_stride = in_strides[axis_];
  size_t axis_size = shape[axis_];
  if (out_strides.size() == in_strides.size()) {
    out_strides.erase(out_strides.begin() + axis_);
  }
  in_strides.erase(in_strides.begin() + axis_);
  shape.erase(shape.begin() + axis_);
  size_t ndim = shape.size();

  // ArgReduce
  int simd_size = 32;
  int n_reads = 4;
  auto& compute_encoder = d.get_command_encoder(s.index);
  {
    auto kernel = d.get_kernel(op_name + type_to_name(in));
    NS::UInteger thread_group_size = std::min(
        (axis_size + n_reads - 1) / n_reads,
        kernel->maxTotalThreadsPerThreadgroup());
    // round up to the closest number divisible by simd_size
    thread_group_size =
        (thread_group_size + simd_size - 1) / simd_size * simd_size;
    assert(thread_group_size <= kernel->maxTotalThreadsPerThreadgroup());

    auto gd = get_2d_grid_dims(out.shape(), out.strides());
    MTL::Size grid_dims = MTL::Size(thread_group_size, gd.width, gd.height);
    MTL::Size group_dims = MTL::Size(thread_group_size, 1, 1);
    compute_encoder.set_compute_pipeline_state(kernel);
    compute_encoder.set_input_array(in, 0);
    compute_encoder.set_output_array(out, 1);
    if (ndim == 0) {
      // Pass place holders so metal doesn't complain
      int shape_ = 0;
      int64_t stride_ = 0;
      compute_encoder.set_bytes(shape_, 2);
      compute_encoder.set_bytes(stride_, 3);
      compute_encoder.set_bytes(stride_, 4);
    } else {
      compute_encoder.set_vector_bytes(shape, 2);
      compute_encoder.set_vector_bytes(in_strides, 3);
      compute_encoder.set_vector_bytes(out_strides, 4);
    }
    compute_encoder.set_bytes(ndim, 5);
    compute_encoder.set_bytes(axis_stride, 6);
    compute_encoder.set_bytes(axis_size, 7);
    compute_encoder.dispatch_threads(grid_dims, group_dims);
  }
}

void Load::eval_gpu(const std::vector<array>& inputs, array& out) {
  throw std::runtime_error("[Load::eval_gpu] Not implemented.");
}

void RandomBits::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 1);

  // keys has shape (N1, ..., NK, 2)
  // out has shape (N1, ..., NK, M1, M2, ...)
  auto& keys = inputs[0];
  size_t num_keys = keys.size() / 2;

  size_t elems_per_key = out.size() / num_keys;
  size_t bytes_per_key = out.itemsize() * elems_per_key;
  out.set_data(allocator::malloc(out.nbytes()));
  if (out.size() == 0) {
    return;
  }

  size_t out_per_key = (bytes_per_key + 4 - 1) / 4;
  size_t half_size = out_per_key / 2;
  bool odd = out_per_key % 2;

  auto& s = stream();
  auto& d = metal::device(s.device);
  std::string kname = keys.flags().row_contiguous ? "rbitsc" : "rbits";
  auto kernel = d.get_kernel(kname);

  // organize into grid nkeys x elem_per_key
  MTL::Size grid_dims = MTL::Size(num_keys, half_size + odd, 1);
  NS::UInteger thread_group_size = kernel->maxTotalThreadsPerThreadgroup();
  auto group_dims = get_block_dims(num_keys, half_size + odd, 1);
  auto& compute_encoder = d.get_command_encoder(s.index);
  compute_encoder.set_compute_pipeline_state(kernel);
  compute_encoder.set_input_array(keys, 0);
  compute_encoder.set_output_array(out, 1);
  compute_encoder.set_bytes(odd, 2);
  compute_encoder.set_bytes(bytes_per_key, 3);

  if (!keys.flags().row_contiguous) {
    int ndim = keys.ndim();
    compute_encoder.set_bytes(ndim, 4);
    compute_encoder.set_vector_bytes(keys.shape(), 5);
    compute_encoder.set_vector_bytes(keys.strides(), 6);
  }

  compute_encoder.dispatch_threads(grid_dims, group_dims);
}

void DynamicSlice::eval_gpu(const std::vector<array>& inputs, array& out) {
  if (out.size() == 0) {
    out.set_data(nullptr);
    return;
  }

  auto& in = inputs[0];
  auto& start = inputs[1];
  out.set_data(allocator::malloc(out.nbytes()));
  auto s = stream();
  auto in_offset = compute_dynamic_offset(start, in.strides(), axes_, s);
  copy_gpu_inplace(
      /* const array& src = */ in,
      /* array& dst = */ out,
      /* const Shape& data_shape = */ out.shape(),
      /* const Strides& i_strides = */ in.strides(),
      /* const Strides& o_strides = */ out.strides(),
      /* int64_t i_offset = */ 0,
      /* int64_t o_offset = */ 0,
      /* CopyType ctype = */ CopyType::GeneralGeneral,
      /* const Stream& s = */ s,
      /* const std::optional<array>& dynamic_i_offset = */ in_offset,
      /* const std::optional<array>& dynamic_o_offset = */ std::nullopt);
}

void DynamicSliceUpdate::eval_gpu(
    const std::vector<array>& inputs,
    array& out) {
  if (out.size() == 0) {
    out.set_data(nullptr);
    return;
  }

  auto& in = inputs[0];
  auto& upd = inputs[1];
  auto& start_indices = inputs[2];

  if (upd.size() == 0) {
    out.copy_shared_buffer(in);
    return;
  }

  // Copy or donate input to output
  auto s = stream();
  auto& d = metal::device(s.device);
  auto ctype = in.flags().contiguous && in.size() == in.data_size()
      ? CopyType::Vector
      : CopyType::General;
  copy_gpu(in, out, in.data_size() == 1 ? CopyType::Scalar : ctype, s);

  auto out_offset =
      compute_dynamic_offset(start_indices, out.strides(), axes_, s);
  copy_gpu_inplace(
      /* const array& src = */ upd,
      /* array& dst = */ out,
      /* const Shape& data_shape = */ upd.shape(),
      /* const Strides& i_strides = */ upd.strides(),
      /* const Strides& o_strides = */ out.strides(),
      /* int64_t i_offset = */ 0,
      /* int64_t o_offset = */ 0,
      /* CopyType ctype = */ CopyType::GeneralGeneral,
      /* const Stream& s = */ s,
      /* const std::optional<array>& dynamic_i_offset = */ std::nullopt,
      /* const std::optional<array>& dynamic_o_offset = */ out_offset);
}

void QRF::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  throw std::runtime_error("[QRF::eval_gpu] Metal QR factorization NYI.");
}

void SVD::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  throw std::runtime_error("[SVD::eval_gpu] Metal SVD NYI.");
}

void Inverse::eval_gpu(const std::vector<array>& inputs, array& output) {
  throw std::runtime_error("[Inverse::eval_gpu] Metal inversion NYI.");
}

void Cholesky::eval_gpu(const std::vector<array>& inputs, array& out) {
  throw std::runtime_error(
      "[Cholesky::eval_gpu] Metal Cholesky decomposition NYI.");
}

void Eig::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  throw std::runtime_error("[Eig::eval_gpu] Metal Eig NYI.");
}

void Eigh::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  throw std::runtime_error("[Eigh::eval_gpu] Metal Eigh NYI.");
}

void LUF::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  throw std::runtime_error("[LUF::eval_gpu] Metal LU factorization NYI.");
}

} // namespace mlx::core
