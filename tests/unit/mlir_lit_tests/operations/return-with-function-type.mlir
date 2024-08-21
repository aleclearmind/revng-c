//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: not %revngcliftopt %s 2>&1 | FileCheck %s

!void = !clift.primitive<VoidKind 0>

!f = !clift.defined<#clift.function<
  id = 1000,
  name = "f",
  return_type = !void,
  argument_types = []>>

// CHECK: requires void or non-array object type
clift.return {
  %0 = clift.undef !f
  clift.yield %0 : !f
}
