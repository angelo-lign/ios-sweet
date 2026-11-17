/*
 * earplay: a test tone through the receiver (earpiece). iOS routes PlayAndRecord output to the receiver unless the
 * speaker is forced, so this puts the session in that category and plays 440 Hz at a low level for SECS seconds
 * (default 12), then in `speaker` mode (argv[2] = speaker) the same with the speaker override, to compare.
 *   earplay [SECS] [speaker]       MIT, as userland/.
 */
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

int main(int argc, char **argv)
{
	@autoreleasepool {
		int secs = argc > 1 ? atoi(argv[1]) : 12;
		BOOL speaker = argc > 2 && !strcmp(argv[2], "speaker");
		NSError *err = nil;
		AVAudioSession *s = [AVAudioSession sharedInstance];
		[s setCategory:AVAudioSessionCategoryPlayAndRecord error:&err];
		printf("earplay: setCategory %s\n", err ? err.description.UTF8String : "ok");
		err = nil;
		if (speaker)
			[s overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker error:&err];
		err = nil;
		[s setActive:YES error:&err];
		printf("earplay: setActive %s\n", err ? err.description.UTF8String : "ok");
		AVAudioEngine *engine = [AVAudioEngine new];
		__block double phase = 0;
		AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
		AVAudioSourceNode *src = [[AVAudioSourceNode alloc] initWithFormat:fmt renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *ts, AVAudioFrameCount n, AudioBufferList *abl) {
			for (AVAudioFrameCount i = 0; i < n; i++) {
				float v = (float)(0.12 * sin(phase));
				phase += 2.0 * M_PI * 440.0 / 48000.0;
				for (UInt32 b = 0; b < abl->mNumberBuffers; b++)
					((float *)abl->mBuffers[b].mData)[i] = v;
			}
			return noErr;
		}];
		[engine attachNode:src];
		[engine connect:src to:engine.mainMixerNode format:fmt];
		err = nil;
		[engine startAndReturnError:&err];
		printf("earplay: engine %s; route:", err ? err.description.UTF8String : "started");
		for (AVAudioSessionPortDescription *p in s.currentRoute.outputs)
			printf(" %s", p.portType.UTF8String);
		printf("\n");
		[NSThread sleepForTimeInterval:secs];
		[engine stop];
		printf("earplay: done\n");
	}
	return 0;
}
