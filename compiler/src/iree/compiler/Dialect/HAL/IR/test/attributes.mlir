// RUN: iree-opt --allow-unregistered-dialect --split-input-file --mlir-print-local-scope %s | FileCheck %s

// CHECK-LABEL: descriptor_set_layout_binding.basic
"descriptor_set_layout_binding.basic"() {
  // CHECK: dslb0 = #hal.descriptor_set.binding<0, uniform_buffer>
  dslb0 = #hal.descriptor_set.binding<0, uniform_buffer>,
  // CHECK: dslb1 = #hal.descriptor_set.binding<1, storage_buffer>
  dslb1 = #hal.descriptor_set.binding<1, storage_buffer>
} : () -> ()

// -----

// CHECK-LABEL: pipeline_layout.basic
"pipeline_layout.basic"() {
  // CHECK: layout0 = #hal.pipeline.layout<push_constants = 4, sets = [
  // CHECK-SAME: <0, bindings = [
  // CHECK-SAME:   <0, storage_buffer>
  // CHECK-SAME:   <1, storage_buffer>
  // CHECK-SAME: <1, bindings = [
  // CHECK-SAME:   <0, uniform_buffer>
  layout0 = #hal.pipeline.layout<push_constants = 4, sets = [
    #hal.descriptor_set.layout<0, bindings = [
      #hal.descriptor_set.binding<0, storage_buffer>,
      #hal.descriptor_set.binding<1, storage_buffer>
    ]>,
    #hal.descriptor_set.layout<1, bindings = [
      #hal.descriptor_set.binding<0, uniform_buffer>
    ]>
  ]>
} : () -> ()

// -----

"executable.objects"() {
  // CHECK: data = #hal.executable.object<{data = dense<[4, 5, 6, 7]> : vector<4xi8>}>
  data = #hal.executable.object<{data = dense<[4, 5, 6, 7]> : vector<4xi8>}>,
  // CHECK: path = #hal.executable.object<{path = "foo"}>
  path = #hal.executable.object<{path = "foo"}>
} : () -> ()

// -----

#target_a = #hal.executable.target<"llvm-cpu", "a">
#target_b = #hal.executable.target<"llvm-cpu", "b">
#target_c = #hal.executable.target<"llvm-cpu", "c">
// CHECK-LABEL: "executable.target_objects"
"executable.target_objects"() {
  // CHECK-SAME: empty = #hal.executable.objects<{}>
  empty = #hal.executable.objects<{}>,
  // CHECK-SAME: targets_a = #hal.executable.objects<{#hal.executable.target<"llvm-cpu", "a"> = [#hal.executable.object<{path = "a.o"}>]}>
  targets_a = #hal.executable.objects<{
    #target_a = [#hal.executable.object<{path = "a.o"}>]
  }>,
  // CHECK-SAME: targets_bc = #hal.executable.objects<{#hal.executable.target<"llvm-cpu", "b"> = [#hal.executable.object<{path = "b.o"}>], #hal.executable.target<"llvm-cpu", "c"> = [#hal.executable.object<{path = "c.o"}>]}>
  targets_bc = #hal.executable.objects<{
    #target_b = [#hal.executable.object<{path = "b.o"}>],
    #target_c = [#hal.executable.object<{path = "c.o"}>]
  }>
} : () -> ()

// -----

"affinity.queue"() {
  // CHECK: any = #hal.affinity.queue<*>
  any = #hal.affinity.queue<*>,
  // CHECK: q0 = #hal.affinity.queue<[0]>
  q0 = #hal.affinity.queue<[0]>,
  // CHECK: q123 = #hal.affinity.queue<[1, 2, 3]>
  q123 = #hal.affinity.queue<[1, 2, 3]>
} : () -> ()
