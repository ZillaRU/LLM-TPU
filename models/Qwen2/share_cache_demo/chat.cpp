//===----------------------------------------------------------------------===//
//
// Copyright (C) 2023 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <assert.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <getopt.h>
#include <inttypes.h>
#include <iostream>
#include <numeric>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <random>
#include <stdio.h>
#include <vector>

#include "bmruntime_interface.h"
#include "memory.h"
#include "utils.h"

static const float ATTENTION_MASK = -10000.;

class Qwen {
public:
  void init(const std::vector<int> &devid, const std::string& model_path);
  void deinit();
  void free_device();
  void malloc_bmodel_mem();
  void empty_kvcache();
  int forward_first(std::vector<int> &tokens);
  void forward_share(std::vector<int> &tokens);
  int forward_unshare(std::vector<int> &tokens);
  int forward_next();
  std::vector<int> generate(std::vector<int> &history_tokens, int EOS);

  std::mt19937 sgen;
  Qwen() : sgen(std::random_device()()){};

private:
  void net_launch(const bm_net_info_t *net, int stage_idx = 0);
  void dynamic_net_launch(const bm_net_info_t *net, int token_length,
                          int stage_idx = 0);
  inline void d2d(bm_device_mem_t &dst, bm_device_mem_t &src);
  inline void d2d(bm_device_mem_t &dst, bm_device_mem_t &src, size_t offset);
  inline void d2d(bm_device_mem_t &dst, bm_device_mem_t &src, size_t offset,
                  size_t size);

  void head_launch(const bm_net_info_t *net, bm_device_mem_t &logits_mem);
  int greedy_search(const bm_net_info_t *net, bm_device_mem_t &logits_mem);
  int penalty_sample(const bm_net_info_t *net, bm_device_mem_t &logits_mem,
                     std::vector<int> &input_tokens, int &token_length);

public:
  bool io_alone;
  bool is_dynamic;
  bool memory_prealloc;
  bool is_decrypt;
  bool io_alone_reuse;
  std::vector<int> total_tokens;
  std::string lib_path;

  // model
  int hidden_bytes;
  int kv_bytes;
  int share_length;
  int unshare_length;
  int total_length;
  int unshare_flag;
  int SEQLEN;
  int NUM_LAYERS;
  int MAX_SHARE_LENGTH;
  int MAX_UNSHARE_LENGTH;
  int BATCH_SIZE;

  // generation
  float temperature;
  float top_p;
  float repeat_penalty;
  int repeat_last_n;
  int max_new_tokens;
  std::string generation_mode;
  std::string prompt_mode;

private:
  std::vector<bm_handle_t> handles;
  bm_handle_t bm_handle;
  void *p_bmrt;
  std::vector<const bm_net_info_t *> net_blocks;
  std::vector<const bm_net_info_t *> net_blocks_unshare;
  std::vector<const bm_net_info_t *> net_blocks_cache;
  const bm_net_info_t *net_embed;
  const bm_net_info_t *net_embed_unshare;
  const bm_net_info_t *net_embed_cache;
  const bm_net_info_t *net_lm, *net_greedy_head, *net_penalty_sample_head;
  std::vector<bm_device_mem_t> past_key;
  std::vector<bm_device_mem_t> past_value;
  std::vector<bm_device_mem_t> prev_past_key;
  std::vector<bm_device_mem_t> prev_past_value;
  bm_device_mem_t tmp_key_cache;
  bm_device_mem_t tmp_value_cache;

  std::vector<bm_device_mem_u64_t> prealloc_mem_v;
  bm_device_mem_t io_mem_v[100];
  uint64_t io_size;
  uint16_t mask_value;
  mem_info_t mem_info;
};

void Qwen::net_launch(const bm_net_info_t *net, int stage_idx) {
  std::vector<bm_tensor_t> in_tensors(net->input_num);
  std::vector<bm_tensor_t> out_tensors(net->output_num);

  for (int i = 0; i < net->input_num; i++) {
    bmrt_tensor_with_device(
        &in_tensors[i], net->stages[stage_idx].input_mems[i],
        net->input_dtypes[i], net->stages[stage_idx].input_shapes[i]);
  }
  for (int i = 0; i < net->output_num; i++) {
    bmrt_tensor_with_device(
        &out_tensors[i], net->stages[stage_idx].output_mems[i],
        net->output_dtypes[i], net->stages[stage_idx].output_shapes[i]);
  }
  auto ret = bmrt_launch_tensor_ex(p_bmrt, net->name, in_tensors.data(),
                                   net->input_num, out_tensors.data(),
                                   net->output_num, true, false);
  assert(ret);
  bm_thread_sync(bm_handle);
}

void Qwen::dynamic_net_launch(const bm_net_info_t *net, int token_length,
                              int stage_idx) {
  std::vector<bm_tensor_t> in_tensors(net->input_num);
  std::vector<bm_tensor_t> out_tensors(net->output_num);

  for (int i = 0; i < net->input_num; i++) {
    bmrt_tensor_with_device(
        &in_tensors[i], net->stages[stage_idx].input_mems[i],
        net->input_dtypes[i], net->stages[stage_idx].input_shapes[i]);
  }
  for (int i = 0; i < net->output_num; i++) {
    bmrt_tensor_with_device(
        &out_tensors[i], net->stages[stage_idx].output_mems[i],
        net->output_dtypes[i], net->stages[stage_idx].output_shapes[i]);
  }
  // in_tensors[0].shape.dims[1] = token_length;
  // in_tensors[1].shape.dims[1] = token_length;
  // in_tensors[2].shape.dims[2] = token_length;
  // in_tensors[2].shape.dims[3] = token_length;
  // out_tensors[0].shape.dims[1] = token_length;
  // out_tensors[1].shape.dims[1] = token_length;
  // out_tensors[2].shape.dims[1] = token_length;
  auto ret = bmrt_launch_tensor_ex(p_bmrt, net->name, in_tensors.data(),
                                   net->input_num, out_tensors.data(),
                                   net->output_num, true, false);
  assert(ret);
  bm_thread_sync(bm_handle);
}

void Qwen::d2d(bm_device_mem_t &dst, bm_device_mem_t &src) {
  assert(bm_mem_get_device_size(dst) == bm_mem_get_device_size(src));
  bm_memcpy_d2d_byte(bm_handle, dst, 0, src, 0, bm_mem_get_device_size(dst));
}

void Qwen::d2d(bm_device_mem_t &dst, bm_device_mem_t &src, size_t offset) {
  assert(bm_mem_get_device_size(dst) >= bm_mem_get_device_size(src) + offset);
  bm_memcpy_d2d_byte(bm_handle, dst, offset, src, 0,
                     bm_mem_get_device_size(src));
}

void Qwen::d2d(bm_device_mem_t &dst, bm_device_mem_t &src, size_t offset,
               size_t size) {
  assert(bm_mem_get_device_size(dst) >= size + offset);
  bm_memcpy_d2d_byte(bm_handle, dst, offset, src, 0, size);
}

void malloc_device_mem(bm_handle_t bm_handle, memory_t &mem,
                       std::vector<bm_device_mem_u64_t> &prealloc_mem_v) {
  if (mem.size > 0) {
    bm_device_mem_u64_t dmem;
    if (bm_malloc_device_byte_u64(bm_handle, &dmem, mem.size) == BM_SUCCESS) {
      mem.addr = dmem.u.device.device_addr;
      prealloc_mem_v.push_back(dmem);
    }
  }
}

void Qwen::init(const std::vector<int> &devices, const std::string& model_path) {

  // request bm_handle
  std::cout << "Device [ ";
  for (auto d : devices) {
    std::cout << d << " ";
  }
  std::cout << "] loading ....\n";
  for (auto d : devices) {
    bm_handle_t h;
    bm_status_t status = bm_dev_request(&h, d);
    assert(BM_SUCCESS == status);
    handles.push_back(h);
  }
  bm_handle = handles[0];

  // create bmruntime
#ifdef SOC_TARGET
  p_bmrt = bmrt_create(handles[0]);
#else
  p_bmrt = bmrt_create_ex(handles.data(), handles.size());
#endif
  assert(NULL != p_bmrt);

  // load bmodel by file
  printf("Model[%s] loading ....\n", model_path.c_str());
  if (!io_alone_reuse) {
    io_size = 0;
  }
  bool ret = false;

  // decrypt bmodel
  if (is_decrypt && memory_prealloc) {
    ret = bmrt_get_encrypted_bmodel_info(model_path.c_str(), lib_path.c_str(), &mem_info);
  } else if(!is_decrypt && memory_prealloc) {
    ret = bmrt_get_bmodel_info(model_path.c_str(), &mem_info);
  } else {
    throw std::runtime_error("not support now");
  }
  assert(true == ret);

  // prealloc memory
  if (is_decrypt && memory_prealloc) {
    malloc_bmodel_mem();
    ret = bmrt_load_encrypted_bmodel_with_io_mem(p_bmrt, model_path.c_str(), &mem_info, io_mem_v, &io_size, lib_path.c_str());
  } else if (!is_decrypt && !memory_prealloc) {
    ret = bmrt_load_bmodel(p_bmrt, model_path.c_str());
  } else {
    throw std::runtime_error("not support now");
  }
  assert(true == ret);
  printf("Done!\n");

  assert(true == ret);
  printf("Done!\n");

  // net embed and lm_head
  net_embed = bmrt_get_network_info(p_bmrt, "embedding");
  net_embed_unshare = bmrt_get_network_info(p_bmrt, "embedding_unshare");
  net_embed_cache = bmrt_get_network_info(p_bmrt, "embedding_cache");
  net_lm = bmrt_get_network_info(p_bmrt, "lm_head");
  net_greedy_head = bmrt_get_network_info(p_bmrt, "greedy_head");
  net_penalty_sample_head =
      bmrt_get_network_info(p_bmrt, "penalty_sample_head");
  
  auto unshare_name_0 = "block_unshare_" + std::to_string(0);
  unshare_flag = bmrt_get_network_index(p_bmrt, unshare_name_0.c_str());
  auto num_nets = bmrt_get_network_number(p_bmrt);
  if (unshare_flag != -1) {
    NUM_LAYERS = (num_nets - 5) / 3;
  } else {
    NUM_LAYERS = (num_nets - 5) / 2;
  }

  // net blocks
  net_blocks.clear();
  net_blocks_unshare.clear();
  net_blocks_cache.clear();
  for (int i = 0; i < NUM_LAYERS; i++) {
    auto block_name = "block_" + std::to_string(i);
    auto unshare_name = "block_unshare_" + std::to_string(i);
    auto cache_name = "block_cache_" + std::to_string(i);
    net_blocks.emplace_back(bmrt_get_network_info(p_bmrt, block_name.c_str()));
    if (unshare_flag != -1) {
      net_blocks_unshare.emplace_back(
          bmrt_get_network_info(p_bmrt, unshare_name.c_str()));
    }
    net_blocks_cache.emplace_back(
        bmrt_get_network_info(p_bmrt, cache_name.c_str()));
  }

  // convert attention to uint16_t
  if (net_blocks[0]->input_dtypes[0] == BM_FLOAT16) {
    mask_value = fp32_to_fp16_bits(ATTENTION_MASK);
  } else if (net_blocks[0]->input_dtypes[0] == BM_BFLOAT16) {
    mask_value = fp32_to_bf16_bits(ATTENTION_MASK);
  } else {
    std::cerr << "\nError: Invalid attention dtype\n";
    std::cerr << "Supported dtype are 'BM_FLOAT16' or 'BM_BFLOAT16'\n";
    throw std::runtime_error("Invalid attention dtype");
  }

  // read parameters from bmodel
  is_dynamic = net_blocks[0]->is_dynamic;
  auto addr_mode = net_blocks_cache[0]->addr_mode;
  io_alone = addr_mode == 1;
  assert(io_alone == 1);
  hidden_bytes =
      bm_mem_get_device_size(net_blocks_cache[0]->stages[0].output_mems[0]);
  kv_bytes =
      bm_mem_get_device_size(net_blocks_cache[0]->stages[0].output_mems[1]);
  MAX_SHARE_LENGTH = net_blocks[0]->stages[0].input_shapes[0].dims[1];
  if (unshare_flag != -1) {
    MAX_UNSHARE_LENGTH = net_blocks_unshare[0]->stages[0].input_shapes[0].dims[1];
  } else {
    MAX_UNSHARE_LENGTH = 0;
  }
  SEQLEN = net_blocks_cache[0]->stages[0].input_shapes[3].dims[1];

  // resize
  past_key.resize(NUM_LAYERS);
  past_value.resize(NUM_LAYERS);
  prev_past_key.resize(NUM_LAYERS);
  prev_past_value.resize(NUM_LAYERS);
  total_tokens.resize(SEQLEN);

  if (io_alone_reuse) {
    ret = bm_malloc_device_byte(bm_handle, &tmp_key_cache, past_key[0].size);
    assert(BM_SUCCESS == ret);
    ret =
        bm_malloc_device_byte(bm_handle, &tmp_value_cache, past_value[0].size);
    assert(BM_SUCCESS == ret);

    empty(bm_handle, tmp_key_cache);
    empty(bm_handle, tmp_value_cache);
  }

  // declare tmemory location for kvcache
  for (int i = 0; i < NUM_LAYERS; i++) {
    assert(addr_mode == net_blocks_cache[i]->addr_mode);
    if (io_alone_reuse) {
      prev_past_key[i] = past_key[i];
      prev_past_value[i] = past_value[i];
    }
    past_key[i] = net_blocks_cache[i]->stages[0].input_mems[3];
    past_value[i] = net_blocks_cache[i]->stages[0].input_mems[4];
    if (io_alone_reuse) {
      if (i != NUM_LAYERS - 1) {
        assert(prev_past_key[i].u.device.device_addr + prev_past_key[i].size <
               past_key[i + 1].u.device.device_addr);
        assert(prev_past_value[i].u.device.device_addr +
                   prev_past_value[i].size <
               past_value[i + 1].u.device.device_addr);

        assert(prev_past_key[i].u.device.device_addr + prev_past_key[i].size <
               past_value[i + 1].u.device.device_addr);
        assert(prev_past_value[i].u.device.device_addr +
                   prev_past_value[i].size <
               past_key[i + 1].u.device.device_addr);
      }
      d2d(tmp_key_cache, prev_past_key[i], 0, share_length * kv_bytes);
      d2d(tmp_value_cache, prev_past_value[i], 0, share_length * kv_bytes);
      empty(bm_handle, past_key[i]);
      empty(bm_handle, past_value[i]);
      d2d(past_key[i], tmp_key_cache, 0, share_length * kv_bytes);
      d2d(past_value[i], tmp_value_cache, 0, share_length * kv_bytes);
    }
  }
}

void Qwen::empty_kvcache() {
  for (int i = 0; i < NUM_LAYERS; i++) {
    empty(bm_handle, past_key[i]);
    empty(bm_handle, past_value[i]);
    empty(bm_handle, net_blocks_unshare[i]->stages[0].input_mems[3]);
    empty(bm_handle, net_blocks_unshare[i]->stages[0].input_mems[4]);
  }
  return;
}

void Qwen::malloc_bmodel_mem() {

  // if (bmrt_get_bmodel_info(model_path.c_str(), &mem_info) == false) {
  //   throw std::runtime_error("Load bmodel Failed");
  // }
  mem_info.io_mem.size = 0;

  // free all memory without coeff memory
  if (prealloc_mem_v.size() == 0) {
    malloc_device_mem(bm_handle, mem_info.coeff_mem, prealloc_mem_v);
  } else {
    mem_info.coeff_mem.addr = prealloc_mem_v[0].u.device.device_addr;
  }
  malloc_device_mem(bm_handle, mem_info.instruction_mem, prealloc_mem_v);
  malloc_device_mem(bm_handle, mem_info.variable_instruction_mem,
                    prealloc_mem_v);
  malloc_device_mem(bm_handle, mem_info.neuron_mem, prealloc_mem_v);
  malloc_device_mem(bm_handle, mem_info.io_mem, prealloc_mem_v);
}

void Qwen::free_device() {
  while (prealloc_mem_v.size() > 1) {
    bm_free_device_u64(bm_handle, prealloc_mem_v.back());
    prealloc_mem_v.pop_back();
  }
}

void Qwen::deinit() {
  for (size_t i = 0; i < prealloc_mem_v.size(); ++i) {
    bm_free_device_u64(bm_handle, prealloc_mem_v[i]);
  }
  // for (size_t i = 0; i < io_mem_v.size(); i++) {
  //   bm_free_device(bm_handle, io_mem_v[i]);
  // }
  if (false == io_alone) {
    for (int i = 0; i < NUM_LAYERS; i++) {
      bm_free_device(bm_handle, past_key[i]);
      bm_free_device(bm_handle, past_value[i]);
    }
  }
  bmrt_destroy(p_bmrt);
  for (auto h : handles) {
    bm_dev_free(h);
  }
}

void Qwen::head_launch(const bm_net_info_t *net, bm_device_mem_t &logits_mem) {
  std::vector<bm_tensor_t> in_tensors(net->input_num);
  std::vector<bm_tensor_t> out_tensors(net->output_num);

  bmrt_tensor_with_device(&in_tensors[0], logits_mem, net->input_dtypes[0],
                          net->stages[0].input_shapes[0]);

  for (int i = 1; i < net->input_num; i++) {
    bmrt_tensor_with_device(&in_tensors[i], net->stages[0].input_mems[i],
                            net->input_dtypes[i],
                            net->stages[0].input_shapes[i]);
  }
  for (int i = 0; i < net->output_num; i++) {
    bmrt_tensor_with_device(&out_tensors[i], net->stages[0].output_mems[i],
                            net->output_dtypes[i],
                            net->stages[0].output_shapes[i]);
  }
  auto ret = bmrt_launch_tensor_ex(p_bmrt, net->name, in_tensors.data(),
                                   net->input_num, out_tensors.data(),
                                   net->output_num, true, false);
  assert(ret);
  bm_thread_sync(bm_handle);
}

int Qwen::greedy_search(const bm_net_info_t *net, bm_device_mem_t &logits_mem) {
  auto &out_mem = net->stages[0].output_mems[0];
  head_launch(net, logits_mem);
  int token = 0;
  bm_memcpy_d2s(bm_handle, (void *)&token, out_mem);
  return token;
}

int Qwen::penalty_sample(const bm_net_info_t *net, bm_device_mem_t &logits_mem,
                         std::vector<int> &input_tokens, int &token_length) {
  auto &in1_mem = net->stages[0].input_mems[1];
  auto &in2_mem = net->stages[0].input_mems[2];
  auto &in3_mem = net->stages[0].input_mems[3];
  auto &in4_mem = net->stages[0].input_mems[4];
  auto &out0_mem = net->stages[0].output_mems[0];
  auto &out1_mem = net->stages[0].output_mems[1];

  // repeat_penalty + top_p + top_k + temperature
  std::vector<int> generated_tokens(SEQLEN, input_tokens[token_length - 1]);
  repeat_last_n = std::min(repeat_last_n, token_length);
  std::copy(input_tokens.begin() + token_length - repeat_last_n,
            input_tokens.begin() + token_length, generated_tokens.begin());
  bm_memcpy_s2d(bm_handle, in1_mem, (void *)generated_tokens.data());
  bm_memcpy_s2d(bm_handle, in2_mem, (void *)&top_p);
  bm_memcpy_s2d(bm_handle, in3_mem, (void *)&temperature);
  bm_memcpy_s2d(bm_handle, in4_mem, (void *)&repeat_penalty);

  // inference
  head_launch(net, logits_mem);

  // get logit & token
  int candidate_num = net->stages[0].output_shapes[0].dims[1];
  std::vector<float> probs(candidate_num);
  bm_memcpy_d2s(bm_handle, probs.data(), out0_mem);
  std::vector<int> tokens(candidate_num);
  bm_memcpy_d2s(bm_handle, tokens.data(), out1_mem);

  // penalty_sample
  std::discrete_distribution<> dist(probs.begin(), probs.end());
  return tokens[dist(sgen)];
}

int Qwen::forward_first(std::vector<int> &tokens) {
  std::vector<int> position_id(MAX_SHARE_LENGTH, 0);
  std::vector<uint16_t> attention_mask(MAX_SHARE_LENGTH * MAX_SHARE_LENGTH,
                                       mask_value);
  std::fill(total_tokens.begin(), total_tokens.end(), 0);
  std::copy(tokens.begin(), tokens.end(), total_tokens.data());

  total_length = tokens.size();
  share_length = 0;
  unshare_length = 0;

  for (int i = 0; i < total_length; i++) {
    position_id[i] = i;
  }
  for (int i = 0; i < total_length; i++) {
    for (int j = 0; j < MAX_SHARE_LENGTH; j++) {
      if (j <= i) {
        attention_mask[i * MAX_SHARE_LENGTH + j] = 0;
      }
    }
  }

  // empty
  for (int i = 0; i < NUM_LAYERS; i++) {
    empty_net(bm_handle, net_blocks[i]);
    empty_net(bm_handle, net_blocks_cache[i]);
  }

  // forward embeding
  auto &in_mem = net_embed->stages[0].input_mems[0];
  auto &out_mem = net_embed->stages[0].output_mems[0];
  bm_memcpy_s2d(bm_handle, in_mem, (void *)tokens.data());
  net_launch(net_embed); // prefil embedding

  // forward blocks
  for (int idx = 0; idx < NUM_LAYERS; idx++) {
    auto &in0_mem = net_blocks[idx]->stages[0].input_mems[0];
    auto &in1_mem = net_blocks[idx]->stages[0].input_mems[1];
    auto &in2_mem = net_blocks[idx]->stages[0].input_mems[2];
    d2d(in0_mem, out_mem, 0, total_length * hidden_bytes);
    if (idx == 0) {
      // only first time need copy
      bm_memcpy_s2d(bm_handle, in1_mem, (void *)position_id.data());
      bm_memcpy_s2d(bm_handle, in2_mem, (void *)attention_mask.data());
    }
    if (is_dynamic) {
      dynamic_net_launch(net_blocks[idx], total_length);
    } else {
      net_launch(net_blocks[idx]);
    }
    out_mem = net_blocks[idx]->stages[0].output_mems[0];
    d2d(past_key[idx], net_blocks[idx]->stages[0].output_mems[1], 0,
        total_length * kv_bytes);
    d2d(past_value[idx], net_blocks[idx]->stages[0].output_mems[2], 0,
        total_length * kv_bytes);
  }

  // forward lmhead
  auto &lm_in_mem = net_lm->stages[0].input_mems[0];
  auto &lm_out_mem = net_lm->stages[0].output_mems[0];
  bm_memcpy_d2d_byte(bm_handle, lm_in_mem, 0, out_mem,
                     (total_length - 1) * hidden_bytes, hidden_bytes);
  net_launch(net_lm);

  int token = 0;
  if (generation_mode == "greedy") {
    token = greedy_search(net_greedy_head, lm_out_mem);
  } else if (generation_mode == "penalty_sample") {
    token = penalty_sample(net_penalty_sample_head, lm_out_mem, total_tokens,
                           total_length);
  }

  total_tokens[total_length] = token;
  total_length += 1;
  return token;
}

void Qwen::forward_share(std::vector<int> &tokens) {
  std::vector<int> position_id(MAX_SHARE_LENGTH, 0);
  std::vector<uint16_t> attention_mask(MAX_SHARE_LENGTH * MAX_SHARE_LENGTH,
                                       mask_value);
  std::fill(total_tokens.begin(), total_tokens.end(), 0);
  std::copy(tokens.begin(), tokens.end(), total_tokens.data());

  share_length = tokens.size();
  unshare_length = 0;

  for (int i = 0; i < share_length; i++) {
    position_id[i] = i;
  }
  for (int i = 0; i < share_length; i++) {
    for (int j = 0; j < MAX_SHARE_LENGTH; j++) {
      if (j <= i) {
        attention_mask[i * MAX_SHARE_LENGTH + j] = 0;
      }
    }
  }

  // empty
  for (int i = 0; i < NUM_LAYERS; i++) {
    empty_net(bm_handle, net_blocks[i]);
    empty_net(bm_handle, net_blocks_unshare[i]);
    empty_net(bm_handle, net_blocks_cache[i]);
  }

  // forward embeding
  auto &in_mem = net_embed->stages[0].input_mems[0];
  auto &out_mem = net_embed->stages[0].output_mems[0];
  bm_memcpy_s2d(bm_handle, in_mem, (void *)tokens.data());
  net_launch(net_embed); // prefil embedding

  // forward blocks
  for (int idx = 0; idx < NUM_LAYERS; idx++) {
    auto &in0_mem = net_blocks[idx]->stages[0].input_mems[0];
    auto &in1_mem = net_blocks[idx]->stages[0].input_mems[1];
    auto &in2_mem = net_blocks[idx]->stages[0].input_mems[2];
    d2d(in0_mem, out_mem);
    if (idx == 0) {
      // only first time need copy
      bm_memcpy_s2d(bm_handle, in1_mem, (void *)position_id.data());
      bm_memcpy_s2d(bm_handle, in2_mem, (void *)attention_mask.data());
    }
    if (is_dynamic) {
      dynamic_net_launch(net_blocks[idx], share_length);
    } else {
      net_launch(net_blocks[idx]);
    }
    out_mem = net_blocks[idx]->stages[0].output_mems[0];
    d2d(past_key[idx], net_blocks[idx]->stages[0].output_mems[1], 0,
        share_length * kv_bytes);
    d2d(past_value[idx], net_blocks[idx]->stages[0].output_mems[2], 0,
        share_length * kv_bytes);
  }
  return;
}

int Qwen::forward_unshare(std::vector<int> &tokens) {
  std::vector<int> position_id(MAX_UNSHARE_LENGTH, 0);
  std::vector<uint16_t> attention_mask(
      MAX_UNSHARE_LENGTH * (MAX_SHARE_LENGTH + MAX_UNSHARE_LENGTH), mask_value);
  std::fill(total_tokens.begin() + share_length, total_tokens.end(), 0);
  total_tokens.insert(total_tokens.begin() + share_length, tokens.begin(),
                      tokens.end());
  unshare_length = tokens.size();

  for (int i = 0; i < unshare_length; i++) {
    position_id[i] = i + share_length;
  }
  for (int i = 0; i < unshare_length; i++) {
    for (int j = 0; j < share_length; j++) {
      attention_mask[i * (MAX_SHARE_LENGTH + MAX_UNSHARE_LENGTH) + j] = 0;
    }
    for (int j = MAX_SHARE_LENGTH; j < MAX_SHARE_LENGTH + MAX_UNSHARE_LENGTH;
         j++) {
      if (j - MAX_SHARE_LENGTH <= i) {
        attention_mask[i * (MAX_SHARE_LENGTH + MAX_UNSHARE_LENGTH) + j] = 0;
      }
    }
  }

  // forward embeding
  auto &in_mem = net_embed_unshare->stages[0].input_mems[0];
  auto &out_mem = net_embed_unshare->stages[0].output_mems[0];
  bm_memcpy_s2d(bm_handle, in_mem, (void *)tokens.data());
  net_launch(net_embed_unshare); // prefil embedding

  // forward blocks
  int share_size = share_length * kv_bytes;
  int unshare_size = unshare_length * kv_bytes;
  for (int idx = 0; idx < NUM_LAYERS; idx++) {
    auto &in0_mem = net_blocks_unshare[idx]->stages[0].input_mems[0];
    auto &in1_mem = net_blocks_unshare[idx]->stages[0].input_mems[1];
    auto &in2_mem = net_blocks_unshare[idx]->stages[0].input_mems[2];
    auto &in3_mem = net_blocks_unshare[idx]->stages[0].input_mems[3];
    auto &in4_mem = net_blocks_unshare[idx]->stages[0].input_mems[4];
    d2d(in0_mem, out_mem);
    if (io_alone) {
      if (idx == 0) {
        bm_memcpy_s2d(bm_handle, in1_mem, (void *)position_id.data());
        bm_memcpy_s2d(bm_handle, in2_mem, (void *)attention_mask.data());
      } else {
        d2d(in1_mem, net_blocks_unshare[0]->stages[0].input_mems[1]);
        d2d(in2_mem, net_blocks_unshare[0]->stages[0].input_mems[2]);
      }
      d2d(in3_mem, past_key[idx], 0, MAX_SHARE_LENGTH * kv_bytes);
      d2d(in4_mem, past_value[idx], 0, MAX_SHARE_LENGTH * kv_bytes);
    } else {
      throw std::runtime_error("Only support io_alone");
    }
    net_launch(net_blocks_unshare[idx]);
    out_mem = net_blocks_unshare[idx]->stages[0].output_mems[0];
    d2d(past_key[idx], net_blocks_unshare[idx]->stages[0].output_mems[1],
        share_size, unshare_size);
    d2d(past_value[idx], net_blocks_unshare[idx]->stages[0].output_mems[2],
        share_size, unshare_size);
// #ifdef DUMP_TENSOR
//     dump_net_to_file(bm_handle, net_blocks_unshare[idx],
//                      "input_p_with_kvcache_" + std::to_string(idx) + ".npz");
// #endif
  }

  // forward lmhead
  auto &lm_in_mem = net_lm->stages[0].input_mems[0];
  auto &lm_out_mem = net_lm->stages[0].output_mems[0];
  bm_memcpy_d2d_byte(bm_handle, lm_in_mem, 0, out_mem,
                     (unshare_length - 1) * hidden_bytes, hidden_bytes);
  net_launch(net_lm);

  int token = 0;
  if (generation_mode == "greedy") {
    token = greedy_search(net_greedy_head, lm_out_mem);
  } else if (generation_mode == "penalty_sample") {
    token = penalty_sample(net_penalty_sample_head, lm_out_mem, tokens,
                           unshare_length);
  }

  total_length = share_length + unshare_length;
  total_tokens[total_length] = token;
  total_length += 1;
  return token;
}

int Qwen::forward_next() {
  int cur_token = total_tokens[total_length - 1];

  std::vector<uint16_t> attention_mask(SEQLEN + 1, 0);
  for (int i = total_length; i < SEQLEN; i++) {
    attention_mask[i] = mask_value;
  }
  int32_t position_id = total_length - 1;

  // embedding
  auto &in_mem = net_embed_cache->stages[0].input_mems[0];
  auto &out_mem = net_embed_cache->stages[0].output_mems[0];
  bm_memcpy_s2d(bm_handle, in_mem, (void *)&cur_token);
  net_launch(net_embed_cache);

  // blocks
  int token_offset = (total_length - 1) * kv_bytes;
  for (int idx = 0; idx < NUM_LAYERS; idx++) {
    auto &in0_mem = net_blocks_cache[idx]->stages[0].input_mems[0];
    auto &in1_mem = net_blocks_cache[idx]->stages[0].input_mems[1];
    auto &in2_mem = net_blocks_cache[idx]->stages[0].input_mems[2];
    auto &out0_mem = net_blocks_cache[idx]->stages[0].output_mems[0];
    auto &out1_mem = net_blocks_cache[idx]->stages[0].output_mems[1];
    auto &out2_mem = net_blocks_cache[idx]->stages[0].output_mems[2];
    d2d(in0_mem, out_mem);
    if (io_alone) {
      if (idx == 0) {
        bm_memcpy_s2d(bm_handle, in1_mem, (void *)&position_id);
        bm_memcpy_s2d(bm_handle, in2_mem, (void *)attention_mask.data());
      } else {
        d2d(in1_mem, net_blocks_cache[0]->stages[0].input_mems[1]);
        d2d(in2_mem, net_blocks_cache[0]->stages[0].input_mems[2]);
      }
    } else {
      throw std::runtime_error("Only support io_alone");
    }
    net_launch(net_blocks_cache[idx]);
    out_mem = out0_mem;
    bm_memcpy_d2d_byte(bm_handle, past_key[idx], token_offset, out1_mem, 0,
                       kv_bytes);
    bm_memcpy_d2d_byte(bm_handle, past_value[idx], token_offset, out2_mem, 0,
                       kv_bytes);
  }

  // forward lmhead
  auto &lm_in_mem = net_lm->stages[0].input_mems[0];
  auto &lm_out_mem = net_lm->stages[0].output_mems[0];
  d2d(lm_in_mem, out_mem);
  net_launch(net_lm);

  int token = 0;
  if (generation_mode == "greedy") {
    token = greedy_search(net_greedy_head, lm_out_mem);
  } else if (generation_mode == "penalty_sample") {
    token = penalty_sample(net_penalty_sample_head, lm_out_mem, total_tokens,
                           total_length);
  }

  total_tokens[total_length] = token;
  total_length += 1;
  return token;
}

// std::vector<int> Qwen::generate(std::vector<int> &history_tokens, int EOS) {
//   if (history_tokens.empty(bm_handle, )) {
//     printf("Sorry: your question is empty!!\n");
//     history_tokens.clear();
//     return {};
//   }

//   // make sure token not too large
//   if ((int)history_tokens.size() > SEQLEN - 10) {
//     history_tokens.clear();
//     printf("Error: your question is too large!\n");
//     return {};
//   }

//   std::vector<int> result_tokens;
//   int token = forward_first(history_tokens);
//   while (token != EOS && token_length < SEQLEN) {
//     result_tokens.emplace_back(token);
//     token = forward_share_next();
//   }

//   return result_tokens;
// }

PYBIND11_MODULE(chat, m) {
  pybind11::class_<Qwen>(m, "Qwen")
      .def(pybind11::init<>())
      .def("init", &Qwen::init)
      .def("forward_first", &Qwen::forward_first)
      .def("forward_share", &Qwen::forward_share)
      .def("forward_unshare", &Qwen::forward_unshare)
      .def("forward_next", &Qwen::forward_next)
      .def("free_device", &Qwen::free_device)
      .def("deinit", &Qwen::deinit)
      .def("empty_kvcache", &Qwen::empty_kvcache)
      .def_readwrite("SEQLEN", &Qwen::SEQLEN) // read SEQLEN in pipeline.py
      .def_readwrite("MAX_SHARE_LENGTH", &Qwen::MAX_SHARE_LENGTH)
      .def_readwrite("total_length", &Qwen::total_length)
      .def_readwrite("share_length", &Qwen::share_length)
      .def_readwrite("unshare_length", &Qwen::unshare_length)
      .def_readwrite("temperature", &Qwen::temperature)
      .def_readwrite("top_p", &Qwen::top_p)
      .def_readwrite("repeat_penalty", &Qwen::repeat_penalty)
      .def_readwrite("repeat_last_n", &Qwen::repeat_last_n)
      .def_readwrite("max_new_tokens", &Qwen::max_new_tokens)
      .def_readwrite("generation_mode", &Qwen::generation_mode)
      .def_readwrite("prompt_mode", &Qwen::prompt_mode)
      .def_readwrite("memory_prealloc", &Qwen::memory_prealloc)
      .def_readwrite("io_alone_reuse", &Qwen::io_alone_reuse)
      .def_readwrite("lib_path", &Qwen::lib_path)
      .def_readwrite("is_decrypt", &Qwen::is_decrypt);
}
