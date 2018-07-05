//===- OperationImpl.h - Implementation details of *Op classes --*- C++ -*-===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements helper classes for working with the "Op", most of which
// go into the mlir::OpImpl namespace since they are only used by code that is
// defining the op implementations, not by clients.
//
// The purpose of these types are to allow light-weight implementation of
// concrete ops (like DimOp) with very little boilerplate.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_OPERATIONIMPL_H
#define MLIR_IR_OPERATIONIMPL_H

#include "mlir/IR/Operation.h"

namespace mlir {

/// This pointer represents a notional "Operation*" but where the actual
/// storage of the pointer is maintained in the templated "OpType" class.
template <typename OpType>
class OpPointer {
public:
  explicit OpPointer(OpType value) : value(value) {}

  OpType &operator*() { return value; }

  OpType *operator->() { return &value; }

  operator bool() const { return value.getOperation(); }

public:
  OpType value;
};

/// This pointer represents a notional "const Operation*" but where the actual
/// storage of the pointer is maintained in the templated "OpType" class.
template <typename OpType>
class ConstOpPointer {
public:
  explicit ConstOpPointer(OpType value) : value(value) {}

  const OpType &operator*() const { return value; }

  const OpType *operator->() const { return &value; }

  /// Return true if non-null.
  operator bool() const { return value.getOperation(); }

public:
  const OpType value;
};

//===----------------------------------------------------------------------===//
// OpImpl Types
//===----------------------------------------------------------------------===//

namespace OpImpl {

/// Every op should subclass this class to provide the basic storage of the
/// Operation*.
class Storage {
public:
  /// Return the operation that this refers to.
  const Operation *getOperation() const { return state; }
  Operation *getOperation() { return state; }

  /// If the operation has an attribute of the specified type, return it.
  template <typename AttrClass>
  AttrClass *getAttrOfType(StringRef name) const {
    return dyn_cast_or_null<AttrClass>(state->getAttr(name));
  }

  /// If the an attribute exists with the specified name, change it to the new
  /// value.  Otherwise, add a new attribute with the specified name/value.
  void setAttr(Identifier name, Attribute *value) {
    state->setAttr(name, value);
  }

protected:
  /// Mutability management is handled by the OpWrapper/OpConstWrapper classes,
  /// so we can cast it away here.
  explicit Storage(const Operation *state)
      : state(const_cast<Operation *>(state)) {}

private:
  Operation *state;
};

/// This provides public APIs that all operations should have.  The template
/// argument 'ConcreteType' should be the concrete type by CRTP and the others
/// are base classes by the policy pattern.
template <typename ConcreteType, typename... Traits>
class Base : public Traits... {
public:
  /// This is the hook used by the AsmPrinter to emit this to the .mlir file.
  /// Op implementations should provide a print method.
  static void printAssembly(const Operation *op, raw_ostream &os) {
    op->getAs<ConcreteType>()->print(os);
  }
};

/// This class provides the API for ops that are known to have exactly one
/// SSA operand.
class OneOperand {
public:
  void getOperand() const {
    /// TODO.
  }
  void setOperand() {
    /// TODO.
  }
};

/// This class provides the API for ops that are known to have exactly two
/// SSA operands.
class TwoOperands {
public:
  void getOperand() const {
    /// TODO.
  }
  void setOperand() {
    /// TODO.
  }
};

/// This class provides return value APIs for ops that are known to have a
/// single result.
class OneResult {
  // TODO: Implement results!
};

} // end namespace OpImpl

} // end namespace mlir

#endif
