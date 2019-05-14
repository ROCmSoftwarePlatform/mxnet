/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * Copyright (c) 2017 by Contributors
 * \file indexing_op.h
 * \brief Function definition of indexing operator
 * \author Bing Xu, Siyi Li, Chi Zhang, Haibin Lin
*/
#ifndef MXNET_OPERATOR_TENSOR_INDEXING_OP_H_
#define MXNET_OPERATOR_TENSOR_INDEXING_OP_H_

#include <dmlc/logging.h>
#include <dmlc/parameter.h>
#include <mxnet/operator.h>
#include <mxnet/operator_util.h>
#include <map>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <type_traits>
#include "../operator_common.h"
#include "../mshadow_op.h"
#include "../elemwise_op_common.h"
#include "./util/tensor_util-inl.h"
#include "../mxnet_op.h"
#include "./sort_op.h"
#include "./init_op.h"
#include "../../engine/openmp.h"
#include "../../common/utils.h"
#ifdef __HIPCC__
#include "./indexing_op-inl.cuh"
#endif

namespace mxnet {
namespace op {

namespace embedding {
enum EmbeddingOpInputs {kData, kWeight};
enum EmbeddingOpOutputs {kOut};
enum EmbeddingOpResource {kTempSpace};
}  // namespace embedding


struct SparseEmbeddingParam: public dmlc::Parameter<SparseEmbeddingParam> {
  int input_dim;
  int output_dim;
  int dtype;
  bool deterministic;
  DMLC_DECLARE_PARAMETER(SparseEmbeddingParam) {
    DMLC_DECLARE_FIELD(input_dim).set_lower_bound(1)
    .describe("Vocabulary size of the input indices.");
    DMLC_DECLARE_FIELD(output_dim).set_lower_bound(1)
    .describe("Dimension of the embedding vectors.");
    DMLC_DECLARE_FIELD(dtype).set_default(mshadow::kFloat32)
    .add_enum("float32", mshadow::kFloat32)
    .add_enum("float64", mshadow::kFloat64)
    .add_enum("float16", mshadow::kFloat16)
    .add_enum("uint8", mshadow::kUint8)
    .add_enum("int32", mshadow::kInt32)
    .describe("Data type of weight.");
    DMLC_DECLARE_FIELD(deterministic).set_default(false)
    .describe("Force the backward gradient calculation to be executed based on a deterministic"
               " order at the cost of slower speed.");
  }
};

struct EmbeddingParam: public dmlc::Parameter<EmbeddingParam> {
  int input_dim;
  int output_dim;
  int dtype;
  bool sparse_grad;
  DMLC_DECLARE_PARAMETER(EmbeddingParam) {
    DMLC_DECLARE_FIELD(input_dim).set_lower_bound(1)
    .describe("Vocabulary size of the input indices.");
    DMLC_DECLARE_FIELD(output_dim).set_lower_bound(1)
    .describe("Dimension of the embedding vectors.");
    DMLC_DECLARE_FIELD(dtype).set_default(mshadow::kFloat32)
    MXNET_ADD_ALL_TYPES
    .describe("Data type of weight.");
    DMLC_DECLARE_FIELD(sparse_grad).set_default(false)
    .describe("Compute row sparse gradient in the backward calculation. If set to True, "
              "the grad's storage type is row_sparse.");
  }
};

/*!
 * \brief CPU/GPU: Return the amount of temporary storage in bytes required by
                   AddTakeGradLargeBatch
 * \param num_items number of keys
 */
template <typename IndexType, typename xpu>
inline typename std::enable_if<std::is_same<xpu, cpu>::value, size_t>::type
AddTakeGradLargeBatchWorkspaceSize(size_t num_keys) {
  return 0;
}
/*!
 * \brief CPU/GPU: Return the amount of temporary storage in bytes required by
                   AddTakeGradLargeBatch
 * \param num_items number of keys
 */
template <typename IndexType, typename xpu>
inline typename std::enable_if<std::is_same<xpu, gpu>::value, size_t>::type
AddTakeGradLargeBatchWorkspaceSize(size_t num_keys);
/*!
 * \brief CPU/GPU: Gradient accumulate of embedding matrix.
                   dst[sorted[i]] += src[index[i]]
                   Called when the batchsize of src is larger than the featuredim
 * \param dst destination
 * \param sorted the sorted indices
 * \param index original index of the sorted indices
 * \param src source output
 * \param workspace (optional) temporary storage
 */
template<typename IndexType, typename DType>
inline void AddTakeGradLargeBatch(mshadow::Tensor<cpu, 2, DType> dst,
                                  const mshadow::Tensor<cpu, 1, IndexType>& sorted,
                                  const mshadow::Tensor<cpu, 1, IndexType>& index,
                                  const mshadow::Tensor<cpu, 2, DType> &src,
                                  mshadow::Tensor<cpu, 1, char>* workspace = NULL) {
  for (index_t y = 0; y < sorted.size(0); ++y) {
    dst[sorted[y]] += src[index[y]];
  }
}
template<typename ParamType>
inline bool EmbeddingOpShape(const nnvm::NodeAttrs& attrs,
                             std::vector<TShape> *in_attrs,
                             std::vector<TShape> *out_attrs) {
  using namespace mshadow;
  const TShape &dshape = (*in_attrs)[embedding::kData];
  if (dshape.ndim() ==  0) return false;
  const ParamType& param = nnvm::get<ParamType>(attrs.parsed);
  SHAPE_ASSIGN_CHECK(*in_attrs, embedding::kWeight, Shape2(param.input_dim,
                                                           param.output_dim));
  out_attrs->clear();

  TShape oshape(dshape.ndim()+1);
  for (size_t i = 0; i < dshape.ndim(); ++i) {
    oshape[i] = dshape[i];
  }
  oshape[dshape.ndim()] = param.output_dim;

  out_attrs->push_back(oshape);
  return true;
}

template<typename ParamType>
inline bool EmbeddingOpType(const nnvm::NodeAttrs& attrs,
                            std::vector<int> *in_type,
                            std::vector<int> *out_type) {
  const ParamType& param = nnvm::get<ParamType>(attrs.parsed);
  CHECK_EQ(in_type->size(), 2U);
  CHECK_GE(out_type->size(), 1U);
  int itype = (*in_type)[0];
  CHECK_NE(itype, -1) << "First input must have specified type";
  int dtype_in = (*in_type)[1];
  int dtype_out = (*out_type)[0];
  int dtype = param.dtype;
  if (dtype_in != -1 && dtype_out != -1) {
    // Both types defined, make sure they are the same
    CHECK_EQ(dtype_in, dtype_out) << "Input and output weights must have same type";
    dtype = dtype_in;
  } else if (dtype_in != -1 || dtype_out != -1) {
    // One of the types defined, choose the one that was defined
    dtype = (dtype_in != -1) ? dtype_in : dtype_out;
  }
  if ((*in_type)[1] == -1) (*in_type)[1] = dtype;
  out_type->clear();
  out_type->push_back(dtype);
  return true;
}

// storage type inference function for Embedding
inline bool EmbeddingOpForwardStorageType(const nnvm::NodeAttrs& attrs,
                                          const int dev_mask,
                                          DispatchMode* dispatch_mode,
                                          std::vector<int>* in_attrs,
                                          std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  const int& data_stype = in_attrs->at(embedding::kData);
  const int& weight_stype = in_attrs->at(embedding::kWeight);
  int& out_stype = out_attrs->at(embedding::kOut);
  bool dispatched = false;
  if (!dispatched && data_stype == kDefaultStorage && weight_stype == kDefaultStorage) {
    // dns, dns -> dns
    dispatched = storage_type_assign(&out_stype, kDefaultStorage,
                                     dispatch_mode, DispatchMode::kFCompute);
  }
  if (!dispatched && data_stype == kDefaultStorage && weight_stype == kRowSparseStorage) {
    // dns, rsp -> dns
    dispatched = storage_type_assign(&out_stype, kDefaultStorage,
                                     dispatch_mode, DispatchMode::kFComputeEx);
  }
  return dispatched;
}

// storage type inference function for SparseEmbedding
inline bool SparseEmbeddingOpForwardStorageType(const nnvm::NodeAttrs& attrs,
                                                const int dev_mask,
                                                DispatchMode* dispatch_mode,
                                                std::vector<int>* in_attrs,
                                                std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  const int& data_stype = in_attrs->at(embedding::kData);
  const int& weight_stype = in_attrs->at(embedding::kWeight);
  int& out_stype = out_attrs->at(embedding::kOut);
  bool dispatched = false;
  if (!dispatched && data_stype == kDefaultStorage &&
      (weight_stype == kRowSparseStorage || weight_stype == kDefaultStorage)) {
    // dns, rsp/dns -> dns
    dispatched = storage_type_assign(&out_stype, kDefaultStorage,
                                     dispatch_mode, DispatchMode::kFComputeEx);
  }
  return dispatched;
}

// storage type inference function for _backward_Embedding
inline bool EmbeddingOpBackwardStorageType(const nnvm::NodeAttrs& attrs,
                                           const int dev_mask,
                                           DispatchMode* dispatch_mode,
                                           std::vector<int>* in_attrs,
                                           std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 2U);
  const bool sparse_grad = nnvm::get<EmbeddingParam>(attrs.parsed).sparse_grad;
  const NDArrayStorageType target_stype = sparse_grad ? kRowSparseStorage : kDefaultStorage;
  const auto target_mode = sparse_grad ? DispatchMode::kFComputeEx : DispatchMode::kFCompute;

  const int ograd_stype = in_attrs->at(0);
  const int data_stype = in_attrs->at(1);
  int& data_grad_stype = out_attrs->at(0);
  int& weight_grad_stype = out_attrs->at(1);
  bool dispatched = false;
  if (!dispatched && ograd_stype == kDefaultStorage && data_stype == kDefaultStorage) {
    // dns, dns -> dns, dns/rsp
    if (type_assign(&data_grad_stype, kDefaultStorage) &&
        type_assign(&weight_grad_stype, target_stype)) {
      dispatched = dispatch_mode_assign(dispatch_mode, target_mode);
    }
  }
  // Print user friendly error message to notify misuses of sparse_grad
  if (weight_grad_stype != target_stype) {
    LOG(FATAL) << "Cannot use sparse_grad = " << sparse_grad
               << ", while stype of gradients w.r.t embedding weight is "
               << common::stype_string(weight_grad_stype);
  }
  return dispatched;
}

// storage type inference function for _backward_SparseEmbedding
inline bool SparseEmbeddingOpBackwardStorageType(const nnvm::NodeAttrs& attrs,
                                                 const int dev_mask,
                                                 DispatchMode* dispatch_mode,
                                                 std::vector<int>* in_attrs,
                                                 std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 2U);
  const int ograd_stype = in_attrs->at(0);
  const int data_stype = in_attrs->at(1);
  int& data_grad_stype = out_attrs->at(0);
  int& weight_grad_stype = out_attrs->at(1);
  bool dispatched = false;
  if (!dispatched && ograd_stype == kDefaultStorage &&
      data_stype == kDefaultStorage) {
    // dns, dns -> dns, rsp
    if (type_assign(&data_grad_stype, kDefaultStorage) &&
        type_assign(&weight_grad_stype, kRowSparseStorage) &&
        dispatch_mode_assign(dispatch_mode, DispatchMode::kFComputeEx)) {
      dispatched = true;
    }
  }
  const SparseEmbeddingParam& param = nnvm::get<SparseEmbeddingParam>(attrs.parsed);
  if (param.deterministic) {
    common::LogOnce("_SparseEmbedding_backward with deterministic=True may reduce "
                    "speed significantly");
  }
  return dispatched;
}

/*! \brief name the struct Take instead of take
 * to avoid conflict with the take function in mshadow
 */
template<bool clip = true>
struct Take {
  /*!
   * \brief Map function for take operator
   * \param i           global thread id
   * \param out_data    ptr to output buffer
   * \param in_data     ptr to input buffer
   * \param idx         ptr to indices buffer
   * \param in_ndims    # of dims of input tensor
   * \param out_ndims   # of dims of output tensor
   * \param idx_ndims   # of dims of indices tensor
   * \param axis_dim    dim size of the axis dimension
   * \param axis        axis id
   */
  template<typename DType, typename IType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, const DType* in_data, const IType* idx,
                                  const mshadow::Shape<10> in_stride,
                                  const mshadow::Shape<10> out_stride,
                                  const int in_ndims, const int out_ndims, const int idx_ndims,
                                  const int axis_dim, const int axis) {
    // i is the global flattened index in the output
    const int64_t out_head_index = (axis == 0) ? 0 : (i / out_stride[axis - 1]);
    const int64_t out_rest_index = (axis == 0) ? i : (i % out_stride[axis - 1]);
    const int64_t out_mid_index = out_rest_index / in_stride[axis];
    const int64_t out_tail_index = (axis == in_ndims - 1) ?
                                   0 : (out_rest_index % in_stride[axis]);
    int64_t idx_index = static_cast<int64_t>(idx[out_mid_index]);
    if (clip) {
      idx_index = (idx_index < 0) ? 0 : idx_index;
      idx_index = (idx_index > axis_dim - 1) ? (axis_dim - 1) : idx_index;
    }
    idx_index %= axis_dim;
    idx_index += (idx_index < 0) ? axis_dim : 0;
    const int64_t in_tail_index = out_tail_index;
    const int64_t in_head_index = out_head_index;
    int64_t in_src_index = in_tail_index + idx_index * in_stride[axis];
    in_src_index += (axis == 0) ? 0 : in_head_index * in_stride[axis - 1];
    out_data[i] = in_data[in_src_index];
  }
};

// Embedding forward implementation with dense weight
template<typename xpu>
void EmbeddingOpForwardDnsImpl(mshadow::Stream<xpu>* s,
                               const TBlob& data,
                               const TBlob& weight,
                               const OpReqType req,
                               const TBlob& output);

template<int req>
struct TakeRspKernel {
  /*!
   * \brief
   * \param i           thread id
   * \param data        input data
   * \param out         output
   * \param weight_idx  indices of rsp weight
   * \param weight_data data of rsp weight
   * \param row_length  number of elements per row
   * \param nnr         number of non-zero rows
   */
  template<typename DType, typename IType, typename RType>
  MSHADOW_XINLINE static void Map(int i,
                                  const IType* data,
                                  DType* out,
                                  const RType* weight_idx,
                                  const DType* weight_data,
                                  const nnvm::dim_t row_length,
                                  const nnvm::dim_t nnr) {
    using nnvm::dim_t;
    const dim_t val = static_cast<dim_t>(data[i]);
    const DType zero = 0;
    // Use binary search to find the lower_bound of val in weight_idx array
    // (adapted based on the binary search in dot kernel)
    const RType* first = weight_idx;
    const RType* last = weight_idx + nnr;
    const RType* it;
    dim_t count = last - first, step;
    while (count > 0) {
      it = first;
      step = count / 2;
      it += step;
      if (*it < val) {
        first = ++it;
        count -= step + 1;
      } else {
        count = step;
      }
    }
    // end of binary search
    const dim_t idx_offset = first - weight_idx;
    const dim_t out_offset = i * row_length;
    const dim_t weight_offset = idx_offset * row_length;
    // target idx might be missing in weight.idx. For example,
    // weight.idx = [5,10] and data = [3,7], so binary search fails to
    // find any matching indices in weight_idx.
    if (idx_offset >= nnr || *(weight_idx + idx_offset) > val) {
      // val not found, fill zeros
      for (int j = 0; j < row_length; j++) {
        KERNEL_ASSIGN(out[out_offset + j], req, zero);
      }
    } else {
      for (int j = 0; j < row_length; j++) {
        KERNEL_ASSIGN(out[out_offset + j], req, weight_data[weight_offset + j]);
      }
    }
  }
};

template<typename xpu>
inline void EmbeddingOpForwardRspImpl(mshadow::Stream<xpu>* s,
                                      const TBlob& data,
                                      const NDArray& weight,
                                      const OpReqType req,
                                      const TBlob& output) {
  using namespace mxnet_op;
  using namespace rowsparse;
  MSHADOW_TYPE_SWITCH(output.type_flag_, DType, {
    MSHADOW_TYPE_SWITCH(data.type_flag_, IType, {
      MSHADOW_TYPE_SWITCH(weight.aux_type(kIdx), RType, {
        MXNET_ASSIGN_REQ_SWITCH(req, req_t, {
          size_t data_size = data.shape_.Size();
          // only using the second dim since weight.ndim() == 2
          const nnvm::dim_t row_length = weight.shape()[1];
          Kernel<TakeRspKernel<req_t>, xpu>::Launch(s, data_size, data.dptr<IType>(),
                                                    output.dptr<DType>(),
                                                    weight.aux_data(kIdx).dptr<RType>(),
                                                    weight.data().dptr<DType>(),
                                                    row_length, weight.aux_shape(kIdx)[0]);
        });
      });
    });
  });
}


// Embedding forward implementation with row_sparse weight
template<typename xpu>
void SparseEmbeddingOpForwardRspImpl(const OpContext& ctx,
                                     const TBlob& data,
                                     const NDArray& weight,
                                     const OpReqType req,
                                     const TBlob& output);

template<typename xpu>
void EmbeddingOpForward(const nnvm::NodeAttrs& attrs,
                        const OpContext& ctx,
                        const std::vector<TBlob>& inputs,
                        const std::vector<OpReqType>& req,
                        const std::vector<TBlob>& outputs) {
  CHECK_EQ(req[embedding::kOut], kWriteTo);
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 1U);
  CHECK_EQ(inputs[embedding::kWeight].ndim(), 2U)
          << "Embedding layer expects its weight to be two-dimensional. "
          << inputs[embedding::kWeight].ndim()
          << " dimensional input is given instead";
  mshadow::Stream<xpu> *s = ctx.get_stream<xpu>();
  EmbeddingOpForwardDnsImpl<xpu>(s, inputs[embedding::kData], inputs[embedding::kWeight],
                                 req[embedding::kOut], outputs[embedding::kOut]);
}

template<typename xpu>
void SparseEmbeddingOpForwardEx(const nnvm::NodeAttrs& attrs,
                                const OpContext& ctx,
                                const std::vector<NDArray>& inputs,
                                const std::vector<OpReqType>& req,
                                const std::vector<NDArray>& outputs) {
  CHECK_EQ(req[embedding::kOut], kWriteTo);
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 1U);
  const NDArray& data = inputs[embedding::kData];
  const NDArray& weight = inputs[embedding::kWeight];
  const NDArray& out = outputs[embedding::kOut];
  CHECK_EQ(weight.shape().ndim(), 2U)
          << "Embedding layer expects its weight to be two-dimensional. "
          << weight.shape().ndim() << " dimensional input is given instead";
  const auto data_stype = data.storage_type();
  const auto weight_stype = weight.storage_type();
  const auto out_stype = out.storage_type();
  if (data_stype == kDefaultStorage && weight_stype == kRowSparseStorage &&
      out_stype == kDefaultStorage) {
    // dns, rsp -> dns
    SparseEmbeddingOpForwardRspImpl<xpu>(ctx, data.data(), weight, req[0], out.data());
  } else if (data_stype == kDefaultStorage && weight_stype == kDefaultStorage &&
             out_stype == kDefaultStorage) {
    // dns, dns -> dns
    EmbeddingOpForwardDnsImpl<xpu>(ctx.get_stream<xpu>(), data.data(), weight.data(),
                                   req[0], out.data());
  } else {
    LogUnimplementedOp(attrs, ctx, inputs, req, outputs);
  }
}

/*! \brief cast to type and clip to range [0, K - 1]
 */
struct tcast_clip {
  template<typename OType, typename IType>
  MSHADOW_XINLINE static void Map(int i, OType* out_data, const IType* in_data,
                                  const OType K) {
    OType j = static_cast<OType>(in_data[i]);
    if (j <= 0) j = 0;
    else if (j >= K) j = K - 1;
    out_data[i] = j;
  }
};

template<typename xpu>
void EmbeddingOpBackward(const nnvm::NodeAttrs& attrs,
                         const OpContext& ctx,
                         const std::vector<TBlob>& inputs,
                         const std::vector<OpReqType>& req,
                         const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 2U);
  CHECK_EQ(req[embedding::kData], kNullOp)
          << "Embedding layer doesn't support calculate data gradient";
  CHECK_EQ(outputs[1].type_flag_, inputs[0].type_flag_);

  const TShape& ishape = inputs[1].shape_;
  const TShape& oshape = inputs[0].shape_;

  Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_TYPE_SWITCH(outputs[1].type_flag_, DType, {
    MSHADOW_TYPE_SWITCH(inputs[1].type_flag_, IType, {
      Tensor < xpu, 1, IType > data = inputs[1].get_with_shape<xpu, 1, IType>(
        Shape1(ishape.ProdShape(0, ishape.ndim())), s);
      Tensor<xpu, 2, DType> grad_out = inputs[0].get_with_shape<xpu, 2, DType>(
      Shape2(oshape.ProdShape(0, oshape.ndim()-1), oshape[oshape.ndim()-1]), s);
      Tensor<xpu, 2, DType> grad_in = outputs[1].get<xpu, 2, DType>(s);


      if (req[embedding::kWeight] == kWriteTo || req[embedding::kWeight] == kAddTo) {
        if (req[embedding::kWeight] == kWriteTo) {
          grad_in = scalar<DType>(0.0f);
        }
        AddTakeGrad(grad_in, data, grad_out);
      } else {
        LOG(FATAL) << "wrong req";
      }
    });
  });
}

struct AddTakeGradRspKernel {
  /*!
   * \brief Each thread i is responsible for row slices in [segment_start, segment_end)
            of the result gradient
   * \param tid             global thread id
   * \param grad            the gradient to calculate
   * \param prefix_sum      the inclusive prefix sum of row ids of the gradient
   * \param ograd           output gradient
   * \param row_length      the length of the row slices of the gradient
   * \param data_val        the values of input data
   * \param data_size       number of values of input data
   * \param segment_length  the length of row segment to process for each thread
   * \param nnr             total number of non-zero rows of result gradient
   */
  template<typename DType, typename IType>
  MSHADOW_CINLINE static void Map(int tid,
                                  DType* grad,
                                  const nnvm::dim_t* prefix_sum,
                                  const DType* ograd,
                                  const nnvm::dim_t row_length,
                                  const IType* data_val,
                                  const nnvm::dim_t data_size,
                                  const nnvm::dim_t segment_length,
                                  const nnvm::dim_t nnr) {
    using nnvm::dim_t;
    dim_t segment_start = tid * segment_length;
    dim_t segment_end = std::min(nnr, segment_start + segment_length);
    // scan all data
    for (dim_t data_i = 0; data_i < data_size; data_i++) {
      dim_t data = static_cast<dim_t>(data_val[data_i]);
      dim_t grad_row_id = prefix_sum[data] - 1;
      if (grad_row_id < segment_start || grad_row_id >= segment_end) continue;
      // no projection is performed
      dim_t ograd_i = data_i * row_length;
      dim_t grad_i = grad_row_id * row_length;
      for (dim_t offset = 0; offset < row_length; offset++) {
        grad[grad_i + offset] += ograd[ograd_i + offset];
      }
    }
  }
};

template<typename xpu>
inline void SparseEmbeddingOpBackwardRspImpl(const bool deterministic,
                                             const OpContext& ctx,
                                             const TBlob& ograd,
                                             const TBlob& data,
                                             const OpReqType req,
                                             const NDArray& output);

template<typename xpu>
void EmbeddingOpBackwardEx(const nnvm::NodeAttrs& attrs,
                           const OpContext& ctx,
                           const std::vector<NDArray>& inputs,
                           const std::vector<OpReqType>& req,
                           const std::vector<NDArray>& outputs) {
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 2U);
  const NDArray& weight_grad = outputs[1];
  const NDArray& ograd = inputs[0];
  const NDArray& data = inputs[1];
  // check dtype
  CHECK_EQ(weight_grad.dtype(), ograd.dtype());
  // check req
  CHECK_EQ(req[embedding::kData], kNullOp)
          << "Embedding layer doesn't support calculate data gradient";
  if (data.storage_type() == kDefaultStorage && ograd.storage_type() == kDefaultStorage &&
      weight_grad.storage_type() == kRowSparseStorage) {
    SparseEmbeddingOpBackwardRspImpl<xpu>(true, ctx, ograd.data(), data.data(),
                                          req[embedding::kWeight], weight_grad);
  } else {
    LogUnimplementedOp(attrs, ctx, inputs, req, outputs);
  }
}

template<typename xpu>
void SparseEmbeddingOpBackwardEx(const nnvm::NodeAttrs& attrs,
                                 const OpContext& ctx,
                                 const std::vector<NDArray>& inputs,
                                 const std::vector<OpReqType>& req,
                                 const std::vector<NDArray>& outputs) {
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 2U);
  const NDArray& weight_grad = outputs[1];
  const NDArray& ograd = inputs[0];
  const NDArray& data = inputs[1];
  // check dtype
  CHECK_EQ(weight_grad.dtype(), ograd.dtype());
  // check req
  CHECK_EQ(req[embedding::kData], kNullOp)
          << "SparseEmbedding layer doesn't support calculate data gradient";
  const SparseEmbeddingParam& param = nnvm::get<SparseEmbeddingParam>(attrs.parsed);
  if (data.storage_type() == kDefaultStorage && ograd.storage_type() == kDefaultStorage &&
      weight_grad.storage_type() == kRowSparseStorage) {
    SparseEmbeddingOpBackwardRspImpl<xpu>(param.deterministic, ctx, ograd.data(), data.data(),
                                          req[embedding::kWeight], weight_grad);
  } else {
    LogUnimplementedOp(attrs, ctx, inputs, req, outputs);
  }
}

namespace take_ {  // to avoid name conflict
enum TakeOpInputs {kArr, kIdx};
enum TakeOpOutputs {kOut};
enum TakeOpResource {kTempSpace};
enum TakeOpMode {kRaise, kWrap, kClip};
}  // namespace take_

// TODO(somebody): behaviors specified by params
struct TakeParam: public dmlc::Parameter<TakeParam> {
  int axis;
  int mode;
  DMLC_DECLARE_PARAMETER(TakeParam) {
    DMLC_DECLARE_FIELD(axis)
    .set_default(0)
    .describe("The axis of input array to be taken."
              "For input tensor of rank r, it could be in the range of [-r, r-1]");
    DMLC_DECLARE_FIELD(mode)
    .add_enum("raise", take_::kRaise)
    .add_enum("wrap", take_::kWrap)
    .add_enum("clip", take_::kClip)
    .set_default(take_::kClip)
    .describe("Specify how out-of-bound indices bahave. Default is \"clip\"."
              " \"clip\" means clip to the range. So, if all indices mentioned are too large,"
              " they are replaced by the index that addresses the last element along an axis. "
              " \"wrap\" means to wrap around. "
              " \"raise\" means to raise an error, not supported yet.");
  }
};

inline bool TakeOpShape(const nnvm::NodeAttrs& attrs,
                        std::vector<TShape> *in_attrs,
                        std::vector<TShape> *out_attrs) {
  using namespace mshadow;
  const TShape &arrshape = (*in_attrs)[take_::kArr];
  const TShape &idxshape = (*in_attrs)[take_::kIdx];
  if (idxshape.ndim() == 0U || idxshape.Size() == 0U) return false;
  const TakeParam& param = nnvm::get<TakeParam>(attrs.parsed);
  if (param.mode == take_::kRaise) {
    LOG(FATAL) << "Raise is not supported for the time being...";
  }
  CHECK(param.axis >= -1 * (int)arrshape.ndim() && param.axis < (int)arrshape.ndim())
    << "Axis should be in the range of [-r, r-1] where r is the rank of input tensor";

  out_attrs->clear();

  const index_t actual_axis = param.axis + ((param.axis < 0) ? arrshape.ndim() : 0);
  TShape oshape(idxshape.ndim() + arrshape.ndim() - 1);
  for (index_t i = 0; i < idxshape.ndim(); ++i) {
    oshape[i + actual_axis] = idxshape[i];
  }
  for (index_t i = 0; i < arrshape.ndim(); i++) {
    if (i < actual_axis) {
      oshape[i] = arrshape[i];
    } else if (i > actual_axis) {
      oshape[i + idxshape.ndim() - 1] = arrshape[i];
    }
  }
  out_attrs->push_back(oshape);
  return true;
}

inline bool TakeOpType(const nnvm::NodeAttrs& attrs,
                       std::vector<int> *in_attrs,
                       std::vector<int> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  CHECK_NE((*in_attrs)[1], -1) << "Index type must be set for take operator";

  TYPE_ASSIGN_CHECK(*out_attrs, 0, (*in_attrs)[0]);
  TYPE_ASSIGN_CHECK(*in_attrs, 0, (*out_attrs)[0]);
  return (*in_attrs)[0] != -1;
}

// storage type inference function for take
inline bool TakeOpForwardStorageType(const nnvm::NodeAttrs& attrs,
                                     const int dev_mask,
                                     DispatchMode* dispatch_mode,
                                     std::vector<int>* in_attrs,
                                     std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  const int& idx_stype = in_attrs->at(take_::kIdx);
  const int& arr_stype = in_attrs->at(take_::kArr);
  int& out_stype = out_attrs->at(take_::kOut);
  bool dispatched = false;
  const TakeParam& param = nnvm::get<TakeParam>(attrs.parsed);
  if (!dispatched && idx_stype == kDefaultStorage && arr_stype == kDefaultStorage) {
    // dns, dns -> dns
    dispatched = storage_type_assign(&out_stype, kDefaultStorage,
                                     dispatch_mode, DispatchMode::kFCompute);
  }
  if (!dispatched && idx_stype == kDefaultStorage && arr_stype == kCSRStorage &&
      param.axis == 0 && (param.mode == take_::kWrap || param.mode == take_::kClip)) {
    // take(dns, csr, axis=0) -> csr
    dispatched = storage_type_assign(&out_stype, kCSRStorage,
                                     dispatch_mode, DispatchMode::kFComputeEx);
  }
  if (!dispatched) {
    dispatched = dispatch_fallback(out_attrs, dispatch_mode);
  }
  return dispatched;
}


template<typename xpu>
void TakeOpForwardCsrImpl(const TakeParam& params,
                          const OpContext& ctx,
                          const TBlob& idx,
                          const NDArray& arr,
                          OpReqType req,
                          const NDArray& output);


template<typename xpu>
void TakeOpForwardEx(const nnvm::NodeAttrs& attrs,
                     const OpContext& ctx,
                     const std::vector<NDArray>& inputs,
                     const std::vector<OpReqType>& req,
                     const std::vector<NDArray>& outputs) {
  CHECK_EQ(req[take_::kOut], kWriteTo);
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 1U);
  const NDArray& idx = inputs[take_::kIdx];
  const NDArray& arr = inputs[take_::kArr];
  const NDArray& out = outputs[take_::kOut];
  const auto idx_stype = idx.storage_type();
  const auto arr_stype = arr.storage_type();
  const auto out_stype = out.storage_type();
  const auto params = nnvm::get<TakeParam>(attrs.parsed);
  if (idx_stype == kDefaultStorage && arr_stype == kCSRStorage &&
      out_stype == kCSRStorage) {
    // dns, csr -> csr
    TakeOpForwardCsrImpl<xpu>(params, ctx, idx.data(), arr, req[0], out);
  } else {
    LogUnimplementedOp(attrs, ctx, inputs, req, outputs);
  }
}

template<typename xpu>
void TakeOpForward(const nnvm::NodeAttrs& attrs,
                   const OpContext& ctx,
                   const std::vector<TBlob>& inputs,
                   const std::vector<OpReqType>& req,
                   const std::vector<TBlob>& outputs);

struct TakeGradGeneralKernel {
  /*!
   * \brief Map function for general case of take grad
   * \param tid           global thread id
   * \param arr_grad      ptr to in_grad
   * \param ograd         ptr to out_grad
   * \param src_indptr    ptr to indptr to src indices
   * \param original_idx  ptr to original indices of the inputs
   * \param in_strides    strides of inputs
   * \param out_strides   strides of outputs
   * \param in_ndims      # of dims of input tensor
   * \param out_ndims     # of dims of output tensor
   * \param idx_ndims     # of dims of indices tensor
   * \param axis_dim      dim size of the axis dimension
   * \param axis          axis id
   */
  template<typename DType, typename IType>
  MSHADOW_XINLINE static void Map(int tid, DType* arr_grad, const DType* ograd,
                                  const IType* src_indptr, const IType* original_idx,
                                  mshadow::Shape<10> in_strides, mshadow::Shape<10> out_strides,
                                  const int in_ndims, const int out_ndims, const int idx_ndims,
                                  const int axis) {
    const int in_head_index = (axis == 0) ? 0 : tid / in_strides[axis - 1];
    const int in_rest_index = (axis == 0) ? tid : tid % in_strides[axis - 1];
    const int in_mid_index = in_rest_index / in_strides[axis];
    const int in_tail_index = (axis == in_ndims - 1) ?
                              0 : (in_rest_index % in_strides[axis]);
    for (IType i = src_indptr[in_mid_index]; i < src_indptr[in_mid_index + 1]; ++i) {
      const int out_mid_index = original_idx[i];
      int target = in_tail_index + out_mid_index * in_strides[axis];
      target += (axis == 0) ? 0 : in_head_index * out_strides[axis - 1];
      arr_grad[tid] += ograd[target];
    }
  }
};

template<bool clip = true>
void TakeOpBackwardImpl(mshadow::Stream<cpu>* s,
                        const OpContext& ctx,
                        const TBlob& arr,
                        const TBlob& idx,
                        const TBlob& ograd,
                        const int axis) {
  using namespace mxnet_op;
  using namespace mshadow;
  CHECK(axis != 0) << "axis == 0 case should be dispatched to the legacy implementation";
  const TShape& arrshape = arr.shape_;
  const TShape& idxshape = idx.shape_;
  const TShape& oshape = ograd.shape_;
  MSHADOW_TYPE_SWITCH(idx.type_flag_, IType, {
    // get size of temporary storage for sort
    int* src_indptr_ptr = nullptr;
    size_t temp_storage_bytes = SortByKeyWorkspaceSize<int, int, cpu>(idxshape.Size());
    size_t original_idx_bytes = idxshape.Size() * sizeof(int);
    size_t src_indptr_bytes = (arrshape[axis] + 1) * sizeof(int);
    size_t workspace_bytes = src_indptr_bytes + 2 * original_idx_bytes + temp_storage_bytes;
    Tensor<cpu, 1, char> workspace =
      ctx.requested[0].get_space_typed<cpu, 1, char>(Shape1(workspace_bytes), s);
    int* sorted_idx_ptr = reinterpret_cast<int*>(workspace.dptr_);
    int* original_idx_ptr = reinterpret_cast<int*>(workspace.dptr_ + original_idx_bytes);
    src_indptr_ptr = reinterpret_cast<int*>(workspace.dptr_ + 2 * original_idx_bytes);
    Tensor<cpu, 1, char> temp_storage(
      workspace.dptr_ + 2 * original_idx_bytes + src_indptr_bytes, Shape1(temp_storage_bytes), s);
    // Reset indptr to zero
    Kernel<set_zero, cpu>::Launch(s, arrshape[axis] + 1, src_indptr_ptr);
    // Fill original_idx
    Kernel<range_fwd, cpu>::Launch(s, idxshape.Size(), 1, 0, 1, kWriteTo, original_idx_ptr);
    // Fill sorted_idx_ptr with unsorted copy of idx
    Kernel<mshadow_op::identity_with_cast, cpu>::Launch(
      s, idxshape.Size(), sorted_idx_ptr, idx.dptr<IType>());
    if (clip) {
      Kernel<op_with_req<mshadow_op::clip, kWriteTo>, cpu>::Launch(
        s, idxshape.Size(), sorted_idx_ptr, sorted_idx_ptr,
        0, static_cast<int>(arrshape[axis] - 1));
    } else {
      Kernel<op_with_req<mshadow_op::mod, kWriteTo>, cpu>::Launch(
        s, idxshape.Size(), sorted_idx_ptr, sorted_idx_ptr, static_cast<int>(arrshape[axis]));
    }
    Tensor<cpu, 1, int> original_idx(original_idx_ptr, Shape1(idxshape.Size()), s);
    int num_bits = common::ilog2ui(static_cast<unsigned int>(idxshape.Size()) - 1);
    Tensor<cpu, 1, int> sorted_idx(sorted_idx_ptr, Shape1(idxshape.Size()), s);
    SortByKey(sorted_idx, original_idx, true, &temp_storage, 0, num_bits);
    for (size_t i = 0; i < idxshape.Size(); ++i) {
      src_indptr_ptr[sorted_idx_ptr[i] + 1] += 1;
    }
    for (int i = 0; i < arrshape[axis]; ++i) {
      src_indptr_ptr[i + 1] += src_indptr_ptr[i];
    }
    Shape<10> in_strides;
    int stride = 1;
    for (int i = arrshape.ndim() - 1; i >= 0; stride *= arrshape[i], --i) {
      in_strides[i] = stride;
    }
    Shape<10> out_strides;
    stride = 1;
    for (int i = oshape.ndim() - 1; i >= 0; stride *= oshape[i], --i) {
      out_strides[i] = stride;
    }
    MSHADOW_TYPE_SWITCH(arr.type_flag_, DType, {
      Kernel<TakeGradGeneralKernel, cpu>::Launch(
        s, arrshape.Size(), arr.dptr<DType>(), ograd.dptr<DType>(), src_indptr_ptr,
        original_idx_ptr, in_strides, out_strides,
        arrshape.ndim(), oshape.ndim(), idxshape.ndim(), axis);
    });
  });
}

#ifdef __HIPCC__
template<bool clip = true>
void TakeOpBackwardImpl(mshadow::Stream<gpu>* s,
                        const OpContext& ctx,
                        const TBlob& arr,
                        const TBlob& idx,
                        const TBlob& ograd,
                        const int axis) {
  using namespace mxnet_op;
  using namespace mshadow;
  CHECK(axis != 0) << "axis == 0 case should be dispatched to the legacy implementation";
  const TShape& arrshape = arr.shape_;
  const TShape& idxshape = idx.shape_;
  const TShape& oshape = ograd.shape_;
  MSHADOW_TYPE_SWITCH(idx.type_flag_, IType, {
    // get size of temporary storage for sort
    char* temp_storage_ptr = nullptr;
    size_t scan_temp_storage_bytes = 0;
    int* src_indptr_ptr = nullptr;
    hipcub::DeviceScan::ExclusiveSum(temp_storage_ptr,
                                  scan_temp_storage_bytes,
                                  src_indptr_ptr,
                                  src_indptr_ptr,
                                  arrshape[axis] + 1,
                                  mshadow::Stream<gpu>::GetStream(s));
    size_t sort_temp_storage_bytes = SortByKeyWorkspaceSize<int, int, gpu>(idxshape.Size());
    size_t histo_temp_storage_bytes = 0;
    int* sorted_idx_ptr = nullptr;
    hipcub::DeviceHistogram::HistogramEven(temp_storage_ptr,
                                        histo_temp_storage_bytes,
                                        sorted_idx_ptr,
                                        src_indptr_ptr,
                                        static_cast<int>(arrshape[axis] + 1),
                                        0,
                                        static_cast<int>(arrshape[axis] + 1),
                                        static_cast<int>(idxshape.Size()),
                                        mshadow::Stream<gpu>::GetStream(s));
    size_t temp_storage_bytes = max(scan_temp_storage_bytes, sort_temp_storage_bytes);
    temp_storage_bytes = max(temp_storage_bytes, histo_temp_storage_bytes);
    size_t original_idx_bytes = idxshape.Size() * sizeof(int);
    size_t src_indptr_bytes = (arrshape[axis] + 1) * sizeof(int);
    size_t workspace_bytes = src_indptr_bytes + 2 * original_idx_bytes + temp_storage_bytes;
    Tensor<gpu, 1, char> workspace =
      ctx.requested[0].get_space_typed<gpu, 1, char>(Shape1(workspace_bytes), s);
    sorted_idx_ptr = reinterpret_cast<int*>(workspace.dptr_);
    int* original_idx_ptr = reinterpret_cast<int*>(workspace.dptr_ + original_idx_bytes);
    src_indptr_ptr = reinterpret_cast<int*>(workspace.dptr_ + 2 * original_idx_bytes);
    temp_storage_ptr = workspace.dptr_ + 2 * original_idx_bytes + src_indptr_bytes;
    // Reset indptr to zero
    Kernel<set_zero, gpu>::Launch(s, arrshape[axis] + 1, src_indptr_ptr);
    // Fill original_idx
    Kernel<range_fwd, gpu>::Launch(
      s, idxshape.Size(), 1, static_cast<int>(0), static_cast<int>(1),
      kWriteTo, original_idx_ptr);
    // Fill sorted_idx_ptr with unsorted copy of idx
    Kernel<mshadow_op::identity_with_cast, gpu>::Launch(
      s, idxshape.Size(), sorted_idx_ptr, idx.dptr<IType>());
    if (clip) {
      Kernel<op_with_req<mshadow_op::clip, kWriteTo>, gpu>::Launch(
        s, idxshape.Size(), sorted_idx_ptr, sorted_idx_ptr, 0, static_cast<int>(arrshape[axis]));
    } else {
      Kernel<op_with_req<mshadow_op::mod, kWriteTo>, gpu>::Launch(
        s, idxshape.Size(), sorted_idx_ptr, sorted_idx_ptr, static_cast<int>(arrshape[axis]));
    }
    Tensor<gpu, 1, int> original_idx(original_idx_ptr, Shape1(idxshape.Size()), s);
    Tensor<gpu, 1, char> temp_storage(temp_storage_ptr, Shape1(temp_storage_bytes), s);
    int num_bits = common::ilog2ui(static_cast<unsigned int>(idxshape.Size()) - 1);
    Tensor<gpu, 1, int> sorted_idx(sorted_idx_ptr, Shape1(idxshape.Size()), s);
    SortByKey(sorted_idx, original_idx, true, &temp_storage, 0, num_bits);
    hipcub::DeviceScan::ExclusiveSum(temp_storage_ptr,
                                  temp_storage_bytes,
                                  src_indptr_ptr,
                                  src_indptr_ptr,
                                  arrshape[axis] + 1,
                                  mshadow::Stream<gpu>::GetStream(s));

    Shape<10> in_strides;
    int stride = 1;
    for (int i = arrshape.ndim() - 1; i >= 0; stride *= arrshape[i], --i) {
      in_strides[i] = stride;
    }
    Shape<10> out_strides;
    stride = 1;
    for (int i = oshape.ndim() - 1; i >= 0; stride *= oshape[i], --i) {
      out_strides[i] = stride;
    }
    MSHADOW_TYPE_SWITCH(arr.type_flag_, DType, {
      Kernel<TakeGradGeneralKernel, gpu>::Launch(
        s, arrshape.Size(), arr.dptr<DType>(), ograd.dptr<DType>(),
        src_indptr_ptr, original_idx_ptr, in_strides, out_strides,
        arrshape.ndim(), oshape.ndim(), idxshape.ndim(), axis);
    });
  });
}
#endif

template<typename xpu>
void TakeOpBackward(const nnvm::NodeAttrs& attrs,
                    const OpContext& ctx,
                    const std::vector<TBlob>& inputs,
                    const std::vector<OpReqType>& req,
                    const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 2U);
  CHECK_NE(req[take_::kIdx], kAddTo)
    << "take layer doesn't support gradient of req type kAddTo to index";

  const TakeParam& param = nnvm::get<TakeParam>(attrs.parsed);

  // grad_out is the gradient of the outputs in the feed-forward
  // grad_in is the gradient of the inputs in the feed-forward
  Stream<xpu> *s = ctx.get_stream<xpu>();

  MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {  // output data type
    MSHADOW_TYPE_SWITCH(inputs[1].type_flag_, IType, {  // index data type
      // inputs are specified in the .cc file, which are the gradients from
      // the upper layer and the input index
      // outputs are the gradients of inputs in the feed-forward pass
      const TShape& idxshape = inputs[1].shape_;
      const TShape& arrshape = outputs[0].shape_;
      const TShape& oshape = inputs[0].shape_;

      if (req[take_::kIdx] != kNullOp) {
        mxnet_op::Kernel<mxnet_op::set_zero, xpu>::Launch(
          s, idxshape.Size(), outputs[take_::kIdx].dptr<IType>());
      }

      const int actual_axis = param.axis + ((param.axis < 0) ? arrshape.ndim() : 0);

      int idxndim = idxshape.ndim();
      Tensor<xpu, 1, IType> idx = inputs[1].get_with_shape<xpu, 1, IType>(
          Shape1(idxshape.ProdShape(0, idxndim)), s);
      Tensor<xpu, 2, DType> grad_out = inputs[0].get_with_shape<xpu, 2, DType>(
          Shape2(oshape.ProdShape(0, idxndim), oshape.ProdShape(idxndim, oshape.ndim())), s);
      Tensor<xpu, 2, DType> grad_in = outputs[0].get_with_shape<xpu, 2, DType>(
          Shape2(arrshape[0], arrshape.ProdShape(1, arrshape.ndim())), s);

      // re-using the previous code for axis = 0 case
      if (actual_axis == 0) {
        if (req[take_::kArr] == kWriteTo || req[take_::kArr] == kAddTo) {
          if (req[take_::kArr] == kWriteTo) {
            grad_in = scalar<DType>(0.0f);
          }
          AddTakeGrad(grad_in, idx, grad_out);
        } else {
          LOG(FATAL) << "wrong req";
        }
      // for all other cases
      } else {
        const TBlob& idx = inputs[1];
        const TBlob& arr = outputs[0];
        const TBlob& ograd = inputs[0];

        if (param.mode == take_::kClip) {
          TakeOpBackwardImpl<true>(s, ctx, arr, idx, ograd, actual_axis);
        } else {
          TakeOpBackwardImpl<false>(s, ctx, arr, idx, ograd, actual_axis);
        }
      }
    });
  });
}

inline bool BatchTakeOpShape(const nnvm::NodeAttrs& attrs,
                             std::vector<TShape> *in_attrs,
                             std::vector<TShape> *out_attrs) {
  LOG(INFO) << "batch_take is deprecated. Please use pick instead.";
  CHECK_EQ(in_attrs->size(), 2U) << "BatchTake op requires two inputs";
  if ((*in_attrs)[1].ndim() != 0) {
    SHAPE_ASSIGN_CHECK(*out_attrs, 0, (*in_attrs)[1]);
  } else if ((*out_attrs)[0].ndim() != 0) {
    SHAPE_ASSIGN_CHECK(*in_attrs, 1, (*out_attrs)[0]);
  }
  if ((*in_attrs)[0].ndim() == 0) return false;
  CHECK_GE((*in_attrs)[0].ndim(), 2U) << "Data array must have at least 2 dimensional";
  if ((*out_attrs)[0].ndim() == 0) return false;
  CHECK_EQ((*in_attrs)[0].Size()/(*in_attrs)[0][(*in_attrs)[0].ndim()-1],
           (*out_attrs)[0].Size())
    << "Index array's size must be the same as data array's size excluding the first dimension";
  return true;
}

inline bool BatchTakeOpType(const nnvm::NodeAttrs& attrs,
                          std::vector<int> *in_attrs,
                          std::vector<int> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  if ((*in_attrs)[0] != -1) {
    TYPE_ASSIGN_CHECK(*out_attrs, 0, (*in_attrs)[0]);
  } else if ((*out_attrs)[0] != -1) {
    TYPE_ASSIGN_CHECK(*in_attrs, 0, (*out_attrs)[0]);
  }
  TYPE_ASSIGN_CHECK(*in_attrs, 1, mshadow::kInt32);
  return true;
}

/*! \brief take scalar value from 2d data array */
template<int req>
struct batch_take {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, DType* out, const DType* a,
                                  const int *idx, int M) {
    int j = idx[i];
    if (j < 0) j = 0;
    else if (j >= M) j = M-1;
    KERNEL_ASSIGN(out[i], req, a[i*M+j]);
  }
};

template<typename xpu>
void BatchTakeOpForward(const nnvm::NodeAttrs& attrs,
                        const OpContext& ctx,
                        const std::vector<TBlob>& inputs,
                        const std::vector<OpReqType>& req,
                        const std::vector<TBlob>& outputs) {
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 1U);
  CHECK_EQ(req.size(), 1U);
  using namespace mxnet_op;
  mshadow::Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
    MXNET_ASSIGN_REQ_SWITCH(req[0], req_type, {
      Kernel<batch_take<req_type>, xpu>::Launch(s, outputs[0].Size(), outputs[0].dptr<DType>(),
                                                inputs[0].dptr<DType>(), inputs[1].dptr<int>(),
                                                inputs[0].Size()/inputs[0].shape_[0]);
    });
  });
}

/*!
 * \brief The parameters of the one_hot operator.
 */
struct OneHotParam : public dmlc::Parameter<OneHotParam> {
  int depth;
  double on_value;
  double off_value;
  int axis;
  int dtype;
  DMLC_DECLARE_PARAMETER(OneHotParam) {
    DMLC_DECLARE_FIELD(depth)
      .describe("Depth of the one hot dimension.");
    DMLC_DECLARE_FIELD(on_value)
      .set_default(1.0f)
      .describe("The value assigned to the locations represented by indices.");
    DMLC_DECLARE_FIELD(off_value)
      .set_default(0.0f)
      .describe("The value assigned to the locations not represented by indices.");
    DMLC_DECLARE_FIELD(dtype).set_default(mshadow::kFloat32)
      MXNET_ADD_ALL_TYPES
      .describe("DType of the output");
  }
};

inline void GetOneHotParams(const OneHotParam& param, int* depth, double* on_value,
                            double* off_value, int* dtype) {
  *depth = param.depth;
  CHECK_GE(*depth, 0) << "Dimension size, depth, must be a non-negative integer";
  *on_value = param.on_value;
  *off_value = param.off_value;
  *dtype = param.dtype;
}

inline bool OneHotOpShape(const nnvm::NodeAttrs& attrs,
                          std::vector<TShape> *in_attrs,
                          std::vector<TShape> *out_attrs) {
  const OneHotParam& param = nnvm::get<OneHotParam>(attrs.parsed);
  CHECK_EQ(in_attrs->size(), 1U);
  CHECK_EQ(out_attrs->size(), 1U);
  // The shape of indices
  const TShape& ishape = (*in_attrs)[0];

  int depth = 0;
  double on_value = 1.0;
  double off_value = 0.0;
  int dtype = mshadow::kFloat32;
  GetOneHotParams(param, &depth, &on_value, &off_value, &dtype);

  TShape oshape(ishape.ndim() + 1);
  for (index_t i = 0; i < ishape.ndim(); ++i) {
    oshape[i] = ishape[i];
  }
  oshape[oshape.ndim()-1] = depth;
  SHAPE_ASSIGN_CHECK(*out_attrs, 0, oshape);
  return true;
}

inline bool OneHotOpType(const nnvm::NodeAttrs& attrs,
                         std::vector<int>* in_attrs,
                         std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 1U);
  CHECK_EQ(out_attrs->size(), 1U);
  CHECK_NE((*in_attrs)[0], -1) << "Index type must be set for one_hot operator";
  int depth = 0;
  double on_value = 1.0;
  double off_value = 0.0;
  int dtype = -1;
  const OneHotParam& param = nnvm::get<OneHotParam>(attrs.parsed);
  GetOneHotParams(param, &depth, &on_value, &off_value, &dtype);
  TYPE_ASSIGN_CHECK(*out_attrs, 0, dtype);  // assign output type

  return true;
}

template<int req>
struct one_hot {
  template<typename DType, typename IType>
  MSHADOW_XINLINE static void Map(int i, DType* out, const IType* indices,
                                  int depth, DType on_value) {
    int offset = i * depth;
    int j = static_cast<int>(indices[i]);
    if (j >= 0 && j < depth) {
      KERNEL_ASSIGN(out[offset+j], req, on_value);
    }
  }
};

template<typename xpu>
void OneHotOpForward(const nnvm::NodeAttrs& attrs,
                     const OpContext& ctx,
                     const std::vector<TBlob>& inputs,
                     const std::vector<OpReqType>& req,
                     const std::vector<TBlob>& outputs) {
  CHECK_EQ(inputs.size(), 1U);
  CHECK_EQ(outputs.size(), 1U);
  CHECK_EQ(req.size(), 1U);
  // The following line is needed to guard the situation when
  // an output array is empty on GPU. In that case, out.dptr() = 0x0
  if (outputs[0].Size() == 0) return;
  int depth = 0;
  double on_value = 1.0;
  double off_value = 0.0;
  int dtype = mshadow::kFloat32;
  const OneHotParam& param = nnvm::get<OneHotParam>(attrs.parsed);
  GetOneHotParams(param, &depth, &on_value, &off_value, &dtype);
  using namespace mxnet_op;
  using namespace mshadow::expr;
  mshadow::Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {  // output data type switch
    mshadow::Tensor<xpu, 1, DType> out = outputs[0].FlatTo1D<xpu, DType>(s);
    ASSIGN_DISPATCH(out, req[0], static_cast<DType>(off_value));
    MXNET_ASSIGN_REQ_SWITCH(req[0], req_type, {  // request type switch
      MSHADOW_TYPE_SWITCH(inputs[0].type_flag_, IType, {  // indices data type switch
        Kernel<one_hot<req_type>, xpu>::Launch(s, inputs[0].Size(), outputs[0].dptr<DType>(),
                                               inputs[0].dptr<IType>(), depth,
                                               static_cast<DType>(on_value));
      });
    });
  });
}

inline bool GatherNDShape(const nnvm::NodeAttrs& attrs,
                          std::vector<TShape> *in_attrs,
                          std::vector<TShape> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  // The shape of indices
  const TShape& dshape = (*in_attrs)[0];
  const TShape& ishape = (*in_attrs)[1];

  if (shape_is_none(dshape) || shape_is_none(ishape)) return false;

  CHECK_GT(ishape.ndim(), 1)
    << "gather_nd requires index tensor to have at least 2 dimensions";

  CHECK_LE(ishape[0], dshape.ndim())
    << "Number of indices exceeds data dimension";

  CHECK_LE(ishape[0], 10)
    << "gather_nd supports indexing along at most 10 dimensions.";

  TShape oshape(ishape.ndim() - 1 + dshape.ndim() - ishape[0]);

  for (size_t i = 0; i < ishape.ndim() - 1; ++i) oshape[i] = ishape[i+1];
  for (int i = 0; i < dshape.ndim() - ishape[0]; ++i) {
    oshape[ishape.ndim()-1+i] = dshape[ishape[0] + i];
  }

  SHAPE_ASSIGN_CHECK(*out_attrs, 0, oshape);
  return true;
}

inline bool GatherNDType(const nnvm::NodeAttrs& attrs,
                         std::vector<int>* in_attrs,
                         std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  TYPE_ASSIGN_CHECK(*out_attrs, 0, (*in_attrs)[0]);
  TYPE_ASSIGN_CHECK(*in_attrs, 0, (*out_attrs)[0]);
  return true;
}

struct gather_nd {
  template<typename DType, typename IType>
  MSHADOW_XINLINE static void Map(int i, OpReqType req, int N, int M, int K,
                                  const mshadow::Shape<10> strides,
                                  DType* out, const DType* data,
                                  const IType* indices) {
    int offset = 0;
    for (int j = 0; j < M; ++j) {
      offset += strides[j] * static_cast<int>(indices[j*N + i]);
    }
    for (int j = 0; j < K; ++j) {
      KERNEL_ASSIGN(out[i*K + j], req, data[offset+j]);
    }
  }
};

template<typename xpu>
void GatherNDForward(const nnvm::NodeAttrs& attrs,
                     const OpContext& ctx,
                     const std::vector<TBlob>& inputs,
                     const std::vector<OpReqType>& req,
                     const std::vector<TBlob>& outputs) {
  using namespace mxnet_op;
  using namespace mshadow;
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 1U);
  if (req[0] == kNullOp) return;
  mshadow::Stream<xpu> *s = ctx.get_stream<xpu>();
  const TShape& dshape = inputs[0].shape_;
  const TShape& ishape = inputs[1].shape_;
  int M = ishape[0];
  int N = ishape.Size() / M;
  int K = dshape.ProdShape(M, dshape.ndim());
  mshadow::Shape<10> strides;
  for (int i = M-1, stride = K; i >= 0; stride *= dshape[i], --i) strides[i] = stride;
  MSHADOW_TYPE_SWITCH(inputs[0].type_flag_, DType, {  // output data type switch
    MSHADOW_TYPE_SWITCH(inputs[1].type_flag_, IType, {  // indices data type switch
      Kernel<gather_nd, xpu>::Launch(
          s, N, req[0], N, M, K, strides, outputs[0].dptr<DType>(),
          inputs[0].dptr<DType>(), inputs[1].dptr<IType>());
    });
  });
}


struct ScatterNDParam : public dmlc::Parameter<ScatterNDParam> {
  TShape shape;
  DMLC_DECLARE_PARAMETER(ScatterNDParam) {
    DMLC_DECLARE_FIELD(shape)
      .describe("Shape of output.");
  }
};

inline bool ScatterNDShape(const nnvm::NodeAttrs& attrs,
                           std::vector<TShape> *in_attrs,
                           std::vector<TShape> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  const auto& params = dmlc::get<ScatterNDParam>(attrs.parsed);

  SHAPE_ASSIGN_CHECK(*out_attrs, 0, params.shape);

  const TShape& dshape = (*in_attrs)[0];
  const TShape& ishape = (*in_attrs)[1];
  const TShape& oshape = (*out_attrs)[0];

  if (shape_is_none(dshape) || shape_is_none(ishape) || shape_is_none(oshape)) return false;

  CHECK_GT(ishape.ndim(), 1)
    << "scatter_nd requires index tensor to have at least 2 dimensions";

  CHECK_LE(ishape[0], oshape.ndim())
    << "Number of indices exceeds output dimension in operator scatter_nd";

  CHECK_LE(ishape[0], 10)
    << "scatter_nd supports indexing along at most 10 dimensions.";

  bool valid = dshape.ndim() == ishape.ndim() - 1 + oshape.ndim() - ishape[0];

  for (size_t i = 0; i < ishape.ndim() - 1; ++i) {
    valid = valid && dshape[i] == ishape[i+1];
  }
  for (int i = 0; i < oshape.ndim() - ishape[0]; ++i) {
    valid = valid && dshape[ishape.ndim()-1+i] == oshape[ishape[0] + i];
  }

  CHECK(valid)
    << "Invalid data, indices, and output shape combination for scatter_nd: "
    << dshape << ", " << ishape << ", " << oshape;

  return true;
}

inline bool ScatterNDType(const nnvm::NodeAttrs& attrs,
                          std::vector<int>* in_attrs,
                          std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  TYPE_ASSIGN_CHECK(*out_attrs, 0, (*in_attrs)[0]);
  TYPE_ASSIGN_CHECK(*in_attrs, 0, (*out_attrs)[0]);
  return in_attrs->at(0) != -1 && in_attrs->at(1) != -1;
}

struct scatter_nd {
  template<typename DType, typename IType>
  MSHADOW_XINLINE static void Map(int i, OpReqType req, int N, int M, int K,
                                  const mshadow::Shape<10> strides,
                                  DType* out, const DType* data,
                                  const IType* indices) {
    int offset = 0;
    for (int j = 0; j < M; ++j) {
      offset += strides[j] * static_cast<int>(indices[j*N + i]);
    }
    for (int j = 0; j < K; ++j) {
      KERNEL_ASSIGN(out[offset+j], req, data[i*K + j]);
    }
  }
};

template<typename xpu>
void ScatterNDForward(const nnvm::NodeAttrs& attrs,
                      const OpContext& ctx,
                      const std::vector<TBlob>& inputs,
                      const std::vector<OpReqType>& req,
                      const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 1U);
  if (req[0] == kNullOp) return;
  mshadow::Stream<xpu> *s = ctx.get_stream<xpu>();
  const TShape& oshape = outputs[0].shape_;
  const TShape& ishape = inputs[1].shape_;
  int M = ishape[0];
  int N = ishape.Size() / M;
  int K = oshape.ProdShape(M, oshape.ndim());
  mshadow::Shape<10> strides;
  for (int i = M-1, stride = K; i >= 0; stride *= oshape[i], --i) strides[i] = stride;
  if (kWriteTo == req[0]) {
    Fill<true>(s, outputs[0], req[0], 0);
  }
  MSHADOW_TYPE_SWITCH(inputs[0].type_flag_, DType, {  // output data type switch
    MSHADOW_TYPE_SWITCH(inputs[1].type_flag_, IType, {  // indices data type switch
      mxnet_op::Kernel<scatter_nd, xpu>::Launch(
        s, N, req[0], N, M, K, strides, outputs[0].dptr<DType>(),
        inputs[0].dptr<DType>(), inputs[1].dptr<IType>());
    });
  });
}

template<typename DType, typename IType>
inline typename std::enable_if<(!std::is_same<DType, mshadow::half::half_t>::value), void>::type
GatherNDBackwardImpl(int N, int M, int K,
                     const mshadow::Shape<10> strides,
                     DType* out,
                     const DType* data,
                     const IType* indices,
                     mshadow::Stream<cpu> *s);

template<typename DType, typename IType>
inline typename std::enable_if<std::is_same<DType, mshadow::half::half_t>::value, void>::type
GatherNDBackwardImpl(int N, int M, int K,
                     const mshadow::Shape<10> strides,
                     DType* out,
                     const DType* data,
                     const IType* indices,
                     mshadow::Stream<cpu> *s);

template<typename DType, typename IType>
inline void GatherNDBackwardImpl(int N, int M, int K,
                                 const mshadow::Shape<10> strides,
                                 DType* out,
                                 const DType* data,
                                 const IType* indices,
                                 mshadow::Stream<gpu> *s);

template<typename xpu>
void GatherNDBackward(const nnvm::NodeAttrs& attrs,
                      const OpContext& ctx,
                      const std::vector<TBlob>& inputs,
                      const std::vector<OpReqType>& req,
                      const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 1U);
  if (req[0] == kNullOp) return;
  mshadow::Stream<xpu> *s = ctx.get_stream<xpu>();
  const TShape& oshape = outputs[0].shape_;
  const TShape& ishape = inputs[1].shape_;
  int M = ishape[0];
  int N = ishape.Size() / M;
  int K = oshape.ProdShape(M, oshape.ndim());
  mshadow::Shape<10> strides;
  for (int i = M-1, stride = K; i >= 0; stride *= oshape[i], --i) strides[i] = stride;
  if (kWriteTo == req[0]) {
    Fill<true>(s, outputs[0], req[0], 0);
  }
  MXNET_NO_INT8_TYPE_SWITCH(inputs[0].type_flag_, DType, {  // output data type switch
    MSHADOW_TYPE_SWITCH(inputs[1].type_flag_, IType, {  // indices data type switch
      GatherNDBackwardImpl(N, M, K, strides,
                           outputs[0].dptr<DType>(),
                           inputs[0].dptr<DType>(),
                           inputs[1].dptr<IType>(),
                           s);
    });
  });
}

/*!
 * This is for internal use only.
 * DO NOT call this function unless you have to.
 */
template<typename xpu>
void ScatterSetNDForward(const nnvm::NodeAttrs& attrs,
                         const OpContext& ctx,
                         const std::vector<TBlob>& inputs,
                         const std::vector<OpReqType>& req,
                         const std::vector<TBlob>& outputs) {
  CHECK_EQ(inputs.size(), 3U);
  CHECK_EQ(outputs.size(), 1U);
  CHECK_EQ(inputs[0].dptr_, outputs[0].dptr_);
  ScatterNDForward<xpu>(attrs, ctx, {inputs[1], inputs[2]}, {kWriteInplace}, outputs);
}

}  // namespace op
}  // namespace mxnet

#endif  // MXNET_OPERATOR_TENSOR_INDEXING_OP_H_
