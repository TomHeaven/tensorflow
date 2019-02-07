// RUN: mlir-opt %s -split-input-file -verify

// -----

#map = (d0)[s0] -> (d0 + s0)

func @affine_apply_invalid_dim(%arg : index) {
  for %n0 = 0 to 7 {
    %dim = addi %arg, %arg : index

    // expected-error@+1 {{operand cannot be used as a dimension id}}
    %x  = affine.apply #map(%dim)[%arg]
  }
  return
}

// -----

#map0 = (d0)[s0] -> (d0 + s0)

func @affine_apply_invalid_sym() {
  for %i0 = 0 to 7 {
    // expected-error@+1 {{operand cannot be used as a symbol}}
    %0 = affine.apply #map0(%i0)[%i0]
  }
  return
}

// -----

#map = (d0)[s0] -> (d0 + s0)

func @affine_for_lower_bound_invalid_dim(%arg : index) {
  for %n0 = 0 to 7 {
    %dim = addi %arg, %arg : index

    // expected-error@+1 {{operand cannot be used as a dimension id}}
    for %n1 = 0 to #map(%dim)[%arg] {
    }
  }
  return
}

// -----

#map = (d0)[s0] -> (d0 + s0)

func @affine_for_upper_bound_invalid_dim(%arg : index) {
  for %n0 = 0 to 7 {
    %dim = addi %arg, %arg : index

    // expected-error@+1 {{operand cannot be used as a dimension id}}
    for %n1 = #map(%dim)[%arg] to 7 {
    }
  }
  return
}

// -----

#map0 = (d0)[s0] -> (d0 + s0)

func @affine_for_lower_bound_invalid_sym() {
  for %i0 = 0 to 7 {
    // expected-error@+1 {{operand cannot be used as a symbol}}
    for %n0 = #map0(%i0)[%i0] to 7 {
    }
  }
  return
}

// -----

#map0 = (d0)[s0] -> (d0 + s0)

func @affine_for_upper_bound_invalid_sym() {
  for %i0 = 0 to 7 {
    // expected-error@+1 {{operand cannot be used as a symbol}}
    for %n0 = 0 to #map0(%i0)[%i0] {
    }
  }
  return
}

// -----

#set0 = (i)[N] : (i >= 0, N - i >= 0)

func @affine_if_invalid_dim(%arg : index) {
  for %n0 = 0 to 7 {
    %dim = addi %arg, %arg : index

    // expected-error@+1 {{operand cannot be used as a dimension id}}
    if #set0(%dim)[%n0] {}
  }
  return
}

// -----

#set0 = (i)[N] : (i >= 0, N - i >= 0)

func @affine_if_invalid_sym() {
  for %i0 = 0 to 7 {
    // expected-error@+1 {{operand cannot be used as a symbol}}
    if #set0(%i0)[%i0] {}
  }
  return
}

// -----

#set0 = (i)[N] : (i >= 0, N - i >= 0)

func @affine_if_invalid_dimop_dim(%arg0: index, %arg1: index, %arg2: index, %arg3: index) {
  for %n0 = 0 to 7 {
    %0 = alloc(%arg0, %arg1, %arg2, %arg3) : memref<?x?x?x?xf32>
    %dim = dim %0, 0 : memref<?x?x?x?xf32>

    // expected-error@+1 {{operand cannot be used as a dimension id}}
    if #set0(%dim)[%n0] {}
  }
  return
}
