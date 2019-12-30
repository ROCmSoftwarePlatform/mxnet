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
 * Copyright (c) 2015 by Contributors
 * \file gpu_device_storage.h
 * \brief GPU storage implementation.
 */
#ifndef MXNET_STORAGE_GPU_DEVICE_STORAGE_H_
#define MXNET_STORAGE_GPU_DEVICE_STORAGE_H_

#include "mxnet/base.h"
#include "mxnet/storage.h"
#include "../common/cuda_utils.h"
#if MXNET_USE_GPU
#include <hip/hip_runtime.h>
#endif  // MXNET_USE_GPU
#include <new>

namespace mxnet {
namespace storage {

/*!
 * \brief GPU storage implementation.
 */
class GPUDeviceStorage {
 public:
  /*!
   * \brief Allocation.
   * \param handle Handle struct.
   */
  inline static void Alloc(Storage::Handle* handle);
  /*!
   * \brief Deallocation.
   * \param handle Handle struct.
   */
  inline static void Free(Storage::Handle handle);
};  // class GPUDeviceStorage

inline void GPUDeviceStorage::Alloc(Storage::Handle* handle) {
  handle->dptr = nullptr;
  const size_t size = handle->size;
  if (size == 0) return;
#if MXNET_USE_GPU
  mxnet::common::cuda::DeviceStore device_store(handle->ctx.real_dev_id(), true);
#if MXNET_USE_RCCL
  std::lock_guard<std::mutex> l(Storage::Get()->GetMutex(Context::kGPU));
#endif  // MXNET_USE_RCCL
  hipError_t e = hipMalloc(&handle->dptr, size);
  if (e != hipSuccess && e != hipErrorDeinitialized)
    LOG(FATAL) << "CUDA: " << hipGetErrorString(e);
#else   // MXNET_USE_GPU
  LOG(FATAL) << "Please compile with CUDA enabled";
#endif  // MXNET_USE_GPU
}

inline void GPUDeviceStorage::Free(Storage::Handle handle) {
#if MXNET_USE_GPU
  mxnet::common::cuda::DeviceStore device_store(handle.ctx.real_dev_id(), true);
#if MXNET_USE_RCCL
  std::lock_guard<std::mutex> l(Storage::Get()->GetMutex(Context::kGPU));
#endif  // MXNET_USE_RCCL
  // throw special exception for caller to catch.
  hipError_t err = hipFree(handle.dptr);
  // ignore unloading error, as memory has already been recycled
  if (err != hipSuccess && err != hipErrorDeinitialized) {
    LOG(FATAL) << "CUDA: " << hipGetErrorString(err);
  }
#else   // MXNET_USE_GPU
  LOG(FATAL) << "Please compile with CUDA enabled";
#endif  // MXNET_USE_GPU
}

}  // namespace storage
}  // namespace mxnet

#endif  // MXNET_STORAGE_GPU_DEVICE_STORAGE_H_
