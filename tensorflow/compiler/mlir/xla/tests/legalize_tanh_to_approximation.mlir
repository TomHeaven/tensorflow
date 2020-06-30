// RUN: xla-opt -xla-legalize-tanh-to-approximation -split-input-file %s | FileCheck %s

func @tanh_f64(%arg0 : f64) -> f64 {
  %res = tanh %arg0 : f64
  return %res : f64
}

// CHECK-LABEL: @tanh_f64
// CHECK: tanh

// -----

func @tanh_f32(%arg0 : f32) -> f32 {
  %res = tanh %arg0 : f32
  return %res : f32
}

// NOTE: Assertions have been autogenerated by utils/generate-test-checks.py

// CHECK-LABEL:   func @tanh_f32(
// CHECK-SAME:                   %[[VAL_0:.*]]: f32) -> f32 {
// CHECK:           %[[VAL_1:.*]] = constant 4.000000e-04 : f32
// CHECK:           %[[VAL_2:.*]] = constant 7.90531111 : f32
// CHECK:           %[[VAL_3:.*]] = constant -7.90531111 : f32
// CHECK:           %[[VAL_4:.*]] = constant -2.76076837E-16 : f32
// CHECK:           %[[VAL_5:.*]] = constant 2.00018794E-13 : f32
// CHECK:           %[[VAL_6:.*]] = constant -8.60467184E-11 : f32
// CHECK:           %[[VAL_7:.*]] = constant 5.12229725E-8 : f32
// CHECK:           %[[VAL_8:.*]] = constant 1.48572235E-5 : f32
// CHECK:           %[[VAL_9:.*]] = constant 6.37261954E-4 : f32
// CHECK:           %[[VAL_10:.*]] = constant 0.00489352457 : f32
// CHECK:           %[[VAL_11:.*]] = constant 1.19825836E-6 : f32
// CHECK:           %[[VAL_12:.*]] = constant 1.18534706E-4 : f32
// CHECK:           %[[VAL_13:.*]] = constant 0.00226843474 : f32
// CHECK:           %[[VAL_14:.*]] = constant 0.00489352504 : f32
// CHECK:           %[[VAL_15:.*]] = absf %[[VAL_0]] : f32
// CHECK:           %[[VAL_16:.*]] = cmpf "olt", %[[VAL_15]], %[[VAL_1]] : f32
// CHECK:           %[[VAL_17:.*]] = cmpf "ule", %[[VAL_0]], %[[VAL_2]] : f32
// CHECK:           %[[VAL_18:.*]] = select %[[VAL_17]], %[[VAL_0]], %[[VAL_2]] : f32
// CHECK:           %[[VAL_19:.*]] = cmpf "uge", %[[VAL_18]], %[[VAL_3]] : f32
// CHECK:           %[[VAL_20:.*]] = select %[[VAL_19]], %[[VAL_18]], %[[VAL_3]] : f32
// CHECK:           %[[VAL_21:.*]] = mulf %[[VAL_20]], %[[VAL_20]] : f32
// CHECK:           %[[VAL_22:.*]] = mulf %[[VAL_21]], %[[VAL_4]] : f32
// CHECK:           %[[VAL_23:.*]] = addf %[[VAL_22]], %[[VAL_5]] : f32
// CHECK:           %[[VAL_24:.*]] = mulf %[[VAL_21]], %[[VAL_23]] : f32
// CHECK:           %[[VAL_25:.*]] = addf %[[VAL_24]], %[[VAL_6]] : f32
// CHECK:           %[[VAL_26:.*]] = mulf %[[VAL_21]], %[[VAL_25]] : f32
// CHECK:           %[[VAL_27:.*]] = addf %[[VAL_26]], %[[VAL_7]] : f32
// CHECK:           %[[VAL_28:.*]] = mulf %[[VAL_21]], %[[VAL_27]] : f32
// CHECK:           %[[VAL_29:.*]] = addf %[[VAL_28]], %[[VAL_8]] : f32
// CHECK:           %[[VAL_30:.*]] = mulf %[[VAL_21]], %[[VAL_29]] : f32
// CHECK:           %[[VAL_31:.*]] = addf %[[VAL_30]], %[[VAL_9]] : f32
// CHECK:           %[[VAL_32:.*]] = mulf %[[VAL_21]], %[[VAL_31]] : f32
// CHECK:           %[[VAL_33:.*]] = addf %[[VAL_32]], %[[VAL_10]] : f32
// CHECK:           %[[VAL_34:.*]] = mulf %[[VAL_20]], %[[VAL_33]] : f32
// CHECK:           %[[VAL_35:.*]] = mulf %[[VAL_21]], %[[VAL_11]] : f32
// CHECK:           %[[VAL_36:.*]] = addf %[[VAL_35]], %[[VAL_12]] : f32
// CHECK:           %[[VAL_37:.*]] = mulf %[[VAL_21]], %[[VAL_36]] : f32
// CHECK:           %[[VAL_38:.*]] = addf %[[VAL_37]], %[[VAL_13]] : f32
// CHECK:           %[[VAL_39:.*]] = mulf %[[VAL_21]], %[[VAL_38]] : f32
// CHECK:           %[[VAL_40:.*]] = addf %[[VAL_39]], %[[VAL_14]] : f32
// CHECK:           %[[VAL_41:.*]] = divf %[[VAL_34]], %[[VAL_40]] : f32
// CHECK:           %[[VAL_42:.*]] = select %[[VAL_16]], %[[VAL_0]], %[[VAL_41]] : f32
// CHECK:           return %[[VAL_42]] : f32
// CHECK:         }

// -----

func @tanh_f16(%arg0 : f16) -> f16 {
  %res = tanh %arg0 : f16
  return %res : f16
}

// NOTE: Assertions have been autogenerated by utils/generate-test-checks.py

// CHECK-LABEL:   func @tanh_f16(
// CHECK-SAME:                   %[[VAL_0:.*]]: f16) -> f16 {
// CHECK:           %[[VAL_1:.*]] = constant 4.000000e-04 : f32
// CHECK:           %[[VAL_2:.*]] = constant 7.90531111 : f32
// CHECK:           %[[VAL_3:.*]] = constant -7.90531111 : f32
// CHECK:           %[[VAL_4:.*]] = constant -2.76076837E-16 : f32
// CHECK:           %[[VAL_5:.*]] = constant 2.00018794E-13 : f32
// CHECK:           %[[VAL_6:.*]] = constant -8.60467184E-11 : f32
// CHECK:           %[[VAL_7:.*]] = constant 5.12229725E-8 : f32
// CHECK:           %[[VAL_8:.*]] = constant 1.48572235E-5 : f32
// CHECK:           %[[VAL_9:.*]] = constant 6.37261954E-4 : f32
// CHECK:           %[[VAL_10:.*]] = constant 0.00489352457 : f32
// CHECK:           %[[VAL_11:.*]] = constant 1.19825836E-6 : f32
// CHECK:           %[[VAL_12:.*]] = constant 1.18534706E-4 : f32
// CHECK:           %[[VAL_13:.*]] = constant 0.00226843474 : f32
// CHECK:           %[[VAL_14:.*]] = constant 0.00489352504 : f32
// CHECK:           %[[VAL_15:.*]] = fpext %[[VAL_0]] : f16 to f32
// CHECK:           %[[VAL_16:.*]] = absf %[[VAL_15]] : f32
// CHECK:           %[[VAL_17:.*]] = cmpf "olt", %[[VAL_16]], %[[VAL_1]] : f32
// CHECK:           %[[VAL_18:.*]] = cmpf "ule", %[[VAL_15]], %[[VAL_2]] : f32
// CHECK:           %[[VAL_19:.*]] = select %[[VAL_18]], %[[VAL_15]], %[[VAL_2]] : f32
// CHECK:           %[[VAL_20:.*]] = cmpf "uge", %[[VAL_19]], %[[VAL_3]] : f32
// CHECK:           %[[VAL_21:.*]] = select %[[VAL_20]], %[[VAL_19]], %[[VAL_3]] : f32
// CHECK:           %[[VAL_22:.*]] = mulf %[[VAL_21]], %[[VAL_21]] : f32
// CHECK:           %[[VAL_23:.*]] = mulf %[[VAL_22]], %[[VAL_4]] : f32
// CHECK:           %[[VAL_24:.*]] = addf %[[VAL_23]], %[[VAL_5]] : f32
// CHECK:           %[[VAL_25:.*]] = mulf %[[VAL_22]], %[[VAL_24]] : f32
// CHECK:           %[[VAL_26:.*]] = addf %[[VAL_25]], %[[VAL_6]] : f32
// CHECK:           %[[VAL_27:.*]] = mulf %[[VAL_22]], %[[VAL_26]] : f32
// CHECK:           %[[VAL_28:.*]] = addf %[[VAL_27]], %[[VAL_7]] : f32
// CHECK:           %[[VAL_29:.*]] = mulf %[[VAL_22]], %[[VAL_28]] : f32
// CHECK:           %[[VAL_30:.*]] = addf %[[VAL_29]], %[[VAL_8]] : f32
// CHECK:           %[[VAL_31:.*]] = mulf %[[VAL_22]], %[[VAL_30]] : f32
// CHECK:           %[[VAL_32:.*]] = addf %[[VAL_31]], %[[VAL_9]] : f32
// CHECK:           %[[VAL_33:.*]] = mulf %[[VAL_22]], %[[VAL_32]] : f32
// CHECK:           %[[VAL_34:.*]] = addf %[[VAL_33]], %[[VAL_10]] : f32
// CHECK:           %[[VAL_35:.*]] = mulf %[[VAL_21]], %[[VAL_34]] : f32
// CHECK:           %[[VAL_36:.*]] = mulf %[[VAL_22]], %[[VAL_11]] : f32
// CHECK:           %[[VAL_37:.*]] = addf %[[VAL_36]], %[[VAL_12]] : f32
// CHECK:           %[[VAL_38:.*]] = mulf %[[VAL_22]], %[[VAL_37]] : f32
// CHECK:           %[[VAL_39:.*]] = addf %[[VAL_38]], %[[VAL_13]] : f32
// CHECK:           %[[VAL_40:.*]] = mulf %[[VAL_22]], %[[VAL_39]] : f32
// CHECK:           %[[VAL_41:.*]] = addf %[[VAL_40]], %[[VAL_14]] : f32
// CHECK:           %[[VAL_42:.*]] = divf %[[VAL_35]], %[[VAL_41]] : f32
// CHECK:           %[[VAL_43:.*]] = select %[[VAL_17]], %[[VAL_15]], %[[VAL_42]] : f32
// CHECK:           %[[VAL_44:.*]] = fptrunc %[[VAL_43]] : f32 to f16
// CHECK:           return %[[VAL_44]] : f16
// CHECK:         }


