#pragma once

#include <map>
#include <string>

#include <onnx/onnx-ml.pb.h>

#include <chainerx/array.h>

namespace oniku {
namespace runtime {

typedef std::map<std::string, chainerx::Array> InOuts;

InOuts RunGraph(const InOuts& inputs, bool use_trace);

chainerx::Array GetOrDie(const InOuts& m, std::string name);
void SetOrDie(InOuts& m, std::string name, chainerx::Array& a);

// TODO(hamaji): Investigate xChainer's BatchNorm.
chainerx::Array BatchNormONNX(
        chainerx::Array x, chainerx::Array s, chainerx::Array bias, chainerx::Array mean, chainerx::Array var, float epsilon);

chainerx::Shape ArrayToShape(const chainerx::Array& a);

chainerx::Array ShapeToArray(const chainerx::Shape& s);

chainerx::Array MakeArrayFromONNX(const onnx::TensorProto& xtensor);

chainerx::Array MakeArray(chainerx::Dtype dtype, chainerx::Shape shape, const void* src);

chainerx::Array MakeScalarArray(float f);

chainerx::Array MakeHostArray(chainerx::Dtype dtype, chainerx::Shape shape, const void* src);

bool HasNan(const chainerx::Array& a);

bool HasInf(const chainerx::Array& a);

chainerx::Array Concat(const std::vector<chainerx::Array>& inputs, int axis);

std::vector<chainerx::Array> Split(const chainerx::Array& input, const std::vector<int64_t>& split, int axis);

chainerx::Array Pad(const std::vector<chainerx::Array>& inputs, int axis);

}  // namespace runtime
}  // namespace oniku
