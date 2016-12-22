// ----------------------------------------------------------------------------
//  OgmaNeo
//  Copyright(c) 2016 Ogma Intelligent Systems Corp. All rights reserved.
//
//  This copy of OgmaNeo is licensed to you under the terms described
//  in the OGMANEO_LICENSE.md file included in this distribution.
// ----------------------------------------------------------------------------

#include "SparseFeaturesChunk.h"

using namespace ogmaneo;

SparseFeaturesChunk::SparseFeaturesChunk(ComputeSystem &cs, ComputeProgram &sfcProgram,
    const std::vector<VisibleLayerDesc> &visibleLayerDescs, cl_int2 hiddenSize,
    cl_int2 chunkSize,
    int numSamples,
    cl_float2 initWeightRange,
    std::mt19937 &rng)
{
    cl_float4 zeroColor = { 0.0f, 0.0f, 0.0f, 0.0f };

    _type = SparseFeaturesType::_chunk;

    _visibleLayerDescs = visibleLayerDescs;

    _hiddenSize = hiddenSize;

    _chunkSize = chunkSize;

    _numSamples = numSamples;

    cl::array<cl::size_type, 3> zeroOrigin = { 0, 0, 0 };
    cl::array<cl::size_type, 3> hiddenRegion = { static_cast<cl_uint>(_hiddenSize.x), static_cast<cl_uint>(_hiddenSize.y), 1 };

    _visibleLayers.resize(_visibleLayerDescs.size());

    cl::Kernel randomUniform2DKernel = cl::Kernel(sfcProgram.getProgram(), "randomUniform2D");
    cl::Kernel randomUniform3DKernel = cl::Kernel(sfcProgram.getProgram(), "randomUniform3D");

    int chunksInX = static_cast<int>(std::ceil(static_cast<float>(_hiddenSize.x) / static_cast<float>(_chunkSize.x)));
    int chunksInY = static_cast<int>(std::ceil(static_cast<float>(_hiddenSize.y) / static_cast<float>(_chunkSize.y)));

    // Create layers
    for (int vli = 0; vli < _visibleLayers.size(); vli++) {
        VisibleLayer &vl = _visibleLayers[vli];
        VisibleLayerDesc &vld = _visibleLayerDescs[vli];

        vl._hiddenToVisible = cl_float2{ static_cast<float>(vld._size.x) / static_cast<float>(_hiddenSize.x),
            static_cast<float>(vld._size.y) / static_cast<float>(_hiddenSize.y)
        };

        vl._visibleToHidden = cl_float2{ static_cast<float>(_hiddenSize.x) / static_cast<float>(vld._size.x),
            static_cast<float>(_hiddenSize.y) / static_cast<float>(vld._size.y)
        };

        vl._chunkToVisible = cl_float2{ static_cast<float>(vld._size.x) / static_cast<float>(chunksInX),
            static_cast<float>(vld._size.y) / static_cast<float>(chunksInY)
        };

        vl._reverseRadii = cl_int2{ static_cast<cl_int>(std::ceil(vl._visibleToHidden.x * vld._radius) + 1),
            static_cast<cl_int>(std::ceil(vl._visibleToHidden.y * vld._radius) + 1)
        };

        {
            int weightDiam = vld._radius * 2 + 1;

            int numWeights = weightDiam * weightDiam * _numSamples;

            cl_int3 weightsSize = { _hiddenSize.x, _hiddenSize.y, numWeights };

            vl._weights = createDoubleBuffer3D(cs, weightsSize, CL_R, CL_FLOAT);

            randomUniform(vl._weights[_back], cs, randomUniform3DKernel, weightsSize, initWeightRange, rng);
        }

        vl._derivedInput = createDoubleBuffer2D(cs, vld._size, CL_RG, CL_FLOAT);
        cs.getQueue().enqueueFillImage(vl._derivedInput[_back], zeroColor, zeroOrigin, { static_cast<cl::size_type>(vld._size.x), static_cast<cl::size_type>(vld._size.y), 1 });

        vl._samples = createDoubleBuffer3D(cs, { vld._size.x, vld._size.y, numSamples }, CL_R, CL_FLOAT);
        cs.getQueue().enqueueFillImage(vl._samples[_back], zeroColor, zeroOrigin, { static_cast<cl::size_type>(vld._size.x), static_cast<cl::size_type>(vld._size.y), static_cast<cl::size_type>(numSamples) });
    }

    // Hidden state data
    _hiddenStates = createDoubleBuffer2D(cs, _hiddenSize, CL_R, CL_FLOAT);
    _hiddenActivations = createDoubleBuffer2D(cs, _hiddenSize, CL_R, CL_FLOAT);

    _chunkWinners = createDoubleBuffer2D(cs, { chunksInX, chunksInY }, CL_RG, CL_FLOAT);

    _hiddenSummationTemp = createDoubleBuffer2D(cs, _hiddenSize, CL_R, CL_FLOAT);

    cs.getQueue().enqueueFillImage(_hiddenStates[_back], cl_float4{ 0.0f, 1.0f, 0.0f, 0.0f }, zeroOrigin, hiddenRegion);
    cs.getQueue().enqueueFillImage(_hiddenActivations[_back], cl_float4{ 0.0f, 0.0f, 0.0f, 0.0f }, zeroOrigin, hiddenRegion);

    // Create kernels
    _addSampleKernel = cl::Kernel(sfcProgram.getProgram(), "sfcAddSample");
    _stimulusKernel = cl::Kernel(sfcProgram.getProgram(), "sfcStimulus");
    _activateKernel = cl::Kernel(sfcProgram.getProgram(), "sfcActivate");
    _inhibitKernel = cl::Kernel(sfcProgram.getProgram(), "sfcInhibit");
    _inhibitOtherKernel = cl::Kernel(sfcProgram.getProgram(), "sfcInhibitOther");
    _learnWeightsKernel = cl::Kernel(sfcProgram.getProgram(), "sfcLearnWeights");
    _deriveInputsKernel = cl::Kernel(sfcProgram.getProgram(), "sfcDeriveInputs");
}

void SparseFeaturesChunk::activate(ComputeSystem &cs, const std::vector<cl::Image2D> &visibleStates, const cl::Image2D &predictionsPrev, std::mt19937 &rng) {
    cl::array<cl::size_type, 3> zeroOrigin = { 0, 0, 0 };
    cl::array<cl::size_type, 3> hiddenRegion = { static_cast<cl_uint>(_hiddenSize.x), static_cast<cl_uint>(_hiddenSize.y), 1 };

    // Start by clearing stimulus summation buffer to biases
    cs.getQueue().enqueueFillImage(_hiddenSummationTemp[_back], cl_float4{ 0.0f, 0.0f, 0.0f, 0.0f }, zeroOrigin, hiddenRegion);
    //cs.getQueue().enqueueCopyImage(_hiddenBiases[_back], _hiddenSummationTemp[_back], zeroOrigin, zeroOrigin, hiddenRegion);

    // Find up stimulus
    for (int vli = 0; vli < _visibleLayers.size(); vli++) {
        VisibleLayer &vl = _visibleLayers[vli];
        VisibleLayerDesc &vld = _visibleLayerDescs[vli];

        {
            int argIndex = 0;

            _deriveInputsKernel.setArg(argIndex++, visibleStates[vli]);
            _deriveInputsKernel.setArg(argIndex++, vl._derivedInput[_back]);
            _deriveInputsKernel.setArg(argIndex++, vl._derivedInput[_front]);
            _deriveInputsKernel.setArg(argIndex++, vld._lambda);

            cs.getQueue().enqueueNDRangeKernel(_deriveInputsKernel, cl::NullRange, cl::NDRange(vld._size.x, vld._size.y));
        }

        // Add sample
        {
            int argIndex = 0;

            _addSampleKernel.setArg(argIndex++, vl._derivedInput[_front]);
            _addSampleKernel.setArg(argIndex++, vl._samples[_back]);
            _addSampleKernel.setArg(argIndex++, vl._samples[_front]);
            _addSampleKernel.setArg(argIndex++, _numSamples);

            cs.getQueue().enqueueNDRangeKernel(_addSampleKernel, cl::NullRange, cl::NDRange(vld._size.x, vld._size.y));
        }

        {
            int argIndex = 0;

            _stimulusKernel.setArg(argIndex++, vl._samples[_front]);
            _stimulusKernel.setArg(argIndex++, _hiddenSummationTemp[_back]);
            _stimulusKernel.setArg(argIndex++, _hiddenSummationTemp[_front]);
            _stimulusKernel.setArg(argIndex++, vl._weights[_back]);
            _stimulusKernel.setArg(argIndex++, vld._size);
            _stimulusKernel.setArg(argIndex++, vl._chunkToVisible);
            _stimulusKernel.setArg(argIndex++, _chunkSize);
            _stimulusKernel.setArg(argIndex++, vld._radius);
            _stimulusKernel.setArg(argIndex++, _numSamples);
            _stimulusKernel.setArg(argIndex++, vld._ignoreMiddle);

            cs.getQueue().enqueueNDRangeKernel(_stimulusKernel, cl::NullRange, cl::NDRange(_hiddenSize.x, _hiddenSize.y));
        }

        // Swap buffers
        std::swap(_hiddenSummationTemp[_front], _hiddenSummationTemp[_back]);
    }

    // Activate
    {
        int argIndex = 0;

        _activateKernel.setArg(argIndex++, _hiddenSummationTemp[_back]);
        _activateKernel.setArg(argIndex++, _hiddenStates[_back]);
        _activateKernel.setArg(argIndex++, _hiddenActivations[_front]);

        cs.getQueue().enqueueNDRangeKernel(_activateKernel, cl::NullRange, cl::NDRange(_hiddenSize.x, _hiddenSize.y));
    }

    // Inhibit
    {
        int chunksInX = static_cast<int>(std::ceil(static_cast<float>(_hiddenSize.x) / static_cast<float>(_chunkSize.x)));
        int chunksInY = static_cast<int>(std::ceil(static_cast<float>(_hiddenSize.y) / static_cast<float>(_chunkSize.y)));

        int argIndex = 0;

        _inhibitKernel.setArg(argIndex++, _hiddenActivations[_front]);
        _inhibitKernel.setArg(argIndex++, _hiddenStates[_front]);
        _inhibitKernel.setArg(argIndex++, _chunkWinners[_front]);
        _inhibitKernel.setArg(argIndex++, _hiddenSize);
        _inhibitKernel.setArg(argIndex++, _chunkSize);

        cs.getQueue().enqueueNDRangeKernel(_inhibitKernel, cl::NullRange, cl::NDRange(chunksInX, chunksInY));
    }
}

void SparseFeaturesChunk::stepEnd(ComputeSystem &cs) {
    cl::array<cl::size_type, 3> zeroOrigin = { 0, 0, 0 };
    cl::array<cl::size_type, 3> hiddenRegion = { static_cast<cl_uint>(_hiddenSize.x), static_cast<cl_uint>(_hiddenSize.y), 1 };

    std::swap(_hiddenStates[_front], _hiddenStates[_back]);
    std::swap(_hiddenActivations[_front], _hiddenActivations[_back]);
    std::swap(_chunkWinners[_front], _chunkWinners[_back]);

    // Swap buffers
    for (int vli = 0; vli < _visibleLayers.size(); vli++) {
        VisibleLayer &vl = _visibleLayers[vli];
        VisibleLayerDesc &vld = _visibleLayerDescs[vli];

        std::swap(vl._derivedInput[_front], vl._derivedInput[_back]);
        std::swap(vl._samples[_front], vl._samples[_back]);
    }
}

void SparseFeaturesChunk::learn(ComputeSystem &cs, const cl::Image2D &predictionsPrev, std::mt19937 &rng) {
    // Learn weights
    for (int vli = 0; vli < _visibleLayers.size(); vli++) {
        VisibleLayer &vl = _visibleLayers[vli];
        VisibleLayerDesc &vld = _visibleLayerDescs[vli];

        // Weight update
        {
            int argIndex = 0;

            _learnWeightsKernel.setArg(argIndex++, _chunkWinners[_front]);
            _learnWeightsKernel.setArg(argIndex++, _chunkWinners[_back]);
            _learnWeightsKernel.setArg(argIndex++, vl._samples[_front]);
            _learnWeightsKernel.setArg(argIndex++, vl._weights[_back]);
            _learnWeightsKernel.setArg(argIndex++, vl._weights[_front]);
            _learnWeightsKernel.setArg(argIndex++, _hiddenSize);
            _learnWeightsKernel.setArg(argIndex++, vld._size);
            _learnWeightsKernel.setArg(argIndex++, vl._chunkToVisible);
            _learnWeightsKernel.setArg(argIndex++, _chunkSize);
            _learnWeightsKernel.setArg(argIndex++, vld._radius);
            _learnWeightsKernel.setArg(argIndex++, vld._weightAlpha);
            _learnWeightsKernel.setArg(argIndex++, _numSamples);

            cs.getQueue().enqueueNDRangeKernel(_learnWeightsKernel, cl::NullRange, cl::NDRange(_hiddenSize.x, _hiddenSize.y));
        }

        std::swap(vl._weights[_front], vl._weights[_back]);
    }
}

void SparseFeaturesChunk::inhibit(ComputeSystem &cs, const cl::Image2D &activations, cl::Image2D &states, std::mt19937 &rng) {
    // Inhibit
    {
        int chunksInX = static_cast<int>(std::ceil(static_cast<float>(_hiddenSize.x) / static_cast<float>(_chunkSize.x)));
        int chunksInY = static_cast<int>(std::ceil(static_cast<float>(_hiddenSize.y) / static_cast<float>(_chunkSize.y)));

        int argIndex = 0;

        _inhibitOtherKernel.setArg(argIndex++, activations);
        _inhibitOtherKernel.setArg(argIndex++, states);
        _inhibitOtherKernel.setArg(argIndex++, _hiddenSize);
        _inhibitOtherKernel.setArg(argIndex++, _chunkSize);

        cs.getQueue().enqueueNDRangeKernel(_inhibitOtherKernel, cl::NullRange, cl::NDRange(chunksInX, chunksInY));
    }
}

void SparseFeaturesChunk::clearMemory(ComputeSystem &cs) {
    cl_float4 zeroColor = { 0.0f, 0.0f, 0.0f, 0.0f };

    cl::array<cl::size_type, 3> zeroOrigin = { 0, 0, 0 };
    cl::array<cl::size_type, 3> hiddenRegion = { static_cast<cl_uint>(_hiddenSize.x), static_cast<cl_uint>(_hiddenSize.y), 1 };

    // Clear buffers
    cs.getQueue().enqueueFillImage(_hiddenStates[_back], zeroColor, zeroOrigin, hiddenRegion);
    cs.getQueue().enqueueFillImage(_hiddenActivations[_back], zeroColor, zeroOrigin, hiddenRegion);

    for (int vli = 0; vli < _visibleLayers.size(); vli++) {
        VisibleLayer &vl = _visibleLayers[vli];
        VisibleLayerDesc &vld = _visibleLayerDescs[vli];

        cs.getQueue().enqueueFillImage(vl._derivedInput[_back], zeroColor, zeroOrigin, { static_cast<cl::size_type>(vld._size.x), static_cast<cl::size_type>(vld._size.y), 1 });
        cs.getQueue().enqueueFillImage(vl._samples[_back], zeroColor, zeroOrigin, { static_cast<cl::size_type>(vld._size.x), static_cast<cl::size_type>(vld._size.y), static_cast<cl::size_type>(_numSamples) });
    }
}

void SparseFeaturesChunk::VisibleLayerDesc::load(const schemas::VisibleChunkLayerDesc* fbVisibleChunkLayerDesc, ComputeSystem &cs) {
    _size = cl_int2{ fbVisibleChunkLayerDesc->_size().x(), fbVisibleChunkLayerDesc->_size().y() };
    _radius = fbVisibleChunkLayerDesc->_radius();
    _ignoreMiddle = fbVisibleChunkLayerDesc->_ignoreMiddle();
    _weightAlpha = fbVisibleChunkLayerDesc->_weightAlpha();
    _lambda = fbVisibleChunkLayerDesc->_lambda();
}

schemas::VisibleChunkLayerDesc SparseFeaturesChunk::VisibleLayerDesc::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    schemas::int2 size(_size.x, _size.y);
    return schemas::VisibleChunkLayerDesc(size, _radius, _ignoreMiddle, _weightAlpha, _lambda);
}

void SparseFeaturesChunk::VisibleLayer::load(const schemas::VisibleChunkLayer* fbVisibleChunkLayer, ComputeSystem &cs) {
    ogmaneo::load(_derivedInput, fbVisibleChunkLayer->_derivedInput(), cs);
    ogmaneo::load(_samples, fbVisibleChunkLayer->_samples(), cs);
    ogmaneo::load(_weights, fbVisibleChunkLayer->_weights(), cs);
    _hiddenToVisible = cl_float2{ fbVisibleChunkLayer->_hiddenToVisible()->x(), fbVisibleChunkLayer->_hiddenToVisible()->y() };
    _visibleToHidden = cl_float2{ fbVisibleChunkLayer->_visibleToHidden()->x(), fbVisibleChunkLayer->_visibleToHidden()->y() };
    _reverseRadii = cl_int2{ fbVisibleChunkLayer->_reverseRadii()->x(), fbVisibleChunkLayer->_reverseRadii()->y() };
}

flatbuffers::Offset<schemas::VisibleChunkLayer> SparseFeaturesChunk::VisibleLayer::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    schemas::float2 hiddenToVisible(_hiddenToVisible.x, _hiddenToVisible.y);
    schemas::float2 visibleToHidden(_visibleToHidden.x, _visibleToHidden.y);
    schemas::int2 reverseRadii(_reverseRadii.x, _reverseRadii.y);

    return schemas::CreateVisibleChunkLayer(builder,
        ogmaneo::save(_derivedInput, builder, cs),
        ogmaneo::save(_samples, builder, cs),
        ogmaneo::save(_weights, builder, cs),
        &hiddenToVisible, &visibleToHidden, &reverseRadii);
}

void SparseFeaturesChunk::SparseFeaturesChunkDesc::load(const schemas::SparseFeaturesChunkDesc* fbSparseFeaturesChunkDesc, ComputeSystem &cs) {
    assert(_hiddenSize.x == fbSparseFeaturesChunkDesc->_hiddenSize()->x());
    assert(_hiddenSize.y == fbSparseFeaturesChunkDesc->_hiddenSize()->y());
    assert(_visibleLayerDescs.size() == fbSparseFeaturesChunkDesc->_visibleLayerDescs()->Length());

    _hiddenSize = cl_int2{ fbSparseFeaturesChunkDesc->_hiddenSize()->x(), fbSparseFeaturesChunkDesc->_hiddenSize()->y() };
    _chunkSize = cl_int2{ fbSparseFeaturesChunkDesc->_chunkSize()->x(), fbSparseFeaturesChunkDesc->_chunkSize()->y() };
    _initWeightRange = cl_float2{ fbSparseFeaturesChunkDesc->_initWeightRange()->x(), fbSparseFeaturesChunkDesc->_initWeightRange()->y() };

    for (flatbuffers::uoffset_t i = 0; i < fbSparseFeaturesChunkDesc->_visibleLayerDescs()->Length(); i++) {
        _visibleLayerDescs[i].load(fbSparseFeaturesChunkDesc->_visibleLayerDescs()->Get(i), cs);
    }
}

flatbuffers::Offset<schemas::SparseFeaturesChunkDesc> SparseFeaturesChunk::SparseFeaturesChunkDesc::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    schemas::int2 hiddenSize(_hiddenSize.x, _hiddenSize.y);
    schemas::int2 chunkSize(_chunkSize.x, _chunkSize.y);
    schemas::float2 initWeightRange(_initWeightRange.x, _initWeightRange.y);

    std::vector<schemas::VisibleChunkLayerDesc> visibleLayerDescs;
    for (VisibleLayerDesc layerDesc : _visibleLayerDescs)
        visibleLayerDescs.push_back(layerDesc.save(builder, cs));

    return schemas::CreateSparseFeaturesChunkDesc(builder,
        &hiddenSize, &chunkSize, _numSamples,
        &initWeightRange, builder.CreateVectorOfStructs(visibleLayerDescs));
}

void SparseFeaturesChunk::load(const schemas::SparseFeatures* fbSparseFeatures, ComputeSystem &cs) {
    assert(fbSparseFeatures->_sf_type() == schemas::SparseFeaturesType::SparseFeaturesType_SparseFeaturesChunk);
    schemas::SparseFeaturesChunk* fbSparseFeaturesChunk =
        (schemas::SparseFeaturesChunk*)(fbSparseFeatures->_sf());

    assert(_hiddenSize.x == fbSparseFeaturesChunk->_hiddenSize()->x());
    assert(_hiddenSize.y == fbSparseFeaturesChunk->_hiddenSize()->y());
    assert(_visibleLayerDescs.size() == fbSparseFeaturesChunk->_visibleLayerDescs()->Length());
    assert(_visibleLayers.size() == fbSparseFeaturesChunk->_visibleLayers()->Length());

    _hiddenSize = cl_int2{ fbSparseFeaturesChunk->_hiddenSize()->x(), fbSparseFeaturesChunk->_hiddenSize()->y() };
    _chunkSize = cl_int2{ fbSparseFeaturesChunk->_chunkSize()->x(), fbSparseFeaturesChunk->_chunkSize()->y() };

    _numSamples = fbSparseFeaturesChunk->_numSamples();

    ogmaneo::load(_hiddenStates, fbSparseFeaturesChunk->_hiddenStates(), cs);
    ogmaneo::load(_hiddenActivations, fbSparseFeaturesChunk->_hiddenActivations(), cs);
    ogmaneo::load(_chunkWinners, fbSparseFeaturesChunk->_chunkWinners(), cs);
    ogmaneo::load(_hiddenSummationTemp, fbSparseFeaturesChunk->_hiddenSummationTemp(), cs);

    for (flatbuffers::uoffset_t i = 0; i < fbSparseFeaturesChunk->_visibleLayerDescs()->Length(); i++) {
        _visibleLayerDescs[i].load(fbSparseFeaturesChunk->_visibleLayerDescs()->Get(i), cs);
    }

    for (flatbuffers::uoffset_t i = 0; i < fbSparseFeaturesChunk->_visibleLayers()->Length(); i++) {
        _visibleLayers[i].load(fbSparseFeaturesChunk->_visibleLayers()->Get(i), cs);
    }
}

flatbuffers::Offset<schemas::SparseFeatures> SparseFeaturesChunk::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    schemas::int2 hiddenSize(_hiddenSize.x, _hiddenSize.y);
    schemas::int2 chunkSize(_chunkSize.x, _chunkSize.y);

    std::vector<schemas::VisibleChunkLayerDesc> visibleLayerDescs;
    for (VisibleLayerDesc layerDesc : _visibleLayerDescs)
        visibleLayerDescs.push_back(layerDesc.save(builder, cs));

    std::vector<flatbuffers::Offset<schemas::VisibleChunkLayer>> visibleLayers;
    for (VisibleLayer layer : _visibleLayers)
        visibleLayers.push_back(layer.save(builder, cs));

    flatbuffers::Offset<schemas::SparseFeaturesChunk> sf = schemas::CreateSparseFeaturesChunk(builder,
        ogmaneo::save(_hiddenStates, builder, cs),
        ogmaneo::save(_hiddenActivations, builder, cs),
        ogmaneo::save(_chunkWinners, builder, cs),
        &hiddenSize, &chunkSize, _numSamples,
        ogmaneo::save(_hiddenSummationTemp, builder, cs),
        builder.CreateVectorOfStructs(visibleLayerDescs),
        builder.CreateVector(visibleLayers));

    return schemas::CreateSparseFeatures(builder,
        schemas::SparseFeaturesType::SparseFeaturesType_SparseFeaturesChunk, sf.Union());
}
