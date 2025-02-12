#include "env.h"

#include "async_wrap.h"
#include "memory_tracker-inl.h"
#include "node_buffer.h"
#include "node_context_data.h"
#include "node_errors.h"
#include "node_file.h"
#include "node_internals.h"
#include "node_native_module.h"
#include "node_options-inl.h"
#include "node_process.h"
#include "node_v8_platform-inl.h"
#include "node_worker.h"
#include "tracing/agent.h"
#include "tracing/traced_value.h"
#include "util-inl.h"
#include "v8-profiler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>

namespace node {

using errors::TryCatchScope;
using v8::ArrayBuffer;
using v8::Boolean;
using v8::Context;
using v8::EmbedderGraph;
using v8::Function;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::NewStringType;
using v8::Number;
using v8::Object;
using v8::Private;
using v8::SnapshotCreator;
using v8::StackTrace;
using v8::String;
using v8::Symbol;
using v8::TracingController;
using v8::Undefined;
using v8::Value;
using worker::Worker;

int const Environment::kNodeContextTag = 0x6e6f64;
void* const Environment::kNodeContextTagPtr = const_cast<void*>(
    static_cast<const void*>(&Environment::kNodeContextTag));

std::vector<size_t> IsolateData::Serialize(SnapshotCreator* creator) {
  Isolate* isolate = creator->GetIsolate();
  std::vector<size_t> indexes;
  HandleScope handle_scope(isolate);
  // XXX(joyeecheung): technically speaking, the indexes here should be
  // consecutive and we could just return a range instead of an array,
  // but that's not part of the V8 API contract so we use an array
  // just to be safe.

#define VP(PropertyName, StringValue) V(v8::Private, PropertyName)
#define VY(PropertyName, StringValue) V(v8::Symbol, PropertyName)
#define VS(PropertyName, StringValue) V(v8::String, PropertyName)
#define V(TypeName, PropertyName)                                              \
  indexes.push_back(creator->AddData(PropertyName##_.Get(isolate)));
  PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(VP)
  PER_ISOLATE_SYMBOL_PROPERTIES(VY)
  PER_ISOLATE_STRING_PROPERTIES(VS)
#undef V
#undef VY
#undef VS
#undef VP

  return indexes;
}

void IsolateData::DeserializeProperties(const std::vector<size_t>* indexes) {
  size_t i = 0;
  HandleScope handle_scope(isolate_);

#define VP(PropertyName, StringValue) V(v8::Private, PropertyName)
#define VY(PropertyName, StringValue) V(v8::Symbol, PropertyName)
#define VS(PropertyName, StringValue) V(v8::String, PropertyName)
#define V(TypeName, PropertyName)                                              \
  do {                                                                         \
    MaybeLocal<TypeName> field =                                               \
        isolate_->GetDataFromSnapshotOnce<TypeName>((*indexes)[i++]);          \
    if (field.IsEmpty()) {                                                     \
      fprintf(stderr, "Failed to deserialize " #PropertyName "\n");            \
    }                                                                          \
    PropertyName##_.Set(isolate_, field.ToLocalChecked());                     \
  } while (0);
  PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(VP)
  PER_ISOLATE_SYMBOL_PROPERTIES(VY)
  PER_ISOLATE_STRING_PROPERTIES(VS)
#undef V
#undef VY
#undef VS
#undef VP
}

void IsolateData::CreateProperties() {
  // Create string and private symbol properties as internalized one byte
  // strings after the platform is properly initialized.
  //
  // Internalized because it makes property lookups a little faster and
  // because the string is created in the old space straight away.  It's going
  // to end up in the old space sooner or later anyway but now it doesn't go
  // through v8::Eternal's new space handling first.
  //
  // One byte because our strings are ASCII and we can safely skip V8's UTF-8
  // decoding step.

  HandleScope handle_scope(isolate_);

#define V(PropertyName, StringValue)                                           \
  PropertyName##_.Set(                                                         \
      isolate_,                                                                \
      Private::New(isolate_,                                                   \
                   String::NewFromOneByte(                                     \
                       isolate_,                                               \
                       reinterpret_cast<const uint8_t*>(StringValue),          \
                       NewStringType::kInternalized,                           \
                       sizeof(StringValue) - 1)                                \
                       .ToLocalChecked()));
  PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(V)
#undef V
#define V(PropertyName, StringValue)                                           \
  PropertyName##_.Set(                                                         \
      isolate_,                                                                \
      Symbol::New(isolate_,                                                    \
                  String::NewFromOneByte(                                      \
                      isolate_,                                                \
                      reinterpret_cast<const uint8_t*>(StringValue),           \
                      NewStringType::kInternalized,                            \
                      sizeof(StringValue) - 1)                                 \
                      .ToLocalChecked()));
  PER_ISOLATE_SYMBOL_PROPERTIES(V)
#undef V
#define V(PropertyName, StringValue)                                           \
  PropertyName##_.Set(                                                         \
      isolate_,                                                                \
      String::NewFromOneByte(isolate_,                                         \
                             reinterpret_cast<const uint8_t*>(StringValue),    \
                             NewStringType::kInternalized,                     \
                             sizeof(StringValue) - 1)                          \
          .ToLocalChecked());
  PER_ISOLATE_STRING_PROPERTIES(V)
#undef V
}

IsolateData::IsolateData(Isolate* isolate,
                         uv_loop_t* event_loop,
                         MultiIsolatePlatform* platform,
                         ArrayBufferAllocator* node_allocator,
                         const std::vector<size_t>* indexes)
    : isolate_(isolate),
      event_loop_(event_loop),
      allocator_(isolate->GetArrayBufferAllocator()),
      node_allocator_(node_allocator == nullptr ? nullptr
                                                : node_allocator->GetImpl()),
      uses_node_allocator_(allocator_ == node_allocator_),
      platform_(platform) {
  CHECK_NOT_NULL(allocator_);

  options_.reset(
      new PerIsolateOptions(*(per_process::cli_options->per_isolate)));

  if (indexes == nullptr) {
    CreateProperties();
  } else {
    DeserializeProperties(indexes);
  }
}

void IsolateData::MemoryInfo(MemoryTracker* tracker) const {
#define V(PropertyName, StringValue)                                           \
  tracker->TrackField(#PropertyName, PropertyName(isolate()));
  PER_ISOLATE_SYMBOL_PROPERTIES(V)
#undef V

#define V(PropertyName, StringValue)                                           \
  tracker->TrackField(#PropertyName, PropertyName(isolate()));
  PER_ISOLATE_STRING_PROPERTIES(V)
#undef V

  if (node_allocator_ != nullptr) {
    tracker->TrackFieldWithSize(
        "node_allocator", sizeof(*node_allocator_), "NodeArrayBufferAllocator");
  } else {
    tracker->TrackFieldWithSize(
        "allocator", sizeof(*allocator_), "v8::ArrayBuffer::Allocator");
  }
  tracker->TrackFieldWithSize(
      "platform", sizeof(*platform_), "MultiIsolatePlatform");
  // TODO(joyeecheung): implement MemoryRetainer in the option classes.
}

void InitThreadLocalOnce() {
  CHECK_EQ(0, uv_key_create(&Environment::thread_local_env));
}

void TrackingTraceStateObserver::UpdateTraceCategoryState() {
  if (!env_->owns_process_state()) {
    // Ideally, we’d have a consistent story that treats all threads/Environment
    // instances equally here. However, tracing is essentially global, and this
    // callback is called from whichever thread calls `StartTracing()` or
    // `StopTracing()`. The only way to do this in a threadsafe fashion
    // seems to be only tracking this from the main thread, and only allowing
    // these state modifications from the main thread.
    return;
  }

  bool async_hooks_enabled = (*(TRACE_EVENT_API_GET_CATEGORY_GROUP_ENABLED(
                                 TRACING_CATEGORY_NODE1(async_hooks)))) != 0;

  Isolate* isolate = env_->isolate();
  HandleScope handle_scope(isolate);
  Local<Function> cb = env_->trace_category_state_function();
  if (cb.IsEmpty())
    return;
  TryCatchScope try_catch(env_);
  try_catch.SetVerbose(true);
  Local<Value> args[] = {Boolean::New(isolate, async_hooks_enabled)};
  cb->Call(env_->context(), Undefined(isolate), arraysize(args), args)
      .ToLocalChecked();
}

static std::atomic<uint64_t> next_thread_id{0};

uint64_t Environment::AllocateThreadId() {
  return next_thread_id++;
}

void Environment::CreateProperties() {
  HandleScope handle_scope(isolate_);
  Local<Context> ctx = context();
  Local<FunctionTemplate> templ = FunctionTemplate::New(isolate());
  templ->InstanceTemplate()->SetInternalFieldCount(1);
  Local<Object> obj = templ->GetFunction(ctx)
                          .ToLocalChecked()
                          ->NewInstance(ctx)
                          .ToLocalChecked();
  obj->SetAlignedPointerInInternalField(0, this);
  set_as_callback_data(obj);
  set_as_callback_data_template(templ);

  // Store primordials setup by the per-context script in the environment.
  Local<Object> per_context_bindings =
      GetPerContextExports(ctx).ToLocalChecked();
  Local<Value> primordials =
      per_context_bindings->Get(ctx, primordials_string()).ToLocalChecked();
  CHECK(primordials->IsObject());
  set_primordials(primordials.As<Object>());

  Local<Object> process_object =
      node::CreateProcessObject(this).FromMaybe(Local<Object>());
  set_process_object(process_object);
}

std::string GetExecPath(const std::vector<std::string>& argv) {
  char exec_path_buf[2 * PATH_MAX];
  size_t exec_path_len = sizeof(exec_path_buf);
  std::string exec_path;
  if (uv_exepath(exec_path_buf, &exec_path_len) == 0) {
    exec_path = std::string(exec_path_buf, exec_path_len);
  } else {
    exec_path = argv[0];
  }

  // On OpenBSD process.execPath will be relative unless we
  // get the full path before process.execPath is used.
#if defined(__OpenBSD__)
  uv_fs_t req;
  req.ptr = nullptr;
  if (0 ==
      uv_fs_realpath(nullptr, &req, exec_path.c_str(), nullptr)) {
    CHECK_NOT_NULL(req.ptr);
    exec_path = std::string(static_cast<char*>(req.ptr));
  }
  uv_fs_req_cleanup(&req);
#endif

  return exec_path;
}

Environment::Environment(IsolateData* isolate_data,
                         Local<Context> context,
                         const std::vector<std::string>& args,
                         const std::vector<std::string>& exec_args,
                         Flags flags,
                         uint64_t thread_id)
    : isolate_(context->GetIsolate()),
      isolate_data_(isolate_data),
      immediate_info_(context->GetIsolate()),
      tick_info_(context->GetIsolate()),
      timer_base_(uv_now(isolate_data->event_loop())),
      exec_argv_(exec_args),
      argv_(args),
      exec_path_(GetExecPath(args)),
      should_abort_on_uncaught_toggle_(isolate_, 1),
      stream_base_state_(isolate_, StreamBase::kNumStreamBaseStateFields),
      flags_(flags),
      thread_id_(thread_id == kNoThreadId ? AllocateThreadId() : thread_id),
      fs_stats_field_array_(isolate_, kFsStatsBufferLength),
      fs_stats_field_bigint_array_(isolate_, kFsStatsBufferLength),
      context_(context->GetIsolate(), context) {
  // We'll be creating new objects so make sure we've entered the context.
  HandleScope handle_scope(isolate());
  Context::Scope context_scope(context);

  set_env_vars(per_process::system_environment);

  // We create new copies of the per-Environment option sets, so that it is
  // easier to modify them after Environment creation. The defaults are
  // part of the per-Isolate option set, for which in turn the defaults are
  // part of the per-process option set.
  options_.reset(new EnvironmentOptions(*isolate_data->options()->per_env));
  inspector_host_port_.reset(new HostPort(options_->debug_options().host_port));

#if HAVE_INSPECTOR
  // We can only create the inspector agent after having cloned the options.
  inspector_agent_ = std::make_unique<inspector::Agent>(this);
#endif

  AssignToContext(context, ContextInfo(""));

  if (tracing::AgentWriterHandle* writer = GetTracingAgentWriter()) {
    trace_state_observer_ = std::make_unique<TrackingTraceStateObserver>(this);
    TracingController* tracing_controller = writer->GetTracingController();
    tracing_controller->AddTraceStateObserver(trace_state_observer_.get());
  }

  destroy_async_id_list_.reserve(512);
  BeforeExit(
      [](void* arg) {
        Environment* env = static_cast<Environment*>(arg);
        if (!env->destroy_async_id_list()->empty())
          AsyncWrap::DestroyAsyncIdsCallback(env);
      },
      this);

  performance_state_ =
      std::make_unique<performance::performance_state>(isolate());
  performance_state_->Mark(
      performance::NODE_PERFORMANCE_MILESTONE_ENVIRONMENT);
  performance_state_->Mark(performance::NODE_PERFORMANCE_MILESTONE_NODE_START,
                           per_process::node_start_time);
  performance_state_->Mark(
      performance::NODE_PERFORMANCE_MILESTONE_V8_START,
      performance::performance_v8_start);

  if (*TRACE_EVENT_API_GET_CATEGORY_GROUP_ENABLED(
          TRACING_CATEGORY_NODE1(environment)) != 0) {
    auto traced_value = tracing::TracedValue::Create();
    traced_value->BeginArray("args");
    for (const std::string& arg : args) traced_value->AppendString(arg);
    traced_value->EndArray();
    traced_value->BeginArray("exec_args");
    for (const std::string& arg : exec_args) traced_value->AppendString(arg);
    traced_value->EndArray();
    TRACE_EVENT_NESTABLE_ASYNC_BEGIN1(TRACING_CATEGORY_NODE1(environment),
                                      "Environment",
                                      this,
                                      "args",
                                      std::move(traced_value));
  }

  // By default, always abort when --abort-on-uncaught-exception was passed.
  should_abort_on_uncaught_toggle_[0] = 1;

  std::string debug_cats;
  credentials::SafeGetenv("NODE_DEBUG_NATIVE", &debug_cats, this);
  set_debug_categories(debug_cats, true);

  if (options_->no_force_async_hooks_checks) {
    async_hooks_.no_force_checks();
  }

  // TODO(joyeecheung): deserialize when the snapshot covers the environment
  // properties.
  CreateProperties();
}

Environment::~Environment() {
  isolate()->GetHeapProfiler()->RemoveBuildEmbedderGraphCallback(
      BuildEmbedderGraph, this);

  // Make sure there are no re-used libuv wrapper objects.
  // CleanupHandles() should have removed all of them.
  CHECK(file_handle_read_wrap_freelist_.empty());

  HandleScope handle_scope(isolate());

#if HAVE_INSPECTOR
  // Destroy inspector agent before erasing the context. The inspector
  // destructor depends on the context still being accessible.
  inspector_agent_.reset();
#endif

  context()->SetAlignedPointerInEmbedderData(
      ContextEmbedderIndex::kEnvironment, nullptr);

  if (trace_state_observer_) {
    tracing::AgentWriterHandle* writer = GetTracingAgentWriter();
    CHECK_NOT_NULL(writer);
    TracingController* tracing_controller = writer->GetTracingController();
    tracing_controller->RemoveTraceStateObserver(trace_state_observer_.get());
  }

  delete[] heap_statistics_buffer_;
  delete[] heap_space_statistics_buffer_;
  delete[] http_parser_buffer_;
  delete[] heap_code_statistics_buffer_;

  TRACE_EVENT_NESTABLE_ASYNC_END0(
    TRACING_CATEGORY_NODE1(environment), "Environment", this);

  // Do not unload addons on the main thread. Some addons need to retain memory
  // beyond the Environment's lifetime, and unloading them early would break
  // them; with Worker threads, we have the opportunity to be stricter.
  // Also, since the main thread usually stops just before the process exits,
  // this is far less relevant here.
  if (!is_main_thread()) {
    // Dereference all addons that were loaded into this environment.
    for (binding::DLib& addon : loaded_addons_) {
      addon.Close();
    }
  }
}

void Environment::InitializeLibuv(bool start_profiler_idle_notifier) {
  HandleScope handle_scope(isolate());
  Context::Scope context_scope(context());

  CHECK_EQ(0, uv_timer_init(event_loop(), timer_handle()));
  uv_unref(reinterpret_cast<uv_handle_t*>(timer_handle()));

  uv_check_init(event_loop(), immediate_check_handle());
  uv_unref(reinterpret_cast<uv_handle_t*>(immediate_check_handle()));

  uv_idle_init(event_loop(), immediate_idle_handle());

  uv_check_start(immediate_check_handle(), CheckImmediate);

  // Inform V8's CPU profiler when we're idle.  The profiler is sampling-based
  // but not all samples are created equal; mark the wall clock time spent in
  // epoll_wait() and friends so profiling tools can filter it out.  The samples
  // still end up in v8.log but with state=IDLE rather than state=EXTERNAL.
  // TODO(bnoordhuis) Depends on a libuv implementation detail that we should
  // probably fortify in the API contract, namely that the last started prepare
  // or check watcher runs first.  It's not 100% foolproof; if an add-on starts
  // a prepare or check watcher after us, any samples attributed to its callback
  // will be recorded with state=IDLE.
  uv_prepare_init(event_loop(), &idle_prepare_handle_);
  uv_check_init(event_loop(), &idle_check_handle_);
  uv_unref(reinterpret_cast<uv_handle_t*>(&idle_prepare_handle_));
  uv_unref(reinterpret_cast<uv_handle_t*>(&idle_check_handle_));

  thread_stopper()->Install(
    this, static_cast<void*>(this), [](uv_async_t* handle) {
      Environment* env = static_cast<Environment*>(handle->data);
      uv_stop(env->event_loop());
    });
  thread_stopper()->set_stopped(false);
  uv_unref(reinterpret_cast<uv_handle_t*>(thread_stopper()->GetHandle()));

  // Register clean-up cb to be called to clean up the handles
  // when the environment is freed, note that they are not cleaned in
  // the one environment per process setup, but will be called in
  // FreeEnvironment.
  RegisterHandleCleanups();

  if (start_profiler_idle_notifier) {
    StartProfilerIdleNotifier();
  }

  static uv_once_t init_once = UV_ONCE_INIT;
  uv_once(&init_once, InitThreadLocalOnce);
  uv_key_set(&thread_local_env, this);
}

void Environment::ExitEnv() {
  set_can_call_into_js(false);
  thread_stopper()->Stop();
  isolate_->TerminateExecution();
}

void Environment::RegisterHandleCleanups() {
  HandleCleanupCb close_and_finish = [](Environment* env, uv_handle_t* handle,
                                        void* arg) {
    handle->data = env;

    env->CloseHandle(handle, [](uv_handle_t* handle) {
#ifdef DEBUG
      memset(handle, 0xab, uv_handle_size(handle->type));
#endif
    });
  };

  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(timer_handle()),
      close_and_finish,
      nullptr);
  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(immediate_check_handle()),
      close_and_finish,
      nullptr);
  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(immediate_idle_handle()),
      close_and_finish,
      nullptr);
  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(&idle_prepare_handle_),
      close_and_finish,
      nullptr);
  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(&idle_check_handle_),
      close_and_finish,
      nullptr);
}

void Environment::CleanupHandles() {
  for (ReqWrapBase* request : req_wrap_queue_)
    request->Cancel();

  for (HandleWrap* handle : handle_wrap_queue_)
    handle->Close();

  for (HandleCleanup& hc : handle_cleanup_queue_)
    hc.cb_(this, hc.handle_, hc.arg_);
  handle_cleanup_queue_.clear();

  while (handle_cleanup_waiting_ != 0 ||
         request_waiting_ != 0 ||
         !handle_wrap_queue_.IsEmpty()) {
    uv_run(event_loop(), UV_RUN_ONCE);
  }

  file_handle_read_wrap_freelist_.clear();
}

void Environment::StartProfilerIdleNotifier() {
  if (profiler_idle_notifier_started_)
    return;

  profiler_idle_notifier_started_ = true;

  uv_prepare_start(&idle_prepare_handle_, [](uv_prepare_t* handle) {
    Environment* env = ContainerOf(&Environment::idle_prepare_handle_, handle);
    env->isolate()->SetIdle(true);
  });

  uv_check_start(&idle_check_handle_, [](uv_check_t* handle) {
    Environment* env = ContainerOf(&Environment::idle_check_handle_, handle);
    env->isolate()->SetIdle(false);
  });
}

void Environment::StopProfilerIdleNotifier() {
  profiler_idle_notifier_started_ = false;
  uv_prepare_stop(&idle_prepare_handle_);
  uv_check_stop(&idle_check_handle_);
}

void Environment::PrintSyncTrace() const {
  if (!options_->trace_sync_io) return;

  HandleScope handle_scope(isolate());

  fprintf(
      stderr, "(node:%d) WARNING: Detected use of sync API\n", uv_os_getpid());
  PrintStackTrace(
      isolate(),
      StackTrace::CurrentStackTrace(isolate(), 10, StackTrace::kDetailed));
}

void Environment::RunCleanup() {
  started_cleanup_ = true;
  TraceEventScope trace_scope(TRACING_CATEGORY_NODE1(environment),
                              "RunCleanup", this);
  thread_stopper()->Uninstall();
  CleanupHandles();

  while (!cleanup_hooks_.empty()) {
    // Copy into a vector, since we can't sort an unordered_set in-place.
    std::vector<CleanupHookCallback> callbacks(
        cleanup_hooks_.begin(), cleanup_hooks_.end());
    // We can't erase the copied elements from `cleanup_hooks_` yet, because we
    // need to be able to check whether they were un-scheduled by another hook.

    std::sort(callbacks.begin(), callbacks.end(),
              [](const CleanupHookCallback& a, const CleanupHookCallback& b) {
      // Sort in descending order so that the most recently inserted callbacks
      // are run first.
      return a.insertion_order_counter_ > b.insertion_order_counter_;
    });

    for (const CleanupHookCallback& cb : callbacks) {
      if (cleanup_hooks_.count(cb) == 0) {
        // This hook was removed from the `cleanup_hooks_` set during another
        // hook that was run earlier. Nothing to do here.
        continue;
      }

      cb.fn_(cb.arg_);
      cleanup_hooks_.erase(cb);
    }
    CleanupHandles();
  }
}

void Environment::RunBeforeExitCallbacks() {
  TraceEventScope trace_scope(TRACING_CATEGORY_NODE1(environment),
                              "BeforeExit", this);
  for (ExitCallback before_exit : before_exit_functions_) {
    before_exit.cb_(before_exit.arg_);
  }
  before_exit_functions_.clear();
}

void Environment::BeforeExit(void (*cb)(void* arg), void* arg) {
  before_exit_functions_.push_back(ExitCallback{cb, arg});
}

void Environment::RunAtExitCallbacks() {
  TraceEventScope trace_scope(TRACING_CATEGORY_NODE1(environment),
                              "AtExit", this);
  for (ExitCallback at_exit : at_exit_functions_) {
    at_exit.cb_(at_exit.arg_);
  }
  at_exit_functions_.clear();
}

void Environment::AtExit(void (*cb)(void* arg), void* arg) {
  at_exit_functions_.push_back(ExitCallback{cb, arg});
}

void Environment::RunAndClearNativeImmediates() {
  TraceEventScope trace_scope(TRACING_CATEGORY_NODE1(environment),
                              "RunAndClearNativeImmediates", this);
  size_t ref_count = 0;
  size_t count = 0;
  std::unique_ptr<NativeImmediateCallback> head;
  head.swap(native_immediate_callbacks_head_);
  native_immediate_callbacks_tail_ = nullptr;

  auto drain_list = [&]() {
    TryCatchScope try_catch(this);
    for (; head; head = head->get_next()) {
      DebugSealHandleScope seal_handle_scope(isolate());
      count++;
      if (head->is_refed())
        ref_count++;

      head->Call(this);
      if (UNLIKELY(try_catch.HasCaught())) {
        if (!try_catch.HasTerminated())
          errors::TriggerUncaughtException(isolate(), try_catch);

        // We are done with the current callback. Move one iteration along,
        // as if we had completed successfully.
        head = head->get_next();
        return true;
      }
    }
    return false;
  };
  while (head && drain_list()) {}

  DCHECK_GE(immediate_info()->count(), count);
  immediate_info()->count_dec(count);
  immediate_info()->ref_count_dec(ref_count);
}


void Environment::ScheduleTimer(int64_t duration_ms) {
  if (started_cleanup_) return;
  uv_timer_start(timer_handle(), RunTimers, duration_ms, 0);
}

void Environment::ToggleTimerRef(bool ref) {
  if (started_cleanup_) return;

  if (ref) {
    uv_ref(reinterpret_cast<uv_handle_t*>(timer_handle()));
  } else {
    uv_unref(reinterpret_cast<uv_handle_t*>(timer_handle()));
  }
}

void Environment::RunTimers(uv_timer_t* handle) {
  Environment* env = Environment::from_timer_handle(handle);
  TraceEventScope trace_scope(TRACING_CATEGORY_NODE1(environment),
                              "RunTimers", env);

  if (!env->can_call_into_js())
    return;

  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  Local<Object> process = env->process_object();
  InternalCallbackScope scope(env, process, {0, 0});

  Local<Function> cb = env->timers_callback_function();
  MaybeLocal<Value> ret;
  Local<Value> arg = env->GetNow();
  // This code will loop until all currently due timers will process. It is
  // impossible for us to end up in an infinite loop due to how the JS-side
  // is structured.
  do {
    TryCatchScope try_catch(env);
    try_catch.SetVerbose(true);
    ret = cb->Call(env->context(), process, 1, &arg);
  } while (ret.IsEmpty() && env->can_call_into_js());

  // NOTE(apapirovski): If it ever becomes possible that `call_into_js` above
  // is reset back to `true` after being previously set to `false` then this
  // code becomes invalid and needs to be rewritten. Otherwise catastrophic
  // timers corruption will occur and all timers behaviour will become
  // entirely unpredictable.
  if (ret.IsEmpty())
    return;

  // To allow for less JS-C++ boundary crossing, the value returned from JS
  // serves a few purposes:
  // 1. If it's 0, no more timers exist and the handle should be unrefed
  // 2. If it's > 0, the value represents the next timer's expiry and there
  //    is at least one timer remaining that is refed.
  // 3. If it's < 0, the absolute value represents the next timer's expiry
  //    and there are no timers that are refed.
  int64_t expiry_ms =
      ret.ToLocalChecked()->IntegerValue(env->context()).FromJust();

  uv_handle_t* h = reinterpret_cast<uv_handle_t*>(handle);

  if (expiry_ms != 0) {
    int64_t duration_ms =
        llabs(expiry_ms) - (uv_now(env->event_loop()) - env->timer_base());

    env->ScheduleTimer(duration_ms > 0 ? duration_ms : 1);

    if (expiry_ms > 0)
      uv_ref(h);
    else
      uv_unref(h);
  } else {
    uv_unref(h);
  }
}


void Environment::CheckImmediate(uv_check_t* handle) {
  Environment* env = Environment::from_immediate_check_handle(handle);
  TraceEventScope trace_scope(TRACING_CATEGORY_NODE1(environment),
                              "CheckImmediate", env);

  if (env->immediate_info()->count() == 0)
    return;

  HandleScope scope(env->isolate());
  Context::Scope context_scope(env->context());

  env->RunAndClearNativeImmediates();

  if (!env->can_call_into_js())
    return;

  do {
    MakeCallback(env->isolate(),
                 env->process_object(),
                 env->immediate_callback_function(),
                 0,
                 nullptr,
                 {0, 0}).ToLocalChecked();
  } while (env->immediate_info()->has_outstanding() && env->can_call_into_js());

  if (env->immediate_info()->ref_count() == 0)
    env->ToggleImmediateRef(false);
}

void Environment::ToggleImmediateRef(bool ref) {
  if (started_cleanup_) return;

  if (ref) {
    // Idle handle is needed only to stop the event loop from blocking in poll.
    uv_idle_start(immediate_idle_handle(), [](uv_idle_t*){ });
  } else {
    uv_idle_stop(immediate_idle_handle());
  }
}


Local<Value> Environment::GetNow() {
  uv_update_time(event_loop());
  uint64_t now = uv_now(event_loop());
  CHECK_GE(now, timer_base());
  now -= timer_base();
  if (now <= 0xffffffff)
    return Integer::NewFromUnsigned(isolate(), static_cast<uint32_t>(now));
  else
    return Number::New(isolate(), static_cast<double>(now));
}

void Environment::set_debug_categories(const std::string& cats, bool enabled) {
  std::string debug_categories = cats;
  while (!debug_categories.empty()) {
    std::string::size_type comma_pos = debug_categories.find(',');
    std::string wanted = ToLower(debug_categories.substr(0, comma_pos));

#define V(name)                                                          \
    {                                                                    \
      static const std::string available_category = ToLower(#name);      \
      if (available_category.find(wanted) != std::string::npos)          \
        set_debug_enabled(DebugCategory::name, enabled);                 \
    }

    DEBUG_CATEGORY_NAMES(V)
#undef V

    if (comma_pos == std::string::npos)
      break;
    // Use everything after the `,` as the list for the next iteration.
    debug_categories = debug_categories.substr(comma_pos + 1);
  }
}

void CollectExceptionInfo(Environment* env,
                          Local<Object> obj,
                          int errorno,
                          const char* err_string,
                          const char* syscall,
                          const char* message,
                          const char* path,
                          const char* dest) {
  obj->Set(env->context(),
           env->errno_string(),
           Integer::New(env->isolate(), errorno)).Check();

  obj->Set(env->context(), env->code_string(),
           OneByteString(env->isolate(), err_string)).Check();

  if (message != nullptr) {
    obj->Set(env->context(), env->message_string(),
             OneByteString(env->isolate(), message)).Check();
  }

  Local<Value> path_buffer;
  if (path != nullptr) {
    path_buffer =
      Buffer::Copy(env->isolate(), path, strlen(path)).ToLocalChecked();
    obj->Set(env->context(), env->path_string(), path_buffer).Check();
  }

  Local<Value> dest_buffer;
  if (dest != nullptr) {
    dest_buffer =
      Buffer::Copy(env->isolate(), dest, strlen(dest)).ToLocalChecked();
    obj->Set(env->context(), env->dest_string(), dest_buffer).Check();
  }

  if (syscall != nullptr) {
    obj->Set(env->context(), env->syscall_string(),
             OneByteString(env->isolate(), syscall)).Check();
  }
}

void Environment::CollectUVExceptionInfo(Local<Value> object,
                                         int errorno,
                                         const char* syscall,
                                         const char* message,
                                         const char* path,
                                         const char* dest) {
  if (!object->IsObject() || errorno == 0)
    return;

  Local<Object> obj = object.As<Object>();
  const char* err_string = uv_err_name(errorno);

  if (message == nullptr || message[0] == '\0') {
    message = uv_strerror(errorno);
  }

  node::CollectExceptionInfo(this, obj, errorno, err_string,
                             syscall, message, path, dest);
}

void ImmediateInfo::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("fields", fields_);
}

void TickInfo::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("fields", fields_);
}

void AsyncHooks::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("providers", providers_);
  tracker->TrackField("async_ids_stack", async_ids_stack_);
  tracker->TrackField("fields", fields_);
  tracker->TrackField("async_id_fields", async_id_fields_);
}

void AsyncHooks::grow_async_ids_stack() {
  async_ids_stack_.reserve(async_ids_stack_.Length() * 3);

  env()->async_hooks_binding()->Set(
      env()->context(),
      env()->async_ids_stack_string(),
      async_ids_stack_.GetJSArray()).Check();
}

uv_key_t Environment::thread_local_env = {};

void Environment::Exit(int exit_code) {
  if (is_main_thread()) {
    stop_sub_worker_contexts();
    DisposePlatform();
    exit(exit_code);
  } else {
    worker_context_->Exit(exit_code);
  }
}

void Environment::stop_sub_worker_contexts() {
  while (!sub_worker_contexts_.empty()) {
    Worker* w = *sub_worker_contexts_.begin();
    remove_sub_worker_context(w);
    w->Exit(1);
    w->JoinThread();
  }
}

#if HAVE_INSPECTOR

#endif  // HAVE_INSPECTOR

void MemoryTracker::TrackField(const char* edge_name,
                               const CleanupHookCallback& value,
                               const char* node_name) {
  v8::HandleScope handle_scope(isolate_);
  // Here, we utilize the fact that CleanupHookCallback instances
  // are all unique and won't be tracked twice in one BuildEmbedderGraph
  // callback.
  MemoryRetainerNode* n =
      PushNode("CleanupHookCallback", sizeof(value), edge_name);
  // TODO(joyeecheung): at the moment only arguments of type BaseObject will be
  // identified and tracked here (based on their deleters),
  // but we may convert and track other known types here.
  BaseObject* obj = value.GetBaseObject();
  if (obj != nullptr && obj->IsDoneInitializing()) {
    TrackField("arg", obj);
  }
  CHECK_EQ(CurrentNode(), n);
  CHECK_NE(n->size_, 0);
  PopNode();
}

void Environment::BuildEmbedderGraph(Isolate* isolate,
                                     EmbedderGraph* graph,
                                     void* data) {
  MemoryTracker tracker(isolate, graph);
  Environment* env = static_cast<Environment*>(data);
  tracker.Track(env);
}

inline size_t Environment::SelfSize() const {
  size_t size = sizeof(*this);
  // Remove non pointer fields that will be tracked in MemoryInfo()
  // TODO(joyeecheung): refactor the MemoryTracker interface so
  // this can be done for common types within the Track* calls automatically
  // if a certain scope is entered.
  size -= sizeof(thread_stopper_);
  size -= sizeof(async_hooks_);
  size -= sizeof(tick_info_);
  size -= sizeof(immediate_info_);
  return size;
}

void Environment::MemoryInfo(MemoryTracker* tracker) const {
  // Iteratable STLs have their own sizes subtracted from the parent
  // by default.
  tracker->TrackField("isolate_data", isolate_data_);
  tracker->TrackField("native_modules_with_cache", native_modules_with_cache);
  tracker->TrackField("native_modules_without_cache",
                      native_modules_without_cache);
  tracker->TrackField("destroy_async_id_list", destroy_async_id_list_);
  tracker->TrackField("exec_argv", exec_argv_);
  tracker->TrackField("should_abort_on_uncaught_toggle",
                      should_abort_on_uncaught_toggle_);
  tracker->TrackField("stream_base_state", stream_base_state_);
  tracker->TrackField("fs_stats_field_array", fs_stats_field_array_);
  tracker->TrackField("fs_stats_field_bigint_array",
                      fs_stats_field_bigint_array_);
  tracker->TrackField("thread_stopper", thread_stopper_);
  tracker->TrackField("cleanup_hooks", cleanup_hooks_);
  tracker->TrackField("async_hooks", async_hooks_);
  tracker->TrackField("immediate_info", immediate_info_);
  tracker->TrackField("tick_info", tick_info_);

#define V(PropertyName, TypeName)                                              \
  tracker->TrackField(#PropertyName, PropertyName());
  ENVIRONMENT_STRONG_PERSISTENT_VALUES(V)
#undef V

  // FIXME(joyeecheung): track other fields in Environment.
  // Currently MemoryTracker is unable to track these
  // correctly:
  // - Internal types that do not implement MemoryRetainer yet
  // - STL containers with MemoryRetainer* inside
  // - STL containers with numeric types inside that should not have their
  //   nodes elided e.g. numeric keys in maps.
  // We also need to make sure that when we add a non-pointer field as its own
  // node, we shift its sizeof() size out of the Environment node.
}

char* Environment::Reallocate(char* data, size_t old_size, size_t size) {
  if (old_size == size) return data;
  // If we know that the allocator is our ArrayBufferAllocator, we can let
  // if reallocate directly.
  if (isolate_data()->uses_node_allocator()) {
    return static_cast<char*>(
        isolate_data()->node_allocator()->Reallocate(data, old_size, size));
  }
  // Generic allocators do not provide a reallocation method; we need to
  // allocate a new chunk of memory and copy the data over.
  char* new_data = AllocateUnchecked(size);
  if (new_data == nullptr) return nullptr;
  memcpy(new_data, data, std::min(size, old_size));
  if (size > old_size)
    memset(new_data + old_size, 0, size - old_size);
  Free(data, old_size);
  return new_data;
}

void AsyncRequest::Install(Environment* env, void* data, uv_async_cb target) {
  CHECK_NULL(async_);
  env_ = env;
  async_ = new uv_async_t;
  async_->data = data;
  CHECK_EQ(uv_async_init(env_->event_loop(), async_, target), 0);
}

void AsyncRequest::Uninstall() {
  if (async_ != nullptr) {
    env_->CloseHandle(async_, [](uv_async_t* async) { delete async; });
    async_ = nullptr;
  }
}

void AsyncRequest::Stop() {
  set_stopped(true);
  if (async_ != nullptr) uv_async_send(async_);
}

uv_async_t* AsyncRequest::GetHandle() {
  return async_;
}

void AsyncRequest::MemoryInfo(MemoryTracker* tracker) const {
  if (async_ != nullptr) tracker->TrackField("async_request", *async_);
}

AsyncRequest::~AsyncRequest() {
  CHECK_NULL(async_);
}

// Not really any better place than env.cc at this moment.
void BaseObject::DeleteMe(void* data) {
  BaseObject* self = static_cast<BaseObject*>(data);
  delete self;
}

bool BaseObject::IsDoneInitializing() const { return true; }

Local<Object> BaseObject::WrappedObject() const {
  return object();
}

}  // namespace node
