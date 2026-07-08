#include "NAMProcessor.h"

NAMProcessor::NAMProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

NAMProcessor::~NAMProcessor()
{
    const juce::ScopedLock sl(modelLock);
#if SLOPSMITH_NAM_SUPPORT
    model.reset();
    pendingModel.reset();
#endif
}

bool NAMProcessor::loadModel(const juce::File& namFile)
{
#if SLOPSMITH_NAM_SUPPORT
    if (!namFile.existsAsFile()) return false;

    try
    {
        std::filesystem::path namPath(namFile.getFullPathName().toStdString());
        auto newModel = nam::get_dsp(namPath);
        if (!newModel) return false;

        // Prepare the new model at current sample rate
        newModel->Reset(currentSampleRate, currentBlockSize);

        // Swap atomically
        {
            const juce::ScopedLock sl(modelLock);
            model = std::move(newModel);
        }

        currentModelName = namFile.getFileNameWithoutExtension();
        currentModelPath = namFile.getFullPathName();
        modelLoaded.store(true);
        return true;
    }
    catch (const std::exception& e)
    {
        DBG("NAM load error: " + juce::String(e.what()));
        return false;
    }
#else
    juce::ignoreUnused(namFile);
    return false;
#endif
}

void NAMProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    monoBuffer.resize((size_t)samplesPerBlock);

#if SLOPSMITH_NAM_SUPPORT
    const juce::ScopedLock sl(modelLock);
    if (model)
        model->Reset(sampleRate, samplesPerBlock);
#endif
}

void NAMProcessor::releaseResources()
{
    monoBuffer.clear();
}

void NAMProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
#if SLOPSMITH_NAM_SUPPORT
    const juce::ScopedLock sl(modelLock);
    if (!model)
        return;

    int numSamples = buffer.getNumSamples();
    int numChannels = buffer.getNumChannels();

    // Pre-allocated buffers (avoid heap allocation in audio callback)
    thread_local std::vector<double> inputBuf;
    thread_local std::vector<double> outputBuf;
    inputBuf.resize((size_t)numSamples);
    outputBuf.resize((size_t)numSamples);

    // Mix input to mono with input level
    float inLevel = inputLevel.load();
    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            sum += buffer.getSample(ch, i);
        inputBuf[(size_t)i] = (double)((sum / (float)numChannels) * inLevel);
    }

    // Process through NAM model (double** in, double** out), in slices no larger
    // than the block size the model was Reset() with. The NAM core pre-allocates
    // its conv ring/output buffers to that maxBufferSize and only asserts (a
    // release-build no-op) on larger blocks — one oversized block (WASAPI shared
    // mode delivers them right after a device start) writes past those buffers
    // and leaves the conv ring misaligned, garbling ALL subsequent audio until
    // the next Reset().
    const int maxChunk = currentBlockSize > 0 ? currentBlockSize : numSamples;
    for (int offset = 0; offset < numSamples; offset += maxChunk)
    {
        const int chunk = juce::jmin(maxChunk, numSamples - offset);
        double* inPtr = inputBuf.data() + offset;
        double* outPtr = outputBuf.data() + offset;
        double** inPtrs = &inPtr;
        double** outPtrs = &outPtr;
        model->process(inPtrs, outPtrs, chunk);
    }

    // Copy mono result to all output channels with output level
    float outLevel = outputLevel.load();
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample(ch, i, (float)outputBuf[(size_t)i] * outLevel);
#else
    juce::ignoreUnused(buffer);
#endif
}

void NAMProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = new juce::DynamicObject();
    state->setProperty("modelPath", currentModelPath);
    state->setProperty("inputLevel", (double)inputLevel.load());
    state->setProperty("outputLevel", (double)outputLevel.load());
    auto json = juce::JSON::toString(juce::var(state));
    destData.append(json.toRawUTF8(), json.getNumBytesAsUTF8());
}

void NAMProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto json = juce::String::fromUTF8((const char*)data, sizeInBytes);
    auto parsed = juce::JSON::parse(json);

    if (auto* obj = parsed.getDynamicObject())
    {
        auto path = obj->getProperty("modelPath").toString();
        if (path.isNotEmpty())
            loadModel(juce::File(path));

        if (obj->hasProperty("inputLevel"))
            inputLevel.store((float)(double)obj->getProperty("inputLevel"));
        if (obj->hasProperty("outputLevel"))
            outputLevel.store((float)(double)obj->getProperty("outputLevel"));
    }
}
