/*
 * Copyright (c) OpenMMLab. All rights reserved.
 * Copyright (c) 2019-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Modified from
// https://github.com/NVIDIA/FasterTransformer/blob/main/src/fastertransformer/models/multi_gpu_gpt/ParallelGptDecoderLayerWeight.h

#pragma once

#include "src/turbomind/models/llama/LlamaDenseWeight.h"
#include "src/turbomind/models/llama/llama_params.h"
#include "src/turbomind/utils/Tensor.h"

namespace turbomind {

template<typename T>
struct LlamaDecoderLayerWeight {
public:
    LlamaDecoderLayerWeight() = delete;
    LlamaDecoderLayerWeight(int        layer_idx,
                            size_t     head_num,
                            size_t     kv_head_num,
                            size_t     size_per_head,
                            size_t     hidden_units,
                            size_t     inter_size,
                            WeightType weight_type,
                            int        group_size,
                            LoraParams lora_params,
                            bool       attn_bias,
                            size_t     tensor_para_size,
                            size_t     tensor_para_rank);
    ~LlamaDecoderLayerWeight();
    LlamaDecoderLayerWeight(const LlamaDecoderLayerWeight& other) = delete;
    LlamaDecoderLayerWeight& operator=(const LlamaDecoderLayerWeight& other) = delete;

    void loadModel(std::string dir_path, FtCudaDataType model_file_type);

    TensorMap getParams(std::string prefix);

    void prepare(void* workspace, size_t size, const cudaDeviceProp& prop);

    size_t workspace_size() const noexcept;

    T*                      self_attn_norm_weights{};
    T*                      ffn_norm_weights{};
    LlamaAttentionWeight<T> self_attn_weights{};
    LlamaFfnWeight<T>       ffn_weights{};

private:
    size_t     head_num_;
    size_t     kv_head_num_;
    size_t     size_per_head_;
    size_t     hidden_units_;
    size_t     inter_size_;
    WeightType weight_type_;
    size_t     bit_size_;
    bool       attn_bias_;
    size_t     tensor_para_size_;
    size_t     tensor_para_rank_;
    bool       is_maintain_buffer_ = false;
    bool       fused_up_and_gate_;

    void mallocWeights();
};

}  // namespace turbomind
