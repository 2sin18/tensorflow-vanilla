/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/pjrt/gpu/se_gpu_pjrt_client.h"

#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_stream_executor_client.h"
#include "tensorflow/compiler/xla/stream_executor/device_memory.h"
#include "tensorflow/tsl/framework/bfc_allocator.h"
#include "tensorflow/tsl/platform/errors.h"

#ifdef GOOGLE_CUDA
#include "third_party/gpus/cuda/include/cuda.h"
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"
#include "tensorflow/compiler/xla/pjrt/gpu/nccl_id_store.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_activation.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_cudamallocasync_allocator.h"
#endif  // GOOGLE_CUDA

#ifdef TENSORFLOW_USE_ROCM
#include "rocm/rocm_config.h"
#include "tensorflow/compiler/xla/pjrt/gpu/nccl_id_store.h"  // NOLINT(build/include)
#endif  // TENSORFLOW_USE_ROCM

#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_executable_run_options.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/stream_executor/device_host_allocator.h"
#include "tensorflow/compiler/xla/stream_executor/device_mem_allocator.h"
#include "tensorflow/compiler/xla/stream_executor/tf_allocator_adapter.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/tsl/framework/device_id.h"
#include "tensorflow/tsl/util/env_var.h"

namespace xla {
namespace {

#if defined(GOOGLE_CUDA) && CUDA_VERSION >= 11020

StatusOr<std::unique_ptr<se::MultiDeviceAdapter>> CreateCudaAsyncAllocator(
    se::Platform* platform,
    const std::map<int, std::unique_ptr<LocalDeviceState>>& addressable_devices,
    double memory_fraction, bool preallocate) {
  CHECK_GT(addressable_devices.size(), 0);
  std::vector<se::MultiDeviceAdapter::AllocatorWithStream> allocators;

  for (auto& ordinal_and_device : addressable_devices) {
    se::StreamExecutor* executor = ordinal_and_device.second->executor();
    int device_ordinal = executor->device_ordinal();

    int64_t free_memory;
    int64_t total_memory;
    if (!executor->DeviceMemoryUsage(&free_memory, &total_memory)) {
      return Unavailable("Failed to query available memory from device %i",
                         device_ordinal);
    }
    // To allow full GPU memory to be visible to the BFC allocator if using
    // unified memory.
    // When unified memory is enabled, allow GPU memory oversubscription by
    // setting memory_fraction > 1.
    size_t allocator_memory = total_memory * memory_fraction;
    if (preallocate) {
      LOG(INFO) << "XLA backend allocating " << allocator_memory
                << " bytes on device " << device_ordinal
                << " for BFCAllocator.";
    } else {
      LOG(INFO) << "XLA backend will use up to " << allocator_memory
                << " bytes on device " << device_ordinal
                << " for BFCAllocator.";
    }

    auto allocator = std::make_unique<se::GpuCudaMallocAsyncAllocator>(
        tsl::PlatformDeviceId(device_ordinal), allocator_memory, preallocate);
    allocator->SetStreamAndPreallocateMemory(
        ordinal_and_device.second->compute_stream()
            ->implementation()
            ->GpuStreamMemberHack());
    allocators.emplace_back(std::move(allocator),
                            ordinal_and_device.second->compute_stream());
  }
  return std::make_unique<se::MultiDeviceAdapter>(platform,
                                                  std::move(allocators));
}

#else  // defined(GOOGLE_CUDA) && CUDA_VERSION >= 11020

StatusOr<std::unique_ptr<se::MultiDeviceAdapter>> CreateCudaAsyncAllocator(
    se::Platform* platform,
    const std::map<int, std::unique_ptr<LocalDeviceState>>& addressable_devices,
    double memory_fraction, bool preallocate) {
  return FailedPrecondition("CUDA async allocator requires CUDA >= 11.2");
}

#endif  // defined(GOOGLE_CUDA) && CUDA_VERSION >= 11020

// A custom PjRtClient that overrides the device assignment method.
class StreamExecutorGpuClient : public xla::PjRtStreamExecutorClient {
 public:
  using xla::PjRtStreamExecutorClient::PjRtStreamExecutorClient;

  xla::StatusOr<xla::DeviceAssignment> GetDefaultDeviceAssignment(
      int num_replicas, int num_partitions) const override;

  absl::string_view platform_version() const override {
#define STRINGIFY2(X) #X
#define STRINGIFY(X) STRINGIFY2(X)
#if TENSORFLOW_USE_ROCM && defined(TF_ROCM_VERSION)  // rocm
    // TF_ROCM_VERSION fomrat may change in future. Use it
    // cautiously
    return "rocm " STRINGIFY(TF_ROCM_VERSION);
#elif GOOGLE_CUDA && defined(CUDART_VERSION)  // cuda
    return "cuda " STRINGIFY(CUDART_VERSION);
#else
    return "<unknown>";
#endif  // TENSORFLOW_USE_ROCM && defined(TF_ROCM_VERSION)
  }
};

xla::StatusOr<xla::DeviceAssignment>
StreamExecutorGpuClient::GetDefaultDeviceAssignment(int num_replicas,
                                                    int num_partitions) const {
  if (num_partitions == 1 && num_replicas <= addressable_devices().size()) {
    xla::DeviceAssignment assignment(num_replicas, 1);
    for (int i = 0; i < num_replicas; ++i) {
      assignment(i, 0) = addressable_devices().at(i)->id();
    }
    return assignment;
  }
  // Fallback to default global device assignment if we can't run locally.
  return PjRtStreamExecutorClient::GetDefaultDeviceAssignment(num_replicas,
                                                              num_partitions);
}

// Builds a LocalDeviceState for each GPU present.
StatusOr<std::map<int, std::unique_ptr<LocalDeviceState>>>
BuildLocalDeviceStates(LocalClient* xla_client, bool asynchronous) {
  std::map<int, std::unique_ptr<LocalDeviceState>> addressable_devices;
  for (se::StreamExecutor* executor :
       xla_client->backend().stream_executors()) {
    addressable_devices.emplace(
        executor->device_ordinal(),
        std::make_unique<LocalDeviceState>(
            executor, xla_client, LocalDeviceState::kComputeSynchronized,
            /*max_inflight_computations=*/32,
            /*allow_event_reuse=*/true, /*use_callback_stream=*/true));
  }
  return std::move(addressable_devices);
}

// Constructs a GPU device memory allocator to use, according to the allocator
// configuration the client requested.
StatusOr<std::unique_ptr<se::DeviceMemoryAllocator>>
GetStreamExecutorGpuDeviceAllocator(
    se::Platform* platform, const GpuAllocatorConfig& allocator_config,
    const std::map<int, std::unique_ptr<LocalDeviceState>>&
        addressable_devices) {
  std::unique_ptr<se::DeviceMemoryAllocator> allocator;
  switch (allocator_config.kind) {
    case GpuAllocatorConfig::Kind::kCudaAsync: {
      auto allocator_or = CreateCudaAsyncAllocator(
          platform, addressable_devices, allocator_config.memory_fraction,
          allocator_config.preallocate);
      if (allocator_or.ok()) {
        LOG(INFO) << "Using CUDA async allocator.";
        allocator = std::move(allocator_or.value());
        break;
      }
      LOG(ERROR) << "Failed to initialize CUDA async allocator: "
                 << allocator_or.status() << "; falling back to BFC.";
      [[fallthrough]];
    }

    case GpuAllocatorConfig::Kind::kDefault:
    case GpuAllocatorConfig::Kind::kBFC: {
      LOG(INFO) << "Using BFC allocator.";
      std::vector<se::StreamExecutor*> executors;
      executors.reserve(addressable_devices.size());
      std::vector<se::MultiDeviceAdapter::AllocatorWithStream>
          allocators_and_streams;
      for (const auto& ordinal_and_device : addressable_devices) {
        TF_ASSIGN_OR_RETURN(
            auto bfc_allocator,
            CreateBFCAllocator(ordinal_and_device.second->executor(),
                               allocator_config.memory_fraction,
                               allocator_config.preallocate));
        allocators_and_streams.emplace_back(
            std::move(bfc_allocator),
            ordinal_and_device.second->compute_stream());
      }
      allocator = std::make_unique<se::MultiDeviceAdapter>(
          platform, std::move(allocators_and_streams));
      break;
    }

    case GpuAllocatorConfig::Kind::kPlatform:
      LOG(INFO) << "Using platform allocator.";
      break;
  }
  return std::move(allocator);
}

std::vector<std::unique_ptr<PjRtStreamExecutorDevice>> BuildLocalDevices(
    std::map<int, std::unique_ptr<LocalDeviceState>> local_device_states) {
  std::vector<std::unique_ptr<PjRtStreamExecutorDevice>> devices;
  for (auto& ordinal_and_device : local_device_states) {
    const se::DeviceDescription& description =
        ordinal_and_device.second->executor()->GetDeviceDescription();
    auto device = std::make_unique<StreamExecutorGpuDevice>(
        ordinal_and_device.first, std::move(ordinal_and_device.second),
        description.name(), description.device_vendor(),
        /*node_id=*/0);
    devices.push_back(std::move(device));
  }
  return devices;
}

// Exists on Linux systems. Unique per OS kernel restart.
static constexpr char kBootIdPath[] = "/proc/sys/kernel/random/boot_id";

// Retrieve content of /proc/sys/kernel/random/boot_id as a string.
// Note that procfs file may have file size 0 which throws off generic file
// readers such as tsl::ReadFileToString.
StatusOr<std::string> GetBootIdString() {
  std::string boot_id_str;
#ifdef __linux__
  std::ifstream file(kBootIdPath);
  if (!file) {
    return NotFound("%s not found.", kBootIdPath);
  }
  std::string line;
  while (std::getline(file, line)) {
    absl::StripAsciiWhitespace(&line);
    absl::StrAppend(&boot_id_str, line);
  }
#endif
  return boot_id_str;
}

Status BuildDistributedDevices(
    std::map<int, std::unique_ptr<LocalDeviceState>> local_device_states,
    std::shared_ptr<DistributedRuntimeClient> distributed_client, int node_id,
    std::vector<std::unique_ptr<PjRtStreamExecutorDevice>>* devices,
    gpu::GpuExecutableRunOptions* gpu_executable_run_options) {
  LocalTopologyProto local_topology;
  local_topology.set_node_id(node_id);
  std::string boot_id_str;
  auto boot_id_str_or_status = GetBootIdString();
  if (!boot_id_str_or_status.ok()) {
    LOG(INFO) << boot_id_str_or_status.status();
  } else {
    boot_id_str = boot_id_str_or_status.value();
  }
  local_topology.set_boot_id(boot_id_str);
  for (const auto& ordinal_and_device : local_device_states) {
    const se::Platform* platform =
        ordinal_and_device.second->executor()->platform();
    TF_ASSIGN_OR_RETURN(
        std::unique_ptr<xla::se::DeviceDescription> desc,
        platform->DescriptionForDevice(ordinal_and_device.first));
    DeviceProto* device_proto = local_topology.add_devices();
    device_proto->set_local_device_ordinal(ordinal_and_device.first);
    device_proto->set_name(desc->name());
    device_proto->set_vendor(desc->device_vendor());
  }
  VLOG(3) << "GPU Local Topology:\n" << local_topology.DebugString();

  GlobalTopologyProto global_topology;
  TF_RETURN_IF_ERROR(
      distributed_client->EnumerateDevices(local_topology, &global_topology));
  VLOG(3) << "GPU Global Topology:\n" << global_topology.DebugString();

  std::map<int, GlobalDeviceId> gpu_device_ids;
  absl::flat_hash_map<GlobalDeviceId, int> device_to_node;
  for (const LocalTopologyProto& node : global_topology.nodes()) {
    for (const DeviceProto& device_proto : node.devices()) {
      GlobalDeviceId global_device_id(device_proto.global_device_id());
      device_to_node[global_device_id] = node.node_id();
      std::unique_ptr<LocalDeviceState> local_device;
      if (node.node_id() == node_id) {
        auto it = local_device_states.find(device_proto.local_device_ordinal());
        TF_RET_CHECK(it != local_device_states.end())
            << device_proto.local_device_ordinal();
        TF_RET_CHECK(it->second != nullptr);
        local_device = std::move(it->second);
        gpu_device_ids[device_proto.local_device_ordinal()] = global_device_id;
      }
      auto device = std::make_unique<StreamExecutorGpuDevice>(
          device_proto.global_device_id(), std::move(local_device),
          device_proto.name(), device_proto.vendor(), node.node_id(),
          device_proto.slice_index());
      devices->push_back(std::move(device));
    }
  }
  for (const auto& device : local_device_states) {
    TF_RET_CHECK(device.second == nullptr);
  }
  gpu_executable_run_options->set_gpu_global_device_ids(
      std::move(gpu_device_ids));
#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  auto nccl_id_store = std::make_shared<NcclIdStore>(
      node_id, distributed_client, device_to_node);
  gpu_executable_run_options->set_nccl_unique_id_callback(
      [nccl_id_store](const gpu::NcclCliqueKey& key) {
        return nccl_id_store->GetNcclUniqueId(key);
      });
#endif  // GOOGLE_CUDA
  return OkStatus();
}

}  // namespace

StreamExecutorGpuDevice::StreamExecutorGpuDevice(
    int id, std::unique_ptr<LocalDeviceState> local_device_state,
    std::string device_kind, std::string device_vendor, int node_id,
    int slice_index)
    : PjRtStreamExecutorDevice(id, std::move(local_device_state),
                               std::move(device_kind), node_id),
      device_vendor_(std::move(device_vendor)),
      slice_index_(slice_index) {
  attributes_ = {
      {"device_vendor", PjRtDeviceAttribute(device_vendor_)},
      {"slice_index", PjRtDeviceAttribute(slice_index)},
  };
  to_string_ = absl::StrFormat(
      "StreamExecutorGpuDevice(id=%i, process_index=%i, slice_index=%i)", id,
      process_index(), slice_index);
}

int StreamExecutorGpuDevice::slice_index() const { return slice_index_; }

absl::string_view StreamExecutorGpuDevice::device_vendor() const {
  return device_vendor_;
}

absl::string_view StreamExecutorGpuDevice::ToString() const {
  return to_string_;
}

StatusOr<std::unique_ptr<PjRtClient>> GetStreamExecutorGpuClient(
    bool asynchronous, const GpuAllocatorConfig& allocator_config,
    std::shared_ptr<DistributedRuntimeClient> distributed_client, int node_id,
    const std::optional<std::set<int>>& allowed_devices,
    std::optional<std::string> platform_name) {
  TF_ASSIGN_OR_RETURN(LocalClient * xla_client,
                      GetGpuXlaClient(platform_name, allowed_devices));
  std::map<int, std::unique_ptr<LocalDeviceState>> local_device_states;
  TF_ASSIGN_OR_RETURN(local_device_states,
                      BuildLocalDeviceStates(xla_client, asynchronous));
  EnablePeerAccess(xla_client->backend().stream_executors());
  TF_ASSIGN_OR_RETURN(
      auto allocator,
      GetStreamExecutorGpuDeviceAllocator(
          xla_client->platform(), allocator_config, local_device_states));
  auto host_memory_allocator =
      GetGpuHostAllocator(local_device_states.begin()->second->executor());

  std::vector<std::unique_ptr<PjRtStreamExecutorDevice>> devices;
  auto gpu_run_options = std::make_unique<gpu::GpuExecutableRunOptions>();
  if (distributed_client) {
    TF_RETURN_IF_ERROR(BuildDistributedDevices(
        std::move(local_device_states), std::move(distributed_client), node_id,
        &devices, gpu_run_options.get()));
  } else {
    devices = BuildLocalDevices(std::move(local_device_states));
  }

  return std::unique_ptr<PjRtClient>(std::make_unique<StreamExecutorGpuClient>(
      GpuName(), xla_client, std::move(devices),
      /*node_id=*/node_id, std::move(allocator),
      std::move(host_memory_allocator),
      /*should_stage_host_to_device_transfers=*/true,
      /*gpu_run_options=*/std::move(gpu_run_options)));
}

}  // namespace xla
