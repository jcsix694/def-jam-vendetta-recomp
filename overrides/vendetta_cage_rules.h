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
 * Units: the game's (a wrestler about 22 tall, the mat at 10.5, the floor outside the ring at 0); angles in degrees, 0 = facing +z. A side of the ring
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

#define CAGE_RULES_VERSION 2

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
#define CAGE_CLIP_PULL_UP 0x3245      /* at the top hold: pulling up to look over the top */
#define CAGE_CLIP_STRAIN 0x3246       /* pulled up, straining (a loop): the first bar */
#define CAGE_CLIP_PULL_BACK 0x3247    /* from pulled up back down to the hang */
#define CAGE_CLIP_ONTO_TOP 0x3249     /* from pulled up onto the top, crouching */
#define CAGE_CLIP_ON_TOP 0x324A       /* crouched on top (a loop): the second bar, then resting there */
#define CAGE_CLIP_OFF_TOP 0x324B      /* from crouched on top back down to the hang */
#define CAGE_CLIP_OVER 0x320B         /* from sitting on top over it, ending on the outside facing in: the escape */
#define CAGE_CLIP_DROP 0x3206         /* outside: the drop to the floor */

/* The climber's phases */
#define CAGE_PHASE_NONE 0
#define CAGE_PHASE_ONTO 1
#define CAGE_PHASE_HANG 2
#define CAGE_PHASE_STEP 3
#define CAGE_PHASE_SLIP 4
#define CAGE_PHASE_DANGLE 5
#define CAGE_PHASE_RECOVER 6
#define CAGE_PHASE_LET_GO 7
#define CAGE_PHASE_PULL_UP 8     /* ESCAPE on: the first bar from here */
#define CAGE_PHASE_STRAIN 9
#define CAGE_PHASE_PULL_BACK 10
#define CAGE_PHASE_ONTO_TOP 11   /* ESCAPE on: the second bar from here */
#define CAGE_PHASE_ON_TOP 12     /* resting on top: over, the dive or back down */
#define CAGE_PHASE_OFF_TOP 13
#define CAGE_PHASE_OVER 14
#define CAGE_PHASE_DOWN_OUTSIDE 15 /* a step down the outside (the Blazin' step down, CAGE_CLIP_DOWN_BLAZIN) */
#define CAGE_PHASE_DROP 16
#define CAGE_PHASE_CROUCH 17     /* crouched on top, the second bar */

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
    int32_t tapsPart1;      /* "taps, part 1 (pulling up)": the first bar, straining at the top hold (ESCAPE on) */
    int32_t tapsPart2;      /* "taps, part 2 (crouched on top)": the second bar, then resting on top */
    int32_t downPictures;   /* "resting on top, down held to climb back down, pictures": down with triangle before then = over */
    float topDepth;         /* "the top's depth": the cage's top (its truss) out past the walls */
    float topHeight;        /* "the top's height": the cage's top down from the top */
    float diveOut;          /* "the dive's take-off, out past the walls": where a dive from the top stands */
    float diveFeet;         /* "the dive's take-off, feet over the top" */
    int32_t restPictures;   /* "resting on top, pictures before cross or triangle count": the taps that filled the
                               second bar (any button) don't dive or go over */
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
    float hangHips[3];             /* the hang's first frame: the hips (clip space x, y, z): over the top's end turns there */
    float dropLanding;             /* the drop's last frame: the lower ankle's height (the drop starts this high over the floor) */
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
    int32_t outside;      /* over the top: on the cage's outside, facing in */
    int32_t taps;         /* the bar now: taps counted since it began */
    int32_t tapsInWindow; /* at most two counted a window (the game's pin: two a 6.0 of its time) */
    float tapWindow;      /* the window's time so far */
    int32_t downHeld;     /* sitting on top: pictures down has been held */
    int32_t escaped;      /* over the top's end reached: the escape told (once) */
    int32_t stepsOutside; /* steps down the outside still to go before the drop */
    float fromOut;        /* over the top: the anchor's out as it began */
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
    float step;        /* this picture's time step (1 at full speed) */
    int32_t pressed;   /* a button newly pressed this picture that counts as a tap */
    int32_t triangle;  /* triangle newly pressed this picture */
    int32_t cross;     /* cross newly pressed this picture */
    int32_t escape;    /* ESCAPE on: the bar at the top hold; off: up there sits on top at once */
    int32_t mayGoOver; /* over the top may be climbed now (ESCAPE on, and the caller's own rules) */
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
    int32_t bar;           /* show the climber's tapping bar: */
    float barNeeded, barLeft; /* taps needed, taps still to make (full when none left) */
    int32_t escaped;       /* the climber is over the top this picture: the escape (no clip started this picture) */
    int32_t dive;          /* sitting on top, cross pressed: the caller starts the wrestler's own top-rope dive if one fits
                              (then sets the climber's phase to none) */
    int32_t outside;       /* with landed: standing on the floor outside the cage */
} CagePictureOut;

/* The number above: a program built for another version should not use these rules */
CAGE_RULES_API int32_t cage_rules_version(void);

/* The standard cage's numbers (what its cage.json holds; a key a file doesn't have keeps its value from here) */
CAGE_RULES_API void cage_rules_standard_numbers(CageNumbers *numbers);

/* A climber's heights from its clips: getting on's frames (onto, ontoFrames) and rope bends, the hang's first frame,
 * letting go's frames, the drop's last frame. 1 = worked out; 0 = a clip of no frames or more than CAGE_MOST_FRAMES. */
CAGE_RULES_API int32_t cage_rules_work_out_heights(const CageNumbers *numbers, const CageClipFrame *onto, int32_t ontoFrames,
                                                   const CageRopeBend *bends, int32_t bendCount,
                                                   const CageClipFrame *hangFirst, const CageClipFrame *letGo,
                                                   int32_t letGoFrames, const CageClipFrame *dropLast,
                                                   CageHeights *heights);

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
