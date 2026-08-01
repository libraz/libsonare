import type * as analysis from '../../dist/analysis.js';

type Assert<T extends true> = T;

type NoMasterAudio = Assert<'masterAudio' extends keyof typeof analysis ? false : true>;
type NoMixStereo = Assert<'mixStereo' extends keyof typeof analysis ? false : true>;
type NoProject = Assert<'Project' extends keyof typeof analysis ? false : true>;
type NoRealtimeEngine = Assert<'RealtimeEngine' extends keyof typeof analysis ? false : true>;
type NoRoomMorph = Assert<'roomMorph' extends keyof typeof analysis ? false : true>;

void (true as NoMasterAudio);
void (true as NoMixStereo);
void (true as NoProject);
void (true as NoRealtimeEngine);
void (true as NoRoomMorph);
