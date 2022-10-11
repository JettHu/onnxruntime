// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/xnnpack/nn/resize.h"

#include <utility>

#include "core/common/common.h"
#include "core/framework/op_kernel.h"
#include "core/optimizer/initializer.h"

namespace onnxruntime {
namespace xnnpack {

bool Resize::IsOnnxNodeSupported(const NodeUnit& node_unit,
                                 const GraphViewer&) {
  bool supported = false;
  do {
    // Resize has 1-4 input.
    const auto& inputs = node_unit.Inputs();
    const auto& x_arg = inputs[0].node_arg;

    const auto* x_type = x_arg.TypeAsProto();
    if (x_type == nullptr ||
        (x_type->tensor_type().elem_type() != ONNX_NAMESPACE::TensorProto_DataType_FLOAT &&
         x_type->tensor_type().elem_type() != ONNX_NAMESPACE::TensorProto_DataType_UINT8 &&
         x_type->tensor_type().elem_type() != ONNX_NAMESPACE::TensorProto_DataType_INT8)) {
      break;
    }
    const auto* x_shape = x_arg.Shape();
    //'bilinear' == 2-D input or 4-D input with outermost 2 scales as 1 or
    // 4-D input with outermost and innermost scales as 1
    // but we just support 4-d tensor for now
    if (!x_shape || x_shape->dim_size() != 4 || x_shape->dim(1).dim_value() <= 0) {
      break;
    }
    const auto* output_shape = node_unit.Outputs()[0].node_arg.Shape();
    bool length_resized_compatible_pytorch_half_pixel = true;
    if (output_shape->dim(2).dim_value() <= 1 || output_shape->dim(1).dim_value() <= 1) {
      length_resized_compatible_pytorch_half_pixel = false;
    }

    // Refer to onnxruntime/core/providers/cpu/tensor/upsamplebase.h,
    // But I can't find the definition of Resize with Opset less than 10
    // besides, opset 18 is too complicated, so we don't support it temperately.
    auto opset_version = node_unit.SinceVersion();
    if (opset_version >= 18) {
      break;
    }
    ProtoHelperNodeContext nc(node_unit.GetNode());
    OpNodeProtoHelper info(&nc);

    std::string mode;
    info.GetAttrOrDefault<std::string>("mode", &mode, "nearest");
    if (mode != "linear") {
      break;
    }
    auto extrapolation_value = info.GetAttrOrDefault<float>("extrapolation_value", 0.0f);
    if (extrapolation_value != 0.0F) {
      break;
    }
    // Coordinate transformation mode attr was introduced in version 11.
    // before that asymmetric mode was the only available transformation mode
    std::string coordinate_transform_mode_name =
        opset_version > 10
            ? info.GetAttrOrDefault<std::string>("coordinate_transformation_mode", "half_pixel")
            : "asymmetric";
    if (coordinate_transform_mode_name != "asymmetric" &&
        coordinate_transform_mode_name != "half_pixel" &&
        coordinate_transform_mode_name != "align_corners" &&
        (coordinate_transform_mode_name != "pytorch_half_pixel" || !length_resized_compatible_pytorch_half_pixel)) {
      break;
    }
    auto exclude_outside = info.GetAttrOrDefault<int64_t>("exclude_outside", 0) == 0 ? false : true;
    if (exclude_outside) {
      break;
    }

    supported = true;
  } while (false);

  return supported;
}

Resize::Resize(const OpKernelInfo& info) : UpsampleBase(info), XnnpackKernel{info} {
  const auto& node = info.node();
  auto input_defs = node.InputDefs();
  int x_dtype = 0;
  ORT_ENFORCE(GetType(*input_defs[0], x_dtype));
  if (x_dtype == ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
    op_type_ = OpComputeType::op_compute_type_fp32;
  } else if (x_dtype == ONNX_NAMESPACE::TensorProto_DataType_UINT8 ||
             x_dtype == ONNX_NAMESPACE::TensorProto_DataType_INT8) {
    op_type_ = x_dtype == ONNX_NAMESPACE::TensorProto_DataType_UINT8
                   ? OpComputeType::op_compute_type_qu8
                   : OpComputeType::op_compute_type_qs8;
  } else {
    auto stype = DataTypeImpl::ToString(DataTypeImpl::TypeFromProto(*input_defs[0]->TypeAsProto()));
    ORT_THROW("unsupported op in Resize, we have FLOAT|UINT8, but got ", stype);
  }
  const auto* x_shape = input_defs[0]->Shape();
  auto input_shape = utils::GetTensorShapeFromTensorShapeProto(*x_shape);
  int64_t channels = input_shape[3];
  // nchw -> nhwc
  if (scales_.size() == 4) {
    std::swap(scales_[1], scales_[2]);
    std::swap(scales_[3], scales_[2]);
  }
  ORT_ENFORCE(channels > 0, "can't retrieve channel from input_shape");
  uint32_t flags = 0;
  ORT_ENFORCE(mode_ == UpsampleMode::LINEAR, "only support bilinear resize");
  if (coordinate_transform_mode_ == ResizeCoordinateTransformationMode::ALIGN_CORNERS) {
    flags |= XNN_FLAG_ALIGN_CORNERS;
  } else if (!(coordinate_transform_mode_ == ResizeCoordinateTransformationMode::HALF_PIXEL ||
               coordinate_transform_mode_ == ResizeCoordinateTransformationMode::PYTORCH_HALF_PIXEL)) {
    flags |= XNN_FLAG_TENSORFLOW_LEGACY_MODE;
  }

  xnn_status xstatus = xnn_status_invalid_state;
  struct xnn_operator* p = nullptr;
  if (op_type_ == OpComputeType::op_compute_type_fp32) {
    xstatus = xnn_create_resize_bilinear2d_nhwc_f32(
        channels,
        channels,
        channels,
        flags,
        &p);
  } else if (op_type_ == OpComputeType::op_compute_type_qu8) {
    xstatus = xnn_create_resize_bilinear2d_nhwc_u8(
        channels,
        channels,
        channels,
        flags,
        &p);
  } else {
    xstatus = xnn_create_resize_bilinear2d_nhwc_s8(
        channels,
        channels,
        channels,
        flags,
        &p);
  }
  ORT_ENFORCE(xstatus == xnn_status_success, "xnn_create_resize_bilinear2d_nhwc_",
              OpTypeToString(op_type_), " failed. Status:", xstatus);
  op0_.reset(p);
}

// compute method of Resize
Status Resize::ComputeInternal(OpKernelContext* ctx, const Tensor* input,
                               const TensorShapeVector& output_dims) const {
  const auto& X_shape = input->Shape();
  auto N = X_shape[0];
  auto H = X_shape[1];
  auto W = X_shape[2];
  Tensor* output = ctx->Output(0, TensorShape(output_dims));

  pthreadpool_t t_pool = GetThreadPool();
  xnn_status status = xnn_status_invalid_state;
  if (op_type_ == OpComputeType::op_compute_type_fp32) {
    status = xnn_setup_resize_bilinear2d_nhwc_f32(
        op0_.get(),
        N,
        H, W, output_dims[1], output_dims[2],
        input->Data<float>(),
        output->MutableData<float>(),
        t_pool);
  } else if (op_type_ == OpComputeType::op_compute_type_qu8) {
    status = xnn_setup_resize_bilinear2d_nhwc_u8(
        op0_.get(),
        N,
        H, W, output_dims[1], output_dims[2],
        input->Data<uint8_t>(),
        output->MutableData<uint8_t>(),
        t_pool);
  } else {
    status = xnn_setup_resize_bilinear2d_nhwc_s8(
        op0_.get(),
        N,
        H, W, output_dims[1], output_dims[2],
        input->Data<int8_t>(),
        output->MutableData<int8_t>(),
        t_pool);
  }
  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_setup_Resize_nc_",
                           OpTypeToString(op_type_), " returned ", status);
  }
  status = xnn_run_operator(op0_.get(), t_pool);
  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_run_operator returned ", status);
  }
  return Status::OK();
}
Status Resize::Compute(OpKernelContext* ctx) const {
  const auto* X = ctx->Input<Tensor>(0);
  const auto& X_shape = X->Shape();

  TensorShapeVector output_dims(X_shape.NumDimensions());

  if (OpKernel::Node().InputDefs().size() == 1) {
    ComputeOutputShape(scales_, X_shape.GetDims(), output_dims);
    return ComputeInternal(ctx, X, output_dims);
  }

  const auto* sizes = ctx->Input<Tensor>(sizes_input_idx_);
  if (scales_cached_) {
    ORT_ENFORCE(sizes == nullptr, "Only one of scales or sizes must be provided as input.");

    // Compute output shape from scales and input dims
    ComputeOutputShape(scales_, X->Shape().GetDims(), output_dims);
    return ComputeInternal(ctx, X, output_dims);
  }

  const auto* scales = ctx->Input<Tensor>(scales_input_idx_);
  // Get scales data
  std::vector<float> scales_array(X->Shape().GetDims().size());

  if (scales != nullptr && scales->Shape().Size() != 0) {
    ORT_ENFORCE(sizes == nullptr, "Only one of scales or sizes must be provided as input.");
    ParseScalesData(scales, scales_array);

    // Compute output shape from scales and input dims
    ComputeOutputShape(scales_array, X->Shape().GetDims(), output_dims);
  } else {
    ORT_ENFORCE(sizes != nullptr && sizes->Shape().Size() != 0, "Either scales or sizes MUST be provided as input.");

    // When sizes input is available directly populate it into the output_dims array.
    memcpy(output_dims.data(), sizes->template Data<int64_t>(), sizes->Shape().Size() * sizeof(int64_t));

    ORT_ENFORCE(X->Shape().GetDims().size() == output_dims.size(),
                "Resize: input tensor's rank does not match the output tensor's rank.");

    ParseScalesDataFromOutputSize(output_dims, X->Shape().GetDims(), scales_array);
  }

  return ComputeInternal(ctx, X, output_dims);
}

ONNX_OPERATOR_VERSIONED_KERNEL_EX(Resize, kMSInternalNHWCDomain, 1, 9, kXnnpackExecutionProvider,
                                  KernelDefBuilder().TypeConstraint("T1", DataTypeImpl::GetTensorType<float>()),
                                  Resize);
ONNX_OPERATOR_VERSIONED_KERNEL_EX(Resize, kMSInternalNHWCDomain, 10, 10, kXnnpackExecutionProvider,
                                  KernelDefBuilder().TypeConstraint("T1", DataTypeImpl::GetTensorType<float>()),
                                  Resize);
ONNX_OPERATOR_VERSIONED_KERNEL_EX(Resize, kMSInternalNHWCDomain, 11, 12, kXnnpackExecutionProvider,
                                  KernelDefBuilder().TypeConstraint("T1", DataTypeImpl::GetTensorType<float>()),
                                  Resize);
ONNX_OPERATOR_KERNEL_EX(Resize, kMSInternalNHWCDomain, 13, kXnnpackExecutionProvider,
                        KernelDefBuilder().TypeConstraint("T1", DataTypeImpl::GetTensorType<float>()),
                        Resize);

}  // namespace xnnpack
}  // namespace onnxruntime
