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
#ifndef TENSORFLOW_C_EAGER_ABSTRACT_CONTEXT_H_
#define TENSORFLOW_C_EAGER_ABSTRACT_CONTEXT_H_

#include <vector>

#include "tensorflow/c/eager/abstract_function.h"
#include "tensorflow/c/eager/abstract_operation.h"

namespace tensorflow {

// Abstract interface to a context.
//
// This serves as a factory for creating `AbstractOperation`s and for
// registering traced functions.
// Operations creation within a context can only be executed in that context
// (for now at least).
// Implementations of the context may contain some state e.g. an execution
// environment, a traced representation etc.
class AbstractContext {
 protected:
  enum AbstractContextKind { kTracing, kImmediateExecution };
  explicit AbstractContext(AbstractContextKind kind) : kind_(kind) {}
  virtual ~AbstractContext() {}

 public:
  AbstractContextKind getKind() const { return kind_; }

  // Release any underlying resources, including the interface object.
  //
  // WARNING: The destructor of this class is marked as protected to disallow
  // clients from directly destroying this object since it may manage it's own
  // lifetime through ref counting. Thus clients MUST call Release() in order to
  // destroy an instance of this class.
  virtual void Release() = 0;

  // Creates an operation builder and ties it to this context.
  // The returned object can be used for setting operation's attributes,
  // adding inputs and finally executing (immediately or lazily as in tracing)
  // it in this context.
  virtual AbstractOperation* CreateOperation() = 0;

  // Registers a function with this context, after this the function is
  // available to be called/referenced by its name in this context.
  virtual Status RegisterFunction(AbstractFunction*) = 0;
  // Remove a function. 'func' argument is the name of a previously added
  // FunctionDef. The name is in fdef.signature.name.
  virtual Status RemoveFunction(const string& func) = 0;

 private:
  const AbstractContextKind kind_;
};

}  // namespace tensorflow

#endif  // TENSORFLOW_C_EAGER_ABSTRACT_CONTEXT_H_
