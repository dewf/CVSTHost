// LibraryTest.cpp : Defines the entry point for the console application.
//

#include <stdio.h>
#include <algorithm>

#include "../../../source/CVSTHost.h"
#include "../deps/openwl/source/openwl.h"
#include "../deps/CASIOClient/source/CASIOClient.h"
#include "../deps/CWin32MIDI/source/CWin32MIDI.h"

#define ASIO_DEVICE_NAME "Traktor Audio 2 MK2"
#define MIDI_DEVICE_NAME "Impact LX25+" // 2- UA-4FX" //"MIDIIN2 (padKONTROL)"

static wl_WindowRef editorWindow = nullptr;
static CASIO_Device asioDevice = nullptr;
static CASIO_DeviceProperties asioProps;
static double sampleRate;
static float **asioInputs;
static float **asioOutputs;

static CVST_Plugin vstPlugin;
static CVST_Properties vstProps;
static float **vstInputs;
static float **vstOutputs;

static CWin32Midi_Device midiDevice;

int CDECL wlCallback(wl_WindowRef window, struct wl_Event *event, void *userData)
{
    event->handled = true;
    switch (event->eventType) {
    case wl_EventType::wl_kEventTypeWindowDestroyed:
        if (window == editorWindow) {
            wl_ExitRunloop();
        }
        break;
    default:
        event->handled = false;
    }
    return 0;
}

int CDECL vstHostCallback(CVST_HostEvent *event, CVST_Plugin plugin, void *userData)
{
    event->handled = true;
    switch (event->eventType) {
    case CVST_EventType_Log:
        printf("VST>> %s\n", event->logEvent.message);
        break;
    case CVST_EventType_Automation:
        printf(" = vst automation [%03d] value %.2f\n", event->automationEvent.index, event->automationEvent.value);
        break;
    case CVST_EventType_GetVendorInfo:
        event->vendorInfoEvent.vendor = "Derp";
        event->vendorInfoEvent.product = "LibraryTest";
        event->vendorInfoEvent.version = 1234;
        break;
    default:
        event->handled = false;
    }
    return 0;
}

#define MIDI_BUFFER_LEN 2048
static CWin32Midi_MidiMsg midiEvents[MIDI_BUFFER_LEN];
static CVST_MidiEvent vstMidiEvents[MIDI_BUFFER_LEN];
static int midiEventCount;

void bufferSwitch(CASIO_Event *event) {
    if (asioProps.sampleFormat == CASIO_SampleFormat_Int32) {
        // convert raw asio inputs to float inputs
        auto rawInputs = (int **)event->bufferSwitchEvent.inputs;
        for (int i = 0; i < asioProps.numInputs; i++) {
            for (int j = 0; j < asioProps.bufferSampleLength; j++) {
                asioInputs[i][j] = (float) (rawInputs[i][j] / (1 << 30));
            }
        }

        // === process the VST ====
        
        // copy max asio inputs to vst inputs
        int inputsToCopy = min(asioProps.numInputs, vstProps.numInputs);
        for (int i = 0; i < inputsToCopy; i++) {
            memcpy(vstInputs[i], asioInputs[i], asioProps.bufferSampleLength * sizeof(float));
        }

        // process midi
        bool blockSent = false;
        do {
            CWin32Midi_ReadInput(midiDevice, midiEvents, MIDI_BUFFER_LEN, &midiEventCount);
            for (int i = 0; i < midiEventCount; i++) {
                //printf("midievent: %08X @ %d\n", midiEvents[i].data.uint32, midiEvents[i].relTime);
                vstMidiEvents[i].sampleOffs = (unsigned long)
                    min((midiEvents[i].relTime * sampleRate) / 1000, asioProps.bufferSampleLength-1); // clamp
                vstMidiEvents[i].data.uint32 = midiEvents[i].data.uint32;
            }

            // currently this can only be called once per block
            // so for now only send the first batch (2048 events is pleeeeenty for small ~12ms buffers! that's 176k events/sec)
            if (!blockSent) {
                CVST_SetBlockEvents(vstPlugin, vstMidiEvents, midiEventCount);
                blockSent = true;
            }

            // the CWin32Midi internal reference time won't reset until we've called ReadInput and it returns less than our total buffer size
            // hence the do-while loop, until that's the case
        } while (midiEventCount >= MIDI_BUFFER_LEN);

        // process!
        CVST_ProcessReplacing(vstPlugin, vstInputs, vstOutputs, asioProps.bufferSampleLength);

        // copy as many outputs as possible
        int outputsToCopy = min(vstProps.numOutputs, asioProps.numOutputs);
        for (int i = 0; i < outputsToCopy; i++) {
            memcpy(asioOutputs[i], vstOutputs[i], asioProps.bufferSampleLength * sizeof(float));
        }

        // convert back to ASIO native format
        auto rawOutputs = (int **)event->bufferSwitchEvent.outputs;
        for (int i = 0; i < asioProps.numOutputs; i++) {
            for (int j = 0; j < asioProps.bufferSampleLength; j++) {
                rawOutputs[i][j] = (int)(asioOutputs[i][j] * (1 << 30));
            }
        }
    }
    else {
        for (int i = 0; i < asioProps.numOutputs; i++) {
            memset(event->bufferSwitchEvent.outputs[i], 0, asioProps.bufferByteLength);
        }
    }
}

int CDECL asioCallback(CASIO_Event *event, CASIO_Device device, void *userData)
{
    event->handled = true;
    switch (event->eventType) {
    case CASIO_EventType_Log:
        printf("ASIO>> %s\n", event->logEvent.message);
        break;
    case CASIO_EventType_BufferSwitch:
        bufferSwitch(event);
        break;
    default:
        event->handled = false;
    }
    return 0;
}

int CDECL midiCallback(CWin32Midi_Event *event, CWin32Midi_Device device, void *userData)
{
    event->handled = true;
    switch (event->eventType) {
    case CWin32Midi_EventType_Log:
        printf("MIDI>> %s\n", event->logEvent.message);
        break;
    case CWin32Midi_EventType_Data:
        printf("midi data (callback): %08X\n", event->dataEvent.uint32);
        break;
    default:
        event->handled = false;
    }
    return 0;
}

void allocBuffers() {
    asioInputs = new float*[asioProps.numInputs];
    for (int i = 0; i < asioProps.numInputs; i++) {
        asioInputs[i] = new float[asioProps.bufferSampleLength];
        // don't need to zero, they always get erased by incoming
    }

    asioOutputs = new float*[asioProps.numOutputs];
    for (int i = 0; i < asioProps.numOutputs; i++) {
        asioOutputs[i] = new float[asioProps.bufferSampleLength];
        memset(asioOutputs[i], 0, sizeof(float)*asioProps.bufferSampleLength);
    }

    // vst
    vstInputs = new float*[vstProps.numInputs];
    for (int i = 0; i < vstProps.numInputs; i++) {
        vstInputs[i] = new float[asioProps.bufferSampleLength];
        memset(vstInputs[i], 0, sizeof(float)*asioProps.bufferSampleLength);
    }
    vstOutputs = new float*[vstProps.numOutputs];
    for (int i = 0; i < vstProps.numOutputs; i++) {
        vstOutputs[i] = new float[asioProps.bufferSampleLength];
        // don't need to zero, always replaced by outgoing
    }
}

#include <boost/range/algorithm.hpp>
#include <vector>

int openAsioByName(const char *name) {
    CASIO_DeviceInfo *asioList;
    int asioCount;
    CASIO_EnumerateDevices(&asioList, &asioCount);

    auto range = std::vector<CASIO_DeviceInfo>(asioList, &asioList[asioCount]);
    auto toOpen = boost::range::find_if(range, [name](CASIO_DeviceInfo info) { return !strcmp(info.name, name); });
    if (toOpen != range.end()) {
        return CASIO_OpenDevice(toOpen->id, nullptr, &asioDevice);
    }
    return -1;
}

int openMidiByName(const char *name) {
    // enumerate MIDI inputs, open one
    CWin32Midi_DeviceInfo *midiList;
    int midiCount;
    CWin32Midi_EnumerateInputs(&midiList, &midiCount);
    for (int i = 0; i < midiCount; i++) {
        printf("midi: %s\n", midiList[i].name);
    }

    auto range = std::vector<CWin32Midi_DeviceInfo>(midiList, &midiList[midiCount]);
    auto toOpen = boost::range::find_if(range, [name](CWin32Midi_DeviceInfo info) { return !strcmp(info.name, name); });
    if (toOpen != range.end()) {
        return CWin32Midi_OpenInput(toOpen->id, nullptr, CWin32Midi_InputMode_Queue, &midiDevice);
    }
    return -1;
}


int main()
{
	wl_PlatformOptions opts{};
    wl_Init(wlCallback, &opts);
    CVST_Init(vstHostCallback);
    CASIO_Init(asioCallback);
    CWin32Midi_Init(midiCallback);

	CASIO_DeviceInfo *devices;
	int deviceCount;
	CASIO_EnumerateDevices(&devices, &deviceCount);

	for (int i = 0; i < deviceCount; i++) {
		printf("device %p - [%s]\n", devices[i].id, devices[i].name);
	}

    if (openAsioByName(ASIO_DEVICE_NAME) != 0) {
        printf("failed to open ASIO device\n");
    }
    CASIO_GetProperties(asioDevice, &asioProps, &sampleRate);

	CWin32Midi_DeviceInfo *midiInputs;
	int numMidiInputs;
	CWin32Midi_EnumerateInputs(&midiInputs, &numMidiInputs);
	for (int i = 0; i < numMidiInputs; i++) {
		auto dev = &midiInputs[i];
		printf("midi dev %p [%s]\n", dev->id, dev->name);
	}
	printf("done enumerating midi devices\n");

    if (openMidiByName(MIDI_DEVICE_NAME) != 0) {
        printf("failed to open midi device\n");
    }

    // instantiate plugin
#ifdef _WIN64
    vstPlugin = CVST_LoadPlugin("C:\\Program Files\\Steinberg\\VSTPlugins\\dexed.dll", nullptr);
#else
    vstPlugin = CVST_LoadPlugin("C:\\Program Files (x86)\\Steinberg\\VSTPlugins\\dexed.dll", nullptr);
#endif
    CVST_GetProperties(vstPlugin, &vstProps);
    CVST_SetBlockSize(vstPlugin, asioProps.bufferSampleLength);

    allocBuffers();

    // open editor window
    int width, height;
    CVST_GetEditorSize(vstPlugin, &width, &height);
    printf("editor size: %d,%d\n", width, height);

    editorWindow = wl_WindowCreate(width, height, "plugin editor", nullptr, nullptr);
    wl_WindowShow(editorWindow);
    CVST_OpenEditor(vstPlugin, wl_WindowGetOSHandle(editorWindow));

    // start playback
    CWin32Midi_Start(midiDevice);
    CASIO_Start(asioDevice);

    // run until editor window closed
    wl_Runloop(); 

    // stop playback
    CASIO_Stop(asioDevice);
    CWin32Midi_Stop(midiDevice);

    // unload plugin
    CVST_Destroy(vstPlugin);

    // close MIDI
    CWin32Midi_CloseInput(midiDevice);

    // close ASIO
    CASIO_CloseDevice(asioDevice);

    CWin32Midi_Shutdown();
    CASIO_Shutdown();
    CVST_Shutdown();
    wl_Shutdown();
    return 0;
}

