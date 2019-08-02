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
 * \file fft-inl.h
 * \brief
 * \author Chen Zhu
 */

#include "hip/hip_runtime.h"
#if defined(__HIP_PLATFORM_HCC__) && !defined(__HIP_PLATFORM_NVCC__)
#include "./rocfft-inl.h"
#elif defined(__HIP_PLATFORM_NVCC__) && !defined(__HIP_PLATFORM_HCC__)
#include "./cudafft-inl.h"
#else
#error("Must define exactly one of __HIP_PLATFORM_HCC__ or __HIP_PLATFORM_NVCC__");
#endif

namespace mxnet {
namespace op {

template<>
Operator* CreateOp<gpu>(FFTParam param, int dtype) {
  Operator *op = NULL;
  MSHADOW_REAL_TYPE_SWITCH(dtype, DType, {
      op = new FFTOp<gpu, DType>(param);
  })
  return op;
}

}  // namespace op
}  // namespace mxnet
