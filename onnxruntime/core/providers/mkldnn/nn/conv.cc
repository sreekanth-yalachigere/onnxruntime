// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef _WIN32
#pragma warning(disable : 4244)
#endif
#include <thread>
#include <mutex>

#include "core/providers/mkldnn/nn/conv.h"
#include "core/providers/mkldnn/mkldnn_common.h"
#include "core/providers/mkldnn/mkldnn_fwd.h"

namespace onnxruntime {
namespace mkl_dnn {

namespace {
// Struct which encapsulates parameters for MKLDNN Conv primitive.
struct ConvParams {
  const mkldnn::memory::dims& src_dims;
  const mkldnn::memory::dims& filter_dims;
  const mkldnn::memory::dims& bias_dims;
  const mkldnn::memory::dims& dst_dims;
  const mkldnn::memory::dims& strides;
  const mkldnn::memory::dims& dilations;
  const mkldnn::memory::dims& padding_left;
  const mkldnn::memory::dims& padding_right;

  ConvParams(const mkldnn::memory::dims& src_dims, const mkldnn::memory::dims& filter_dims,
             const mkldnn::memory::dims& bias_dims, const mkldnn::memory::dims& dst_dims,
             const mkldnn::memory::dims& strides, const mkldnn::memory::dims& dilations,
             const mkldnn::memory::dims& padding_left, const mkldnn::memory::dims& padding_right)
      : src_dims(src_dims),
        filter_dims(filter_dims),
        bias_dims(bias_dims),
        dst_dims(dst_dims),
        strides(strides),
        dilations(dilations),
        padding_left(padding_left),
        padding_right(padding_right) {}

  // Used as the key for Conv Primitive Reuse Pool.
  std::string ToString() const {
    std::string key;
    key.reserve(128);
    key.append("conv_");
    AddDimsToKey(key, src_dims);
    AddDimsToKey(key, filter_dims);
    AddDimsToKey(key, bias_dims);
    AddDimsToKey(key, dst_dims);
    AddDimsToKey(key, strides);
    AddDimsToKey(key, dilations);
    AddDimsToKey(key, padding_left);
    AddDimsToKey(key, padding_right);
    return key;
  }
};

static mkldnn::engine& GetGpuEngine() {
  static mkldnn::engine engine = mkldnn::engine(mkldnn::engine::kind::gpu, 0);
  return engine;
}

template <typename T>
class ConvPrimitive : public PrimitiveBase {
 public:
  explicit ConvPrimitive(const ConvParams& params)
      : cpu_engine_(GetEngine()), gpu_engine_(GetGpuEngine()) {
    context_.stream.reset(new mkldnn::stream(gpu_engine_));
    if (context_.conv_fwd == nullptr) {
      Initialize(params);
    }
  }

  ~ConvPrimitive() = default;

  void Compute(const T* src_data, const T* filter_data,
               T* dst_data, const T* bias_data = nullptr) {
    if (bias_data != nullptr) {
      context_.conv_fwd->execute(
          *context_.stream,
          {{MKLDNN_ARG_SRC, *context_.src_gmem},
           {MKLDNN_ARG_WEIGHTS, *context_.filter_gmem},
           {MKLDNN_ARG_BIAS, *context_.bias_gmem},
           {MKLDNN_ARG_DST, *context_.dst_gmem}});
    } else {
      context_.conv_fwd->execute(
          *context_.stream,
          {{MKLDNN_ARG_SRC, *context_.src_gmem},
           {MKLDNN_ARG_WEIGHTS, *context_.filter_gmem},
           {MKLDNN_ARG_DST, *context_.dst_gmem}});
    }

    context_.src_mem->set_data_handle(nullptr);
    context_.filter_mem->set_data_handle(nullptr);
    if (bias_data != nullptr) {
      context_.bias_mem->set_data_handle(nullptr);
    }
    context_.dst_mem->set_data_handle(nullptr);
    return;
  }

  mkldnn::memory::format_tag GetSrcMemoryformat_tag() const { return context_.src_fmt; }

  mkldnn::memory::format_tag GetFilterMemoryformat_tag() const { return context_.filter_fmt; }

  mkldnn::memory::format_tag GetDstMemoryformat_tag() const { return context_.dst_fmt; }

  size_t GetSrcSize() const { return context_.src_size; }

  size_t GetFilterSize() const { return context_.filter_size; }

  size_t GetDstSize() const { return context_.dst_size; }

  mkldnn::convolution_forward::primitive_desc* GetPrimitiveDesc() const {
    return context_.conv_fwd_pd.get();
  }

  mkldnn::memory* GetSrcGmem() const {
    return context_.src_gmem.get();
  }

  mkldnn::memory* GetDstGmem() const {
    return context_.dst_gmem.get();
  }

  mkldnn::memory* GetWeightGmem() const {
    return context_.filter_gmem.get();
  }

  mkldnn::memory* GetBiasGmem() const {
    return context_.bias_gmem.get();
  }

 private:
  struct ConvContext {
    mkldnn::memory::format_tag src_fmt;
    mkldnn::memory::format_tag filter_fmt;
    mkldnn::memory::format_tag dst_fmt;

    size_t src_size;
    size_t filter_size;
    size_t dst_size;

    std::unique_ptr<mkldnn::memory> src_mem;
    std::unique_ptr<mkldnn::memory> filter_mem;
    std::unique_ptr<mkldnn::memory> bias_mem;
    std::unique_ptr<mkldnn::memory> dst_mem;

    std::unique_ptr<mkldnn::memory> src_gmem;
    std::unique_ptr<mkldnn::memory> filter_gmem;
    std::unique_ptr<mkldnn::memory> bias_gmem;
    std::unique_ptr<mkldnn::memory> dst_gmem;

    std::unique_ptr<mkldnn::convolution_forward::desc> fwd_desc;

    std::unique_ptr<mkldnn::memory::desc> src_md;
    std::unique_ptr<mkldnn::memory::desc> filter_md;
    std::unique_ptr<mkldnn::memory::desc> bias_md;
    std::unique_ptr<mkldnn::memory::desc> dst_md;

    std::unique_ptr<mkldnn::convolution_forward::primitive_desc> conv_fwd_pd;
    std::unique_ptr<mkldnn::primitive> conv_fwd;

    std::unique_ptr<mkldnn::stream> stream;
    std::unique_ptr<mkldnn::stream> gstream;

    ConvContext()
        : src_fmt(mkldnn::memory::format_tag::any),
          filter_fmt(mkldnn::memory::format_tag::any),
          dst_fmt(mkldnn::memory::format_tag::any),
          src_size(0),
          filter_size(0),
          dst_size(0),
          src_mem(nullptr),
          filter_mem(nullptr),
          bias_mem(nullptr),
          dst_mem(nullptr),
          fwd_desc(nullptr),
          src_md(nullptr),
          filter_md(nullptr),
          bias_md(nullptr),
          conv_fwd_pd(nullptr),
          conv_fwd(nullptr),
          stream(nullptr) {}
  };

  void Initialize(const ConvParams& params) {
    // Set the memory descriptors to format_tag::any to allow MKLDNN to decide what the optimal memory layout should be
    // for the computation given the input params.
    context_.src_md.reset(new mkldnn::memory::desc(
        {params.src_dims}, MklDnnType<T>(), mkldnn::memory::format_tag::any));
    context_.filter_md.reset(new mkldnn::memory::desc(
        {params.filter_dims}, MklDnnType<T>(), mkldnn::memory::format_tag::any));
    context_.dst_md.reset(new mkldnn::memory::desc(
        {params.dst_dims}, MklDnnType<T>(), mkldnn::memory::format_tag::any));
    if (!params.bias_dims.empty())
      context_.bias_md.reset(new mkldnn::memory::desc(
          {params.bias_dims}, MklDnnType<T>(), mkldnn::memory::format_tag::any));

    if (!params.bias_dims.empty()) {
      context_.fwd_desc.reset(new mkldnn::convolution_forward::desc(
          mkldnn::prop_kind::forward_inference, mkldnn::algorithm::convolution_direct, *context_.src_md,
          *context_.filter_md, *context_.bias_md, *context_.dst_md,
          params.strides, params.dilations, params.padding_left,
          params.padding_right));
    } else {
      context_.fwd_desc.reset(new mkldnn::convolution_forward::desc(
          mkldnn::prop_kind::forward_inference, mkldnn::algorithm::convolution_direct, *context_.src_md,
          *context_.filter_md, *context_.dst_md, params.strides,
          params.dilations, params.padding_left,
          params.padding_right));
    }

    context_.conv_fwd_pd.reset(new mkldnn::convolution_forward::primitive_desc(
        *context_.fwd_desc, gpu_engine_));

    //context_.filter_fmt = static_cast<mkldnn::memory::format_tag>(
    //    context_.conv_fwd_pd.get()->weights_desc().desc().data.format_tag);

    //context_.dst_fmt = static_cast<mkldnn::memory::format_tag>(
    //    context_.conv_fwd_pd.get()->dst_desc().desc().data.format_tag);

    context_.src_size = context_.conv_fwd_pd.get()->src_desc().get_size();

    context_.filter_size = context_.conv_fwd_pd.get()->weights_desc().get_size();

    context_.dst_size = context_.conv_fwd_pd.get()->dst_desc().get_size();

    context_.src_mem.reset(
        new mkldnn::memory(context_.conv_fwd_pd.get()->src_desc(), cpu_engine_, nullptr));
    context_.filter_mem.reset(
        new mkldnn::memory(context_.conv_fwd_pd.get()->weights_desc(), cpu_engine_, nullptr));
    context_.dst_mem.reset(
        new mkldnn::memory(context_.conv_fwd_pd.get()->dst_desc(), cpu_engine_, nullptr));

    context_.src_gmem.reset(
        new mkldnn::memory(context_.conv_fwd_pd.get()->src_desc(), gpu_engine_));
    context_.filter_gmem.reset(
        new mkldnn::memory(context_.conv_fwd_pd.get()->weights_desc(), gpu_engine_));
    context_.dst_gmem.reset(
        new mkldnn::memory(context_.conv_fwd_pd.get()->dst_desc(), gpu_engine_));

    if (!params.bias_dims.empty()) {
      context_.bias_mem.reset(
          new mkldnn::memory(context_.conv_fwd_pd.get()->bias_desc(), cpu_engine_, nullptr));
      context_.bias_gmem.reset(
          new mkldnn::memory(context_.conv_fwd_pd.get()->bias_desc(), gpu_engine_));

      context_.conv_fwd.reset(new mkldnn::convolution_forward(
          *context_.conv_fwd_pd));
    } else {
      context_.conv_fwd.reset(
          new mkldnn::convolution_forward(*context_.conv_fwd_pd));
    }
  }

  ConvContext context_;
  mkldnn::engine& cpu_engine_;
  mkldnn::engine& gpu_engine_;
};

// Pool which allows for reuse of MKLDNN Conv primitives which are expensive to instantiate.
// To address thread safety, the primitives are stored in a map on thread local storage.
template <typename T>
class ConvPrimitivePool : public PrimitivePool<T> {
 public:
  static ConvPrimitive<T>* Get(const ConvParams& params) {
    ConvPrimitive<T>* primitive = dynamic_cast<ConvPrimitive<T>*>(
        ConvPrimitivePool<T>::GetInstance().GetPrimitive(params.ToString()));

    if (primitive == nullptr) {
      auto conv_primitive = std::make_unique<ConvPrimitive<T>>(params);
      primitive = conv_primitive.get();
      ConvPrimitivePool<T>::GetInstance().SetPrimitive(params.ToString(), std::move(conv_primitive));
    }
    return primitive;
  }

 private:
  ConvPrimitivePool() = default;
  ~ConvPrimitivePool() = default;

  static ConvPrimitivePool& GetInstance() {
    static ConvPrimitivePool pool;
    return pool;
  }
};
}  // namespace

template <typename T>
Status Conv<T>::Compute(OpKernelContext* context) const {
  size_t num_inputs = OpKernel::Node().InputDefs().size();

  const Tensor* X = context->Input<Tensor>(0);
  const Tensor* W = context->Input<Tensor>(1);
  const Tensor* B = num_inputs == 3 ? context->Input<Tensor>(2) : nullptr;
  const int64_t N = X->Shape()[0];
  const int64_t M = W->Shape()[0];
  const int group_mkl = static_cast<int>(onnxruntime::ConvBase::group_);

  ORT_RETURN_IF_ERROR(onnxruntime::ConvBase::ValidateInputShape(X, W));

  std::vector<int64_t> kernel_shape;
  ORT_RETURN_IF_ERROR(onnxruntime::ConvBase::ComputeKernelShape(W->Shape(), kernel_shape));
  const size_t kernel_rank = kernel_shape.size();

  if (kernel_rank > 3) {
    // Fall Back to CPU implementation.
    return onnxruntime::Conv<T>::Compute(context);
  }

  if (kernel_rank + 2 != W->Shape().NumDimensions()) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "kernel_shape num_dims is not compatible with W num_dims.",
                           " kernel_shape: ", TensorShape(kernel_shape).ToString().c_str(),
                           " W: ", W->Shape().ToString().c_str());
  }

  for (size_t i = 0; i < kernel_rank; ++i) {
    if (kernel_shape[i] != W->Shape()[i + 2]) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "kernel_shape is not compatible with W shape.",
                             " kernel_shape: ", TensorShape(kernel_shape).ToString().c_str(),
                             " W: ", W->Shape().ToString().c_str());
    }
  }

  std::vector<int64_t> pads(onnxruntime::ConvBase::pads_);
  if (pads.empty()) {
    pads.resize(kernel_rank * 2, 0);
  }
  std::vector<int64_t> dilations(onnxruntime::ConvBase::dilations_);
  if (dilations.empty()) {
    dilations.resize(kernel_rank, 1);
  }
  std::vector<int64_t> strides(onnxruntime::ConvBase::strides_);
  if (strides.empty()) {
    strides.resize(kernel_rank, 1);
  }

  std::vector<int64_t> Y_dims;
  Y_dims.insert(Y_dims.begin(), {N, M});
  TensorShape input_shape = X->Shape().Slice(2);
  ORT_RETURN_IF_ERROR(onnxruntime::ConvBase::InferOutputShape(input_shape, kernel_shape, strides, dilations, &pads, &Y_dims));
  Tensor* Y = context->Output(0, TensorShape(Y_dims));
  TensorShape output_shape = Y->Shape().Slice(2);

  mkldnn::memory::dims src_dims_mkl(X->Shape().GetDims().begin(), X->Shape().GetDims().end());
  mkldnn::memory::dims filter_dims_mkl;
  if (group_mkl == 1) {
    filter_dims_mkl.assign(W->Shape().GetDims().begin(), W->Shape().GetDims().end());
  } else {
    filter_dims_mkl.assign({group_mkl,
                            static_cast<int>(W->Shape()[0] / group_mkl)});
    filter_dims_mkl.insert(filter_dims_mkl.end(), W->Shape().GetDims().begin() + 1, W->Shape().GetDims().end());
  }
  mkldnn::memory::dims strides_mkl(strides.begin(), strides.end());
  mkldnn::memory::dims dilations_mkl(dilations.begin(), dilations.end());
  // mkldnn dilations start from 0 so we need to subtract 1 from each dim.
  for (size_t dim = 0; dim < kernel_rank; dim++) {
    dilations_mkl[dim] -= 1;
  }

  mkldnn::memory::dims padding_left_mkl(pads.begin(), pads.begin() + kernel_rank);
  mkldnn::memory::dims padding_right_mkl(pads.begin() + kernel_rank, pads.end());
  mkldnn::memory::dims dst_dims_mkl(Y_dims.begin(), Y_dims.end());
  mkldnn::memory::dims bias_dims_mkl;
  if (B != nullptr) {
    bias_dims_mkl.assign(B->Shape().GetDims().begin(), B->Shape().GetDims().end());
  }

  AllocatorPtr alloc;
  ORT_RETURN_IF_ERROR(context->GetTempSpaceAllocator(&alloc));
  IAllocatorUniquePtr<void> src_reorder_buffer;
  IAllocatorUniquePtr<void> dst_reorder_buffer;

  const T* src_data = X->template Data<T>();
  const T* filter_data = W->template Data<T>();
  T* dst_data = Y->template MutableData<T>();
  const T* bias_data = nullptr;
  if (B != nullptr) {
    bias_data = B->template Data<T>();
  }

  try {
    ConvParams conv_params(src_dims_mkl, filter_dims_mkl, bias_dims_mkl,
                           dst_dims_mkl, strides_mkl, dilations_mkl,
                           padding_left_mkl, padding_right_mkl);
    ConvPrimitive<T>* conv_primitive = ConvPrimitivePool<T>::Get(conv_params);
    mkldnn::engine& cpu_engine = GetEngine();
    mkldnn::engine& gpu_engine = GetGpuEngine();
    auto conv_fwd_pd = conv_primitive->GetPrimitiveDesc();

    enum mkldnn::memory::format_tag src_format_tag = mkldnn::memory::format_tag::undef;
    enum mkldnn::memory::format_tag filter_format_tag = mkldnn::memory::format_tag::undef;
    enum mkldnn::memory::format_tag dst_format_tag = mkldnn::memory::format_tag::undef;

    if (kernel_rank == 1) {
      src_format_tag = mkldnn::memory::format_tag::ncw;
      if (group_mkl == 1) {
        filter_format_tag = mkldnn::memory::format_tag::oiw;
      } else {
        filter_format_tag = mkldnn::memory::format_tag::goiw;
      }
      dst_format_tag = mkldnn::memory::format_tag::ncw;
    } else if (kernel_rank == 2) {
      src_format_tag = mkldnn::memory::format_tag::nchw;
      if (group_mkl == 1) {
        filter_format_tag = mkldnn::memory::format_tag::oihw;
      } else {
        filter_format_tag = mkldnn::memory::format_tag::goihw;
      }
      dst_format_tag = mkldnn::memory::format_tag::nchw;
    } else {
      src_format_tag = mkldnn::memory::format_tag::ncdhw;
      if (group_mkl == 1) {
        filter_format_tag = mkldnn::memory::format_tag::oidhw;
      } else {
        filter_format_tag = mkldnn::memory::format_tag::goidhw;
      }
      dst_format_tag = mkldnn::memory::format_tag::ncdhw;
    }

    auto src_md = mkldnn::memory::desc(src_dims_mkl, MklDnnType<T>(), src_format_tag);
    auto dst_md = mkldnn::memory::desc(dst_dims_mkl, MklDnnType<T>(), dst_format_tag);
    auto filter_md = mkldnn::memory::desc(filter_dims_mkl, MklDnnType<T>(), filter_format_tag);

    auto cpu_stream = mkldnn::stream(cpu_engine);
    auto gpu_stream = mkldnn::stream(gpu_engine);

    auto src_pd = mkldnn::memory::desc(src_md);
    mkldnn::memory src_mem = mkldnn::memory(src_pd, cpu_engine, (void*)src_data);
    mkldnn::reorder(src_mem, *conv_primitive->GetSrcGmem())
        .execute(gpu_stream, src_mem, *conv_primitive->GetSrcGmem());

    auto filter_pd = mkldnn::memory::desc(mkldnn::memory::desc(
        filter_dims_mkl, MklDnnType<T>(), filter_format_tag));
    mkldnn::memory filter_mem = mkldnn::memory(filter_pd, cpu_engine, (void*)filter_data);
    mkldnn::reorder(filter_mem, *conv_primitive->GetWeightGmem())
        .execute(gpu_stream, filter_mem, *conv_primitive->GetWeightGmem());

    if (bias_data != nullptr) {
      mkldnn::memory bias_mem = mkldnn::memory(conv_fwd_pd->bias_desc(), cpu_engine, (void*)bias_data);
      mkldnn::reorder(bias_mem, *conv_primitive->GetBiasGmem())
          .execute(gpu_stream, bias_mem, *conv_primitive->GetBiasGmem());
    }

    conv_primitive->Compute(src_data, filter_data, dst_data, bias_data);

    // mkldnn::memory src = mkldnn::memory(conv_fwd_pd->dst_desc(), gpu_engine, (void*)dst_data);
    auto dst_pd = mkldnn::memory::desc(dst_md);
    mkldnn::memory dst_mem = mkldnn::memory(dst_pd, cpu_engine, Y->template MutableData<T>());
    mkldnn::reorder(*conv_primitive->GetDstGmem(), dst_mem)
        .execute(gpu_stream, *conv_primitive->GetDstGmem(), dst_mem);
  } catch (const mkldnn::error& e) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Status: ", e.status, ", message: ", e.what());
  }

  return Status::OK();
}

ONNX_OPERATOR_KERNEL_EX(
    Conv,
    kOnnxDomain,
    1,
    kMklDnnExecutionProvider,
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
    Conv<float>);

}  // namespace mkl_dnn
}  // namespace onnxruntime
