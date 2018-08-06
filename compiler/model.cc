#include "model.h"

#include <onnx/onnx.pb.h>

#include <common/log.h>
#include <compiler/graph.h>
#include <compiler/serializer_util.h>

namespace oniku {

Model::Model(const onnx::ModelProto& xmodel)
    : ir_version_(xmodel.ir_version()),
      opset_import_(xmodel.opset_import().begin(), xmodel.opset_import().end()),
      producer_name_(xmodel.producer_name()),
      producer_version_(xmodel.producer_version()),
      domain_(xmodel.domain()),
      model_version_(xmodel.model_version()),
      doc_string_(xmodel.doc_string()),
      graph_(new Graph(xmodel.graph())) {
    for (const onnx::StringStringEntryProto& metadata : xmodel.metadata_props()) {
        CHECK(metadata_props_.emplace(metadata.key(), metadata.value()).second) << "Duplicated metadata key: " << metadata.key();
    }
}

Model::~Model() {}

void Model::ToONNX(onnx::ModelProto* xmodel) const {
    SET_PRIM(xmodel, ir_version);
    for (const onnx::OperatorSetIdProto& opset : opset_import_) {
        *xmodel->add_opset_import() = opset;
    }
    SET_STRING(xmodel, producer_name);
    SET_STRING(xmodel, producer_version);
    SET_STRING(xmodel, domain);
    SET_STRING(xmodel, doc_string);
    graph_->ToONNX(xmodel->mutable_graph());
    for (const auto& p : metadata_props_) {
        onnx::StringStringEntryProto* metadata = xmodel->add_metadata_props();
        metadata->set_key(p.first);
        metadata->set_value(p.second);
    }
}

}  // namespace oniku
