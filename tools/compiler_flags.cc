#include "tools/compiler_flags.h"

#include <compiler/flags.h>

namespace chainer_compiler {
namespace runtime {

void AddCompilerFlags(cmdline::parser* args) {
    args->add("compiler_log", '\0', "Show logs from compiler");
    args->add("permissive", '\0', "Relax checks to accept more kinds of ONNX");
    args->add("skip_inference", '\0', "Skip dtype/shape inference");
    args->add("fuse_operations", '\0', "Fuse consecutive operations");
    args->add("use_nvrtc", '\0', "Use NVRTC");
    args->add("use_tvm", '\0', "Use TVM");
    args->add("reuse_tvm_code", '\0', "Reuse TVM code (unsafe)");
    args->add("use_ngraph", '\0', "Use nGraph");
    args->add<std::string>("ngraph_device", '\0', "The device of nGraph (e.g., CPU and INTELGPU)", false);
    args->add("use_dldt", '\0', "Use dldt");
    args->add("use_dldt_fp16", '\0', "Use fp16 with dldt");
    args->add<std::string>("dldt_device", '\0', "The device of dldt (e.g., CPU and GPU)", false);
    args->add("reset_shape", '\0', "Reset all shape information");
    args->add("reset_output_shape", '\0', "Reset output shape information");
    args->add<std::string>("dump_autotvm_task_dir", '\0', "Output AutoTVM tasks in this directory", false);
    args->add<std::string>("autotvm_log", '\0', "A tuning log of AutoTVM which contains best scheduling parameters", false);
    args->add("dump_after_inference", '\0', "Dump the ONNX graph after dtype/shape inference");
    args->add("dump_after_simplification", '\0', "Dump the ONNX graph after graph simplification");
    args->add("dump_after_gradient", '\0', "Dump the ONNX graph after adding nodes for gradients");
    args->add("dump_after_fusion", '\0', "Dump the ONNX graph after operator fusion");
    args->add("dump_after_scheduling", '\0', "Dump the ONNX graph after scheduling");
    args->add("dump_subgraphs", '\0', "Dump the subgraph tree of the ONNX graph");
    args->add<std::string>("computation_order", '\0', "Run the specified policy of computation order (backprop only)", false);
    args->add<int>("chen_budget", '\0', "Memory budget of Chen's policy (in MB)", 0);
}

void ApplyCompilerFlags(const cmdline::parser& args) {
    g_compiler_log = args.exist("compiler_log");
    g_permissive = args.exist("permissive");
    g_skip_inference = args.exist("skip_inference");
    g_fuse_operations = args.exist("fuse_operations");
    g_use_nvrtc = args.exist("use_nvrtc");
    g_use_tvm = args.exist("use_tvm");
    g_reuse_tvm_code = args.exist("reuse_tvm_code");
    g_use_ngraph = args.exist("use_ngraph");
    g_ngraph_device = args.get<std::string>("ngraph_device");
    g_use_dldt = args.exist("use_dldt");
    g_use_dldt_fp16 = args.exist("use_dldt_fp16");
    g_dldt_device = args.get<std::string>("dldt_device");
    g_reset_shape = args.exist("reset_shape");
    g_reset_output_shape = args.exist("reset_output_shape");
    g_dump_autotvm_task_dir = args.get<std::string>("dump_autotvm_task_dir");
    g_autotvm_log = args.get<std::string>("autotvm_log");
    g_dump_after_inference = args.exist("dump_after_inference");
    g_dump_after_simplification = args.exist("dump_after_simplification");
    g_dump_after_gradient = args.exist("dump_after_gradient");
    g_dump_after_fusion = args.exist("dump_after_fusion");
    g_dump_after_scheduling = args.exist("dump_after_scheduling");
    g_dump_subgraphs = args.exist("dump_subgraphs");
    g_computation_order = args.get<std::string>("computation_order");
    g_chen_budget = args.get<int>("chen_budget");
    if (args.exist("trace")) g_trace_level = 1;
    if (args.exist("verbose")) g_trace_level = 2;
}

}  // namespace runtime
}  // namespace chainer_compiler
