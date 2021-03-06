// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cuda/math/clip_impl.h"
#include "core/providers/cuda/cu_inc/common.cuh"

namespace onnxruntime {
namespace cuda {
template <typename T>
__global__ void _Clip(const T* input, T* output, T min, T max, size_t N) {
  CALCULATE_ELEMENTWISE_INDEX_OR_EXIT(id, N);
  output[id] = (input[id] < min) ? min : ((input[id] > max) ? max : input[id]);
}

template <typename T>
void ClipImpl(const T* input_data, T* output_data, T min, T max, size_t count) {
  typedef typename ToCudaType<T>::MappedType CudaT;

  int blocksPerGrid = (int)(ceil(static_cast<float>(count) / GridDim::maxThreadsPerBlock));
  _Clip<CudaT><<<blocksPerGrid, GridDim::maxThreadsPerBlock, 0>>>(reinterpret_cast<const CudaT*>(input_data),
                                                                  reinterpret_cast<CudaT*>(output_data),
                                                                  *reinterpret_cast<CudaT*>(&min),
                                                                  *reinterpret_cast<CudaT*>(&max),
                                                                  count);
}

template void ClipImpl<float>(const float* input_data, float* output_data, float min, float max, size_t count);
template void ClipImpl<double>(const double* input_data, double* output_data, double min, double max, size_t count);
template void ClipImpl<MLFloat16>(const MLFloat16* input_data, MLFloat16* output_data, MLFloat16 min, MLFloat16 max, size_t count);

}  // namespace cuda
}  // namespace onnxruntime
