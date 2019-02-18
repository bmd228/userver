#include <components/component_context.hpp>

#include <cassert>
#include <queue>

#include <boost/algorithm/string/join.hpp>

#include <engine/task/cancel.hpp>
#include <engine/task/task_processor.hpp>
#include <logging/log.hpp>
#include <tracing/tracer.hpp>
#include <utils/async.hpp>

#include "component_context_component_info.hpp"

namespace components {

namespace {
const std::string kOnAllComponentsLoadedRootName = "all_components_loaded";
const std::string kClearComponentsRootName = "clear_components";

const std::chrono::seconds kPrintAddingComponentsPeriod{10};
}  // namespace

ComponentsLoadCancelledException::ComponentsLoadCancelledException()
    : std::runtime_error("Components load cancelled") {}

ComponentsLoadCancelledException::ComponentsLoadCancelledException(
    const std::string& message)
    : std::runtime_error(message) {}

ComponentContext::TaskToComponentMapScope::TaskToComponentMapScope(
    ComponentContext& context, const std::string& component_name)
    : context_(context) {
  std::lock_guard<engine::Mutex> lock(context_.component_mutex_);
  auto res = context_.task_to_component_map_.emplace(
      engine::current_task::GetCurrentTaskContext(), component_name);
  if (!res.second)
    throw std::runtime_error(
        "can't create multiple components in the same task simultaneously: "
        "component " +
        res.first->second + " is already registered for current task");
}

ComponentContext::TaskToComponentMapScope::~TaskToComponentMapScope() {
  std::lock_guard<engine::Mutex> lock(context_.component_mutex_);
  context_.task_to_component_map_.erase(
      engine::current_task::GetCurrentTaskContext());
}

ComponentContext::ComponentContext(
    const Manager& manager, TaskProcessorMap task_processor_map,
    const std::set<std::string>& loading_component_names)
    : manager_(manager),
      task_processor_map_(std::move(task_processor_map)),
      print_adding_components_stopped_(false) {
  for (const auto& component_name : loading_component_names) {
    components_.emplace(
        std::piecewise_construct, std::tie(component_name),
        std::forward_as_tuple(
            std::make_unique<impl::ComponentInfo>(component_name)));
  }
  StartPrintAddingComponentsTask();
}

ComponentContext::~ComponentContext() {}

ComponentBase* ComponentContext::AddComponent(const std::string& name,
                                              const ComponentFactory& factory) {
  TaskToComponentMapScope task_to_component_map_scope(*this, name);

  auto& component_info = *components_.at(name);
  if (component_info.GetComponent())
    throw std::runtime_error("trying to add component " + name +
                             " multiple times");

  component_info.SetComponent(factory(*this));

  return component_info.GetComponent();
}

void ComponentContext::OnAllComponentsLoaded() {
  StopPrintAddingComponentsTask();
  tracing::Span span(kOnAllComponentsLoadedRootName);
  return ProcessAllComponentLifetimeStageSwitchings(
      {impl::ComponentLifetimeStage::kRunning,
       &impl::ComponentInfo::OnAllComponentsLoaded, "OnAllComponentsLoaded()",
       DependencyType::kNormal, true});
}

void ComponentContext::OnAllComponentsAreStopping() {
  LOG_INFO() << "Sending stopping notification to all components";
  ProcessAllComponentLifetimeStageSwitchings(
      {impl::ComponentLifetimeStage::kReadyForClearing,
       &impl::ComponentInfo::OnAllComponentsAreStopping,
       "OnAllComponentsAreStopping()", DependencyType::kInverted, false});
}

void ComponentContext::ClearComponents() {
  StopPrintAddingComponentsTask();
  tracing::Span span(kClearComponentsRootName);
  OnAllComponentsAreStopping();

  LOG_INFO() << "Stopping components";
  ProcessAllComponentLifetimeStageSwitchings(
      {impl::ComponentLifetimeStage::kNull,
       &impl::ComponentInfo::ClearComponent, "ClearComponent()",
       DependencyType::kInverted, false});

  LOG_INFO() << "Stopped all components";
}

engine::TaskProcessor& ComponentContext::GetTaskProcessor(
    const std::string& name) const {
  auto it = task_processor_map_.find(name);
  if (it == task_processor_map_.cend()) {
    throw std::runtime_error("Failed to find task processor with name: " +
                             name);
  }
  return *it->second.get();
}

ComponentContext::TaskProcessorPtrMap ComponentContext::GetTaskProcessorsMap()
    const {
  TaskProcessorPtrMap result;
  for (const auto& it : task_processor_map_)
    result.emplace(it.first, it.second.get());

  return result;
}

const Manager& ComponentContext::GetManager() const { return manager_; }

void ComponentContext::CancelComponentsLoad() {
  CancelComponentLifetimeStageSwitching();
  if (components_load_cancelled_.test_and_set()) return;
  for (const auto& component_item : components_) {
    component_item.second->OnLoadingCancelled();
  }
}

void ComponentContext::ProcessSingleComponentLifetimeStageSwitching(
    const std::string& name, impl::ComponentInfo& component_info,
    ComponentLifetimeStageSwitchingParams& params) {
  LOG_DEBUG() << "Preparing to call " << params.stage_switch_handler_name
              << " for component " << name;

  auto wait_cb = [&](const std::string& component_name) {
    auto& dependency_from =
        (params.dependency_type == DependencyType::kNormal ? name
                                                           : component_name);
    auto& dependency_to =
        (params.dependency_type == DependencyType::kInverted ? name
                                                             : component_name);
    auto& other_component_info = *components_.at(component_name);
    if (other_component_info.GetStage() != params.next_stage) {
      LOG_DEBUG() << "Cannot call " << params.stage_switch_handler_name
                  << " for component " << name << " yet (" << dependency_from
                  << " depends on " << dependency_to << ")";
      other_component_info.WaitStage(params.next_stage,
                                     params.stage_switch_handler_name);
    }
  };
  try {
    if (params.dependency_type == DependencyType::kNormal)
      component_info.ForEachItDependsOn(wait_cb);
    else
      component_info.ForEachDependsOnIt(wait_cb);

    LOG_INFO() << "Call " << params.stage_switch_handler_name
               << " for component " << name;
    (component_info.*params.stage_switch_handler)();
  } catch (const impl::StageSwitchingCancelledException& ex) {
    LOG_WARNING() << params.stage_switch_handler_name
                  << " failed for component " << name << ": " << ex.what();
    component_info.SetStage(params.next_stage);
    throw;
  } catch (const std::exception& ex) {
    LOG_ERROR() << params.stage_switch_handler_name << " failed for component "
                << name << ": " << ex.what();
    if (params.allow_cancelling) {
      component_info.SetStageSwitchingCancelled(true);
      if (!params.is_component_lifetime_stage_switchings_cancelled.exchange(
              true)) {
        CancelComponentLifetimeStageSwitching();
      }
      component_info.SetStage(params.next_stage);
      throw;
    }
  }
  component_info.SetStage(params.next_stage);
}

void ComponentContext::ProcessAllComponentLifetimeStageSwitchings(
    ComponentLifetimeStageSwitchingParams params) {
  PrepareComponentLifetimeStageSwitching();

  std::vector<std::pair<std::string, engine::TaskWithResult<void>>> tasks;
  for (const auto& component_item : components_) {
    const auto& name = component_item.first;
    auto& component_info = *component_item.second;
    tasks.emplace_back(name, engine::impl::CriticalAsync([&]() {
                         ProcessSingleComponentLifetimeStageSwitching(
                             name, component_info, params);
                       }));
  }

  try {
    for (auto& task_item : tasks) {
      try {
        task_item.second.Get();
      } catch (const impl::StageSwitchingCancelledException& ex) {
      }
    }
  } catch (const std::exception& ex) {
    if (params.allow_cancelling &&
        !params.is_component_lifetime_stage_switchings_cancelled.exchange(
            true)) {
      CancelComponentLifetimeStageSwitching();
    }

    for (auto& task_item : tasks) {
      if (task_item.second.IsValid()) task_item.second.Wait();
    }

    throw;
  }

  if (params.is_component_lifetime_stage_switchings_cancelled)
    throw std::logic_error(
        params.stage_switch_handler_name +
        " cancelled but only StageSwitchingCancelledExceptions were caught");
}

ComponentBase* ComponentContext::DoFindComponent(
    const std::string& name) const {
  AddDependency(name);

  auto& component_info = *components_.at(name);
  auto component = component_info.GetComponent();
  if (component) return component;

  {
    engine::TaskCancellationBlocker block_cancel;
    std::unique_lock<engine::Mutex> lock(component_mutex_);
    LOG_INFO() << "component " << name << " is not loaded yet, component "
               << GetLoadingComponentName(lock) << " is waiting for it to load";
  }

  return component_info.WaitAndGetComponent();
}

void ComponentContext::AddDependency(const std::string& name) const {
  std::unique_lock<engine::Mutex> lock(component_mutex_);

  const auto& current_component_name = GetLoadingComponentName(lock);
  if (components_.at(current_component_name)->CheckItDependsOn(name)) return;

  LOG_INFO() << "Resolving dependency " << current_component_name << " -> "
             << name;
  CheckForDependencyCycle(current_component_name, name, lock);

  components_.at(current_component_name)->AddItDependsOn(name);
  components_.at(name)->AddDependsOnIt(current_component_name);
}

bool ComponentContext::FindDependencyPathDfs(
    const std::string& current, const std::string& target,
    std::set<std::string>& handled, std::vector<std::string>& dependency_path,
    std::unique_lock<engine::Mutex>& lock) const {
  handled.insert(current);
  bool found = (current == target);

  components_.at(current)->ForEachDependsOnIt([&](const std::string& name) {
    if (!found && !handled.count(name))
      found =
          FindDependencyPathDfs(name, target, handled, dependency_path, lock);
  });

  if (found) dependency_path.push_back(current);

  return found;
}

void ComponentContext::CheckForDependencyCycle(
    const std::string& new_dependency_from,
    const std::string& new_dependency_to,
    std::unique_lock<engine::Mutex>& lock) const {
  std::set<std::string> handled;
  std::vector<std::string> dependency_chain;

  if (FindDependencyPathDfs(new_dependency_from, new_dependency_to, handled,
                            dependency_chain, lock)) {
    dependency_chain.push_back(new_dependency_to);
    LOG_ERROR() << "Found circular dependency between components: "
                << boost::algorithm::join(dependency_chain, " -> ");
    throw std::runtime_error("circular components dependency");
  }
}

void ComponentContext::PrepareComponentLifetimeStageSwitching() {
  for (const auto& component_item : components_) {
    component_item.second->SetStageSwitchingCancelled(false);
  }
}

void ComponentContext::CancelComponentLifetimeStageSwitching() {
  for (const auto& component_item : components_) {
    component_item.second->SetStageSwitchingCancelled(true);
  }
}

std::string ComponentContext::GetLoadingComponentName(
    std::unique_lock<engine::Mutex>&) const {
  try {
    return task_to_component_map_.at(
        engine::current_task::GetCurrentTaskContext());
  } catch (const std::exception&) {
    throw std::runtime_error(
        "FindComponent() can be called only from a task of component creation");
  }
}

void ComponentContext::StartPrintAddingComponentsTask() {
  print_adding_components_task_ =
      std::make_unique<engine::TaskWithResult<void>>(
          engine::impl::CriticalAsync([this]() {
            for (;;) {
              {
                std::unique_lock<engine::Mutex> lock(component_mutex_);
                print_adding_components_cv_.WaitFor(
                    lock, kPrintAddingComponentsPeriod,
                    [this]() { return print_adding_components_stopped_; });
                if (print_adding_components_stopped_) return;
              }
              PrintAddingComponents();
            }
          }));
}

void ComponentContext::StopPrintAddingComponentsTask() {
  LOG_DEBUG() << "Stopping adding components printing";
  {
    std::lock_guard<engine::Mutex> lock(component_mutex_);
    print_adding_components_stopped_ = true;
  }
  print_adding_components_cv_.NotifyAll();
  print_adding_components_task_.reset();
}

void ComponentContext::PrintAddingComponents() const {
  std::vector<std::string> adding_components;
  {
    std::lock_guard<engine::Mutex> lock(component_mutex_);
    for (auto elem : task_to_component_map_) {
      adding_components.push_back(elem.second);
    }
  }
  LOG_INFO() << "still adding components: ["
             << boost::algorithm::join(adding_components, ", ") << ']';
}

}  // namespace components
