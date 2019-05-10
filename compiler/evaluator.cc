#include "compiler/evaluator.h"

#include <stdlib.h>
#include <string.h>

#include <set>

#include <chainerx/array.h>
#include <chainerx/device.h>
#include <chainerx/native/native_backend.h>

#include <common/strutil.h>
#include <compiler/log.h>
#include <compiler/node.h>
#include <compiler/tensor.h>
#include <compiler/value.h>
#include <compiler/xcvm/emitter.h>
#include <runtime/chainerx_util.h>
#include <runtime/xcvm.h>
#include <runtime/xcvm.pb.h>
#include <runtime/xcvm_state.h>
#include <runtime/xcvm_var.h>

namespace chainer_compiler {

namespace {

Tensor* ArrayToTensor(const std::string& name, const chainerx::Array& a) {
    Tensor::UniqueData data(std::malloc(a.GetNBytes()), &std::free);
    memcpy(data.get(), a.Copy().ToNative().raw_data(), a.GetNBytes());
    std::vector<int64_t> dims{a.shape().begin(), a.shape().end()};
    return new Tensor(name, Dtype(a.dtype()), dims, data.release());
}

}  // namespace

EvaluatedValue::EvaluatedValue(Tensor* tensor) : tensor_(tensor) {
}

EvaluatedValue::EvaluatedValue(std::vector<std::unique_ptr<Tensor>>&& sequence) : sequence_(std::move(sequence)) {
}

Tensor* EvaluatedValue::ReleaseTensor() {
    CHECK(is_tensor());
    return tensor_.release();
}

std::vector<std::unique_ptr<Tensor>> EvaluatedValue::ReleaseSequence() {
    std::vector<std::unique_ptr<Tensor>> ret;
    std::swap(ret, sequence_);
    return ret;
}

void Eval(
        const std::vector<Node*>& nodes,
        const std::vector<std::pair<Value*, Tensor*>>& feeds,
        const std::vector<Value*>& fetches,
        std::vector<std::unique_ptr<EvaluatedValue>>* outputs) {
    runtime::XCProgramProto program;
    std::vector<int> input_ids;
    std::vector<int> output_ids;
    {
        std::vector<Value*> feed_values;
        for (const auto& p : feeds) {
            feed_values.push_back(p.first);
        }
        xcvm::Emit(nodes, feed_values, fetches, &program, &input_ids, &output_ids);
        // CLOG() << "Evaluate " << program.DebugString();
    }

    chainerx::DeviceScope device_scope{chainerx::GetNativeBackend().GetDevice(0)};

    runtime::XCVM xcvm(program);
    runtime::XCVMOptions xcvm_options;
    runtime::XCVMState state(xcvm_options, xcvm.num_variables(), {});

    for (size_t i = 0; i < feeds.size(); ++i) {
        int input_id = input_ids[i];
        const Tensor* t = feeds[i].second;
        CHECK_NE(Dtype::kUnknown, t->dtype());
        chainerx::Dtype dtype = static_cast<chainerx::Dtype>(static_cast<int>(t->dtype()));
        chainerx::Shape shape(t->dims());
        chainerx::Array array = runtime::MakeHostArray(dtype, shape, t->GetRawData());
        state.SetArray(input_id, array);
    }

    xcvm.Run(&state);

    for (size_t i = 0; i < fetches.size(); ++i) {
        const std::string& name = fetches[i]->name();
        int output_id = output_ids[i];
        runtime::XCVMVar* var = state.GetVar(output_id);

        switch (var->kind()) {
            case runtime::XCVMVar::Kind::kArray: {
                outputs->emplace_back(new EvaluatedValue(ArrayToTensor(name, state.GetArray(output_id))));
                break;
            }

            case runtime::XCVMVar::Kind::kSequence: {
                const runtime::XCVMSequence& seq = *var->GetSequence();
                std::vector<std::unique_ptr<Tensor>> tensors;
                for (size_t j = 0; j < seq.size(); ++j) {
                    // TODO(hamaji): Support nested sequences.
                    tensors.emplace_back(ArrayToTensor(StrCat(name, '_', j), seq[j].GetArray()));
                }
                outputs->emplace_back(new EvaluatedValue(std::move(tensors)));
                break;
            }

            default:
                // TODO(hamaji): Support other types.
                CHECK(false) << "Not supported yet: " << var->DebugString();
        }
    }
}

void Eval(const std::vector<Node*>& nodes, const std::vector<Value*>& fetches, std::vector<std::unique_ptr<EvaluatedValue>>* outputs) {
    Eval(nodes, {}, fetches, outputs);
}

}  // namespace chainer_compiler
