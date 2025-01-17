/*
 * Copyright (c) OpenMMLab. All rights reserved.
 * Copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
 * Copyright (c) 2021, NAVER Corp.  Authored by CLOVA.
 * Copyright (c) 2022, SK Telecom Authored by A. Dialog
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
// https://github.com/NVIDIA/FasterTransformer/blob/main/src/fastertransformer/models/multi_gpu_gpt/ParallelGpt.cc

#include "src/turbomind/models/llama/LlamaV2.h"
#include "src/turbomind/kernels/decoding_kernels.h"
#include "src/turbomind/kernels/gemm/tuner/params.h"
#include "src/turbomind/kernels/gpt_kernels.h"
#include "src/turbomind/macro.h"
#include "src/turbomind/models/llama/LlamaBatch.h"
#include "src/turbomind/models/llama/LlamaDenseWeight.h"
#include "src/turbomind/models/llama/LlamaNcclGuard.h"
#include "src/turbomind/models/llama/LlamaWeight.h"
#include "src/turbomind/models/llama/Request.h"
#include "src/turbomind/models/llama/SequenceManager.h"
#include "src/turbomind/models/llama/llama_params.h"
#include "src/turbomind/models/llama/llama_utils.h"
#include "src/turbomind/models/llama/unified_decoder.h"
#include "src/turbomind/utils/Tensor.h"
#include "src/turbomind/utils/allocator.h"
#include "src/turbomind/utils/anomaly_handler.h"
#include "src/turbomind/utils/cuda_utils.h"
#include "src/turbomind/utils/logger.h"
#include "src/turbomind/utils/memory_utils.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <ratio>
#include <sstream>

namespace turbomind {

template<typename T>
LlamaV2<T>::LlamaV2(size_t                       head_num,
                    size_t                       kv_head_num,
                    size_t                       size_per_head,
                    size_t                       hidden_units,
                    size_t                       inter_size,
                    size_t                       num_layer,
                    size_t                       vocab_size,
                    float                        norm_eps,
                    const LlamaAttentionParams&  attn_params,
                    int                          start_id,
                    int                          end_id,
                    int                          cache_block_seq_len,
                    int                          quant_policy,
                    bool                         use_context_fmha,
                    const EngineParams&          engine_params,
                    const LoraParams&            lora_params,
                    std::shared_ptr<SharedState> shared_state,
                    LlamaWeight<T>*              weights,
                    NcclParam                    tensor_para,
                    cudaStream_t                 stream,
                    cublasMMWrapper*             cublas_wrapper,
                    IAllocator*                  allocator,
                    IAllocator*                  peer_alloctor,
                    bool                         is_free_buffer_after_forward,
                    cudaDeviceProp*              cuda_device_prop):
    head_num_(head_num),
    size_per_head_(size_per_head),
    inter_size_(inter_size),
    num_layer_(num_layer),
    vocab_size_(vocab_size),
    attn_params_(attn_params),
    vocab_size_padded_(vocab_size),
    rmsnorm_eps_(norm_eps),
    start_id_(start_id),
    end_id_(end_id),
    hidden_units_(hidden_units),
    local_head_num_(head_num / tensor_para.world_size_),
    local_kv_head_num_(kv_head_num / tensor_para.world_size_),
    weights_(weights),
    tensor_para_(tensor_para),
    stream_(stream),
    cublas_wrapper_(cublas_wrapper),
    allocator_(allocator),
    peer_allcator_(peer_alloctor),
    is_free_buffer_after_forward_(is_free_buffer_after_forward),
    cuda_device_prop_(cuda_device_prop),
    debug_(isDebug()),
    linear_{cublas_wrapper, stream},
    lora_params_(lora_params),
    shared_state_(shared_state)

{
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);
    TM_LOG_INFO("NCCL group_id = %d", tensor_para_.group_id_);

    vocab_size_padded_ =
        (vocab_size_padded_ + tensor_para_.world_size_ - 1) / tensor_para_.world_size_ * tensor_para_.world_size_;

    batch_ = std::make_unique<LlamaBatch<T>>(engine_params, cache_block_seq_len, quant_policy, this);

    initialize(attn_params, kv_head_num, use_context_fmha, cache_block_seq_len, quant_policy);

    unified_decoder_->allocateBuffer(engine_params.max_batch_size);

    /// TODO: decouple Llama model and batch inference
    batch_->Start();
}

template<typename T>
LlamaV2<T>::~LlamaV2()
{
    unified_decoder_.reset();
    delete dynamic_decode_layer_;
}

template<typename T>
void LlamaV2<T>::initialize(const LlamaAttentionParams& attn_params,
                            size_t                      kv_head_num,
                            bool                        use_context_fmha,
                            int                         cache_block_seq_len,
                            int                         quant_policy)
{
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);

    unified_decoder_.reset(new UnifiedDecoder<T>(head_num_,
                                                 kv_head_num,
                                                 size_per_head_,
                                                 hidden_units_,
                                                 inter_size_,
                                                 num_layer_,
                                                 attn_params,
                                                 rmsnorm_eps_,
                                                 tensor_para_,
                                                 stream_,
                                                 linear_,
                                                 allocator_,
                                                 lora_params_,
                                                 is_free_buffer_after_forward_,
                                                 use_context_fmha,
                                                 cache_block_seq_len,
                                                 quant_policy));

    dynamic_decode_layer_ = new DynamicDecodeLayer<float>(vocab_size_,
                                                          vocab_size_padded_,
                                                          0,  // end_id, deprecated
                                                          stream_,
                                                          cublas_wrapper_,
                                                          allocator_,
                                                          is_free_buffer_after_forward_,
                                                          cuda_device_prop_);
}

template<typename T>
void LlamaV2<T>::embeddingLookup(T* embeddings, const int* token_ids_buf, int batch_size, int step)
{
    NvtxScope scope("embeddingLookup");
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);
    // ! This kernel can't be used in context decoding
    invokeEmbeddingLookupPosEncodingPadCount(embeddings,
                                             weights_->pre_decoder_embedding_table,
                                             static_cast<T*>(nullptr),  // position encoding
                                             token_ids_buf,
                                             static_cast<int*>(nullptr),  // padding count, not used w/o pos-code
                                             batch_size,
                                             hidden_units_,
                                             static_cast<T>(1.),  // scale
                                             step,                // step, used int index into output_ids_buf_
                                             batch_size,          // token_num
                                             0,                   // ite
                                             stream_);
    sync_check_cuda_error();

    count_and_fix(embeddings, batch_size * hidden_units_, "embedding", 1);
}

template<typename T>
void LlamaV2<T>::updateEmbedding(T*               decoder_input,
                                 const int        bsz,
                                 const int*       h_input_length,
                                 const Sequence** sequences,
                                 int              token_num,
                                 int*             lora_mask,
                                 bool*            have_embeddings)
{
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);

    *have_embeddings          = false;
    int*             mask_ptr = nullptr;
    std::vector<int> mask;
    if (lora_mask != nullptr) {
        mask     = std::vector<int>(token_num);
        mask_ptr = mask.data();
    }

    for (int i = 0; i < bsz; i++) {
        const auto& seq        = *sequences[i];
        const auto& embeddings = seq.input_embeddings;
        const auto& ranges     = seq.input_embedding_ranges;
        for (int j = embeddings.size() - 1; j >= 0; j--) {
            int begin = ranges[j].first;
            int end   = ranges[j].second;
            if (seq.cache_len + h_input_length[i] - 1 < begin) {
                continue;
            }
            if (end <= seq.cache_len) {
                break;
            }
            int off_dst = std::max(0, begin - seq.cache_len);
            int off_src = std::max(0, seq.cache_len - begin);
            // calculate intersection of [begin, end) and [seq.cache_len, seq.cache_len + h_input_length[i])
            begin            = std::max(begin, seq.cache_len);
            end              = std::min(end, seq.cache_len + h_input_length[i]);
            size_t byte_size = (end - begin) * hidden_units_ * sizeof(T);
            T*     dst_ptr   = decoder_input + off_dst * hidden_units_;
            auto   src_ptr   = embeddings[j].data() + off_src * hidden_units_ * sizeof(T);
            cudaMemcpyAsync(dst_ptr, src_ptr, byte_size, cudaMemcpyDefault, stream_);
            if (lora_mask != nullptr) {
                std::fill_n(mask_ptr + off_dst, (end - begin), 1);
                *have_embeddings = true;
            }
        }
        decoder_input += h_input_length[i] * hidden_units_;
        mask_ptr += h_input_length[i];
    }

    if (lora_mask != nullptr && *have_embeddings) {
        cudaMemcpyAsync(lora_mask, mask.data(), sizeof(int) * token_num, cudaMemcpyDefault, stream_);
        cudaStreamSynchronize(stream_);
    }
    sync_check_cuda_error();
}

template<typename T>
void LlamaV2<T>::forwardUnified(T*               out,
                                T*               decoder_output,
                                T*               decoder_input,
                                void**           block_ptrs,
                                const int*       cu_block_cnts,
                                const int*       input_ids,
                                const int*       h_input_length,
                                const int*       h_context_length,
                                const float*     rope_theta,
                                const bool*      finished,
                                size_t           token_num,
                                int              dc_batch_size,
                                int              pf_batch_size,
                                int*             lora_mask,
                                const Sequence** sequences)
{
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);

    invokeInputIdsEmbeddingLookupPosEncoding(decoder_input,
                                             nullptr,  // processed somewhere else
                                             weights_->pre_decoder_embedding_table,
                                             static_cast<T*>(nullptr),
                                             pPromptTuningParam<T>{},
                                             input_ids,
                                             0,  // only used for position encoding
                                             token_num,
                                             token_num,
                                             1,
                                             hidden_units_,
                                             stream_);

    count_and_fix(decoder_input, token_num * hidden_units_, "embedding", 1);

    bool have_embeddings = false;
    updateEmbedding(decoder_input,
                    dc_batch_size + pf_batch_size,
                    h_input_length,
                    sequences,
                    token_num,
                    lora_mask,
                    &have_embeddings);

    sync_check_cuda_error();

    const auto   dtype = getTensorType<T>();
    const size_t bsz   = dc_batch_size + pf_batch_size;

    TensorMap inputs{{"decoder_input", {MEMORY_GPU, dtype, {token_num, hidden_units_}, decoder_input}},
                     {"output_norm_weight", {MEMORY_GPU, dtype, {hidden_units_}, weights_->output_norm_weight}},
                     {"h_q_len", {MEMORY_CPU, TYPE_INT32, {bsz}, h_input_length}},
                     {"h_k_len", {MEMORY_CPU, TYPE_INT32, {bsz}, h_context_length}},
                     {"finished", {MEMORY_GPU, TYPE_BOOL, {bsz}, finished}},
                     {"dc_batch_size", {MEMORY_CPU, TYPE_INT32, {1}, &dc_batch_size}},
                     {"pf_batch_size", {MEMORY_CPU, TYPE_INT32, {1}, &pf_batch_size}},
                     {"rope_theta", {MEMORY_GPU, TYPE_FP32, {hidden_units_}, rope_theta}},
                     {"cu_block_counts", {MEMORY_GPU, TYPE_INT32, {bsz}, cu_block_cnts}}};

    TensorMap outputs{{"decoder_output", {MEMORY_GPU, dtype, {token_num, hidden_units_}, decoder_output}},
                      {"block_ptrs", {MEMORY_GPU, TYPE_UINT64, {bsz}, block_ptrs}},
                      {"last_token_hidden_units", {MEMORY_GPU, dtype, {bsz, hidden_units_}, out}}};

    if (lora_mask != nullptr && have_embeddings) {
        inputs.insert({"lora_mask", {MEMORY_GPU, TYPE_INT32, {token_num}, lora_mask}});
    }

    unified_decoder_->forward(&outputs, &inputs, &weights_->decoder_layer_weights);
}

template<typename T>
void LlamaV2<T>::postDecodeEmbedding(float* logits, float* local_logits, const T* decoder_output, int batch_size)
{
    NvtxScope scope("postDecodeEmbedding");
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);
    cudaDataType_t data_type = getCudaDataType<T>();
    float          alpha     = 1.f;
    float          beta      = 0.f;
    if (tensor_para_.world_size_ == 1) {
        cublas_wrapper_->Gemm(CUBLAS_OP_T,
                              CUBLAS_OP_N,
                              vocab_size_,  // n
                              batch_size,
                              hidden_units_,  // k
                              &alpha,
                              weights_->post_decoder_embedding_kernel,
                              data_type,
                              hidden_units_,  // k
                              decoder_output,
                              data_type,
                              hidden_units_,  // k
                              &beta,
                              logits,
                              CUDA_R_32F,
                              vocab_size_,  // n
                              CUDA_R_32F,
                              cublasGemmAlgo_t(-1));
    }
    else {
        FT_CHECK(vocab_size_padded_ % tensor_para_.world_size_ == 0);
        const size_t local_vocab_size = vocab_size_padded_ / tensor_para_.world_size_;
        cublas_wrapper_->Gemm(CUBLAS_OP_T,
                              CUBLAS_OP_N,
                              local_vocab_size,  // n
                              batch_size,
                              hidden_units_,  // k
                              &alpha,
                              weights_->post_decoder_embedding_kernel
                                  + tensor_para_.rank_ * local_vocab_size * hidden_units_,
                              data_type,
                              hidden_units_,  // k
                              decoder_output,
                              data_type,
                              hidden_units_,  // k
                              &beta,
                              local_logits + tensor_para_.rank_ * batch_size * local_vocab_size,
                              CUDA_R_32F,
                              local_vocab_size,  // n
                              CUDA_R_32F,
                              cublasGemmAlgo_t(-1));
        {
            NcclGuard nccl_guard(tensor_para_, stream_);
            ftNcclAllGather(local_logits,                   // send_buf
                            local_logits,                   // recv_buf
                            batch_size * local_vocab_size,  // data_size
                            tensor_para_.rank_,
                            tensor_para_,
                            stream_);
            sync_check_cuda_error();
        }
        invokeTransposeAxis01(logits, local_logits, tensor_para_.world_size_, batch_size, local_vocab_size, stream_);
        sync_check_cuda_error();
    }
}

template<typename T>
void LlamaV2<T>::dynamicDecode(int*            token_ids,
                               bool*           finished,
                               int*            sequence_length,
                               bool*           should_stop,
                               curandState_t*  curand_state,
                               TensorMap*      inputs,
                               TensorMap*      outputs,
                               const float*    logits,
                               const uint32_t* seq_limit_len,
                               const int*      context_length,
                               const int*      end_ids,
                               int             step,
                               int             ite,
                               size_t          max_context_len,
                               size_t          token_ids_len,
                               size_t          batch_size)
{
    NvtxScope scope("dynamicDecode");
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);
    int local_batch_size = (int)batch_size;

    std::unordered_map<std::string, Tensor> dynamic_decode_input_tensors{
        {"logits", {MEMORY_GPU, TYPE_FP32, {batch_size, (size_t)1, vocab_size_padded_}, logits}},
        {"step", {MEMORY_CPU, TYPE_INT32, {1}, &step}},
        {"max_input_length", {MEMORY_CPU, TYPE_INT32, {1}, &max_context_len}},
        {"sequence_limit_length", {MEMORY_GPU, TYPE_UINT32, {batch_size}, seq_limit_len}},
        {"input_lengths", {MEMORY_GPU, TYPE_INT32, {batch_size, 1}, context_length}},
        {"ite", {MEMORY_CPU, TYPE_UINT32, {1}, &ite}},
        {"end_id", {MEMORY_GPU, TYPE_INT32, {batch_size}, end_ids}},
        {"local_batch_size", {MEMORY_CPU, TYPE_INT32, {1}, &local_batch_size}},
    };

    const std::vector<std::string> optional_inputs{"stop_words_list",
                                                   "bad_words_list",
                                                   "runtime_top_k",
                                                   "runtime_top_p",
                                                   "temperature",
                                                   "repetition_penalty",
                                                   "random_seed"};
    for (const auto& key : optional_inputs) {
        if (inputs->isExist(key)) {
            dynamic_decode_input_tensors.insert({key, inputs->at(key)});
        }
    }

    std::unordered_map<std::string, Tensor> dynamic_decode_output_tensors{
        {"output_ids", {MEMORY_GPU, TYPE_INT32, {token_ids_len, batch_size, 1U}, token_ids}},
        {"finished", {MEMORY_GPU, TYPE_BOOL, {batch_size}, finished}},
        {"sequence_length", {MEMORY_GPU, TYPE_INT32, {batch_size}, sequence_length}},
        {"should_stop", {MEMORY_CPU, TYPE_BOOL, {1}, should_stop}},
        {"curand_state", {MEMORY_GPU, TYPE_VOID, {batch_size}, curand_state}}};

    const std::vector<std::string> optional_outputs{
        "cum_log_probs", "output_log_probs", "sampled_indexes", "sampled_logprobs", "sampled_nums"};
    for (const auto& key : optional_outputs) {
        if (outputs->isExist(key)) {
            dynamic_decode_output_tensors.insert({key, outputs->at(key)});
        }
    }

    dynamic_decode_layer_->forward(&dynamic_decode_output_tensors, &dynamic_decode_input_tensors);
}

static inline Tensor slice(const Tensor& tensor, int index)
{
    auto shape = tensor.shape;
    if (shape.at(0) == 1) {
        return tensor;
    }
    shape[0]          = 1;
    const auto offset = std::accumulate(shape.begin(), shape.end(), (size_t)index, std::multiplies<>{});
    return tensor.slice(shape, offset);
}

// ! implicit conversion from `unordered_map` to `TensorMap` drops 0-sized tensors
static inline TensorMap slice(const std::unordered_map<std::string, Tensor>& src, int index)
{
    TensorMap dst;
    for (const auto& kv : src) {
        dst.insert({kv.first, slice(kv.second, index)});
    }
    return dst;
}

template<typename T>
void LlamaV2<T>::forward(std::unordered_map<std::string, Tensor>*       outputs,
                         const std::unordered_map<std::string, Tensor>* inputs,
                         Control                                        control)
{
    if (debug_) {
        if (tensor_para_.rank_ == 0) {
            for (const auto& kv : *inputs) {
                TM_LOG_INFO("[forward][rank=%d] INPUT: %s", (int)tensor_para_.rank_, format(kv).c_str());
            }
            for (const auto& kv : *outputs) {
                TM_LOG_INFO("[forward][rank=%d] OUTPUT: %s", (int)tensor_para_.rank_, format(kv).c_str());
            }
        }
    }

    const int batch_size = outputs->at("output_ids").shape[0];

    const auto rank = tensor_para_.rank_;
    FT_CHECK(rank == 0);

    std::vector<std::shared_ptr<Request>> requests(batch_size);

    // allocates all requests for the batch
    for (int i = 0; i < batch_size; ++i) {
        requests[i] = std::make_shared<Request>();
    }

    for (int i = 0; i < batch_size; ++i) {
        auto& r = requests[i];

        r->inputs  = slice(*inputs, i);
        r->outputs = slice(*outputs, i);

        if (rank == 0) {
            r->id         = r->inputs.getVal<uint64_t>("CORRID", i);
            r->start_flag = r->inputs.getVal<int>("START", 1);
            r->end_flag   = r->inputs.getVal<int>("END", 1);
            r->stop_flag  = r->inputs.getVal<int>("STOP", 0);
            r->stream_cb  = control.callback;
        }
    }

    // Submits the tasks and wait for finish
    std::vector<int> error_codes;
    bool             has_error = 0;

    TM_LOG_INFO("[forward] Enqueue requests");

    std::vector<uint64_t> ids;
    for (const auto& r : requests) {
        ids.push_back(r->id);
    }

    auto futures = shared_state_->request_queue.enqueue(std::move(requests));

    FT_CHECK_WITH_INFO(ids.size() == futures.size(), "check failed");

    TM_LOG_INFO("[forward] Wait for requests to complete ...");

    for (int i = 0; i < futures.size(); ++i) {
        auto ec = futures[i].get();
        error_codes.push_back(ec);
        if (ec) {
            has_error = true;
            TM_LOG_WARNING("[forward] Request failed for %ld, code %d", (long)ids[i], (int)ec);
        }
        else {
            TM_LOG_INFO("[forward] Request completed for %ld", (long)ids[i]);
        }
    }

    if (has_error) {
        std::stringstream ss;
        for (int i = 0; i < error_codes.size(); ++i) {
            ss << (i ? "" : " ") << error_codes[i];
        }
        throw std::runtime_error(ss.str());
    }
}

template<class First, class Last>
static std::string Join(First first, Last last, const std::string& delim)
{
    if (first == last) {
        return {};
    }
    std::ostringstream oss;
    oss << *first++;
    while (first != last) {
        oss << delim << *first++;
    }
    return oss.str();
}

// Only called when `weight_type == INT4` for now
template<typename T>
void LlamaV2<T>::tune()
{

    if (auto str = std::getenv("TM_GEMM_IMPORT")) {
        std::ifstream ifs(str);
        const int     n_imported = linear_.Import(ifs);
        TM_LOG_INFO("[Gemm2] %d records imported", n_imported);
        return;
    }

    std::vector<int> bss = linear_.GetTuningSeq();
    if (bss.empty()) {
        bss = gemm::GenerateTuningSequence(gemm::GetDefaultTuningGenerators());
    }

    {
        auto str = Join(bss.begin(), bss.end(), ", ");
        TM_LOG_INFO("[Gemm2] Tuning sequence: %s", str.c_str());
    }

    LlamaAttentionWeight<T>& attn = weights_->decoder_layer_weights[0]->self_attn_weights;
    LlamaFfnWeight<T>&       ffn  = weights_->decoder_layer_weights[0]->ffn_weights;

    std::vector<LlamaDenseWeight<T>*> weights{&attn.qkv, &attn.output, &ffn.output};

    for (auto& layer : weights_->decoder_layer_weights) {
        if (layer->ffn_weights.gating.kernel) {
            weights.push_back(&layer->ffn_weights.gating);
            break;
        }
    }
    for (auto& layer : weights_->decoder_layer_weights) {
        if (layer->ffn_weights.fused_gating_intermediate.kernel) {
            weights.push_back(&layer->ffn_weights.fused_gating_intermediate);
            break;
        }
    }

    const int max_bs  = *std::max_element(bss.begin(), bss.end());
    int       max_in  = 0;
    int       max_out = 0;
    for (auto& w : weights) {
        max_in  = std::max<int>(max_in, w->input_dims);
        max_out = std::max<int>(max_out, w->output_dims);
    }

    T* in_data  = (T*)allocator_->malloc(sizeof(T) * (size_t)max_bs * max_in);
    T* out_data = (T*)allocator_->malloc(sizeof(T) * (size_t)max_bs * max_out);

    cudaRandomUniform(in_data, (size_t)max_bs * max_in);
    cudaDeviceSynchronize();

    linear_.set_measure(true);

    auto tick = std::chrono::steady_clock::now();

    for (auto bs : bss) {
        TM_LOG_INFO("[Gemm2] %d", bs);
        for (auto& w : weights) {
            linear_.forward(out_data, in_data, bs, *w);
        }
    }

    auto tock = std::chrono::steady_clock::now();

    TM_LOG_INFO("[Gemm2] Tuning finished in %.2f seconds.",
                std::chrono::duration<float, std::ratio<1, 1>>(tock - tick).count());

    linear_.set_measure(false);

    allocator_->free((void**)&in_data);
    allocator_->free((void**)&out_data);

    // Only rank-0 exports the dispatch cache
    if (tensor_para_.rank_ == 0) {
        if (auto path = std::getenv("TM_GEMM_EXPORT")) {
            std::ofstream ofs(path);
            const auto    n_records = linear_.Export(ofs);
            TM_LOG_INFO("[Gemm2] %d records exported.", n_records);
        }
    }
}

template class LlamaV2<half>;
#ifdef ENABLE_FP32
template class LlamaV2<float>;
#endif
#ifdef ENABLE_BF16
template class LlamaV2<__nv_bfloat16>;
#endif

}  // namespace turbomind
