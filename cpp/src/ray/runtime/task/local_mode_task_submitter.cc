
#include "local_mode_task_submitter.h"

#include <ray/api/ray_exception.h>

#include <boost/asio/post.hpp>
#include <memory>

#include "../../util/address_helper.h"
#include "../abstract_ray_runtime.h"

namespace ray {
namespace api {

LocalModeTaskSubmitter::LocalModeTaskSubmitter(
    LocalModeRayRuntime &local_mode_ray_tuntime)
    : local_mode_ray_tuntime_(local_mode_ray_tuntime) {
  thread_pool_.reset(new boost::asio::thread_pool(10));
}

ObjectID LocalModeTaskSubmitter::Submit(const InvocationSpec &invocation) {
  /// TODO(Guyang Song): Make the infomation of TaskSpecification more reasonable
  /// We just reuse the TaskSpecification class and make the single process mode work.
  /// Maybe some infomation of TaskSpecification are not reasonable or invalid.
  /// We will enhance this after implement the cluster mode.
  if (dynamic_library_base_addr == 0) {
    dynamic_library_base_addr =
        GetBaseAddressOfLibraryFromAddr((void *)invocation.fptr.function_pointer);
  }
  auto func_offset =
      (size_t)(invocation.fptr.function_pointer - dynamic_library_base_addr);
  auto exec_func_offset =
      (size_t)(invocation.fptr.exec_function_pointer - dynamic_library_base_addr);
  auto functionDescriptor = FunctionDescriptorBuilder::BuildCpp(
      "SingleProcess", std::to_string(func_offset), std::to_string(exec_func_offset));
  rpc::Address address;
  std::unordered_map<std::string, double> required_resources;
  std::unordered_map<std::string, double> required_placement_resources;
  TaskSpecBuilder builder;
  std::string task_name =
      invocation.name.empty() ? functionDescriptor->DefaultTaskName() : invocation.name;
  builder.SetCommonTaskSpec(invocation.task_id, task_name, rpc::Language::CPP,
                            functionDescriptor, local_mode_ray_tuntime_.GetCurrentJobID(),
                            local_mode_ray_tuntime_.GetCurrentTaskId(), 0,
                            local_mode_ray_tuntime_.GetCurrentTaskId(), address, 1,
                            required_resources, required_placement_resources,
                            PlacementGroupID::Nil());
  if (invocation.task_type == TaskType::NORMAL_TASK) {
  } else if (invocation.task_type == TaskType::ACTOR_CREATION_TASK) {
    builder.SetActorCreationTaskSpec(invocation.actor_id);
  } else if (invocation.task_type == TaskType::ACTOR_TASK) {
    const TaskID actor_creation_task_id =
        TaskID::ForActorCreationTask(invocation.actor_id);
    const ObjectID actor_creation_dummy_object_id =
        ObjectID::FromIndex(actor_creation_task_id, 1);
    builder.SetActorTaskSpec(invocation.actor_id, actor_creation_dummy_object_id,
                             ObjectID(), invocation.actor_counter);
  } else {
    throw RayException("unknown task type");
  }
  auto buffer = std::make_shared<::ray::LocalMemoryBuffer>(
      reinterpret_cast<uint8_t *>(invocation.args->data()), invocation.args->size(),
      true);
  /// TODO(Guyang Song): Use both 'AddByRefArg' and 'AddByValueArg' to distinguish
  auto arg = TaskArgByValue(
      std::make_shared<::ray::RayObject>(buffer, nullptr, std::vector<ObjectID>()));
  builder.AddArg(arg);
  auto task_specification = builder.Build();
  ObjectID return_object_id = task_specification.ReturnId(0);

  std::shared_ptr<msgpack::sbuffer> actor;
  std::shared_ptr<absl::Mutex> mutex;
  if (invocation.task_type == TaskType::ACTOR_TASK) {
    absl::MutexLock lock(&actor_contexts_mutex_);
    actor = actor_contexts_.at(invocation.actor_id).get()->current_actor;
    mutex = actor_contexts_.at(invocation.actor_id).get()->actor_mutex;
  }
  AbstractRayRuntime *runtime = &local_mode_ray_tuntime_;
  if (invocation.task_type == TaskType::ACTOR_CREATION_TASK ||
      invocation.task_type == TaskType::ACTOR_TASK) {
    /// TODO(Guyang Song): Handle task dependencies.
    /// Execute actor task directly in the main thread because we must guarantee the actor
    /// task executed by calling order.
    TaskExecutor::Invoke(task_specification, actor, runtime, dynamic_library_base_addr);
  } else {
    boost::asio::post(*thread_pool_.get(),
                      std::bind(
                          [actor, mutex, runtime](TaskSpecification &ts) {
                            if (mutex) {
                              absl::MutexLock lock(mutex.get());
                            }
                            TaskExecutor::Invoke(ts, actor, runtime,
                                                 dynamic_library_base_addr);
                          },
                          std::move(task_specification)));
  }
  return return_object_id;
}

ObjectID LocalModeTaskSubmitter::SubmitTask(const InvocationSpec &invocation) {
  return Submit(invocation);
}

ActorID LocalModeTaskSubmitter::CreateActor(const InvocationSpec &invocation) {
  if (dynamic_library_base_addr == 0) {
    dynamic_library_base_addr =
        GetBaseAddressOfLibraryFromAddr((void *)invocation.fptr.function_pointer);
  }
  ActorID id = local_mode_ray_tuntime_.GetNextActorID();
  typedef std::shared_ptr<msgpack::sbuffer> (*ExecFunction)(
      uintptr_t base_addr, size_t func_offset, std::shared_ptr<msgpack::sbuffer> args);
  ExecFunction exec_function = (ExecFunction)(invocation.fptr.exec_function_pointer);
  auto data = (*exec_function)(
      dynamic_library_base_addr,
      (size_t)(invocation.fptr.function_pointer - dynamic_library_base_addr),
      invocation.args);
  std::unique_ptr<ActorContext> actorContext(new ActorContext());
  actorContext->current_actor = data;
  absl::MutexLock lock(&actor_contexts_mutex_);
  actor_contexts_.emplace(id, std::move(actorContext));
  return id;
}

ObjectID LocalModeTaskSubmitter::SubmitActorTask(const InvocationSpec &invocation) {
  return Submit(invocation);
}

}  // namespace api
}  // namespace ray
