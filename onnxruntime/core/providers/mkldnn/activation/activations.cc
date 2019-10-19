// Copyright(C) 2018 Intel Corporation
// Licensed under the MIT License

#ifdef _WIN32
#pragma warning(disable : 4244)
#endif

#include "core/providers/mkldnn/mkldnn_common.h"
#include "core/providers/mkldnn/activation/activations.h"
#include "core/providers/mkldnn/mkldnn_fwd.h"

namespace onnxruntime {
namespace mkl_dnn {

namespace {
// Struct which encapsulates parameters for MKLDNN Pool primitive.
struct ReluParams {
  const mkldnn::memory::dims& src_dims;
  const mkldnn::memory::dims& dst_dims;

  ReluParams(const mkldnn::memory::dims& src_dims, const mkldnn::memory::dims& dst_dims)
      : src_dims(src_dims),
        dst_dims(dst_dims) {}

  // Used as the key for Pool Primitive Reuse Pool.
  std::string ToString() const {
    std::string key;
    key.reserve(64);
    key.append("Relu_");
    AddDimsToKey(key, src_dims);
    AddDimsToKey(key, dst_dims);
    return key;
  }
};

static mkldnn::engine& GetGpuEngine() {
  static mkldnn::engine engine = mkldnn::engine(mkldnn::engine::kind::gpu, 0);
  return engine;
}
template <typename T>
class ReluPrimitive final : public PrimitiveBase {
 public:
  explicit ReluPrimitive(const ReluParams& params)
      : cpu_engine_(GetEngine()), gpu_engine_(GetGpuEngine()) {
    context_.stream.reset(new mkldnn::stream(gpu_engine_));
    if (context_.relu_fwd == nullptr) {
      Initialize(params);
    }
  }

  ~ReluPrimitive() = default;

  void Compute(const T* src_data, T* dst_data) {
    context_.src_mem->set_data_handle(
        static_cast<void*>(const_cast<T*>(src_data)));
    context_.dst_mem->set_data_handle(
        static_cast<void*>(dst_data));
    mkldnn::reorder(*context_.src_mem, *context_.src_mem_gpu).execute(*context_.stream, *context_.src_mem, *context_.src_mem_gpu);
    context_.relu_fwd->execute(
        *context_.stream,
        {{MKLDNN_ARG_SRC, *context_.src_mem_gpu},
         {MKLDNN_ARG_DST, *context_.dst_mem_gpu}});
    mkldnn::reorder(*context_.dst_mem_gpu, *context_.dst_mem).execute(*context_.stream, *context_.dst_mem_gpu, *context_.dst_mem);

    context_.src_mem->set_data_handle(nullptr);
    context_.dst_mem->set_data_handle(nullptr);
    return;
  }

 private:
  struct ReluContext {
    std::unique_ptr<mkldnn::memory> src_mem;
    std::unique_ptr<mkldnn::memory> dst_mem;

	std::unique_ptr<mkldnn::memory> src_mem_gpu;
    std::unique_ptr<mkldnn::memory> dst_mem_gpu;

    size_t src_size;
    size_t dst_size;

    std::unique_ptr<mkldnn::eltwise_forward::desc> fwd_desc;
    std::unique_ptr<mkldnn::eltwise_forward::primitive_desc> relu_fwd_pd;
    std::unique_ptr<mkldnn::primitive> relu_fwd;

    std::unique_ptr<mkldnn::memory::desc> src_md;
    std::unique_ptr<mkldnn::memory::desc> dst_md;

    std::unique_ptr<mkldnn::stream> stream;
    std::vector<mkldnn::primitive> net;
  };

  void Initialize(const ReluParams& params) {
    mkldnn::memory::format_tag fmt = mkldnn::memory::format_tag::any;
    switch (params.src_dims.size()) {
      case 1: {
        fmt = mkldnn::memory::format_tag::x;
        break;
      }
      case 2: {
        fmt = mkldnn::memory::format_tag::nc;
        break;
      }
      case 3: {
        fmt = mkldnn::memory::format_tag::ntc;
        break;
      }
      case 4: {
        fmt = mkldnn::memory::format_tag::nchw;
        break;
      }
      case 5: {
        fmt = mkldnn::memory::format_tag::ncdhw;
        break;
      }
      default: {
        fmt = mkldnn::memory::format_tag::any;
        break;
      }
    }

    context_.src_md.reset(new mkldnn::memory::desc({params.src_dims}, MklDnnType<T>(), fmt));

    mkldnn::algorithm algo = mkldnn::algorithm::eltwise_relu;
    context_.fwd_desc.reset(new mkldnn::eltwise_forward::desc(
        mkldnn::prop_kind::forward_inference, algo, *context_.src_md, 0));

    context_.relu_fwd_pd.reset(new mkldnn::eltwise_forward::primitive_desc(
        *context_.fwd_desc, gpu_engine_));

    context_.src_size = context_.relu_fwd_pd.get()->src_desc().get_size();
    context_.dst_size = context_.relu_fwd_pd.get()->dst_desc().get_size();

    context_.src_mem.reset(new mkldnn::memory(context_.relu_fwd_pd.get()->src_desc(), cpu_engine_, nullptr));
    context_.dst_mem.reset(new mkldnn::memory(context_.relu_fwd_pd.get()->dst_desc(), cpu_engine_, nullptr));

    context_.src_mem_gpu.reset(new mkldnn::memory(context_.relu_fwd_pd.get()->src_desc(), gpu_engine_));
    context_.dst_mem_gpu.reset(new mkldnn::memory(context_.relu_fwd_pd.get()->dst_desc(), gpu_engine_));

	context_.relu_fwd.reset(
        new mkldnn::eltwise_forward(*context_.relu_fwd_pd));
  }

  ReluContext context_;
  mkldnn::engine& cpu_engine_;
  mkldnn::engine& gpu_engine_;
};

// Pool which allows for reuse of MKLDNN Relu primitives which are expensive
// to instantiate. To address thread safety, the primitives are stored in a map
// on thread local storage.
template <typename T>
class ReluPrimitivePool : public PrimitivePool<T> {
 public:
  static ReluPrimitive<T>* Get(const ReluParams& params) {
    ReluPrimitive<T>* primitive = dynamic_cast<ReluPrimitive<T>*>(
        ReluPrimitivePool<T>::GetInstance().GetPrimitive(params.ToString()));

    if (primitive == nullptr) {
      auto relu_primitive = onnxruntime::make_unique<ReluPrimitive<T>>(params);
      primitive = relu_primitive.get();
      ReluPrimitivePool<T>::GetInstance().SetPrimitive(params.ToString(),
                                                       std::move(relu_primitive));
    }
    return primitive;
  }

 private:
  ReluPrimitivePool() = default;
  ~ReluPrimitivePool() = default;

  static ReluPrimitivePool& GetInstance() {
    static ReluPrimitivePool pool;
    return pool;
  }
};
}  // namespace

template <typename T>
Status Relu<T>::Compute(OpKernelContext* context) const {
  const Tensor* X = context->Input<Tensor>(0);
  Tensor* Y = context->Output(0, X->Shape());

  const TensorShape& x_shape = X->Shape();
  const auto& x_dims = x_shape.GetDims();

  auto engine = mkldnn::engine(mkldnn::engine::kind::gpu, 0);
  ORT_UNUSED_PARAMETER(engine);

  if (X->Shape().NumDimensions() > 5) {
    // Fall Back to CPU implementation.
    // mkldnn support up to dim of size 5
    return onnxruntime::Relu<T>::Compute(context);
  }

  const TensorShape& y_shape = Y->Shape();
  auto& y_dims = y_shape.GetDims();

  const T* src_data = X->template Data<T>();
  T* dst_data = Y->template MutableData<T>();

  mkldnn::memory::dims src_dims_mkl(x_dims.begin(), x_dims.end());
  mkldnn::memory::dims dst_dims_mkl(y_dims.begin(), y_dims.end());

  try {
    ReluParams pool_params(src_dims_mkl, dst_dims_mkl);
    ReluPrimitive<T>* relu_primitive = ReluPrimitivePool<T>::Get(pool_params);

    relu_primitive->Compute(src_data, dst_data);
  } catch (const mkldnn::error& e) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Status: ", e.status,
                           ", message: ", e.what());
  }

  return Status::OK();
}

ONNX_OPERATOR_KERNEL_EX(
    Relu,
    kOnnxDomain,
    6,
    kMklDnnExecutionProvider,
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
    Relu<float>);

}  // namespace mkl_dnn
}  // namespace onnxruntime