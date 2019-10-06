// Copyright(C) 2018 Intel Corporation
// Licensed under the MIT License

#ifdef _WIN32
#pragma warning(disable : 4244)
#endif

#include "core/providers/mkldnn/mkldnn_common.h"
#include "core/providers/mkldnn/math/sum.h"
#include "core/providers/mkldnn/mkldnn_fwd.h"

namespace onnxruntime {
namespace mkl_dnn {

namespace {
// Struct which encapsulates parameters for MKLDNN Sum primitives.
struct SumParams {
  const std::vector<mkldnn::memory::dims>& src_dims;
  const mkldnn::memory::dims& dst_dim;
  const int num_inputs;
  const int num_dimensions;

  SumParams(const std::vector<mkldnn::memory::dims>& dims,
            const mkldnn::memory::dims& dst_dims, int numinputs,
            int dimensions)
      : src_dims(dims),
        dst_dim(dst_dims),
        num_inputs(numinputs),
        num_dimensions(dimensions) {}

  // Used as the key for Sum Primitive Reuse Sum.
  std::string ToString() const {
    std::string key;
    key.reserve(64);
    key.append("sum_");
    for (size_t i = 0; i < src_dims.size(); i++) {
      AddDimsToKey(key, src_dims[i]);
    }
    AddDimsToKey(key, dst_dim);
    return key;
  }
};

template <typename T>
class SumPrimitive final : public PrimitiveBase {
 public:
  explicit SumPrimitive(const SumParams& params)
      : cpu_engine_(GetEngine()) {
    context_.stream.reset(new mkldnn::stream(cpu_engine_));
    if (context_.sum_pd == nullptr) {
      Initialize(params);
    }
  }

  ~SumPrimitive() = default;

  void Compute(OpKernelContext* context, int numinputs) {
    const Tensor* X1 = context->Input<Tensor>(0);
    Tensor* Y = context->Output(0, X1->Shape());
    T* dst_data = Y->template MutableData<T>();

    context_.dst_mem->set_data_handle(
        static_cast<void*>(dst_data));

    for (int i = 0; i < numinputs; i++) {
      const Tensor* X = context->Input<Tensor>(i);
      const T* src_data = X->template Data<T>();
      context_.srcs_memory[i].set_data_handle(
          static_cast<void*>(const_cast<T*>(src_data)));
    }

    std::unordered_map<int, mkldnn::memory> args{
        {MKLDNN_ARG_DST, *context_.dst_mem}};
    for (int i = 0; i < (int)numinputs; i++) {
      args.insert({MKLDNN_ARG_MULTIPLE_SRC + i, context_.srcs_memory[i]});
    }

    context_.net[0].execute(
        *context_.stream, args);

    //for (int i = 0; i < numinputs; i++) {
    //  context_.srcs_memory[i].set_data_handle(nullptr);
    //}
  }

  std::unique_ptr<mkldnn::memory::desc> GetDstMemoryDesc() const {
    return context_.dst_md;
  }

  std::unique_ptr<mkldnn::sum::primitive_desc>
  GetPrimitiveDesc() const {
    return context_.sum_pd;
  }

 private:
  struct SumContext {
    std::unique_ptr<mkldnn::memory::desc> src_md;
    std::unique_ptr<mkldnn::memory::desc> dst_md;

    std::vector<mkldnn::memory> srcs_memory;
    std::unique_ptr<mkldnn::memory> dst_mem;

    std::vector<mkldnn::memory::desc> srcs_pd;
    std::unique_ptr<mkldnn::memory::desc> src_mpd;
    std::unique_ptr<mkldnn::memory::desc> dst_pd;
    std::unique_ptr<mkldnn::sum::primitive_desc> sum_pd;

    std::unique_ptr<mkldnn::stream> stream;
    std::vector<mkldnn::primitive> net;
  };

  void Initialize(const SumParams& params) {
    std::vector<float> coeff;

    mkldnn::memory::format_tag fmt = mkldnn::memory::format_tag::any;
    switch (params.num_dimensions) {
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

    for (int i = 0; i < params.num_inputs; i++) {
      context_.src_md.reset(
          new mkldnn::memory::desc({params.src_dims[i]}, MklDnnType<T>(), fmt));
      auto mpd = mkldnn::memory::desc(*context_.src_md);
      auto src_memory = mkldnn::memory(mpd, cpu_engine_, nullptr);

      context_.srcs_pd.push_back(mpd);
      context_.srcs_memory.push_back(src_memory);
      coeff.push_back(1.0);
    }

    std::unique_ptr<mkldnn::memory> dst;
    context_.dst_md.reset(new mkldnn::memory::desc(
        {params.dst_dim}, MklDnnType<T>(), mkldnn::memory::format_tag::any));
    context_.sum_pd.reset(new mkldnn::sum::primitive_desc(
        *context_.dst_md, coeff, context_.srcs_pd, cpu_engine_));
    context_.dst_mem.reset(new mkldnn::memory(
        context_.sum_pd->dst_desc(), cpu_engine_, nullptr));

    std::vector<mkldnn::memory> inputs;
    for (int i = 0; i < params.num_inputs; i++) {
      inputs.push_back(context_.srcs_memory[i]);
    }
    auto c = mkldnn::sum(*context_.sum_pd);
    context_.net.push_back(c);
  }

  SumContext context_;
  mkldnn::engine& cpu_engine_;
};

// Pool which allows for reuse of MKLDNN Sum primitives which are
// expensive to instantiate. To address thread safety, the primitives
// are stored in a map on thread local storage.

template <typename T>
class SumPrimitivePool : public PrimitivePool<T> {
 public:
  static SumPrimitive<T>* Get(const SumParams& params) {
    SumPrimitive<T>* primitive = dynamic_cast<SumPrimitive<T>*>(
        SumPrimitivePool<T>::GetInstance().GetPrimitive(params.ToString()));

    if (primitive == nullptr) {
      auto sum_primitive = onnxruntime::make_unique<SumPrimitive<T>>(params);
      primitive = sum_primitive.get();
      SumPrimitivePool<T>::GetInstance().SetPrimitive(
          params.ToString(), std::move(sum_primitive));
    }
    return primitive;
  }

 private:
  SumPrimitivePool() = default;
  ~SumPrimitivePool() = default;

  static SumPrimitivePool& GetInstance() {
    static SumPrimitivePool pool;
    return pool;
  }
};
}  // namespace

template <typename T>
Status Sum<T>::Compute(OpKernelContext* context) const {
  int num_inputs = static_cast<int>(OpKernel::Node().InputDefs().size());

  ORT_ENFORCE(num_inputs > 0, "MKLDNN Sum kernel: Must have at least one input");

  if (num_inputs == 1) {
    // Fall Back to CPU implementation. For one input,  use CPU implementation
    // to copy input to output
    return onnxruntime::Sum_6<T>::Compute(context);
  }

  std::vector<mkldnn::memory::dims> src_dims;

  const Tensor* X1 = context->Input<Tensor>(0);
  if (X1 == nullptr) return Status(common::ONNXRUNTIME, common::FAIL, "input count mismatch");
  Tensor* Y = context->Output(0, X1->Shape());
  int dimensions = static_cast<int>(X1->Shape().NumDimensions());

  const TensorShape& x_shape = X1->Shape();
  const auto& x_dims = x_shape.GetDims();
  mkldnn::memory::dims src_dim(x_dims.begin(), x_dims.end());

  mkldnn::memory::dims dst_dims_mkl(
      Y->Shape().GetDims().begin(), Y->Shape().GetDims().end());

  for (int i = 0; i < num_inputs; i++) {
    const Tensor* X = context->Input<Tensor>(i);
    if (X == nullptr) return Status(common::ONNXRUNTIME, common::FAIL, "input count mismatch");
    mkldnn::memory::dims src_dims_mkl(
        X->Shape().GetDims().begin(), X->Shape().GetDims().end());
    src_dims.push_back(src_dims_mkl);
  }
  try {
    SumParams parameters(src_dims, dst_dims_mkl, num_inputs, dimensions);
    SumPrimitive<T>* sum_primitive = SumPrimitivePool<T>::Get(parameters);
    ORT_RETURN_IF_NOT(sum_primitive != nullptr);
    sum_primitive->Compute(context, num_inputs);
  } catch (const mkldnn::error& e) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Status: ", e.status,
                           ", message: ", e.what());
  }

  return Status::OK();
}

ONNX_OPERATOR_KERNEL_EX(
    Sum,
    kOnnxDomain,
    6,
    kMklDnnExecutionProvider,
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
    Sum<float>);

}  // namespace mkl_dnn
}  // namespace onnxruntime
