#pragma once

#include <ATen/core/dispatch/OperatorEntry.h>
#include <ATen/core/dispatch/RegistrationHandleRAII.h>
#include <ATen/core/Variadic.h>
#include <c10/util/Exception.h>
#include <mutex>
#include <list>

namespace c10 {

class CAFFE2_API OperatorHandle;

namespace impl {

// Take a TensorTypeSet for a Tensor, and combine it with the current thread
// local valid (implemented) and enabled (not implemented) TensorTypeSets
// to determine what the actual dispatch TensorTypeId should be.  Unlike
// Tensor::type_set(), the value of this on a tensor can change depending
// on TLS.
//
// NB: I didn't make this take a Tensor to avoid header include shenanigans.
//
// TODO: I'm not sure if this should live in this header or not; the operant
// question is whether or not we have access to all the relevant TLS at this
// point.
static inline TensorTypeId dispatchTypeId(TensorTypeSet ts) {
  c10::impl::LocalTensorTypeSet local = c10::impl::tls_local_tensor_type_set();
  return ((ts | local.included_) - local.excluded_).highestPriorityTypeId();
}

}

namespace detail {
  struct MultiDispatchTensorTypeSet : at::IterArgs<MultiDispatchTensorTypeSet> {
    TensorTypeSet ts;
    void operator()(const at::Tensor& x) {
      ts = ts | x.type_set();
    }
    void operator()(TensorOptions x) {
      ts = ts | x.type_set();
    }
    void operator()(at::ArrayRef<at::Tensor> xs) {
      for (const auto& x : xs) {
        ts = ts | x.type_set();
      }
    }
    template <typename T>
    void operator()(const T& x) {
      // do nothing
    }
  };

  // NB: take by const reference (Don't do universal forwarding here! You
  // don't want to move into this function!)
  template <typename... Args>
  TensorTypeSet multi_dispatch_tensor_type_set(const Args&... args) {
    return MultiDispatchTensorTypeSet().apply(args...).ts;
  }
}

namespace detail {

class KernelTable_ final {
 public:
  void set(TensorTypeId key, const KernelFunction& value, const std::string& operator_name) {
    auto emplaced = map_.emplace(key, value);
    if (!emplaced.second) {
      // Element already existed. Overwrite it.
      emplaced.first->second = value;
      TORCH_WARN("Registered a kernel for operator ", operator_name," with dispatch key ", toString(key), " that overwrote a previously registered kernel with the same dispatch key for the same operator.");
    }
  }

  void removeIfExists(TensorTypeId key, const std::string& operator_name) {
    auto num_removed = map_.erase(key);
    TORCH_INTERNAL_ASSERT(num_removed <= 1); // This is not a multi-map
  }

  const KernelFunction* lookup(TensorTypeId key) const {
    auto found = map_.find(key);
    if (found != map_.end()) {
      return &found->second;
    } else {
      return nullptr;
    }
  }

  size_t size() const {
    return map_.size();
  }

  std::string list_all_dispatch_keys() const {
    if (map_.size() == 0) {
      return "[]";
    }
    std::ostringstream str;
    str << "[" << toString(map_.begin()->first);
    for (auto iter = ++map_.begin(); iter != map_.end(); ++iter) {
      str << ", " << toString(iter->first);
    }
    str << "]";
    return str.str();
  }

 private:
   ska::flat_hash_map<TensorTypeId, KernelFunction> map_;
};
} // namespace detail

/**
 * Implement this interface and register your instance with the dispatcher
 * to get notified when operators are registered or deregistered with
 * the dispatcher.
 */
class CAFFE2_API OpRegistrationListener {
public:
  virtual ~OpRegistrationListener();

  virtual void onOperatorRegistered(const OperatorHandle& op) = 0;
  virtual void onOperatorDeregistered(const OperatorHandle& op) = 0;
};

namespace detail {
class RegistrationListenerList;
}
class SchemaRegistrationHandleRAII;

/**
 * Top-level dispatch interface for dispatching via the dynamic dispatcher.
 */
class CAFFE2_API Dispatcher final {
private:
  struct OperatorDef final {
    explicit OperatorDef(FunctionSchema&& schema, OperatorOptions&& options)
    : op(std::move(schema), std::move(options)), refcount(0) {}

    impl::OperatorEntry op;
    size_t refcount;
  };
  friend class OperatorHandle;

public:
  ~Dispatcher();

  // Implementation note: this class abstracts over the fact that we have per-operator
  // dispatch tables.  This could be easily adjusted to have a single global hash
  // table.

  static Dispatcher& singleton();

  /**
   * Register a new operator schema.
   *
   * If a schema with the same operator name and overload name already exists,
   * this function will check that both schemas are exactly identical.
   *
   * @return An OperatorHandle for the registered schema which can be used to
   *         register kernels for the operator and a RegistrationHandleRAII RAII
   *         object that manages the lifetime of the registration. Once that
   *         object is destructed, the kernel will be deregistered.
   */
  SchemaRegistrationHandleRAII registerSchema(FunctionSchema schema, OperatorOptions options);

  /**
   * Looks for an operator schema with the given name and overload name
   * and returns it if it is registered.
   * Returns nullopt otherwise.
   */
  c10::optional<OperatorHandle> findSchema(const OperatorName& operator_name);

  /**
   * Register a kernel to the dispatch table for an operator.
   * If dispatch_key is nullopt, then this registers a fallback kernel.
   *
   * @return A RAII object that manages the lifetime of the registration.
   *         Once that object is destructed, the kernel will be deregistered.
   */
  RegistrationHandleRAII registerKernel(const OperatorHandle& op, TensorTypeId dispatch_key, KernelFunction kernel);

  /**
   * Register a fallback kernel for an operator.
   * After this, when trying to lookup a kernel for an unknown dispatch key,
   * it will not fail anymore, but return the fallback kernel instead.
   *
   * @return A RAII object that manages the lifetime of the registration.
   *         Once that object is destructed, the kernel will be deregistered.
   */
  RegistrationHandleRAII registerCatchallKernel(const OperatorHandle& op, KernelFunction kernel);

  template<class Return, class... Args>
  Return callUnboxed(const OperatorHandle& op, Args... args) const;

  template<class Return, class... Args>
  Return callUnboxedOnly(const OperatorHandle& op, Args... args) const;

  void callBoxed(const OperatorHandle& op, Stack* stack) const;

  /**
   * Add a listener that gets called whenever a new op is registered or an existing
   * op is deregistered. Immediately after registering, this listener gets called
   * for all previously registered ops, so it can be used to keep track of ops
   * registered with this dispatcher.
   */
  void addRegistrationListener(std::unique_ptr<OpRegistrationListener> listener);

private:
  Dispatcher();

  OperatorHandle findOrRegisterSchema_(FunctionSchema&& schema, OperatorOptions&& options);

  void deregisterSchema_(const OperatorHandle& op, const OperatorName& op_name);

  const KernelFunction& dispatch_(const DispatchTable& dispatchTable, c10::optional<TensorTypeId> dispatch_key) const;

  std::list<OperatorDef> operators_;
  LeftRight<ska::flat_hash_map<OperatorName, OperatorHandle>> operatorLookupTable_;
  std::unique_ptr<detail::RegistrationListenerList> listeners_;
  std::mutex mutex_;
};

/**
 * This is a handle to an operator schema registered with the dispatcher.
 * This handle can be used to register kernels with the dispatcher or
 * to lookup a kernel for a certain set of arguments.
 */
class CAFFE2_API OperatorHandle final {
public:
  OperatorHandle(OperatorHandle&&) noexcept = default;
  OperatorHandle& operator=(OperatorHandle&&) noexcept = default;
  OperatorHandle(const OperatorHandle&) = default;
  OperatorHandle& operator=(const OperatorHandle&) = default;

  const FunctionSchema& schema() const {
    return operatorIterator_->op.schema();
  }

  const OperatorOptions& options() const {
    return operatorIterator_->op.options();
  }

private:
  explicit OperatorHandle(std::list<Dispatcher::OperatorDef>::iterator operatorIterator)
  : operatorIterator_(std::move(operatorIterator)) {}
  friend class Dispatcher;

  std::list<Dispatcher::OperatorDef>::iterator operatorIterator_;
};

class CAFFE2_API SchemaRegistrationHandleRAII final {
public:
  const OperatorHandle& opHandle() const {
    return opHandle_;
  }

private:
  friend class Dispatcher;
  explicit SchemaRegistrationHandleRAII(OperatorHandle opHandle, RegistrationHandleRAII registrationHandle)
    : opHandle_(std::move(opHandle)), registrationHandle_(std::move(registrationHandle)) {}

  OperatorHandle opHandle_;
  RegistrationHandleRAII registrationHandle_;
};

template<class Return, class... Args>
inline Return Dispatcher::callUnboxed(const OperatorHandle& op, Args... args) const {
  // note: this doesn't need the mutex because write operations on the list keep iterators intact.
  return op.operatorIterator_->op.readDispatchTable([&] (const DispatchTable& dispatchTable) -> Return {
    c10::optional<TensorTypeId> dispatchKey = dispatchTable.dispatchKeyExtractor().getDispatchKeyUnboxed(args...);
    const KernelFunction& kernel = dispatch_(dispatchTable, dispatchKey);
    return kernel.template callUnboxed<Return, Args...>(std::forward<Args>(args)...);
  });
}

template<class Return, class... Args>
inline Return Dispatcher::callUnboxedOnly(const OperatorHandle& op, Args... args) const {
  // note: this doesn't need the mutex because write operations on the list keep iterators intact.
  return op.operatorIterator_->op.readDispatchTable([&] (const DispatchTable& dispatchTable) -> Return {
    c10::optional<TensorTypeId> dispatchKey = dispatchTable.dispatchKeyExtractor().getDispatchKeyUnboxed(args...);
    const KernelFunction& kernel = dispatch_(dispatchTable, dispatchKey);
    return kernel.template callUnboxedOnly<Return, Args...>(std::forward<Args>(args)...);
  });
}

inline void Dispatcher::callBoxed(const OperatorHandle& op, Stack* stack) const {
  // note: this doesn't need the mutex because write operations on the list keep iterators intact.
  return op.operatorIterator_->op.readDispatchTable([&] (const DispatchTable& dispatchTable) {
    c10::optional<TensorTypeId> dispatchKey = dispatchTable.dispatchKeyExtractor().getDispatchKeyBoxed(stack);
    const KernelFunction& kernel = dispatch_(dispatchTable, dispatchKey);
    kernel.callBoxed(stack);
  });
}

inline const KernelFunction& Dispatcher::dispatch_(const DispatchTable& dispatchTable, c10::optional<TensorTypeId> dispatchKey) const {
  if (dispatchKey.has_value()) {
    const KernelFunction* backendKernel = dispatchTable.lookup(*dispatchKey);

    if (nullptr != backendKernel) {
      return *backendKernel;
    }
  }

  const KernelFunction* catchallKernel = dispatchTable.lookupCatchallKernel();
  if (nullptr != catchallKernel) {
    return *catchallKernel;
  }

  if (!dispatchKey.has_value() || *dispatchKey == TensorTypeId::UndefinedTensorId) {
    TORCH_CHECK(false,
          "There were no tensor arguments to this function (e.g., you passed an "
          "empty list of Tensors), but no fallback function is registered for schema ", dispatchTable.operatorName(),
          ".  This usually means that this function requires a non-empty list of Tensors.  "
          "Available functions are ", dispatchTable.listAllDispatchKeys())
  }

  const std::string dispatchKeyStr = dispatchKey.has_value() ? toString(*dispatchKey) : "None";
  TORCH_CHECK(false, "Didn't find kernel to dispatch to for operator '", dispatchTable.operatorName(),
           "'. Tried to look up kernel for dispatch key '", dispatchKeyStr,
           "'. Registered dispatch keys are: ", dispatchTable.listAllDispatchKeys());
}

} // namespace c10
