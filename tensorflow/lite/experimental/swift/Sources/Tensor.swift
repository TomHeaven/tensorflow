// Copyright 2018 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import Foundation
import TensorFlowLiteC

/// An input or output tensor in a TensorFlow Lite graph.
public struct Tensor: Equatable, Hashable {
  /// The name of the tensor.
  public let name: String

  /// The data type of the tensor.
  public let dataType: DataType

  /// The shape of the tensor.
  public let shape: Shape

  /// The data in the input or output tensor.
  public let data: Data

  /// The quantization parameters for the tensor if using a quantized model.
  public let quantizationParameters: QuantizationParameters?

  /// Creates a new input or output tensor instance.
  ///
  /// - Parameters:
  ///   - name: Name of the tensor.
  ///   - dataType: Data type of the tensor.
  ///   - shape: Shape of the tensor.
  ///   - data: Data in the input tensor.
  ///   - quantizationParameters Quantization parameters for the tensor if using a quantized model.
  ///       Default is `nil`.
  init(
    name: String,
    dataType: DataType,
    shape: Shape,
    data: Data,
    quantizationParameters: QuantizationParameters? = nil
  ) {
    self.name = name
    self.dataType = dataType
    self.shape = shape
    self.data = data
    self.quantizationParameters = quantizationParameters
  }
}

extension Tensor {
  /// The supported `Tensor` data types.
  public enum DataType: Equatable, Hashable {
    /// A boolean.
    case bool
    /// An 8-bit unsigned integer.
    case uInt8
    /// A 16-bit signed integer.
    case int16
    /// A 32-bit signed integer.
    case int32
    /// A 64-bit signed integer.
    case int64
    /// A 16-bit half precision floating point.
    case float16
    /// A 32-bit single precision floating point.
    case float32

    /// Creates a new instance from the given `TfLiteType` or `nil` if the data type is unsupported
    /// or could not be determined because there was an error.
    ///
    /// - Parameter type: Data type supported by a tensor.
    init?(type: TfLiteType) {
      switch type {
      case kTfLiteBool:
        self = .bool
      case kTfLiteUInt8:
        self = .uInt8
      case kTfLiteInt16:
        self = .int16
      case kTfLiteInt32:
        self = .int32
      case kTfLiteInt64:
        self = .int64
      case kTfLiteFloat16:
        self = .float16
      case kTfLiteFloat32:
        self = .float32
      case kTfLiteNoType:
        fallthrough
      default:
        return nil
      }
    }
  }
}

extension Tensor {
  /// The shape of a `Tensor`.
  public struct Shape: Equatable, Hashable {
    /// The number of dimensions of the tensor.
    public let rank: Int

    /// An array of dimensions for the tensor.
    public let dimensions: [Int]

    /// An array of `Int32` dimensions for the tensor.
    var int32Dimensions: [Int32] { return dimensions.map(Int32.init) }

    /// Creates a new instance with the given array of dimensions.
    ///
    /// - Parameters:
    ///   - dimensions: Dimensions for the tensor.
    public init(_ dimensions: [Int]) {
      self.rank = dimensions.count
      self.dimensions = dimensions
    }

    /// Creates a new instance with the given elements representing the dimensions.
    ///
    /// - Parameters:
    ///   - elements: Dimensions for the tensor.
    public init(_ elements: Int...) {
      self.init(elements)
    }
  }
}

extension Tensor.Shape: ExpressibleByArrayLiteral {
  /// Creates a new instance with the given array literal representing the dimensions.
  ///
  /// - Parameters:
  ///   - arrayLiteral: Dimensions for the tensor.
  public init(arrayLiteral: Int...) {
    self.init(arrayLiteral)
  }
}
