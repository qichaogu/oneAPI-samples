#include <iostream>

// oneAPI headers
#include <sycl/ext/intel/fpga_extensions.hpp>
#include <sycl/sycl.hpp>

#include "exception_handler.hpp"

// Forward declare the kernel name in the global scope. This is an FPGA best
// practice that reduces name mangling in the optimization reports.
class IDSimpleVAdd;

// Forward declare pipe names to reduce name mangling
class IDPipeA;
class IDPipeB;
class IDPipeC;

using PipeProps = decltype(sycl::ext::oneapi::experimental::properties(
    sycl::ext::intel::experimental::ready_latency<0>));

using InputPipeA =
    sycl::ext::intel::experimental::pipe<IDPipeA, int, 0, PipeProps>;
using InputPipeB =
    sycl::ext::intel::experimental::pipe<IDPipeB, int, 0, PipeProps>;

using CsrOutProperties = decltype(sycl::ext::oneapi::experimental::properties(
    sycl::ext::intel::experimental::protocol<
        // Host doesn't care about possibly missing an update, so no need for
        // protocol_name::avalon_mm_uses_ready
        sycl::ext::intel::experimental::protocol_name::avalon_mm>));

using OutputPipeC =
    sycl::ext::intel::experimental::pipe<IDPipeC, int, 0, CsrOutProperties>;

struct SimpleVAddKernel {
  int len;

  void operator()() const {
    int sum_total = 0;
    for (int idx = 0; idx < len; idx++) {
      int a_val = InputPipeA::read();
      int b_val = InputPipeB::read();
      int sum = a_val + b_val;

      sum_total += sum;
    }

    // Write to OutputPipeC only once per kernel invocation. Since we requested
    // protcol_avalon_mm instead of protocol_avalon_mm_uses_ready, this write is
    // effectively non-blocking.
    OutputPipeC::write(sum_total);
  }
};

constexpr int kVectorSize = 256;

int main() {
  try {
    // Use compile-time macros to select either:
    //  - the FPGA emulator device (CPU emulation of the FPGA)
    //  - the FPGA device (a real FPGA)
    //  - the simulator device
#if FPGA_SIMULATOR
    auto selector = sycl::ext::intel::fpga_simulator_selector_v;
#elif FPGA_HARDWARE
    auto selector = sycl::ext::intel::fpga_selector_v;
#else  // #if FPGA_EMULATOR
    auto selector = sycl::ext::intel::fpga_emulator_selector_v;
#endif

    // create the device queue
    sycl::queue q(selector, fpga_tools::exception_handler);

    auto device = q.get_device();

    std::cout << "Running on device: "
              << device.get_info<sycl::info::device::name>().c_str()
              << std::endl;

    // Vector size is a constant here, but it could be a run-time variable too.
    int count = kVectorSize;

    int expected_sum = 0;

    // push data into pipes before invoking kernel
    int *a = new int[count];
    int *b = new int[count];
    for (int i = 0; i < count; i++) {
      a[i] = i;
      b[i] = (count - i);

      expected_sum += (a[i] + b[i]);
      // When writing to a host pipe in non kernel code,
      // you must pass the sycl::queue as the first argument
      InputPipeA::write(q, a[i]);
      InputPipeB::write(q, b[i]);
    }

    std::cout << "Add two vectors of size " << count << std::endl;

    sycl::event e = q.single_task<IDSimpleVAdd>(SimpleVAddKernel{count});

    // Verify that outputs are correct, after the kernel has finished running.
    // Since the write to OutputPipeC is non-blocking, no need to worry about
    // deadlock.
    e.wait();
    bool passed = true;

    // Only read from OutputPipeC once, since the kernel only wrote to it once
    int calc = OutputPipeC::read(q);
    if (calc != expected_sum) {
      std::cout << "result " << calc << ", expected (" << expected_sum << ")"
                << std::endl;
      passed = false;
    }

    std::cout << (passed ? "PASSED" : "FAILED") << std::endl;

    delete[] a;
    delete[] b;

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;

  } catch (sycl::exception const &e) {
    std::cerr << "Caught a synchronous SYCL exception: " << e.what()
              << std::endl;
    std::cerr << "   If you are targeting an FPGA hardware, "
                 "ensure that your system is plugged to an FPGA board that is "
                 "set up correctly"
              << std::endl;
    std::terminate();
  }
}