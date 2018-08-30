#include "xcvm_emitter.h"

#include <map>

#include <common/log.h>
#include <compiler/graph.h>
#include <compiler/model.h>
#include <compiler/node.h>
#include <compiler/value.h>
#include <runtime/xcvm.pb.h>
#include <runtime/xcvm_proto_util.h>

namespace oniku {
namespace xcvm {
namespace {

using oniku::runtime::XCProgramProto;

class XCVMEmitter {
public:
    explicit XCVMEmitter(const Graph& graph) : graph_(graph), value_ids_(AssignValueIds(graph)) {
    }

    void Emit(XCProgramProto* program) {
        EmitInputs(program);

        std::map<const Value*, int> num_users;
        for (const Value* value : graph_.temp_values()) {
            num_users.emplace(value, value->users().size());
        }

        std::vector<const Node*> nodes(graph_.GetComputationSequence());
        for (const Node* node : nodes) {
            EmitNode(*node, program);

            for (const Value* input : node->inputs()) {
                auto found = num_users.find(input);
                if (found == num_users.end()) continue;
                if (--found->second == 0) {
                    AddFreeOp(program, GetValueId(input));
                }
            }
        }

        EmitOutputs(program);
    }

    static std::map<const Value*, int> AssignValueIds(const Graph& graph) {
        int id = 1;
        std::map<const Value*, int> value_ids;
        for (const Value* v : graph.input_values()) {
            CHECK(value_ids.emplace(v, id++).second);
        }
        for (const Value* v : graph.temp_values()) {
            CHECK(value_ids.emplace(v, id++).second);
        }
        for (const Value* v : graph.output_values()) {
            CHECK(value_ids.emplace(v, id++).second);
        }
        return value_ids;
    }

private:
    int GetValueId(const Value* v) const {
        auto found = value_ids_.find(v);
        CHECK(found != value_ids_.end()) << "Value not exist: " << v->name();
        return found->second;
    }

    void EmitNode(const Node& node, XCProgramProto* prog) {
        auto in = [this, &node](int i) {
            CHECK_LT(i, node.inputs().size());
            return GetValueId(node.inputs()[i]);
        };

        // Optional input.
        auto oin = [this, in, &node](int i) {
            if (i >= static_cast<int>(node.inputs().size())) return -1;
            return in(i);
        };

        auto out = [this, &node](int i) {
            CHECK_LT(i, node.outputs().size());
            return GetValueId(node.outputs()[i]);
        };

        // Optional output.
        auto oout = [this, out, &node](int i) {
            if (i >= static_cast<int>(node.outputs().size())) return -1;
            return out(i);
        };

        auto pads = [&node]() {
            std::vector<int> pads = node.pads();
            if (pads.empty()) {
                pads = {0, 0};
            } else {
                // Both Chainer and xChainer expect paddings for beginning
                // and end are the same.
                CHECK_EQ(pads.size() % 2, 0);
                for (size_t i = 0; i < pads.size() / 2; ++i) {
                    CHECK_EQ(pads[i], pads[i + pads.size() / 2]);
                }
                pads.resize(pads.size() / 2);
            }
            return pads;
        };

        auto strides = [&node]() {
            std::vector<int> strides = node.strides();
            // TODO(hamaji): Infer strides for non-2D convolutions/pools.
            if (strides.empty()) strides = {1, 1};
            return strides;
        };

        const std::string& debug_info = node.DebugString();

#define EMIT(op, ...)                                                                          \
    do {                                                                                       \
        Add##op##Op(prog, __VA_ARGS__);                                                        \
        prog->mutable_instructions(prog->instructions_size() - 1)->set_debug_info(debug_info); \
    } while (0);

#define EMIT_SIMPLE_UNARY_OP(name, sym)           \
    do {                                          \
        if (node.op_type() == name) {             \
            CHECK_EQ(1UL, node.inputs().size());  \
            CHECK_EQ(1UL, node.outputs().size()); \
            EMIT(sym, out(0), in(0));             \
            return;                               \
        }                                         \
    } while (0)

#define EMIT_SIMPLE_BINARY_OP(name, sym)          \
    do {                                          \
        if (node.op_type() == name) {             \
            CHECK_EQ(2UL, node.inputs().size());  \
            CHECK_EQ(1UL, node.outputs().size()); \
            EMIT(sym, out(0), in(0), in(1));      \
            return;                               \
        }                                         \
    } while (0)

        EMIT_SIMPLE_UNARY_OP(Node::kNeg, Neg);
        EMIT_SIMPLE_UNARY_OP(Node::kExp, Exp);
        EMIT_SIMPLE_UNARY_OP(Node::kLog, Log);
        EMIT_SIMPLE_UNARY_OP(Node::kSqrt, Sqrt);
        EMIT_SIMPLE_UNARY_OP(Node::kTanh, Tanh);
        EMIT_SIMPLE_UNARY_OP(Node::kRelu, Relu);
        EMIT_SIMPLE_UNARY_OP(Node::kSigmoid, Sigmoid);
        EMIT_SIMPLE_UNARY_OP(Node::kNot, Not);
        EMIT_SIMPLE_UNARY_OP(Node::kIdentity, Identity);

        EMIT_SIMPLE_BINARY_OP(Node::kAdd, Add);
        EMIT_SIMPLE_BINARY_OP(Node::kSub, Sub);
        EMIT_SIMPLE_BINARY_OP(Node::kMul, Mul);
        EMIT_SIMPLE_BINARY_OP(Node::kDiv, Div);
        EMIT_SIMPLE_BINARY_OP(Node::kPow, Pow);
        EMIT_SIMPLE_BINARY_OP(Node::kEqual, Equal);
        EMIT_SIMPLE_BINARY_OP(Node::kGreater, Greater);

        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxReluGrad, ReluGrad);
        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxMaxPoolGrad, MaxPoolGrad);
        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxAveragePoolGrad, AveragePoolGrad);
        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxSelectItem, SelectItem);

        if (node.op_type() == Node::kDropout) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_LE(1UL, node.outputs().size());
            CHECK_GE(2UL, node.outputs().size());
            if (node.outputs().size() >= 2UL) {
                WARN_ONCE("The second output of Dropout is not handled yet");
            }
            // TODO(hamaji): Dropout does nothing for now.
            EMIT(Identity, out(0), in(0));
        } else if (node.op_type() == Node::kConv) {
            CHECK_LE(2UL, node.inputs().size());
            CHECK_GE(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            // TODO(xchainer): Support dilation.
            for (int d : node.dilations()) CHECK_EQ(d, 1) << "Dilation is not supported yet";
            EMIT(Conv, out(0), in(0), in(1), oin(2), strides(), pads());
        } else if (node.op_type() == Node::kConvTranspose) {
            CHECK_LE(2UL, node.inputs().size());
            CHECK_GE(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            // TODO(xchainer): Support dilation.
            for (int d : node.dilations()) CHECK_EQ(d, 1) << "Dilation is not supported yet";
            // TODO(hamaji): Handle output_padding and output_shape.
            std::vector<int> output_shape = node.output_shape();
            EMIT(ConvTranspose, out(0), in(0), in(1), oin(2), strides(), pads(), output_shape);
        } else if (node.op_type() == Node::kOnikuxConvTransposeWithDynamicOutputShape) {
            CHECK_EQ(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ConvTransposeWithDynamicShape, out(0), in(0), in(1), in(2), strides(), pads());
        } else if (node.op_type() == Node::kConvGradWeight) {
            CHECK_EQ(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            // TODO(xchainer): Support dilation.
            for (int d : node.dilations()) CHECK_EQ(d, 1) << "Dilation is not supported yet";
            EMIT(ConvGradWeight, out(0), in(0), in(1), in(2), strides(), pads());
        } else if (node.op_type() == Node::kLSTM) {
            CHECK_LE(3, node.inputs().size());
            CHECK_GE(3, node.outputs().size());
            EMIT(LSTM, oout(0), oout(1), oout(2), in(0), in(1), in(2), oin(3), oin(4), oin(5), oin(6), oin(7), node.hidden_size());
        } else if (node.op_type() == Node::kShape) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Shape, out(0), in(0));
        } else if (node.op_type() == Node::kSize) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Size, out(0), in(0));
        } else if (node.op_type() == Node::kReshape) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Reshape, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kExpand) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Expand, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kSqueeze) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Squeeze, out(0), in(0), node.axes());
        } else if (node.op_type() == Node::kUnsqueeze) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Unsqueeze, out(0), in(0), node.axes());
        } else if (node.op_type() == Node::kMatMul) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(MatMul, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kGemm) {
            CHECK_EQ(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Gemm, out(0), in(0), in(1), in(2), node.alpha(), node.beta(), node.trans_a(), node.trans_b());
        } else if (node.op_type() == Node::kBatchNormalization) {
            // TODO(hamaji): Handle running mean and variance for training mode.
            CHECK_EQ(5UL, node.inputs().size());
            EMIT(BatchNormalization, out(0), in(0), in(1), in(2), in(3), in(4), node.epsilon(), node.momentum(), node.spatial());
        } else if (node.op_type() == Node::kLRN) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(LRN, out(0), in(0), node.alpha(), node.beta(), node.bias(), node.size());
        } else if (node.op_type() == Node::kOnikuxLRNGrad) {
            CHECK_EQ(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(LRNGrad, out(0), in(0), in(1), in(2), node.alpha(), node.beta(), node.bias(), node.size());
        } else if (node.op_type() == Node::kMaxPool) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(MaxPool, out(0), in(0), node.kernel_shape(), strides(), pads());
        } else if (node.op_type() == Node::kAveragePool) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(AveragePool, out(0), in(0), node.kernel_shape(), strides(), pads(), node.count_include_pad());
        } else if (node.op_type() == Node::kSoftmax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            int axis = node.axis();
            if (axis < 0) axis = 1;
            EMIT(Softmax, out(0), in(0), axis);
        } else if (node.op_type() == Node::kLogSoftmax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            int axis = node.axis();
            if (axis < 0) axis = 1;
            EMIT(LogSoftmax, out(0), in(0), axis);
        } else if (node.op_type() == Node::kArgMax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ArgMax, out(0), in(0), node.axis(), node.keepdims());
        } else if (node.op_type() == Node::kHardmax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Hardmax, out(0), in(0), node.axis());
        } else if (node.op_type() == Node::kReduceMax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceMax, out(0), in(0), node.axes(), node.keepdims());
        } else if (node.op_type() == Node::kReduceSum) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceSum, out(0), in(0), node.axes(), node.keepdims());
        } else if (node.op_type() == Node::kReduceSumSquare) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceSumSquare, out(0), in(0), node.axes(), node.keepdims());
        } else if (node.op_type() == Node::kOnikuxReduceSumTo) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceSumTo, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kReduceMean) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceMean, out(0), in(0), node.axes(), node.keepdims());
        } else if (node.op_type() == Node::kCast) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Cast, out(0), in(0), node.to());
        } else if (node.op_type() == Node::kSlice) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            CHECK_NE(0UL, node.starts().size());
            CHECK_NE(0UL, node.ends().size());
            CHECK_EQ(node.starts().size(), node.ends().size());
            std::vector<int> axes{node.axes()};
            if (axes.empty()) {
                for (size_t i = 0; i < node.starts().size(); ++i) axes.push_back(i);
            } else {
                CHECK_EQ(node.starts().size(), axes.size());
            }
            EMIT(Slice, out(0), in(0), axes, node.starts(), node.ends());
        } else if (node.op_type() == Node::kGather) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Gather, out(0), in(0), in(1), node.axis());
        } else if (node.op_type() == Node::kConcat) {
            CHECK_EQ(1UL, node.outputs().size());
            std::vector<int> ins;
            for (size_t i = 0; i < node.inputs().size(); ++i) ins.push_back(in(i));
            EMIT(Concat, out(0), ins, node.axis());
        } else if (node.op_type() == Node::kOnikuxBatchNormalizationGrad) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(3UL, node.outputs().size());
            EMIT(BatchNormalizationGrad, out(0), out(1), out(2), in(0), in(1));
        } else if (node.op_type() == Node::kOnikuxSelectItemGrad) {
            CHECK_EQ(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(SelectItemGrad, out(0), in(0), in(1), in(2));
        } else {
            CHECK(false) << "Unsupported op: " << node.op_type();
        }
    }

    void EmitInputs(XCProgramProto* prog) {
        for (const Value* value : graph_.GetNecessaryInputs()) {
            AddInOp(prog, GetValueId(value), value->name());
            prog->mutable_instructions(prog->instructions_size() - 1)->set_debug_info(value->name());
        }
    }

    void EmitOutputs(XCProgramProto* prog) {
        for (const Value* value : graph_.output_values()) {
            AddOutOp(prog, value->name(), GetValueId(value));
            prog->mutable_instructions(prog->instructions_size() - 1)->set_debug_info(value->name());
        }
    }

    const Graph& graph_;
    std::map<const Value*, int> value_ids_;
};

}  // namespace

void Emit(const Model& model, XCProgramProto* program) {
    const Graph& graph = model.graph();
    XCVMEmitter emitter(graph);
    emitter.Emit(program);
}

void Emit(const Model& model, std::ostream& out) {
    XCProgramProto program;
    Emit(model, &program);
    CHECK(program.SerializeToOstream(&out));
}

}  // namespace xcvm
}  // namespace oniku
