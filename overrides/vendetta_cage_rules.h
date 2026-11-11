/* Def Jam Vendetta (USA, SLUS-20639) - the cage match: the climb's rules, one copy for every program that plays the climb.
 * Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
 *
 * Everything that decides where a climber is and what plays next: the heights worked out from the cage's clips, the
 * start of a climb, and the climb picture by picture. Nothing else: no game memory, no files, no logging, no globals.
 * The caller reads what it needs, poses the clips, and hands the rules plain numbers; the rules hand back plain numbers.
 * The game (vendetta_cage_climb.cpp) calls them every picture; another program can build this file into a library
 * (CAGE_RULES_DLL defined: the functions exported) and call the same functions.
 *
 * Plain C structs of float and int32_t only (0 = no, 1 = yes), so any language can lay them out; plain C++17 and the
 * standard library inside. cage_rules_version() changes whenever a struct or a function here changes.
 *
 * Units: the game's (a wrestler about 22 tall, the mat at 10.5); angles in degrees, 0 = facing +z. A side of the ring
 * is described by its outward normal (nx, nz) and its way along (tx, tz) = (nz, -nx). "Out" is along the normal from the
 * ring's middle, "along" along the side, "up" the height.
 */
#ifndef VENDETTA_CAGE_RULES_H
#define VENDETTA_CAGE_RULES_H

#include <stdint.h>

#if defined(CAGE_RULES_DLL)
#define CAGE_RULES_API __declspec(dllexport)
#else
#define CAGE_RULES_API
#endif

#define CAGE_RULES_VERSION 1

/* The cage's clips by the numbers they had in the game they were converted from; the hang at the rope hold is the hang
 * a second time, with its own playback record (the top rope bent to the foot). */
#define CAGE_CLIP_ONTO 0x3203         /* getting on: up the ropes onto the cage */
#define CAGE_CLIP_HANG 0x3204         /* hanging (a loop) */
#define CAGE_CLIP_LET_GO 0x3205       /* letting go: back into the ring, on the feet */
#define CAGE_CLIP_DOWN 0x3208         /* a step down */
#define CAGE_CLIP_RIGHT 0x3209        /* a step to the climber's right */
#define CAGE_CLIP_LEFT 0x320A         /* a step to the climber's left */
#define CAGE_CLIP_UP_BLAZIN 0x320E    /* a step up in Blazin' */
#define CAGE_CLIP_DOWN_BLAZIN 0x320F
#define CAGE_CLIP_RIGHT_BLAZIN 0x3210
#define CAGE_CLIP_LEFT_BLAZIN 0x3211
#define CAGE_CLIP_UP_SLOWEST 0x3212   /* a step up: 0x3212 to 0x3216 by momentum, slowest to fastest */
#define CAGE_CLIP_SLIP 0x323D         /* too little momentum for a step: slips to dangling */
#define CAGE_CLIP_DANGLE 0x323E       /* dangling (a loop) */
#define CAGE_CLIP_RECOVER 0x323F      /* back to the hang */
#define CAGE_CLIP_HANG_ON_ROPE 0x13204 /* the hang at the rope hold */

/* The climber's phases */
#define CAGE_PHASE_NONE 0
#define CAGE_PHASE_ONTO 1
#define CAGE_PHASE_HANG 2
#define CAGE_PHASE_STEP 3
#define CAGE_PHASE_SLIP 4
#define CAGE_PHASE_DANGLE 5
#define CAGE_PHASE_RECOVER 6
#define CAGE_PHASE_LET_GO 7

/* The body points the ten rooms belong to (the game's 20 body points, FUN_00131438): chest, head, left elbow, left
 * fist, right elbow, right fist, left knee, left ankle, right knee, right ankle */
#define CAGE_ROOM_POINTS {3, 5, 8, 9, 12, 13, 15, 16, 18, 19}
#define CAGE_ROOMS 10

/* The most frames a getting-on or letting-go clip may have for its heights */
#define CAGE_MOST_FRAMES 512

#ifdef __cplusplus
extern "C" {
#endif

/* The cage's and the climb's numbers: cage.json's "numbers", in the file's order (the name each has there in quotes) */
typedef struct CageNumbers
{
    float height;           /* "the cage's height above the mat" */
    float walls;            /* "the cage's walls from the middle": the fight's cage walls and the side climbed */
    float stepUp;           /* "a step up": between holds */
    int32_t steps;          /* "steps to the top hold": from the rope hold */
    float stepSide;         /* "a step sideways" */
    float cornerRoom;       /* "kept from each corner" */
    float stepCost;         /* "a step's momentum": what a step up costs (none in Blazin') */
    int32_t danglePictures; /* "dangling, pictures": before back to the hang */
    float wires;            /* "the wires": the body's outermost part kept this near the middle or nearer */
    float toCage;           /* "toward the cage, a picture": the most the body moves out to the wires a picture */
    float offCage;          /* "back from the cage, a picture": the most it moves back a picture */
    float ankleLeast;       /* "ankles above the mat, least" */
    float ankleUp;          /* "ankles above the mat, standing": where letting go's ankles land */
    float ropePress;        /* "a foot on a rope presses it down": getting on and the rope hold */
    float room[CAGE_ROOMS]; /* "room, chest" .. "room, right ankle" (CAGE_ROOM_POINTS' order) */
} CageNumbers;

typedef struct CagePoint
{
    float x, y, z;
} CagePoint;

/* A frame of a clip in the clip's own space (x the wrestler's left, y up, z forward toward the cage; times the model's
 * draw scale): the root (bone 1), the hips (bone 3), the ankles (bones 29, 39) */
typedef struct CageClipFrame
{
    CagePoint root, hips, ankleLeft, ankleRight;
} CageClipFrame;

/* A rope bend of a clip: the game's window {from frame, to frame, type 1-26} (16-18 the top / middle / bottom rope to
 * the right foot, 19-21 to the left, 22-24 to both) */
typedef struct CageRopeBend
{
    int32_t from, to, what;
} CageRopeBend;

/* The heights worked out from one climber's clips (cage_rules_work_out_heights) */
typedef struct CageHeights
{
    int32_t ontoFrames;            /* getting on's frames */
    float drop[CAGE_MOST_FRAMES];  /* the anchor under the mat each of its frames */
    float hang[3];                 /* its last frame to the hang's first: the anchor moved along, up, out */
    int32_t letGoFrames;           /* letting go's frames */
    float letGoHips[CAGE_MOST_FRAMES]; /* its hips' height each frame */
    int32_t landing;               /* its frame with the hips lowest */
    float landingAnkle;            /* the lower ankle's height then */
} CageHeights;

/* A climber, kept by the caller and handed back each picture */
typedef struct CageClimber
{
    int32_t phase;    /* CAGE_PHASE_* */
    int32_t hold;     /* 0 the rope hold .. steps the top hold */
    int32_t pictures; /* in this phase */
    int32_t settle;   /* pictures the body still shows the clip before a jump of the anchor */
    int32_t clip;     /* the clip to start at the end of the next picture (0 none) */
    int32_t clipBlend;
    float nx, nz, tx, tz; /* the side: its outward normal and its way along */
    float halfSide;       /* half its length at the walls */
    float out, along, y;  /* the anchor */
    float fromAlong, toAlong, fromY, toY; /* a step's ends; fromY: the height a phase started at */
    float raised;         /* letting go: the anchor's rise so far (never back down) */
    float writtenOut, writtenAlong, writtenY; /* the anchor as last handed out: where the body points stand */
} CageClimber;

/* The body this picture (the last pose, at the anchor last handed out), each as (out, along, up) from that anchor:
 * a CagePoint's x = out, y = along, z = up */
typedef struct CageBody
{
    CagePoint rooms[CAGE_ROOMS];      /* CAGE_ROOM_POINTS' points */
    CagePoint ankleLeft, ankleRight;  /* points 16, 19 */
    CagePoint hips;                   /* point 1 */
    CagePoint hipLeft, hipRight;      /* points 14, 17: the way the body faces */
} CageBody;

typedef struct CagePictureIn
{
    float frame;       /* the clip's frame now */
    float progress;    /* how far through it, 0 to 1 */
    int32_t clipEnded; /* it has ended */
    int32_t up, down;  /* the stick */
    int32_t along;     /* -1 / 0 / +1: the stick's left or right, already turned by the caller into the side's way */
    float momentum;
    int32_t blazin;
    CageBody body;
} CagePictureIn;

typedef struct CagePictureOut
{
    float x, y, z, facing; /* the anchor to hand the fighter, in the world, and its facing */
    int32_t clip;          /* the clip to start now (a CAGE_CLIP_*, 0 none) */
    int32_t blend;         /* 1: blended in from the pose shown; 0: a cut */
    float momentumCost;    /* momentum to take off (not below 0) */
    int32_t landed;        /* the climb is over: stand at x, y, z facing `facing` */
    int32_t started;       /* a phase started this picture (for a log): */
    int32_t startedPhase, startedHold;
    float startedOut, startedAlong, startedY;
} CagePictureOut;

/* The number above: a program built for another version should not use these rules */
CAGE_RULES_API int32_t cage_rules_version(void);

/* The standard cage's numbers (what its cage.json holds; a key a file doesn't have keeps its value from here) */
CAGE_RULES_API void cage_rules_standard_numbers(CageNumbers *numbers);

/* A climber's heights from its clips: getting on's frames (onto, ontoFrames) and rope bends, the hang's first frame,
 * letting go's frames. 1 = worked out; 0 = a clip of no frames or more than CAGE_MOST_FRAMES. */
CAGE_RULES_API int32_t cage_rules_work_out_heights(const CageNumbers *numbers, const CageClipFrame *onto, int32_t ontoFrames,
                                                   const CageRopeBend *bends, int32_t bendCount,
                                                   const CageClipFrame *hangFirst, const CageClipFrame *letGo,
                                                   int32_t letGoFrames, CageHeights *heights);

/* Getting on its drop at a frame (between frames a straight line) and the rope hold it ends at */
CAGE_RULES_API float cage_rules_drop_at(const CageHeights *heights, float frame);
CAGE_RULES_API float cage_rules_rope_hold(const CageHeights *heights);

/* A climb starting: the ring's sides (4 or 6) and where the wrestler stands (its anchor); the climber's first state, and
 * the anchor and facing to hand the fighter (the clip, getting on blended in, starts at the first picture) */
CAGE_RULES_API void cage_rules_start(const CageNumbers *numbers, int32_t sides, float x, float y, float z,
                                     CageClimber *climber, CagePictureOut *out);

/* One picture of the climb. 0 = the climber isn't climbing (phase none: the caller makes it stand). */
CAGE_RULES_API int32_t cage_rules_picture(const CageNumbers *numbers, const CageHeights *heights, CageClimber *climber,
                                          const CagePictureIn *in, CagePictureOut *out);

#ifdef __cplusplus
}
#endif

#endif
