/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef NATIVEOBOE_NATIVEAUDIOCONTEXT_H
#define NATIVEOBOE_NATIVEAUDIOCONTEXT_H

#include <dlfcn.h>
#include <jni.h>
#include <thread>
#include <vector>

#include "common/OboeDebug.h"
#include "oboe/Oboe.h"

#include "AudioStreamGateway.h"
#include "flowgraph/ImpulseOscillator.h"
#include "flowgraph/ManyToMultiConverter.h"
#include "flowgraph/MonoToMultiConverter.h"
#include "flowgraph/SinkFloat.h"
#include "flowgraph/SinkI16.h"
#include "flowgraph/SineOscillator.h"
#include "flowgraph/SawtoothOscillator.h"

#include "FullDuplexEcho.h"
#include "FullDuplexGlitches.h"
#include "FullDuplexLatency.h"
#include "FullDuplexStream.h"
#include "InputStreamCallbackAnalyzer.h"
#include "MultiChannelRecording.h"
#include "OboeStreamCallbackProxy.h"
#include "PlayRecordingCallback.h"
#include "SawPingGenerator.h"

// These must match order in strings.xml and in StreamConfiguration.java
#define NATIVE_MODE_UNSPECIFIED  0
#define NATIVE_MODE_OPENSLES     1
#define NATIVE_MODE_AAUDIO       2

#define MAX_SINE_OSCILLATORS     8
#define AMPLITUDE_SINE           1.0
#define AMPLITUDE_SAWTOOTH       0.5
#define FREQUENCY_SAW_PING       800.0
#define AMPLITUDE_SAW_PING       0.8
#define AMPLITUDE_IMPULSE        0.7

#define NANOS_PER_MICROSECOND    ((int64_t) 1000)
#define NANOS_PER_MILLISECOND    (1000 * NANOS_PER_MICROSECOND)
#define NANOS_PER_SECOND         (1000 * NANOS_PER_MILLISECOND)

#define LIB_AAUDIO_NAME          "libaaudio.so"
#define FUNCTION_IS_MMAP         "AAudioStream_isMMapUsed"

/**
 * Abstract base class that corresponds to a test at the Java level.
 */
class ActivityContext {
public:

    ActivityContext() {}
    virtual ~ActivityContext() = default;

    oboe::AudioStream *getStream(int32_t streamIndex) {
        return mOboeStreams[streamIndex]; // TODO range check
    }

    virtual void configureBuilder(bool isInput, oboe::AudioStreamBuilder &builder);

    int open(jint nativeApi,
             jint sampleRate,
             jint channelCount,
             jint format,
             jint sharingMode,
             jint performanceMode,
             jint deviceId,
             jint sessionId,
             jint framesPerBurst,
             jboolean isInput);


    virtual void close(int32_t streamIndex);

    void printScheduler() {
        int scheduler = audioStreamGateway.getScheduler();
        LOGI("scheduler = 0x%08x, SCHED_FIFO = 0x%08X\n", scheduler, SCHED_FIFO);
    }

    virtual void configureForStart() {}

    oboe::Result start();

    oboe::Result pause();

    oboe::Result stopAllStreams();

    virtual oboe::Result stop() {
        return stopAllStreams();
    }

    virtual void setAmplitude(double amplitude) {}

    virtual oboe::Result startPlayback() {
        return oboe::Result::OK;
    }

    virtual oboe::Result stopPlayback() {
        return oboe::Result::OK;
    }

    virtual void runBlockingIO() {};

    static void threadCallback(ActivityContext *context) {
        LOGD("%s: called", __func__);
        context->runBlockingIO();
        LOGD("%s: exiting", __func__);
    }

    void stopBlockingIOThread() {
        if (dataThread != nullptr) {
            // stop a thread that runs in place of the callback
            threadEnabled.store(false); // ask thread to exit its loop
            dataThread->join();
            dataThread = nullptr;
        }
    }

    virtual double getPeakLevel(int index) {
        return 0.0;
    }

    virtual void setEnabled(bool enabled) {
    }

    bool isMMapUsed(int32_t streamIndex);

    int32_t getFramesPerBlock() {
        return (callbackSize == 0) ? mFramesPerBurst : callbackSize;
    }

    int64_t getCallbackCount() {
        return oboeCallbackProxy.getCallbackCount();
    }


    virtual void setChannelEnabled(int channelIndex, bool enabled) {}

    static bool   useCallback;
    static bool   callbackReturnStop;
    static int    callbackSize;


protected:
    oboe::AudioStream * getInputStream();
    oboe::AudioStream * getOutputStream();
    int32_t allocateStreamIndex();
    void freeStreamIndex(int32_t streamIndex);

    virtual void finishOpen(bool isInput, oboe::AudioStream *oboeStream) {}

    virtual oboe::Result startStreams() = 0;

    std::unique_ptr<float []>    dataBuffer{};

    AudioStreamGateway           audioStreamGateway;
    OboeStreamCallbackProxy      oboeCallbackProxy;

    static constexpr int         kMaxStreams = 8;
    oboe::AudioStream           *mOboeStreams[kMaxStreams]{};
    int32_t                      mFramesPerBurst = 0; // TODO per stream
    int32_t                      mChannelCount = 0; // TODO per stream
    int32_t                      mSampleRate = 0; // TODO per stream

    std::atomic<bool>            threadEnabled{false};
    std::thread                 *dataThread = nullptr;

    bool                       (*mAAudioStream_isMMap)(AAudioStream *stream) = nullptr;
    void                        *mLibHandle = nullptr;

private:
};

/**
 * Test a single input stream.
 */
class ActivityTestInput : public ActivityContext {
public:

    ActivityTestInput() {}
    virtual ~ActivityTestInput() = default;

    void configureForStart() override;

    double getPeakLevel(int index) override {
        return mInputAnalyzer.getPeakLevel(index);
    }

    void runBlockingIO() override;

    std::unique_ptr<MultiChannelRecording>  mRecording{};

    InputStreamCallbackAnalyzer  mInputAnalyzer;

protected:
    oboe::Result startStreams() override {
        return getInputStream()->requestStart();
    }

private:
};

/**
 * Record a configured input stream and play it back some simple way.
 */
class ActivityRecording : public ActivityTestInput {
public:

    ActivityRecording() {}
    virtual ~ActivityRecording() = default;

    oboe::Result stop() override {

        oboe::Result resultStopPlayback = stopPlayback();
        oboe::Result resultStopAudio = ActivityContext::stop();

        oboe::Result result = (resultStopPlayback != oboe::Result::OK)
                              ? resultStopPlayback
                              : resultStopAudio;
        return result;
    }

    oboe::Result startPlayback() override;

    oboe::Result stopPlayback() override;

    PlayRecordingCallback        mPlayRecordingCallback;
    oboe::AudioStream           *playbackStream = nullptr;

};

/**
 * Test a single output stream.
 */
class ActivityTestOutput : public ActivityContext {
public:
    ActivityTestOutput()
            : sineOscillators(MAX_SINE_OSCILLATORS)
            , sawtoothOscillators(MAX_SINE_OSCILLATORS) {}

    virtual ~ActivityTestOutput() = default;

    void close(int32_t streamIndex) override;

    oboe::Result startStreams() override {
        return getOutputStream()->start();
    }

    void configureForStart() override;

    virtual void configureStreamGateway();

    void runBlockingIO() override;

    void setAmplitude(double amplitude) override {
        LOGD("%s(%f)", __func__, amplitude);
        for (int i = 0; i < mChannelCount; i++) {
            sineOscillators[i].amplitude.setValue(amplitude);
            sawtoothOscillators[i].amplitude.setValue(amplitude);
        }
        impulseGenerator.amplitude.setValue(amplitude);
    }

    void setChannelEnabled(int channelIndex, bool enabled) override;

    // WARNING - must match order in strings.xml and OboeAudioOutputStream.java
    enum ToneType {
        SawPing = 0,
        Sine = 1,
        Impulse = 2,
        Sawtooth = 3
    };

protected:
    ToneType                     mToneType = ToneType::Sine;
    std::vector<SineOscillator>  sineOscillators;
    std::vector<SawtoothOscillator>  sawtoothOscillators;

    ImpulseOscillator            impulseGenerator;

    std::unique_ptr<ManyToMultiConverter>   manyToMulti;
    std::unique_ptr<MonoToMultiConverter>   monoToMulti;
    std::shared_ptr<flowgraph::SinkFloat>   mSinkFloat;
    std::shared_ptr<flowgraph::SinkI16>     mSinkI16;
};

/**
 * Generate a short beep with a very short attack.
 * This is used by Java to measure output latency.
 */
class ActivityTapToTone : public ActivityTestOutput {
public:
    ActivityTapToTone() {}
    virtual ~ActivityTapToTone() = default;

    void configureForStart() override;

    void setAmplitude(double amplitude) override {
        LOGD("%s(%f)", __func__, amplitude);
        ActivityTestOutput::setAmplitude(amplitude);
        sawPingGenerator.amplitude.setValue(amplitude);
    }

    virtual void setEnabled(bool enabled) override {
        sawPingGenerator.setEnabled(enabled);
    }

    SawPingGenerator             sawPingGenerator;
};

/**
 * Echo input to output through a delay line.
 */
class ActivityFullDuplex : public ActivityContext {
public:

    void configureBuilder(bool isInput, oboe::AudioStreamBuilder &builder) override;

    virtual int32_t getState() { return -1; }
    virtual int32_t getResult() { return -1; }
    virtual bool isAnalyzerDone() { return false; }

    virtual FullDuplexAnalyzer *getFullDuplexAnalyzer() = 0;

    int32_t getResetCount() {
        return getFullDuplexAnalyzer()->getLoopbackProcessor()->getResetCount();
    }
};

/**
 * Echo input to output through a delay line.
 */
class ActivityEcho : public ActivityFullDuplex {
public:

    oboe::Result startStreams() override {
        return mFullDuplexEcho->start();
    }

    void configureBuilder(bool isInput, oboe::AudioStreamBuilder &builder) override;

    void setDelayTime(double delayTimeSeconds) {
        if (mFullDuplexEcho) {
            mFullDuplexEcho->setDelayTime(delayTimeSeconds);
        }
    }

    virtual FullDuplexAnalyzer *getFullDuplexAnalyzer() override {
        return (FullDuplexAnalyzer *) mFullDuplexEcho.get();
    }

protected:
    void finishOpen(bool isInput, oboe::AudioStream *oboeStream) override;

private:
    std::unique_ptr<FullDuplexEcho>   mFullDuplexEcho{};
};

/**
 * Measure Round Trip Latency
 */
class ActivityRoundTripLatency : public ActivityFullDuplex {
public:

    oboe::Result startStreams() override {
        return mFullDuplexLatency->start();
    }

    void configureBuilder(bool isInput, oboe::AudioStreamBuilder &builder) override;

    LatencyAnalyzer *getLatencyAnalyzer() {
        return mFullDuplexLatency->getLatencyAnalyzer();
    }

    int32_t getState() override {
        return getLatencyAnalyzer()->getState();
    }
    int32_t getResult() override {
        return getLatencyAnalyzer()->getState();
    }
    bool isAnalyzerDone() override {
        return mFullDuplexLatency->isDone();
    }

    FullDuplexAnalyzer *getFullDuplexAnalyzer() override {
        return (FullDuplexAnalyzer *) mFullDuplexLatency.get();
    }

protected:
    void finishOpen(bool isInput, oboe::AudioStream *oboeStream) override;

private:
    std::unique_ptr<FullDuplexLatency>   mFullDuplexLatency{};
};

/**
 * Measure Glitches
 */
class ActivityGlitches : public ActivityFullDuplex {
public:

    oboe::Result startStreams() override {
        return mFullDuplexGlitches->start();
    }

    void configureBuilder(bool isInput, oboe::AudioStreamBuilder &builder) override;

    GlitchAnalyzer *getGlitchAnalyzer() {
        return mFullDuplexGlitches->getGlitchAnalyzer();
    }

    int32_t getState() override {
        return getGlitchAnalyzer()->getState();
    }
    int32_t getResult() override {
        return getGlitchAnalyzer()->getResult();
    }
    bool isAnalyzerDone() override {
        return mFullDuplexGlitches->isDone();
    }

    FullDuplexAnalyzer *getFullDuplexAnalyzer() override {
        return (FullDuplexAnalyzer *) mFullDuplexGlitches.get();
    }

protected:
    void finishOpen(bool isInput, oboe::AudioStream *oboeStream) override;

private:
    std::unique_ptr<FullDuplexGlitches>   mFullDuplexGlitches{};
};

/**
 * Switch between various
 */
class NativeAudioContext {
public:

    ActivityContext *getCurrentActivity() {
        return currentActivity;
    };

    void setActivityType(int activityType) {
        LOGD("%s(%d)", __func__, activityType);
        mActivityType = (ActivityType) activityType;
        switch(mActivityType) {
            default:
            case ActivityType::Undefined:
            case ActivityType::TestOutput:
                currentActivity = &mActivityTestOutput;
                break;
            case ActivityType::TestInput:
                currentActivity = &mActivityTestInput;
                break;
            case ActivityType::TapToTone:
                currentActivity = &mActivityTapToTone;
                break;
            case ActivityType::RecordPlay:
                currentActivity = &mActivityRecording;
                break;
            case ActivityType::Echo:
                currentActivity = &mActivityEcho;
                break;
            case ActivityType::RoundTripLatency:
                currentActivity = &mActivityRoundTripLatency;
                break;
            case ActivityType::Glitches:
                currentActivity = &mActivityGlitches;
                break;
        }
    }

    void setDelayTime(double delayTimeMillis) {
        mActivityEcho.setDelayTime(delayTimeMillis);
    }

    ActivityTestOutput           mActivityTestOutput;
    ActivityTestInput            mActivityTestInput;
    ActivityTapToTone            mActivityTapToTone;
    ActivityRecording            mActivityRecording;
    ActivityEcho                 mActivityEcho;
    ActivityRoundTripLatency     mActivityRoundTripLatency;
    ActivityGlitches             mActivityGlitches;

private:

    // WARNING - must match definitions in TestAudioActivity.java
    enum ActivityType {
        Undefined = -1,
        TestOutput = 0,
        TestInput = 1,
        TapToTone = 2,
        RecordPlay = 3,
        Echo = 4,
        RoundTripLatency = 5,
        Glitches = 6,
    };

    ActivityType                 mActivityType = ActivityType::Undefined;
    ActivityContext             *currentActivity = &mActivityTestOutput;

};

#endif //NATIVEOBOE_NATIVEAUDIOCONTEXT_H
