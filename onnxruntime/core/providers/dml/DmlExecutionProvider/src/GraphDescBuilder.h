// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "MLOperatorAuthorImpl.h"

namespace Dml
{
    struct GraphNodeProperties
    {
        std::shared_ptr<const winrt::Windows::AI::MachineLearning::implementation::GraphNodeFactoryRegistration>
        graphNodeFactoryRegistration;

        // These are currently passed from the partitioning step since the only DML operators current 
        // supporting graph nodes don't customize the order of edges or shapes, other than coercing
        // dimension count.  This will change as the supported set of operators as graph nodes increases.
        winrt::Windows::AI::MachineLearning::implementation::EdgeShapes inputShapes;
        winrt::Windows::AI::MachineLearning::implementation::EdgeShapes outputShapes;
    };

    namespace GraphDescBuilder
    {
        // Gets a unique name for the node which survives recreation and graph manipulations between the point
        // that graph partitioning occurs and kernel creation happens
        const std::string& GetUniqueNodeName(const onnxruntime::Node& node);

        struct NodeInfo
        {
            Microsoft::WRL::ComPtr<IDMLOperator> op;
        };

        struct GraphDesc
        {
            std::vector<NodeInfo> nodes;
            std::vector<DML_PREVIEW_INPUT_GRAPH_EDGE> inputEdges;
            std::vector<DML_PREVIEW_OUTPUT_GRAPH_EDGE> outputEdges;
            std::vector<DML_PREVIEW_INTERMEDIATE_GRAPH_EDGE> intermediateEdges;
            bool reuseCommandList;
        };

        GraphDesc BuildGraphDesc(
            const onnxruntime::OpKernelInfo& kernelInfo,
            gsl::span<const uint8_t> isConstGpuGraphInput,
            std::unordered_map<std::string, onnx::TensorProto>& transferredInitializerMap,
            const onnxruntime::Graph& graph,
            const onnxruntime::ConstPointerContainer<std::vector<onnxruntime::NodeArg*>>& fusedNodeInputDefs,
            const onnxruntime::ConstPointerContainer<std::vector<onnxruntime::NodeArg*>>& fusedNodeOutputDefs,
            const std::unordered_map<std::string, GraphNodeProperties>& graphNodePropertyMap,
            IDMLDevice* device,
            const void* executionHandle);
    }
}