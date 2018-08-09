import subprocess

ARRAY = 'ARRAY'
INT = 'INT'
FLOAT = 'FLOAT'
INTS = 'INTS'
STRING = 'STRING'

XC_TYPES = [
    ARRAY, INT, FLOAT, INTS, STRING
]

STACK_VECTOR = 'xchainer::StackVector<int64_t, xchainer::kMaxNdim>'


def Array(name):
    return (ARRAY, name)


def Int(name):
    return (INT, name)


def Float(name):
    return (FLOAT, name)


def Ints(name):
    return (INTS, name)


def String(name):
    return (STRING, name)


XC_OPS = [
    ('In', [String('name')], 'v'),
    ('Out', [String('name'), Array('v')], []),

    ('Add', [Array('a'), Array('b')], ['c']),
    ('Conv',
     [Array('x'), Array('w'), Ints('strides'), Ints('pads')],
     ['y']),
    ('ConvWithBias',
     [Array('x'), Array('w'), Ints('strides'), Ints('pads'), Array('b')],
     ['y']),
    ('Ident', [Array('x')], ['y']),
    ('Relu', [Array('x')], ['y']),
    ('Reshape', [Array('data'), Array('shape')], ['reshaped']),
    ('Softmax', [Array('input'), Int('axis')], ['output']),
    ('LogSoftmax', [Array('input'), Int('axis')], ['output']),
    ('MaxPool',
     [Array('x'), Ints('kernel_shapes'), Ints('strides'), Ints('pads')],
     ['y']),
    ('AveragePool',
     [Array('x'), Ints('kernel_shapes'), Ints('strides'), Ints('pads'),
      Int('count_include_pad')],
     ['y']),
]


def format_code(lines):
    formatted = []
    num_indents = 0
    for line in lines:
        num_indents -= line.count('}') * 4
        if line:
            ni = num_indents
            if line.endswith(':'):
                ni -= 4
            line = ' ' * ni + line
        formatted.append(line + '\n')
        if '}' in line:
            formatted.append('\n')
        num_indents += line.count('{') * 4
    return formatted


def gen_xcvm_proto():
    lines = []
    lines.append('message XCValueProto {')
    lines.append('enum Type {')
    for i, typ in enumerate(XC_TYPES):
        lines.append('%s = %d;' % (typ, i + 1))
    lines.append('}')
    lines.append('required Type type = 1;')
    lines.append('optional int32 array = 2;')
    lines.append('optional int32 i = 3;')
    lines.append('optional float f = 4;')
    lines.append('repeated int32 ints = 5;')
    lines.append('optional string s = 6;')
    lines.append('}')

    lines.append('message XCInstructionProto {')
    lines.append('enum Op {')
    for i, (op, _, _) in enumerate(XC_OPS):
        lines.append('%s = %d;' % (op, i + 1))
    lines.append('}')
    lines.append('required Op op = 1;')
    lines.append('repeated XCValueProto inputs = 2;')
    lines.append('repeated int32 outputs = 3;')
    lines.append('}')

    lines.append('message XCProgramProto {')
    lines.append('repeated XCInstructionProto instructions = 1;')
    lines.append('}')

    with open('xcvm.proto', 'w') as f:
        f.write(r'''// Auto-generated by gen_xcvm.py

syntax = "proto2";

package oniku.runtime;

''')
        f.writelines(format_code(lines))

    subprocess.check_call(['protoc', 'xcvm.proto', '--cpp_out=.'])


def gen_xcvm_ops_h():
    lines = []

    for op, inputs, outputs in XC_OPS:
        lines.append('class %sOp : public XCVMOp {' % op)
        lines.append('public:')
        lines.append('explicit %sOp(const XCInstructionProto& inst);' % op)

        args = ['XCVMState* st']
        for typ, name in inputs:
            if typ != ARRAY:
                continue
            args.append(f'const xchainer::Array& {name}')
        rettype = 'void'
        if len(outputs) == 1:
            rettype = 'xchainer::Array'
        elif len(outputs) > 1:
            raise RuntimeError('Multiple outputs is not defined yet')
        lines.append('%s RunImpl(%s);' % (rettype, ', '.join(args)))

        lines.append('virtual void Run(XCVMState* st) {')
        args = ['st']
        for typ, name in inputs:
            if typ != ARRAY:
                continue
            args.append(f'st->GetVar({name})')
        call = 'RunImpl(%s)' % ', '.join(args)
        if len(outputs) == 1:
            lines.append('st->SetVar(%s, %s);' % (outputs[0], call))
        elif not outputs:
            lines.append(call + ';')

        lines.append('}')

        lines.append('private:')
        for typ, name in inputs:
            ctype = None
            if typ == ARRAY or typ == INT:
                ctype = 'int'
            elif typ == FLOAT:
                ctype = 'float'
            elif typ == STRING:
                ctype = 'std::string'
            elif typ == INTS:
                ctype = STACK_VECTOR
            else:
                raise RuntimeError('Unknown type: %s' % typ)
            lines.append(f'{ctype} {name};')

        for name in outputs:
            lines.append('int %s;' % name)

        lines.append('};')

    with open('xcvm_ops.h', 'w') as f:
        f.write(r'''// Auto-generated by gen_xcvm.py

#pragma once

#include <string>

#include <xchainer/stack_vector.h>

#include <runtime/xcvm_op.h>
#include <runtime/xcvm_state.h>
#include <runtime/xcvm.pb.h>

namespace oniku {
namespace runtime {

''')
        f.writelines(format_code(lines))
        f.write(r'''
}  // namespace runtime
}  // namespace oniku
''')


def gen_init_xcvm_ops_cc():
    lines = []

    for op, inputs, outputs in XC_OPS:
        lines.append('%sOp::%sOp(const XCInstructionProto& inst) {' % (op, op))
        for i, (typ, name) in enumerate(inputs):
            lines.append(f'CHECK_EQ(XCValueProto::{typ}, ' +
                         f'inst.inputs({i}).type()) ' +
                         f'<< "Unexpected type for input#{i} of {op}";')
            if typ == ARRAY:
                lines.append('%s = inst.inputs(%d).array();' % (name, i))
            elif typ == INT:
                lines.append('%s = inst.inputs(%d).i();' % (name, i))
            elif typ == FLOAT:
                lines.append('%s = inst.inputs(%d).f();' % (name, i))
            elif typ == STRING:
                lines.append('%s = inst.inputs(%d).s();' % (name, i))
            elif typ == INTS:
                lines.append(f'{name} = {STACK_VECTOR}(' +
                             f'inst.inputs({i}).ints().begin(), ' +
                             f'inst.inputs({i}).ints().end());')
            else:
                raise RuntimeError('Unknown type: %s' % typ)

        for i, name in enumerate(outputs):
            lines.append('%s = inst.outputs(%d);' % (name, i))

        lines.append('};')

    lines.append('XCVMOp* MakeXCVMOp(const XCInstructionProto& inst) {')
    lines.append('switch (inst.op()) {')
    for op, _, _ in XC_OPS:
        lines.append(f'case XCInstructionProto::{op}:')
        lines.append(f'return new {op}Op(inst);')
    lines.append('default:')
    lines.append('CHECK(false) << "Unknown op: " ' +
                 '<< static_cast<int>(inst.op());')
    lines.append('}')
    lines.append('}')

    with open('init_xcvm_ops.cc', 'w') as f:
        f.write(r'''// Auto-generated by gen_xcvm.py

#include <common/log.h>
#include <runtime/xcvm_ops.h>

namespace oniku {
namespace runtime {

''')
        f.writelines(format_code(lines))
        f.write(r'''
}  // namespace runtime
}  // namespace oniku
''')


def make_proto_signature(op, inputs, outputs):
    args = ['XCProgramProto* program']
    for name in outputs:
        args.append(f'int {name}')
    for typ, name in inputs:
        if typ == ARRAY or typ == INT:
            args.append(f'int {name}')
        elif typ == FLOAT:
            args.append(f'float {name}')
        elif typ == STRING:
            args.append(f'const std::string& {name}')
        elif typ == INTS:
            args.append(f'const std::vector<int>& {name}')
        else:
            raise RuntimeError('Unknown type: %s' % typ)
    args = ', '.join(args)
    return f'void Add{op}Op({args})'


def gen_xcvm_proto_util_h():
    lines = []
    for op, inputs, outputs in XC_OPS:
        signature = make_proto_signature(op, inputs, outputs)
        lines.append(signature + ';')

    with open('xcvm_proto_util.h', 'w') as f:
        f.write(r'''// Auto-generated by gen_xcvm.py

#include <runtime/xcvm.pb.h>

namespace oniku {
namespace runtime {

''')
        f.writelines(format_code(lines))
        f.write(r'''
}  // namespace runtime
}  // namespace oniku
''')


def gen_xcvm_proto_util_cc():
    lines = []
    for op, inputs, outputs in XC_OPS:
        signature = make_proto_signature(op, inputs, outputs)
        lines.append(signature + ' {')

        lines.append('XCInstructionProto* inst = program->add_instructions();')
        lines.append(f'inst->set_op(XCInstructionProto::{op});')

        for typ, name in inputs:
            lines.append('{')
            lines.append('XCValueProto* input_proto = inst->add_inputs();')
            lines.append(f'input_proto->set_type(XCValueProto::{typ});')
            if typ == ARRAY:
                lines.append(f'input_proto->set_array({name});')
            elif typ == INT:
                lines.append(f'input_proto->set_i({name});')
            elif typ == FLOAT:
                lines.append(f'input_proto->set_f({name});')
            elif typ == STRING:
                lines.append(f'input_proto->set_s({name});')
            elif typ == INTS:
                lines.append(f'for (int v : {name}) input_proto->add_ints(v);')
            else:
                raise RuntimeError('Unknown type: %s' % typ)
            lines.append('}')

        for name in outputs:
            lines.append(f'inst->add_outputs({name});')

        lines.append('}')

    with open('xcvm_proto_util.cc', 'w') as f:
        f.write(r'''// Auto-generated by gen_xcvm.py

#include <runtime/xcvm.pb.h>

namespace oniku {
namespace runtime {

''')
        f.writelines(format_code(lines))
        f.write(r'''
}  // namespace runtime
}  // namespace oniku
''')


gen_xcvm_proto()
gen_xcvm_ops_h()
gen_init_xcvm_ops_cc()
gen_xcvm_proto_util_h()
gen_xcvm_proto_util_cc()
