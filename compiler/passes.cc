#include "compiler/passes.h"

#include <iostream>
#include <map>
#include <memory>

#include <compiler/computation_order/core.h>
#include <compiler/config.h>
#include <compiler/constant_propagation.h>
#include <compiler/dtype_inference.h>
#include <compiler/flags.h>
#include <compiler/flops.h>
#include <compiler/fusion.h>
#include <compiler/gradient.h>
#include <compiler/gradient_with_order.h>
#include <compiler/graph.h>
#include <compiler/memory_simulator.h>
#include <compiler/merge.h>
#include <compiler/model.h>
#include <compiler/scheduler.h>
#include <compiler/shape_evaluator.h>
#include <compiler/simplifier.h>
#include <compiler/subgraph_canonicalizer.h>
#include <configs/backend_config.h>

namespace chainer_compiler {

namespace {

void CollectGarbageNode(Graph* graph) {
    for (Node* node : graph->nodes()) {
        if (node->chainer_order() <= 0) graph->DetachNode(node);
    }
    graph->DeleteDetached();
}

void CheckAllOpsSupported(const CompilerConfig& ccfg, Graph* graph) {
    for (Node* node : graph->nodes()) {
        CHECK(ccfg.HasOp(node->op_type())) << "Op not supported by backend (" << ccfg.name() << ")\n" << node->DebugString();
    }
}

template <class Fn>
void Recursively(Fn fn, Graph* graph) {
    fn(graph);
    for (const Node* node : graph->nodes()) {
        for (Graph* subgraph : node->GetSubGraphs()) {
            Recursively(fn, subgraph);
        }
    }
}

}  //  namespace

void RunDefaultPasses(Model* model, bool gen_backprop) {
    RunDefaultPasses(model->mutable_graph(), gen_backprop);
}

void RunDefaultPasses(Graph* graph, bool gen_backprop, bool skip_scheduling) {
    std::unique_ptr<BackendConfig> backend_config(BackendConfig::FromName(g_backend_name));

    // TODO(hamaji): Improve backend selection probably by `CompilerConfig`.
    g_modify_pool_with_imbalanced_pads = !g_use_ngraph;

    if (g_reset_output_shape) {
        for (Value* value : graph->output_values()) {
            value->set_type(new Type());
        }
    }
    if (g_reset_shape) {
        for (const std::unique_ptr<Value>& value : graph->all_values()) {
            value->set_type(new Type());
        }
    }
    if (!g_skip_inference) {
        graph->InferShapes();
        InferAllDtype(graph);
    }

    std::unique_ptr<CompilerConfig> ccfg{GetCompilerConfig(g_backend_name)};

    auto dump_onnx = [&graph](bool cond, const char* msg) {
        if (cond) {
            std::cerr << "=== vvv " << msg << " vvv ===\n";
            std::cerr << graph->DebugString();
            std::cerr << "=== ^^^ " << msg << " ^^^ ===\n";
        }
        Recursively([msg](Graph* g) { g->CheckSanity(msg); }, graph);
    };

    dump_onnx(g_dump_after_inference, "after inference");

    CanonicalizeSubGraphs(graph);

    if (!skip_scheduling) {
        Recursively([&backend_config, gen_backprop](Graph* g) { Simplify(backend_config->GetSimplifyPreproc(), g, gen_backprop); }, graph);

        Recursively(MergeOperations, graph);

        Recursively(PropagateConstants, graph);

        Recursively(EvaluateShapes, graph);

        Recursively([](Graph* g) { g->DeleteDetached(); }, graph);

        dump_onnx(g_dump_after_simplification, "after simplification");
    }

    if (gen_backprop) {
        Recursively([&backend_config, gen_backprop](Graph* g) { Simplify(backend_config->GetSimplify(), g, gen_backprop); }, graph);

        if (g_computation_order.empty()) {
            // normal computation order
            AddGradientNodesForTraining(graph);
        } else {
            // specified computation order
            skip_scheduling = true;
            auto orders = GetComputationOrder(*graph, g_computation_order);
            if (!AddGradientNodesForTrainingWithOrders(graph, orders)) {
                CHECK(false) << "Computation order is not supported in this graph.";
            }
        }
    }

    // TODO(hamaji): Make it possible to infer shapes here.
    // if (!g_skip_inference) graph->InferShapes();

    if (!skip_scheduling) {
        Recursively([&backend_config, gen_backprop](Graph* g) { Simplify(backend_config->GetSimplify(), g, gen_backprop); }, graph);

        Recursively(PropagateConstants, graph);

        Recursively([](Graph* g) { g->DeleteDetached(); }, graph);
    }

    dump_onnx(g_dump_after_gradient, "after gradient generation");

    if (g_dump_subgraphs) {
        graph->DumpSubGraphs();
    }

    if (!skip_scheduling) {
        if (g_fuse_operations) {
            FuseOperations(graph, g_use_tvm, g_use_ngraph);
            dump_onnx(g_dump_after_fusion, "after fusion");
        }
    }

    int64_t order = 0;
    Recursively([&order](Graph* g) { order = ScheduleComputation(*g, order); }, graph);

    if (g_compiler_log) {
        ShowSimulatedMemoryUsage(*graph);
        ShowFlops(*graph);
    }

    Recursively(CollectGarbageNode, graph);

    dump_onnx(g_dump_after_scheduling, "after scheduling");

    Recursively([&ccfg](Graph* g) { CheckAllOpsSupported(*ccfg, g); }, graph);
}

void RunDefaultPassesBeforeGradient(Graph* graph) {
    std::unique_ptr<BackendConfig> backend_config(BackendConfig::FromName(g_backend_name));
    std::unique_ptr<CompilerConfig> ccfg{GetCompilerConfig(g_backend_name)};
    graph->InferShapes();
    CanonicalizeSubGraphs(graph);
    Recursively([&backend_config](Graph* g) { Simplify(backend_config->GetSimplify(), g, true); }, graph);
    Recursively(PropagateConstants, graph);
    Recursively([](Graph* g) { g->DeleteDetached(); }, graph);
    Recursively([&ccfg](Graph* g) { CheckAllOpsSupported(*ccfg, g); }, graph);
}

}  // namespace chainer_compiler
