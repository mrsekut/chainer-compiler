#include "xcvm.h"

#include <numeric>

#include <xchainer/array.h>

#include <runtime/xchainer.h>
#include <runtime/xcvm.pb.h>
#include <runtime/xcvm_op.h>
#include <runtime/xcvm_state.h>

#define RANGE(x) (x).begin(), (x).end()

namespace oniku {
namespace runtime {

XCVM::XCVM(const XCProgramProto& program) {
    num_variables_ = 0;
    for (const XCInstructionProto& inst : program.instructions()) {
        for (int output : inst.outputs()) {
            num_variables_ = std::max(num_variables_, output + 1);
        }
    }

    for (const XCInstructionProto& inst : program.instructions()) {
        XCVMOp* op = MakeXCVMOp(inst);
        op->set_debug_info(inst.debug_info());
        program_.emplace_back(op);
    }
}

XCVM::~XCVM() {
}

InOuts XCVM::Run(const InOuts& program_inputs, int trace_level, bool is_training) {
    XCVMState state(num_variables_, program_inputs);
    state.set_trace_level(trace_level);
    state.set_is_training(is_training);

    while (true) {
        int pc = state.pc();
        if (pc >= program_.size()) break;

        XCVMOp* op = program_[pc].get();
        op->Run(&state);

        state.set_pc(pc + 1);
    }

    return state.GetOutputs();
}

}  // namespace runtime
}  // namespace oniku
