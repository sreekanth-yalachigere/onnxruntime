#pragma once

#include "core/framework/op_kernel.h"
#include "core/providers/cuda/cudnn_common.h"

namespace Lotus {
namespace Cuda {

class CudnnLRNDescriptor final {
 public:
  CudnnLRNDescriptor();
  ~CudnnLRNDescriptor();
  Status Set(uint32_t N, double alpha, double beta, double K);
  operator cudnnLRNDescriptor_t() const { return desc_; }

 private:
  cudnnLRNDescriptor_t desc_;
};

template <typename T>
class LRN : public CudaKernel {
 public:
  LRN(const OpKernelInfo& info);
  Status ComputeInternal(OpKernelContext* p_op_kernel_context) const override;

 private:
  CudnnLRNDescriptor norm_desc_;
};

}  // namespace Cuda
}  // namespace Lotus
