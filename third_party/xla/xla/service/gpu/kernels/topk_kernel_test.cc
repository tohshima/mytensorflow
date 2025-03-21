/* Copyright 2022 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/kernels/topk_kernel.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <functional>
#include <tuple>
#include <vector>

#include "absl/log/check.h"
#include "absl/random/random.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "xla/stream_executor/gpu/gpu_init.h"
#include "xla/stream_executor/gpu/gpu_stream.h"
#include "xla/stream_executor/gpu/gpu_timer.h"
#include "xla/stream_executor/gpu/gpu_types.h"
#include "xla/stream_executor/multi_platform_manager.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream.h"
#include "xla/types.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/test.h"
#include "tsl/platform/test_benchmark.h"

namespace xla::gpu {
namespace {

using se::gpu::GpuStreamHandle;
using ::testing::Combine;
using ::testing::Values;

template <typename T>
std::vector<T> RandomVecRange(int num_elements, T start, T end) {
  std::vector<T> local;
  local.reserve(num_elements);
  thread_local absl::BitGen gen;
  for (int i = 0; i < num_elements; ++i) {
    local.push_back(absl::Uniform<T>(gen, start, end));
  }
  return local;
}

template <typename T>
std::vector<T> RandomVec(int num_elements) {
  return RandomVecRange(num_elements, static_cast<T>(0),
                        static_cast<T>(num_elements));
}

template <typename T>
std::vector<T> RandomVecNegative(int num_elements) {
  return RandomVecRange(num_elements, -static_cast<T>(num_elements),
                        static_cast<T>(0));
}

PrimitiveType Get(float) { return PrimitiveType::F32; }
PrimitiveType Get(bfloat16) { return PrimitiveType::BF16; }

se::StreamExecutor* GetGpuExecutor() {
  auto* platform =
      se::MultiPlatformManager::PlatformWithName(se::GpuPlatformName()).value();
  return platform->ExecutorForDevice(0).value();
}

// Params:
//  - n_kb: number of elements in kilobytes.
//  - k: number of elements to return.
//  - batch_size
//  - offset
using TopkTest = ::testing::TestWithParam<std::tuple<int, int, int, int>>;

// In this test we only check that the TopK logic works with float. For the full
// dtype coverage suite, please add them to topk_test.cc, where we can use XLA
// utilities to simplify the test logic.
TEST_P(TopkTest, TopKFloat) {
  using T = float;

  auto* executor = GetGpuExecutor();
  se::Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  const auto [n_kb, k, batch_size, offset] = GetParam();
  const size_t n = n_kb * 1024 + offset;

  auto input_buffer = executor->AllocateOwnedArray<T>(n * batch_size),
       output_values = executor->AllocateOwnedArray<T>(k * batch_size);
  auto output_indices = executor->AllocateOwnedArray<uint32_t>(k * batch_size);

  ASSERT_TRUE(!(input_buffer.is_null() || output_values.is_null() ||
                output_indices.is_null()));

  auto source = RandomVec<T>(n * batch_size);
  stream.ThenMemcpy(input_buffer.ptr(), source.data(),
                    n * batch_size * sizeof(T));

  ASSERT_TRUE(RunTopk(&stream, Get(T()), *input_buffer, n, *output_values,
                      *output_indices, k, batch_size)
                  .ok());
  std::vector<T> got(k);
  ASSERT_TRUE(stream.BlockHostUntilDone().ok());
  for (int i = 0; i < batch_size; i++) {
    stream.ThenMemcpy(got.data(), output_values->GetSlice(k * i, k),
                      k * sizeof(T));
    std::vector<T> slice(source.data() + n * i, source.data() + n * (i + 1));
    std::sort(slice.begin(), slice.end(), std::greater<T>());
    slice.resize(k);
    EXPECT_THAT(got, ::testing::ElementsAreArray(slice))
        << " k=" << k << ", batch_size=" << batch_size << " i=" << i;
  }
}

TEST_P(TopkTest, TopKPackedNegative) {
  using T = float;

  auto* executor = GetGpuExecutor();
  se::Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  const auto [n_kb, k, batch_size, offset] = GetParam();
  const size_t n = n_kb * 1024 + offset;

  auto input_buffer = executor->AllocateOwnedArray<T>(n * batch_size),
       output_values = executor->AllocateOwnedArray<T>(k * batch_size);
  auto output_indices = executor->AllocateOwnedArray<uint32_t>(k * batch_size);

  ASSERT_TRUE(!(input_buffer.is_null() || output_values.is_null() ||
                output_indices.is_null()));

  auto source = RandomVecNegative<T>(n * batch_size);
  stream.ThenMemcpy(input_buffer.ptr(), source.data(),
                    n * batch_size * sizeof(T));

  ASSERT_TRUE(RunTopk(&stream, Get(T()), *input_buffer, n, *output_values,
                      *output_indices, k, batch_size)
                  .ok());
  std::vector<T> got(k);
  ASSERT_TRUE(stream.BlockHostUntilDone().ok());
  for (int i = 0; i < batch_size; i++) {
    stream.ThenMemcpy(got.data(), output_values->GetSlice(k * i, k),
                      k * sizeof(T));
    std::vector<T> slice(source.data() + n * i, source.data() + n * (i + 1));
    std::sort(slice.begin(), slice.end(), std::greater<T>());
    slice.resize(k);
    EXPECT_THAT(got, ::testing::ElementsAreArray(slice))
        << " k=" << k << ", batch_size=" << batch_size << " i=" << i;
  }
}

INSTANTIATE_TEST_SUITE_P(TopkTests, TopkTest,
                         Combine(
                             /*n_kb=*/Values(1, 8, 12, 64, 128),
                             /*k=*/Values(1, 2, 8, 16, 7, 12),
                             /*batch_size=*/Values(1, 16, 64, 128),
                             /*offset=*/Values(0, 7, 4)),
                         [](const auto& info) {
                           return absl::Substitute(
                               "n$0KiB_k$1_batch_size$2_offset$3",
                               std::get<0>(info.param), std::get<1>(info.param),
                               std::get<2>(info.param),
                               std::get<3>(info.param));
                         });

template <size_t K>
void BM_SmallTopk(benchmark::State& state) {
  using T = float;

  size_t k = K;
  size_t batch_size = state.range(0);
  size_t n = state.range(1) * 1024;
  state.SetLabel(
      absl::Substitute("n=$0Ki k=$1 batch_size=$2", n / 1024, k, batch_size));

  auto* executor = GetGpuExecutor();
  se::Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  auto input_buffer = executor->AllocateOwnedArray<T>(n * batch_size),
       output_values = executor->AllocateOwnedArray<T>(k * batch_size);
  auto output_indices = executor->AllocateOwnedArray<uint32_t>(k * batch_size);

  if (input_buffer.is_null() || output_values.is_null() ||
      output_indices.is_null()) {
    state.SkipWithError("Unable to allocate GPU memory: aborting benchmark");
    return;
  }

  auto source = RandomVec<T>(n);
  // use the same random vector for all batches (otherwise it takes too much
  // time to generate random data)
  for (size_t i = 0; i < batch_size; i++) {
    auto slice = input_buffer->GetSlice(i * n, n);
    stream.ThenMemcpy(&slice, source.data(), n * sizeof(T));
  }

  for (auto _ : state) {
    auto timer = se::gpu::GpuTimer::Create(se::gpu::AsGpuStream(&stream));
    CHECK_OK(timer.status());
    CHECK_OK(RunTopk(&stream, Get(T()), *input_buffer, n, *output_values,
                     *output_indices, k, batch_size));
    CHECK_OK(stream.BlockHostUntilDone());
    auto timer_duration = timer.value().GetElapsedDuration();
    CHECK_OK(timer_duration.status());
    state.SetIterationTime(absl::ToDoubleSeconds(timer_duration.value()));
  }
  size_t items_processed = batch_size * n * state.iterations();
  state.SetItemsProcessed(items_processed);
  state.SetBytesProcessed(items_processed * sizeof(T));
}

BENCHMARK(BM_SmallTopk<1>)->RangePair(1, 1024, 16, 1024)->UseManualTime();
BENCHMARK(BM_SmallTopk<2>)->RangePair(1, 1024, 16, 1024)->UseManualTime();
BENCHMARK(BM_SmallTopk<4>)->RangePair(1, 1024, 16, 1024)->UseManualTime();
BENCHMARK(BM_SmallTopk<8>)->RangePair(1, 1024, 16, 1024)->UseManualTime();
BENCHMARK(BM_SmallTopk<16>)->RangePair(1, 1024, 16, 1024)->UseManualTime();

}  // namespace
}  // namespace xla::gpu
