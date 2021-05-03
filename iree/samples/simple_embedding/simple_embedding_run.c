// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// A example of setting up the HAL mddule to run simple pointwise array
// multiplication with the dylib driver.
#include <stdio.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"

#if IREE_ARCH_RISCV_64
#include "iree/hal/dylib/registration/driver_module.h"
#else
#include "iree/hal/drivers/init.h"
#endif

#include "iree/modules/hal/hal_module.h"
#include "iree/vm/api.h"
#include "iree/vm/bytecode_module.h"

// Compiled module embedded here to avoid file IO:
#if IREE_ARCH_RISCV_64
#include "iree/samples/simple_embedding/simple_embedding_test_llvm_aot_rv64.h"
#else
#include "iree/samples/simple_embedding/simple_embedding_test_bytecode_module_c.h"
#endif

iree_status_t Run(char* hal_driver_name) {
  // TODO(benvanik): move to instance-based registration.
  IREE_RETURN_IF_ERROR(iree_hal_module_register_types());

  iree_vm_instance_t* instance = NULL;
  IREE_RETURN_IF_ERROR(
      iree_vm_instance_create(iree_allocator_system(), &instance));

#if IREE_ARCH_RISCV_64
  // Only register dylib HAL driver
  IREE_RETURN_IF_ERROR(iree_hal_dylib_driver_module_register(
      iree_hal_driver_registry_default()));
#else
  // Register all drivers so it can be selected by the driver name.
  IREE_RETURN_IF_ERROR(iree_hal_register_all_available_drivers(
      iree_hal_driver_registry_default()));
#endif

  // Create the hal driver from the name. The driver name can be assigned as a
  // hard-coded char array such as "dylib" as well.
  iree_hal_driver_t* driver = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_driver_registry_try_create_by_name(
      iree_hal_driver_registry_default(),
      iree_make_cstring_view(hal_driver_name), iree_allocator_system(),
      &driver));
  iree_hal_device_t* device = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_driver_create_default_device(
      driver, iree_allocator_system(), &device));
  iree_vm_module_t* hal_module = NULL;
  IREE_RETURN_IF_ERROR(
      iree_hal_module_create(device, iree_allocator_system(), &hal_module));
  iree_hal_driver_release(driver);

  // Load bytecode module from the embedded data.
#if IREE_ARCH_RISCV_64
  const struct iree_file_toc_t* module_file_toc =
      simple_embedding_test_llvm_aot_rv64_create();
#else
  // Note the setup here only supports native build. The bytecode is not built
  // for the cross-compile execution. The code can be compiled but it will
  // hit runtime error in a cross-compile environment.
  const struct iree_file_toc_t* module_file_toc =
      simple_embedding_test_bytecode_module_c_create();
#endif

  iree_vm_module_t* bytecode_module = NULL;
  iree_const_byte_span_t module_data =
      iree_make_const_byte_span(module_file_toc->data, module_file_toc->size);
  IREE_RETURN_IF_ERROR(iree_vm_bytecode_module_create(
      module_data, iree_allocator_null(), iree_allocator_system(),
      &bytecode_module));

  // Allocate a context that will hold the module state across invocations.
  iree_vm_context_t* context = NULL;
  iree_vm_module_t* modules[] = {hal_module, bytecode_module};
  IREE_RETURN_IF_ERROR(iree_vm_context_create_with_modules(
      instance, &modules[0], IREE_ARRAYSIZE(modules), iree_allocator_system(),
      &context));
  iree_vm_module_release(hal_module);
  iree_vm_module_release(bytecode_module);

  // Lookup the entry point function.
  // Note that we use the synchronous variant which operates on pure type/shape
  // erased buffers.
  const char kMainFunctionName[] = "module.simple_mul";
  iree_vm_function_t main_function;
  IREE_RETURN_IF_ERROR(iree_vm_context_resolve_function(
      context, iree_make_cstring_view(kMainFunctionName), &main_function));

  // Allocate buffers that can be mapped on the CPU and that can also be used
  // on the device. Not all devices support this, but the ones we have now do.
  const int kElementCount = 4;
  iree_hal_buffer_t* arg0_buffer = NULL;
  iree_hal_buffer_t* arg1_buffer = NULL;
  iree_hal_memory_type_t input_memory_type =
      IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE;
  IREE_RETURN_IF_ERROR(iree_hal_allocator_allocate_buffer(
      iree_hal_device_allocator(device), input_memory_type,
      IREE_HAL_BUFFER_USAGE_ALL, sizeof(float) * kElementCount, &arg0_buffer));
  IREE_RETURN_IF_ERROR(iree_hal_allocator_allocate_buffer(
      iree_hal_device_allocator(device), input_memory_type,
      IREE_HAL_BUFFER_USAGE_ALL, sizeof(float) * kElementCount, &arg1_buffer));

  // Populate initial values for 4 * 2 = 8.
  const float kFloat4 = 4.0f;
  const float kFloat2 = 2.0f;
  IREE_RETURN_IF_ERROR(iree_hal_buffer_fill(arg0_buffer, 0, IREE_WHOLE_BUFFER,
                                            &kFloat4, sizeof(float)));
  IREE_RETURN_IF_ERROR(iree_hal_buffer_fill(arg1_buffer, 0, IREE_WHOLE_BUFFER,
                                            &kFloat2, sizeof(float)));

  // Wrap buffers in shaped buffer views.
  iree_hal_dim_t shape[1] = {kElementCount};
  iree_hal_buffer_view_t* arg0_buffer_view = NULL;
  iree_hal_buffer_view_t* arg1_buffer_view = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_buffer_view_create(
      arg0_buffer, IREE_HAL_ELEMENT_TYPE_FLOAT_32, shape, IREE_ARRAYSIZE(shape),
      &arg0_buffer_view));
  IREE_RETURN_IF_ERROR(iree_hal_buffer_view_create(
      arg1_buffer, IREE_HAL_ELEMENT_TYPE_FLOAT_32, shape, IREE_ARRAYSIZE(shape),
      &arg1_buffer_view));
  iree_hal_buffer_release(arg0_buffer);
  iree_hal_buffer_release(arg1_buffer);

  // Setup call inputs with our buffers.
  iree_vm_list_t* inputs = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_list_create(
                           /*element_type=*/NULL,
                           /*capacity=*/2, iree_allocator_system(), &inputs),
                       "can't allocate input vm list");

  iree_vm_ref_t arg0_buffer_view_ref =
      iree_hal_buffer_view_move_ref(arg0_buffer_view);
  iree_vm_ref_t arg1_buffer_view_ref =
      iree_hal_buffer_view_move_ref(arg1_buffer_view);
  IREE_RETURN_IF_ERROR(
      iree_vm_list_push_ref_move(inputs, &arg0_buffer_view_ref));
  IREE_RETURN_IF_ERROR(
      iree_vm_list_push_ref_move(inputs, &arg1_buffer_view_ref));

  // Prepare outputs list to accept the results from the invocation.
  // The output vm list is allocated statically.
  iree_vm_list_t* outputs = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_list_create(
                           /*element_type=*/NULL,
                           /*capacity=*/1, iree_allocator_system(), &outputs),
                       "can't allocate output vm list");

  // Synchronously invoke the function.
  IREE_RETURN_IF_ERROR(iree_vm_invoke(context, main_function,
                                      /*policy=*/NULL, inputs, outputs,
                                      iree_allocator_system()));

  // Get the result buffers from the invocation.
  iree_hal_buffer_view_t* ret_buffer_view =
      (iree_hal_buffer_view_t*)iree_vm_list_get_ref_deref(
          outputs, 0, iree_hal_buffer_view_get_descriptor());
  if (ret_buffer_view == NULL) {
    return iree_make_status(IREE_STATUS_NOT_FOUND,
                            "can't find return buffer view");
  }

  // Read back the results and ensure we got the right values.
  iree_hal_buffer_mapping_t mapped_memory;
  IREE_RETURN_IF_ERROR(iree_hal_buffer_map_range(
      iree_hal_buffer_view_buffer(ret_buffer_view), IREE_HAL_MEMORY_ACCESS_READ,
      0, IREE_WHOLE_BUFFER, &mapped_memory));
  for (int i = 0; i < mapped_memory.contents.data_length / sizeof(float); ++i) {
    if (((const float*)mapped_memory.contents.data)[i] != 8.0f) {
      return iree_make_status(IREE_STATUS_UNKNOWN, "result mismatches");
    }
  }
  iree_hal_buffer_unmap_range(&mapped_memory);

  iree_vm_list_release(inputs);
  iree_vm_list_release(outputs);
  iree_hal_device_release(device);
  iree_vm_context_release(context);
  iree_vm_instance_release(instance);
  return iree_ok_status();
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: simple_embedding_run <HAL driver name>\n");
    return -1;
  }
  char* hal_driver_name = argv[1];
  const iree_status_t result = Run(hal_driver_name);
  if (!iree_status_is_ok(result)) {
    char* message;
    size_t message_length;
    iree_status_to_string(result, &message, &message_length);
    fprintf(stderr, "simple_embedding_run failed: %s\n", message);
    iree_allocator_free(iree_allocator_system(), message);
    return -1;
  }
  printf("simple_embedding_run passed\n");
  return 0;
}