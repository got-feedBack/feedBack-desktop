#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>

// Represents a single processor slot in the signal chain.
// Can hold a VST3/AU/LV2 plugin, NAM model, or IR loader.
struct ProcessorSlot
{
    enum class Type { VST, NAM, IR, Empty };

    Type type = Type::Empty;
    std::unique_ptr<juce::AudioProcessor> processor;
    juce::String name;
    juce::String path; // plugin file path, NAM model path, or IR file path
    bool bypassed = false;
    int id = 0;

    // Stereo routing (pan-only stereo, St-1).
    //   pan    : -1 = full left … 0 = centre … +1 = full right (constant-power),
    //            applied to this slot's output. 0 (default) = no-op → unchanged.
    //   branch : 0 = trunk (serial). >=1 = a parallel branch id. Slots sharing a
    //            branch id form one parallel path; all branches read the same
    //            pre-split signal and their (panned) outputs are summed at merge.
    //            With no slot in a branch the chain runs exactly as before.
    //   branchSrc : which channel of the split source this branch reads — 0 =
    //            both (stereo), 1 = Left only, 2 = Right only. Lets a stereo-out
    //            gear feed its L to one branch and R to another (St-2). Read from
    //            any slot in the branch; 0 = full stereo (default).
    float pan = 0.0f;
    int branch = 0;
    int branchSrc = 0;
    // Linear output gain applied right after the processor (and pan). Carries
    // the per-amp loudness trim / per-branch level; 1.0 (default) = no-op.
    float postGain = 1.0f;

    // For VST plugins — their state as base64 for preset save/load
    juce::MemoryBlock getState() const;
    void setState(const juce::MemoryBlock& state);
};

class SignalChain
{
public:
    SignalChain();
    ~SignalChain();

    void prepare(double sampleRate, int blockSize);
    void releaseResources();
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // Chain management
    int addProcessor(std::unique_ptr<juce::AudioProcessor> processor,
                     ProcessorSlot::Type type,
                     const juce::String& name,
                     const juce::String& path);
    void removeProcessor(int slotId);
    // Swap the processor in an existing slot IN PLACE — same slotId, position,
    // name, path, type and routing are preserved; only the underlying processor
    // changes. Used to promote an in-process VST3 to the out-of-process sandbox
    // when its editor is opened (an in-process editor is the Windows WndProc/Qt
    // crash path). Prepares the incoming processor under the SEH guard, then
    // swaps under the audio lock; the old processor is torn down off the lock.
    // Returns false if the slot is gone or the incoming processor faulted in
    // prepareToPlay (in which case the existing processor is left untouched).
    // The OLD slot name/path are preserved during the prepare/fault window (so a
    // fault is blocklisted against the right path); on SUCCESS, if newName/newPath
    // are non-empty they replace the slot's identity so getChainState()/preset
    // metadata reflect the swapped-in processor (used by replaceIR for cab swaps).
    bool replaceProcessor(int slotId, std::unique_ptr<juce::AudioProcessor> processor,
                          const juce::String& newName = {}, const juce::String& newPath = {});
    // Snapshot a slot's state for sandbox promotion, SAFELY. Runs hasEditor()
    // and getStateInformation() under the audio lock (so they can't race
    // process()'s processBlock on the same instance) and under the SEH/signal
    // guard (so a plugin that faults during the snapshot is contained +
    // blocklisted, not fatal — an unguarded getStateInformation on the very
    // plugins this promotion targets would reintroduce the editor crash on the
    // message thread). Returns true and fills `state` only for a non-sandboxed
    // in-process VST3 slot that actually has an editor; returns false (leaving
    // `state` empty) for a missing/non-VST/editor-less/already-sandboxed slot or
    // if the guarded calls faulted (processor released). A false result means
    // "not promotable" — the caller may safely open the editor in-process.
    bool captureVstStateForPromotion(int slotId, juce::MemoryBlock& state);
    void moveProcessor(int fromIndex, int toIndex);
    void setBypass(int slotId, bool bypassed);
    void setMultiBypass(const juce::Array<std::pair<int, bool>>& changes);
    // Stereo routing (St-1/St-2). pan: -1..+1. branch: 0 = trunk, >=1 = parallel
    // id. branchSrc: 0 = both, 1 = L, 2 = R (channel the branch reads from split).
    void setPan(int slotId, float pan);
    void setBranch(int slotId, int branch);
    void setBranchSrc(int slotId, int src);
    void setPostGain(int slotId, float gain);
    void clear();

    // Info
    int getNumSlots() const;
    const ProcessorSlot* getSlot(int slotId) const;
    juce::Array<const ProcessorSlot*> getAllSlots() const;
    // Current prepared playback format — used to prepare a processor that is
    // swapped in mid-session (replaceProcessor) at the same rate as the chain.
    double getCurrentSampleRate() const { return currentSampleRate; }
    int getCurrentBlockSize() const { return currentBlockSize; }

    // Parameters for a specific slot
    struct ParamInfo
    {
        int index;
        juce::String name;
        float value;
        juce::String label;
        juce::String text;
    };
    juce::Array<ParamInfo> getParameters(int slotId) const;
    void setParameter(int slotId, int paramIndex, float value);

    // Restore a processor's full state (a getStateInformation() blob) by slot
    // id. Used to re-apply per-slot VST state when the tone-switcher rebuilds
    // a chain processor-by-processor rather than via a whole-chain loadPreset.
    void setSlotState(int slotId, const juce::MemoryBlock& state);

    // Preset serialization
    juce::String savePreset() const;
    void loadPreset(const juce::String& json);

    // MIDI message injection (lock-free, called from N-API thread)
    void queueMidiMessage(int targetSlotId, const juce::MidiMessage& msg);

private:
    int findSlotIndex(int slotId) const;
    // process() body for one block of at most currentBlockSize samples. Caller
    // holds `lock`. Split out so process() can slice an oversized device block
    // (WASAPI shared mode delivers them after a device start) into chunks the
    // slots were actually prepared for.
    void processLocked(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    juce::OwnedArray<ProcessorSlot> slots;
    juce::CriticalSection lock;
    int nextSlotId = 1;

    double currentSampleRate = 48000.0;
    int currentBlockSize = 256;

    // Pre-allocated scratch for parallel-branch mixing (St-1). Sized in
    // prepare(); never resized on the audio thread. Only touched when the chain
    // actually has a parallel branch — the all-trunk path uses none of these.
    juce::AudioBuffer<float> splitScratch;   // snapshot of the pre-split signal
    juce::AudioBuffer<float> branchScratch;  // working buffer for one branch
    juce::AudioBuffer<float> accumScratch;   // summed branch outputs (merge bus)

    // Lock-free SPSC MIDI queue (N-API thread writes, audio thread reads)
    struct PendingMidiMessage { int targetSlotId = -1; juce::MidiMessage msg; };
    static constexpr int kMidiQueueSize = 64;
    std::array<PendingMidiMessage, kMidiQueueSize> midiRingBuffer;
    juce::AbstractFifo midiQueueFifo { kMidiQueueSize };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalChain)
};
