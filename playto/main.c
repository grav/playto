#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h> // for usleep()

typedef struct MyAUGraphPlayer
{
    AudioStreamBasicDescription inputFormat; // input file's data stream description
    AudioFileID					inputFile; // reference to your input file
    
    AUGraph graph;
    AudioUnit fileAU;
    
} MyAUGraphPlayer;

void CreateMyAUGraph(MyAUGraphPlayer *player, AudioDeviceID deviceId);
double PrepareFileAU(MyAUGraphPlayer *player);

AudioDeviceID getOutputDevice(char *outputName);

void SetOutput(MyAUGraphPlayer *pPlayer, AUNode output, AudioDeviceID deviceId);


char *getDeviceName(AudioDeviceID id, char *buf, UInt32 maxlen);
int numChannels(AudioDeviceID deviceID, bool inputChannels);


#pragma mark - utility functions -

// generic error handler - if err is nonzero, prints error message and exits program.
static void CheckError(OSStatus error, const char *operation)
{
    if (error == noErr) return;
    
    char str[20];
    // see if it appears to be a 4-char-code
    *(UInt32 *)(str + 1) = CFSwapInt32HostToBig(error);
    if (isprint(str[1]) && isprint(str[2]) && isprint(str[3]) && isprint(str[4])) {
        str[0] = str[5] = '\'';
        str[6] = '\0';
    } else
        // no, format it as an integer
        sprintf(str, "%d", (int)error);
    
    fprintf(stderr, "Error: %s (%s)\n", operation, str);
    
    exit(1);
}

#pragma mark - audio converter -

char *getDeviceName(AudioDeviceID id, char *buf, UInt32 maxlen)
{
    AudioObjectPropertyScope theScope = kAudioDevicePropertyScopeOutput;
    
    AudioObjectPropertyAddress theAddress = { kAudioDevicePropertyDeviceName,
        theScope,
        0 }; // channel
    
    CheckError(AudioObjectGetPropertyData(id, &theAddress, 0, NULL,  &maxlen, buf),"AudioObjectGetPropertyData failed");
    
    return buf;
}

void CreateMyAUGraph(MyAUGraphPlayer *player, AudioDeviceID deviceId)
{
    // create a new AUGraph
    CheckError(NewAUGraph(&player->graph),
               "NewAUGraph failed");
    
    // generate description that will match out output device (speakers)
    AudioComponentDescription outputcd = {0};
    outputcd.componentType = kAudioUnitType_Output;
    outputcd.componentSubType = kAudioUnitSubType_DefaultOutput;
    outputcd.componentManufacturer = kAudioUnitManufacturer_Apple;
    
    // adds a node with above description to the graph
    AUNode outputNode;
    CheckError(AUGraphAddNode(player->graph, &outputcd, &outputNode),
               "AUGraphAddNode[kAudioUnitSubType_DefaultOutput] failed");
    
    
    // generate description that will match a generator AU of type: audio file player
    AudioComponentDescription fileplayercd = {0};
    fileplayercd.componentType = kAudioUnitType_Generator;
    fileplayercd.componentSubType = kAudioUnitSubType_AudioFilePlayer;
    fileplayercd.componentManufacturer = kAudioUnitManufacturer_Apple;
    
    // adds a node with above description to the graph
    AUNode fileNode;
    CheckError(AUGraphAddNode(player->graph, &fileplayercd, &fileNode),
               "AUGraphAddNode[kAudioUnitSubType_AudioFilePlayer] failed");
    
    // opening the graph opens all contained audio units but does not allocate any resources yet
    CheckError(AUGraphOpen(player->graph),
               "AUGraphOpen failed");
    
    // get the reference to the AudioUnit object for the file player graph node
    CheckError(AUGraphNodeInfo(player->graph, fileNode, NULL, &player->fileAU),
               "AUGraphNodeInfo failed");
    
    // connect the output source of the file player AU to the input source of the output node
    CheckError(AUGraphConnectNodeInput(player->graph, fileNode, 0, outputNode, 0),
               "AUGraphConnectNodeInput");
    
    if(deviceId>0){
        char name[64];
        getDeviceName(deviceId, name, 64);
        printf("Using output device '%s' (id %d)\n",name,(int)deviceId);
        SetOutput(player, outputNode, deviceId);
    } else {
        printf("Using default output device.\n");
    }
    
    // now initialize the graph (causes resources to be allocated)
    CheckError(AUGraphInitialize(player->graph),
               "AUGraphInitialize failed");
}

int numChannels(AudioDeviceID deviceID, bool inputChannels){
    OSStatus err;
   	UInt32 propSize;
   	int result = 0;
    
    AudioObjectPropertyScope theScope = inputChannels ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
    
    AudioObjectPropertyAddress theAddress = { kAudioDevicePropertyStreamConfiguration,
        theScope,
        0 }; // channel
    
    err = AudioObjectGetPropertyDataSize(deviceID, &theAddress, 0, NULL, &propSize);
   	if (err) return 0;
    
   	AudioBufferList *buflist = (AudioBufferList *)malloc(propSize);
    err = AudioObjectGetPropertyData(deviceID, &theAddress, 0, NULL, &propSize, buflist);
   	if (!err) {
        for (UInt32 i = 0; i < buflist->mNumberBuffers; ++i) {
            result += buflist->mBuffers[i].mNumberChannels;
        }
   	}
   	free(buflist);
   	return result;
}


AudioDeviceID getOutputDevice(char *outputName) {
    UInt32 propsize;
    
    AudioObjectPropertyAddress theAddress = { kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster };
    
    CheckError(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize),"AudioObjectGetPropertyDataSize failed");
   	int nDevices = propsize / sizeof(AudioDeviceID);
   	AudioDeviceID *devids = malloc(sizeof(AudioDeviceID) * nDevices); // propsize
    CheckError(AudioObjectGetPropertyData(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize, devids),"AudioObjectGetPropertyData failed");
    
    AudioDeviceID deviceID = 0;
   	for (int i = 0; i < nDevices; ++i) {
        AudioDeviceID testId = devids[i];
        char name[64];
        getDeviceName(testId, name, 64);
        if(numChannels(testId, false) && strcmp(outputName, name) == 0) {
            deviceID = testId;
            break;
        }
   	}
    
   	free(devids);
    return deviceID;
}

double PrepareFileAU(MyAUGraphPlayer *player)
{
    
    // tell the file player unit to load the file we want to play
    CheckError(AudioUnitSetProperty(player->fileAU, kAudioUnitProperty_ScheduledFileIDs,
                                    kAudioUnitScope_Global, 0, &player->inputFile, sizeof(player->inputFile)),
               "AudioUnitSetProperty[kAudioUnitProperty_ScheduledFileIDs] failed");
    
    UInt64 nPackets;
    UInt32 propsize = sizeof(nPackets);
    CheckError(AudioFileGetProperty(player->inputFile, kAudioFilePropertyAudioDataPacketCount,
                                    &propsize, &nPackets),
               "AudioFileGetProperty[kAudioFilePropertyAudioDataPacketCount] failed");
    
    // tell the file player AU to play the entire file
    ScheduledAudioFileRegion rgn;
    memset (&rgn.mTimeStamp, 0, sizeof(rgn.mTimeStamp));
    rgn.mTimeStamp.mFlags = kAudioTimeStampSampleTimeValid;
    rgn.mTimeStamp.mSampleTime = 0;
    rgn.mCompletionProc = NULL;
    rgn.mCompletionProcUserData = NULL;
    rgn.mAudioFile = player->inputFile;
    rgn.mLoopCount = 0;
    rgn.mStartFrame = 0;
    rgn.mFramesToPlay = (UInt32) (nPackets * player->inputFormat.mFramesPerPacket);
    
    CheckError(AudioUnitSetProperty(player->fileAU, kAudioUnitProperty_ScheduledFileRegion,
                                    kAudioUnitScope_Global, 0,&rgn, sizeof(rgn)),
               "AudioUnitSetProperty[kAudioUnitProperty_ScheduledFileRegion] failed");
    
    // prime the file player AU with default values
    UInt32 defaultVal = 0;
    CheckError(AudioUnitSetProperty(player->fileAU, kAudioUnitProperty_ScheduledFilePrime,
                                    kAudioUnitScope_Global, 0, &defaultVal, sizeof(defaultVal)),
               "AudioUnitSetProperty[kAudioUnitProperty_ScheduledFilePrime] failed");
    
    // tell the file player AU when to start playing (-1 sample time means next render cycle)
    AudioTimeStamp startTime;
    memset (&startTime, 0, sizeof(startTime));
    startTime.mFlags = kAudioTimeStampSampleTimeValid;
    startTime.mSampleTime = -1;
    CheckError(AudioUnitSetProperty(player->fileAU, kAudioUnitProperty_ScheduleStartTimeStamp,
                                    kAudioUnitScope_Global, 0, &startTime, sizeof(startTime)),
               "AudioUnitSetProperty[kAudioUnitProperty_ScheduleStartTimeStamp]");
    
    // file duration
    return (nPackets * player->inputFormat.mFramesPerPacket) / player->inputFormat.mSampleRate;
}

#pragma mark - main -
int	main(int argc, const char *argv[])
{
    
    AudioDeviceID deviceId = 0;
    if(argc<2){
        printf("playto - audio player that can output to a certain device.\n");
        printf("Usage: playto filename [output device name].\n");
        printf("Ex: playto /path/to/foo.mp3 My\\ Nice\\ Sound\\ Card\n");
        return -1;
    }
    
    
    if(argc>2){
        deviceId = getOutputDevice((char *) argv[2]);
    }
    
    CFStringRef file = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    
    CFURLRef inputFileURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, file, kCFURLPOSIXPathStyle, false);
    MyAUGraphPlayer player = {0};
    
    // open the input audio file
    CheckError(AudioFileOpenURL(inputFileURL, kAudioFileReadPermission, 0, &player.inputFile),
               "AudioFileOpenURL failed");
    CFRelease(inputFileURL);
    
    // get the audio data format from the file
    UInt32 propSize = sizeof(player.inputFormat);
    CheckError(AudioFileGetProperty(player.inputFile, kAudioFilePropertyDataFormat,
                                    &propSize, &player.inputFormat),
               "couldn't get file's data format");
    
    // build a basic fileplayer->speakers graph
    CreateMyAUGraph(&player, deviceId);
    
    
    // configure the file player
    Float64 fileDuration = PrepareFileAU(&player);
    
    // start playing
    CheckError(AUGraphStart(player.graph),
               "AUGraphStart failed");
    
    // sleep until the file is finished
    usleep ((useconds_t) (fileDuration * 1000.0 * 1000.0));
    
cleanup:
    AUGraphStop (player.graph);
    AUGraphUninitialize (player.graph);
    AUGraphClose(player.graph);
    AudioFileClose(player.inputFile);
    
    return 0;
}

void SetOutput(MyAUGraphPlayer *player, AUNode outputNode, AudioDeviceID deviceId) {
    
    // HERE GOES
    
    AudioUnit outputUnit;
    CheckError(AUGraphNodeInfo(player->graph, outputNode, NULL, &outputUnit),
               "AUGraphNodeInfo failed");
    
    
    CheckError(AudioUnitSetProperty(outputUnit,
                                    kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global,
                                    0,
                                    &deviceId,
                                    sizeof(deviceId)),
               "AudioUnitSetProperty failed");
    
    // HERE STOPS
}
