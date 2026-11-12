// Def Jam Vendetta (USA, SLUS-20639) - the cage match: the climb's rules (vendetta_cage_rules.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
// Plain C++17 and the standard library only: no game memory, no files, no logging, no globals.
#include "vendetta_cage_rules.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace cage_rules_inside // named, not unnamed: a build that merges files into one (a unity build) keeps it apart
{
    // the ring (game units): the mat; the ropes' middles, top / middle / bottom (the ring draw's table 0x2585D0)
    constexpr float kMat = 10.5f, kFloor = 0.0f;
    constexpr float kRopeY[3] = {24.1f, 19.3f, 14.5f};
    constexpr float kPi = 3.14159265f;

    float at(const float *values, int32_t count, float frame)
    {
        if (count <= 0)
            return 0.0f;
        const float f = std::clamp(frame, 0.0f, static_cast<float>(count - 1));
        const int32_t a = static_cast<int32_t>(f), b = std::min(a + 1, count - 1);
        return values[a] + (values[b] - values[a]) * (f - static_cast<float>(a));
    }

    // the hang at a hold: at the rope hold its copy that keeps the top rope on the foot
    int32_t hangClip(int32_t hold) { return hold == 0 ? CAGE_CLIP_HANG_ON_ROPE : CAGE_CLIP_HANG; }

    void startPhase(CageClimber &c, CagePictureOut &out, int32_t phase, int32_t clip, int32_t blend)
    {
        c.phase = phase;
        c.pictures = 0;
        c.clip = clip;
        c.clipBlend = blend;
        c.fromY = c.y;
        c.raised = 0.0f;
        c.downHeld = 0;
        out.started = 1;
        out.startedPhase = phase;
        out.startedHold = c.hold;
        out.startedOut = c.out;
        out.startedAlong = c.along;
        out.startedY = c.y;
    }

    void sideNormal(int32_t sides, float x, float z, float &nx, float &nz); // (below)

    // the side the anchor is at: its outward normal (4 sides: normals at 0, 90, 180, 270 degrees; 6 sides: atan2(x, z) =
    // 60 - 60k degrees), the way along, half its length at the walls
    void sideAt(const CageNumbers &n, CageClimber &c, int32_t sides, float x, float z)
    {
        const int32_t count = sides == 6 ? 6 : 4;
        sideNormal(count, x, z, c.nx, c.nz);
        c.tx = c.nz;
        c.tz = -c.nx;
        c.halfSide = count == 6 ? n.walls * std::tan(kPi / 6.0f) : n.walls;
    }

    void handOut(CageClimber &c, CagePictureOut &out)
    {
        out.x = c.nx * c.out + c.tx * c.along;
        out.y = c.y;
        out.z = c.nz * c.out + c.tz * c.along;
        // facing the cage from inside; on its outside or resting on the truss, facing the ring
        out.facing = (c.outside || c.onTruss) ? std::atan2(-c.nx, -c.nz) * 180.0f / kPi : std::atan2(c.nx, c.nz) * 180.0f / kPi;
        c.writtenOut = c.out;
        c.writtenAlong = c.along;
        c.writtenY = c.y;
    }

    // the feet kept out of the mat: the ankles (their up from the anchor last handed out) no lower than "ankles above the
    // mat, least"
    void aboveMat(const CageNumbers &n, CageClimber &c, const CageBody &body)
    {
        if (c.settle > 0)
            return;
        const float lowest = std::min(body.ankleLeft.z, body.ankleRight.z);
        c.y = std::max(c.y, kMat + n.ankleLeast - lowest);
    }

    // the body on the cage's wires: its outermost part (each point with the room its flesh takes) at "the wires"; touch =
    // moved out to them as well as kept from going through. Outside the cage the same on the wires' other face (as far
    // past the walls as the wires are inside them), and where a point is as high as the cage's top, on the top's outer
    // face ("the top's depth" past the walls): each point there or farther out
    void againstCage(const CageNumbers &n, CageClimber &c, const CageBody &body, bool touch)
    {
        if (c.settle > 0)
            return;
        if (c.outside)
        {
            const float topBottom = kMat + n.height - n.topHeight, behind = n.walls - n.wires;
            float move = -1e9f;
            for (int32_t i = 0; i < CAGE_ROOMS; ++i)
            {
                const float face = n.walls + behind + (c.writtenY + body.rooms[i].z >= topBottom ? n.topDepth : 0.0f);
                move = std::max(move, face - (body.rooms[i].x - n.room[i] + c.out));
            }
            if (move < 0.0f && !touch)
                return;
            c.out += std::clamp(move, -n.toCage, n.offCage);
            return;
        }
        float most = -1e9f;
        for (int32_t i = 0; i < CAGE_ROOMS; ++i)
            most = std::max(most, body.rooms[i].x + n.room[i]);
        const float move = n.wires - (most + c.out);
        if (move > 0.0f && !touch)
            return;
        c.out += std::clamp(move, -n.offCage, n.toCage);
    }

    // the bar's taps as the game counts a pin's: a press counts while fewer than two have counted in the window; the
    // window starts again each 6.0 of the game's time
    void countTap(CageClimber &c, const CagePictureIn &in)
    {
        if (in.pressed && c.tapsInWindow < 2)
        {
            ++c.taps;
            ++c.tapsInWindow;
        }
        c.tapWindow += in.step;
        if (c.tapWindow >= 6.0f)
        {
            c.tapWindow = 0.0f;
            c.tapsInWindow = 0;
        }
    }

    // over the top: down the outside a step at a time (each the same, as many as "a step up" fits best) until the drop,
    // which starts high enough for its last frame's lower ankle to land "ankles above the mat, standing" over the floor
    float dropStart(const CageNumbers &n, const CageHeights &h) { return kFloor + n.ankleUp - h.dropLanding; }
    void nextOutside(const CageNumbers &n, const CageHeights &h, CageClimber &c, CagePictureOut &out)
    {
        const float to = dropStart(n, h);
        if (c.stepsOutside > 0 && c.y - to > 0.1f)
        {
            const float step = (c.y - to) / static_cast<float>(c.stepsOutside);
            --c.stepsOutside;
            startPhase(c, out, CAGE_PHASE_DOWN_OUTSIDE, CAGE_CLIP_DOWN_BLAZIN, 0);
            c.toY = c.y - step;
        }
        else
            startPhase(c, out, CAGE_PHASE_DROP, CAGE_CLIP_DROP, 0);
    }

    // the first bar full: onto the top, the second bar starting from nothing
    void ontoTop(CageClimber &c, CagePictureOut &out, int32_t blend)
    {
        c.taps = c.tapsInWindow = 0;
        c.tapWindow = 0.0f;
        startPhase(c, out, CAGE_PHASE_ONTO_TOP, CAGE_CLIP_ONTO_TOP, blend);
    }

    // the second bar full (or ESCAPE off): up onto the truss from where the crouch has the anchor (kept: the truss's
    // clips are made for it)
    void ontoTruss(CageClimber &c, CagePictureOut &out)
    {
        c.dangles = 0;
        c.perchOut = c.out;
        c.perchAlong = c.along;
        c.perchY = c.y;
        startPhase(c, out, CAGE_PHASE_ONTO_TRUSS, CAGE_CLIP_ONTO_TRUSS, 1);
    }

    // off the truss (back or over): the anchor back where the truss's clips are made for, facing the cage, a cut (their
    // first frame is the resting pose turned round)
    void offTruss(CageClimber &c, CagePictureOut &out, int32_t phase, int32_t clip)
    {
        c.onTruss = 0;
        c.out = c.perchOut;
        c.along = c.perchAlong;
        c.y = c.perchY;
        startPhase(c, out, phase, clip, 0);
        c.fromOut = c.out;
    }

    // the phases the body crosses the wires, stands on the truss or falls back into the ring: not kept off the cage
    bool onTheTop(int32_t phase)
    {
        return phase == CAGE_PHASE_OVER || phase == CAGE_PHASE_ONTO_TRUSS || phase == CAGE_PHASE_ON_TRUSS ||
               phase == CAGE_PHASE_OFF_TRUSS || phase == CAGE_PHASE_OVER_TRUSS || phase == CAGE_PHASE_LOSE_GRIP ||
               phase == CAGE_PHASE_FALLING || phase == CAGE_PHASE_LAND;
    }

    bool canBeShaken(int32_t phase)
    {
        switch (phase)
        {
        case CAGE_PHASE_ONTO:
        case CAGE_PHASE_HANG:
        case CAGE_PHASE_STEP:
        case CAGE_PHASE_SLIP:
        case CAGE_PHASE_DANGLE:
        case CAGE_PHASE_RECOVER:
        case CAGE_PHASE_PULL_UP:
        case CAGE_PHASE_STRAIN:
        case CAGE_PHASE_PULL_BACK:
        case CAGE_PHASE_ONTO_TOP:
        case CAGE_PHASE_CROUCH:
        case CAGE_PHASE_OFF_TOP:
            return true;
        default:
            return false;
        }
    }

    // shaken off, or the dangle too many: losing the grip (the dangles in a row start again)
    void loseGrip(CageClimber &c, CagePictureOut &out)
    {
        c.dangles = 0;
        c.knocked = 0;
        c.fallSpeed = 0.0f;
        startPhase(c, out, CAGE_PHASE_LOSE_GRIP, CAGE_CLIP_LOSE_GRIP, 1);
    }

    // into the dangle (a shake's, or a step up with too little momentum): counted; the one that makes "dangles in a row
    // before a fall" is a fall instead; blend: eased in from the pose shown (shaken off the top's stages)
    void slip(const CageNumbers &n, CageClimber &c, CagePictureOut &out, int32_t fromShake, int32_t blend = 0)
    {
        c.knocked = 0;
        if (++c.dangles >= std::max(1, n.danglesToFall))
        {
            loseGrip(c, out);
            return;
        }
        c.shakeDangle = fromShake;
        startPhase(c, out, CAGE_PHASE_SLIP, CAGE_CLIP_SLIP, blend);
    }

    // a shaker's bar full on this climber's side: dangling, off the cage when dangling already; pulled up or crouched
    // on top (stage 1 or 2): straight into the dangle at the top hold, eased in from that pose (the user: no step back
    // down first), the stages then from the first again; getting on, a step, on the way back down: the dangle when at
    // the hang
    void shaken(const CageNumbers &n, CageClimber &c, CagePictureOut &out)
    {
        switch (c.phase)
        {
        case CAGE_PHASE_SLIP:
        case CAGE_PHASE_DANGLE:
            loseGrip(c, out);
            break;
        case CAGE_PHASE_HANG:
            slip(n, c, out, 1);
            break;
        case CAGE_PHASE_PULL_UP:
        case CAGE_PHASE_STRAIN:
        case CAGE_PHASE_ONTO_TOP:
        case CAGE_PHASE_CROUCH:
            slip(n, c, out, 1, 1);
            break;
        default:
            c.knocked = 1;
            break;
        }
        c.taps = c.tapsInWindow = 0;
        c.tapWindow = 0.0f;
    }

    // the sides' outward normals: 4 sides at 0, 90, 180, 270 degrees; 6 sides atan2(x, z) = 60 - 60k degrees
    void sideNormal(int32_t sides, float x, float z, float &nx, float &nz)
    {
        const int32_t count = sides == 6 ? 6 : 4;
        float best = -1e9f;
        nx = 0.0f;
        nz = 1.0f;
        for (int32_t k = 0; k < count; ++k)
        {
            const float a = (count == 6 ? 60.0f - 60.0f * k : 90.0f * k) * kPi / 180.0f;
            const float d = x * std::sin(a) + z * std::cos(a);
            if (d > best)
            {
                best = d;
                nx = std::sin(a);
                nz = std::cos(a);
            }
        }
    }

    // a fall's pull down, in the game's units a picture squared (9.8 m/s^2, a wrestler of 22 about 1.85 m, 60 a second)
    constexpr float kGravity = 9.8f * (22.0f / 1.85f) / 3600.0f;

    int32_t upClip(float momentum)
    {
        return CAGE_CLIP_UP_SLOWEST + std::clamp(static_cast<int32_t>(momentum / 20.0f), 0, 4);
    }

    void step(const CageNumbers &n, CageClimber &c, const CagePictureIn &in, CagePictureOut &out, int32_t up, int32_t side)
    {
        const bool fast = in.blazin != 0;
        c.fromAlong = c.toAlong = c.along;
        c.fromY = c.toY = c.y;
        int32_t clip = CAGE_CLIP_HANG;
        if (up > 0)
        {
            c.toY = c.y + n.stepUp;
            ++c.hold;
            clip = fast ? CAGE_CLIP_UP_BLAZIN : upClip(in.momentum);
            if (!fast)
                out.momentumCost = n.stepCost;
        }
        else if (up < 0)
        {
            c.toY = c.y - n.stepUp;
            --c.hold;
            clip = fast ? CAGE_CLIP_DOWN_BLAZIN : CAGE_CLIP_DOWN;
        }
        else
        {
            c.toAlong = std::clamp(c.along + n.stepSide * static_cast<float>(side), -(c.halfSide - n.cornerRoom),
                                   c.halfSide - n.cornerRoom);
            // the clip's "right" is the climber's right, facing the cage (outward): the side's way turned the other way
            const bool climbersRight = static_cast<float>(side) * (c.tx * -c.nz + c.tz * c.nx) > 0.0f;
            clip = climbersRight ? (fast ? CAGE_CLIP_RIGHT_BLAZIN : CAGE_CLIP_RIGHT)
                                 : (fast ? CAGE_CLIP_LEFT_BLAZIN : CAGE_CLIP_LEFT);
        }
        startPhase(c, out, CAGE_PHASE_STEP, clip, 0); // fromY: the anchor's height now, the step's start
    }

    // the body's facing in the world, from the hips (the wrestler's left the model's +X, its front +Z)
    float bodyFacing(const CageClimber &c, const CageBody &body)
    {
        const float dOut = body.hipLeft.x - body.hipRight.x, dAlong = body.hipLeft.y - body.hipRight.y;
        const float ax = dOut * c.nx + dAlong * c.tx, az = dOut * c.nz + dAlong * c.tz;
        float f = std::atan2(ax, az) * 180.0f / kPi - 90.0f;
        if (f <= -180.0f)
            f += 360.0f;
        return f;
    }
}

extern "C" {

int32_t cage_rules_version(void) { return CAGE_RULES_VERSION; }

void cage_rules_standard_numbers(CageNumbers *numbers)
{
    using namespace cage_rules_inside;
    CageNumbers n{};
    n.height = 44.0f;
    n.walls = 40.0f;
    n.stepUp = 4.8f;
    n.steps = 2;
    n.stepSide = 5.0f;
    n.cornerRoom = 8.0f;
    n.stepCost = 3.0f;
    n.danglePictures = 90;
    n.wires = 39.8f;
    n.toCage = 0.5f;
    n.offCage = 2.0f;
    n.ankleLeast = 1.3f;
    n.ankleUp = 1.7f;
    n.ropePress = 1.0f;
    const float rooms[CAGE_ROOMS] = {2.2f, 1.8f, 1.0f, 0.6f, 1.0f, 0.6f, 1.8f, 1.2f, 1.8f, 1.2f};
    std::copy(rooms, rooms + CAGE_ROOMS, n.room);
    n.tapsPart1 = 30;
    n.tapsPart2 = 30;
    n.downPictures = 20;
    n.topDepth = 4.7f;
    n.topHeight = 4.7f;
    n.diveOut = 2.0f;
    n.diveFeet = 1.0f;
    n.restPictures = 30;
    n.shakeDanglePictures = 300;
    n.danglesToFall = 3;
    n.fallDamage = 10.0f;
    n.shakeDangleMomentum = 10.0f;
    n.shakeOffMomentum = 20.0f;
    *numbers = n;
}

// - getting on: the anchor under the mat each frame, set so a foot with a rope bend (16-24: the top / middle / bottom
//   rope to the right, the left or both feet) stands on that rope pressing it "a foot on a rope presses it down" (two
//   feet on ropes at once: halfway between); between bends a straight line (from nothing at frame 0), after the last its
//   value held; never so low an ankle comes nearer the mat than "ankles above the mat, least"; smoothed over 5 frames
//   (f-2 .. f+2), the ankle rule again. Its last frame's is where the rope hold is;
// - the hang starts with the root where getting on's last frame left it;
// - letting go: the hips' height each frame, the frame they are lowest (the landing) and the lower ankle then;
// - the hang's first frame's hips (over the top ends in the hang turned round: turned there) and the drop's last frame's
//   lower ankle (the drop starts that high over the floor).
int32_t cage_rules_work_out_heights(const CageNumbers *numbers, const CageClipFrame *onto, int32_t ontoFrames,
                                   const CageRopeBend *bends, int32_t bendCount, const CageClipFrame *hangFirst,
                                   const CageClipFrame *letGo, int32_t letGoFrames, const CageClipFrame *dropLast,
                                   const CageClipFrame *trussLast, const CageClipFrame *restFirst,
                                   const CageClipFrame *gripLast, const CageClipFrame *fallPose,
                                   const CageClipFrame *landFirst, CageHeights *heights)
{
    using namespace cage_rules_inside;
    if (!numbers || !onto || !hangFirst || !letGo || !dropLast || !trussLast || !restFirst || !gripLast || !fallPose ||
        !landFirst ||
        !heights || ontoFrames <= 0 ||
        ontoFrames > CAGE_MOST_FRAMES ||
        letGoFrames <= 0 || letGoFrames > CAGE_MOST_FRAMES)
        return 0;
    const CageNumbers &n = *numbers;
    CageHeights &h = *heights;
    h = CageHeights{};
    const int32_t frames = ontoFrames;

    // getting on
    std::vector<float> sum(frames, 0.0f), count(frames, 0.0f);
    for (int32_t b = 0; b < bendCount && bends; ++b)
    {
        const CageRopeBend &w = bends[b];
        if (w.what < 16 || w.what > 24 || w.from < 0)
            continue;
        const int32_t level = (w.what - 16) % 3, foot = (w.what - 16) / 3; // foot: 0 right, 1 left, 2 both
        for (int32_t f = w.from; f <= std::min(w.to, frames - 1); ++f)
        {
            const float y = foot == 0 ? onto[f].ankleRight.y
                            : foot == 1 ? onto[f].ankleLeft.y
                                        : 0.5f * (onto[f].ankleLeft.y + onto[f].ankleRight.y);
            sum[f] += kMat + y - (kRopeY[level] - n.ropePress);
            count[f] += 1.0f;
        }
    }
    std::vector<std::pair<int32_t, float>> known;
    for (int32_t f = 0; f < frames; ++f)
        if (count[f] > 0.0f)
            known.push_back({f, sum[f] / count[f]});
    if (!known.empty() && known.front().first > 0)
        known.insert(known.begin(), {0, 0.0f});
    std::vector<float> drop(frames, 0.0f);
    for (int32_t f = 0, k = 0; f < frames && !known.empty(); ++f)
    {
        while (k + 1 < static_cast<int32_t>(known.size()) && known[k + 1].first <= f)
            ++k;
        if (f <= known[k].first || k + 1 >= static_cast<int32_t>(known.size()))
            drop[f] = known[k].second;
        else
        {
            const float t = static_cast<float>(f - known[k].first) / static_cast<float>(known[k + 1].first - known[k].first);
            drop[f] = known[k].second + (known[k + 1].second - known[k].second) * t;
        }
    }
    auto floorCap = [&]() {
        for (int32_t f = 0; f < frames; ++f)
            drop[f] = std::min(drop[f], std::min(onto[f].ankleLeft.y, onto[f].ankleRight.y) - n.ankleLeast);
    };
    floorCap();
    std::vector<float> smooth(frames);
    for (int32_t f = 0; f < frames; ++f)
    {
        float total = 0.0f;
        int32_t m = 0;
        for (int32_t d = -2; d <= 2; ++d)
        {
            const int32_t g = f + d;
            if (g >= 0 && g < frames)
            {
                total += drop[g];
                ++m;
            }
        }
        smooth[f] = total / static_cast<float>(m);
    }
    drop = smooth;
    floorCap();
    h.ontoFrames = frames;
    std::copy(drop.begin(), drop.end(), h.drop);

    // the hang's start
    const CageClipFrame &end = onto[frames - 1];
    h.hang[0] = end.root.x - hangFirst->root.x;
    h.hang[1] = end.root.y - hangFirst->root.y;
    h.hang[2] = end.root.z - hangFirst->root.z;

    // letting go
    h.letGoFrames = letGoFrames;
    h.landing = 0;
    for (int32_t f = 0; f < letGoFrames; ++f)
    {
        h.letGoHips[f] = letGo[f].hips.y;
        if (h.letGoHips[f] < h.letGoHips[h.landing])
        {
            h.landing = f;
            h.landingAnkle = std::min(letGo[f].ankleLeft.y, letGo[f].ankleRight.y);
        }
    }
    if (h.landing == 0)
        h.landingAnkle = std::min(letGo[0].ankleLeft.y, letGo[0].ankleRight.y);

    // the top and the outside
    h.hangHips[0] = hangFirst->hips.x;
    h.hangHips[1] = hangFirst->hips.y;
    h.hangHips[2] = hangFirst->hips.z;
    h.dropLanding = std::min(dropLast->ankleLeft.y, dropLast->ankleRight.y);

    // onto the truss: the resting clip plays facing the ring (its x the side's way back, its z inward), its first root
    // where onto the truss's last root is (that clip facing the cage: x along, z out)
    h.truss[0] = trussLast->root.x + restFirst->root.x;
    h.truss[1] = trussLast->root.y - restFirst->root.y;
    h.truss[2] = trussLast->root.z + restFirst->root.z;

    // shaken off: falling's root and landing's first root
    h.fallRootY = fallPose->root.y;
    h.landRootY = landFirst->root.y;
    h.grip[0] = gripLast->root.x - fallPose->root.x;
    h.grip[1] = gripLast->root.y - fallPose->root.y;
    h.grip[2] = gripLast->root.z - fallPose->root.z;
    return 1;
}

int32_t cage_rules_can_be_shaken(const CageClimber *climber)
{
    using namespace cage_rules_inside;
    return climber && canBeShaken(climber->phase) ? 1 : 0;
}

int32_t cage_rules_shake_result(const CageNumbers *numbers, const CageClimber *climber)
{
    using namespace cage_rules_inside;
    if (!numbers || !climber || !canBeShaken(climber->phase))
        return 0;
    // the shake on a copy: what it starts is what it does
    CageClimber c = *climber;
    CagePictureOut out{};
    shaken(*numbers, c, out);
    if (c.phase == CAGE_PHASE_LOSE_GRIP)
        return 2;
    if (c.phase == CAGE_PHASE_SLIP)
        return 1;
    // knocked during a step, getting on or going back down: the dangle at the hang, unless it is one too many
    if (c.knocked && !climber->knocked)
        return c.dangles + 1 >= std::max(1, numbers->danglesToFall) ? 2 : 1;
    return 0;
}

void cage_rules_side(int32_t sides, float x, float z, float *nx, float *nz)
{
    using namespace cage_rules_inside;
    float a = 0.0f, b = 1.0f;
    sideNormal(sides, x, z, a, b);
    if (nx)
        *nx = a;
    if (nz)
        *nz = b;
}

float cage_rules_drop_at(const CageHeights *heights, float frame)
{
    using namespace cage_rules_inside;
    return heights ? at(heights->drop, heights->ontoFrames, frame) : 0.0f;
}

float cage_rules_rope_hold(const CageHeights *heights)
{
    using namespace cage_rules_inside;
    if (!heights || heights->ontoFrames <= 0)
        return kMat;
    return kMat - heights->drop[heights->ontoFrames - 1] + heights->hang[1];
}

void cage_rules_start(const CageNumbers *numbers, int32_t sides, float x, float y, float z, CageClimber *climber,
                      CagePictureOut *out)
{
    using namespace cage_rules_inside;
    const CageNumbers &n = *numbers;
    CageClimber c{};
    CagePictureOut o{};
    sideAt(n, c, sides, x, z);
    c.out = x * c.nx + z * c.nz;
    c.along = std::clamp(x * c.tx + z * c.tz, -(c.halfSide - n.cornerRoom), c.halfSide - n.cornerRoom);
    c.y = c.fromY = y;
    c.startOut = c.out;
    startPhase(c, o, CAGE_PHASE_ONTO, CAGE_CLIP_ONTO, 1); // blended in from the stance
    handOut(c, o);
    *climber = c;
    *out = o;
}

int32_t cage_rules_picture(const CageNumbers *numbers, const CageHeights *heights, CageClimber *climber,
                           const CagePictureIn *in, CagePictureOut *out)
{
    using namespace cage_rules_inside;
    const CageNumbers &n = *numbers;
    const CageHeights &h = *heights;
    CageClimber &c = *climber;
    CagePictureOut o{};
    if (c.phase == CAGE_PHASE_NONE)
    {
        *out = o;
        return 0;
    }
    // the body's points are (out, along, up) from the anchor last handed out (CagePoint's x, y, z)
    const CageBody &body = in->body;
    if (c.settle > 0)
        --c.settle;
    ++c.pictures;
    const bool started = c.pictures > 1; // the clip asked for last picture is playing
    // a picture of falling (one pose) back into the ring: down by gravity, back over the ropes to where the climb started
    // as it goes; when the root is as low as landing starts with the anchor on the mat, the landing, there
    auto fallStep = [&]()
    {
        const float landAt = kMat + h.landRootY - h.fallRootY;
        c.fallSpeed += kGravity * in->step;
        c.y -= c.fallSpeed * in->step;
        const float t = c.fallFromY > landAt ? std::clamp((c.fallFromY - c.y) / (c.fallFromY - landAt), 0.0f, 1.0f) : 1.0f;
        const float toOut = std::min(c.startOut, c.fallFromOut); // back into the ring, never out again
        c.out = c.fallFromOut + (toOut - c.fallFromOut) * t;
        if (c.y <= landAt)
        {
            c.y = kMat;
            c.out = toOut;
            startPhase(c, o, CAGE_PHASE_LAND, CAGE_CLIP_LAND, 0);
        }
    };
    // a shaker's bar full on this side: that comes first, this picture's own step not taken
    const bool knockedNow = in->shaken && canBeShaken(c.phase);
    if (knockedNow)
        shaken(n, c, o);
    switch (knockedNow ? CAGE_PHASE_NONE : c.phase)
    {
    case CAGE_PHASE_ONTO:
        // up the ropes (the drop worked out from the clip and its rope bends; nothing before the clip plays: the stance's
        // frame is not its own)
        if (started)
        {
            c.y = c.fromY - at(h.drop, h.ontoFrames, in->frame);
            aboveMat(n, c, in->body);
        }
        if (started && in->clipEnded)
        {
            // the hang starts where getting on ended, this picture: the anchor moved as the root moves between the two
            // clips (the body still shows getting on's last pose for one more picture: settle)
            c.along += h.hang[0];
            c.y += h.hang[1];
            c.out += h.hang[2];
            c.settle = 1;
            startPhase(c, o, CAGE_PHASE_HANG, hangClip(c.hold), 0);
        }
        break;
    case CAGE_PHASE_HANG:
        if (!started)
            break;
        if (c.knocked)
        {
            slip(n, c, o, 1); // shaken on the way here
            break;
        }
        if (in->up)
        {
            if (c.hold >= n.steps)
            {
                // the top hold: pulling up to look over (the bar starts again from nothing each time)
                c.taps = c.tapsInWindow = 0;
                c.tapWindow = 0.0f;
                startPhase(c, o, CAGE_PHASE_PULL_UP, CAGE_CLIP_PULL_UP, 0);
                break;
            }
            if (!in->blazin && in->momentum < n.stepCost)
                slip(n, c, o, 0);
            else
                step(n, c, *in, o, 1, 0);
        }
        else if (in->down)
        {
            if (c.hold > 0)
                step(n, c, *in, o, -1, 0);
            else
            {
                c.dangles = 0;
                startPhase(c, o, CAGE_PHASE_LET_GO, CAGE_CLIP_LET_GO, 0);
            }
        }
        else if (in->along != 0)
        {
            const int32_t side = in->along > 0 ? 1 : -1;
            const float to = c.along + n.stepSide * static_cast<float>(side);
            if (std::fabs(to) <= c.halfSide - n.cornerRoom + 0.01f)
                step(n, c, *in, o, 0, side);
        }
        break;
    case CAGE_PHASE_STEP:
    {
        const float t = in->progress;
        c.along = c.fromAlong + (c.toAlong - c.fromAlong) * t;
        c.y = c.fromY + (c.toY - c.fromY) * t;
        if (started && in->clipEnded)
        {
            c.along = c.toAlong;
            c.y = c.toY;
            startPhase(c, o, CAGE_PHASE_HANG, hangClip(c.hold), 0);
        }
        break;
    }
    case CAGE_PHASE_SLIP:
        if (started && in->clipEnded)
            startPhase(c, o, CAGE_PHASE_DANGLE, CAGE_CLIP_DANGLE, 0);
        break;
    case CAGE_PHASE_DANGLE:
        // a shake's dangle its own length ("dangling after a shake, pictures"), a slip's "dangling, pictures"
        if (c.pictures >= (c.shakeDangle ? n.shakeDanglePictures : n.danglePictures))
            startPhase(c, o, CAGE_PHASE_RECOVER, CAGE_CLIP_RECOVER, 0);
        break;
    case CAGE_PHASE_RECOVER:
        if (started && in->clipEnded)
            startPhase(c, o, CAGE_PHASE_HANG, hangClip(c.hold), 0);
        break;
    case CAGE_PHASE_LET_GO:
        // raised as the clip's hips fall, so the ankles reach "ankles above the mat, standing" at its landing; never down
        if (started)
        {
            const float top = at(h.letGoHips, h.letGoFrames, 0.0f);
            const float low = at(h.letGoHips, h.letGoFrames, static_cast<float>(h.landing));
            const float fall = top > low ? (top - at(h.letGoHips, h.letGoFrames, in->frame)) / (top - low) : 1.0f;
            const float lift = std::max(0.0f, (kMat + n.ankleUp) - (c.fromY + h.landingAnkle));
            c.raised = std::max(c.raised, lift * std::clamp(fall, 0.0f, 1.0f));
            c.y = c.fromY + c.raised;
            aboveMat(n, c, in->body);
        }
        if (started && in->clipEnded)
        {
            // standing where the landing left the body (its hips over the anchor), facing the way the body ended up
            c.phase = CAGE_PHASE_NONE;
            c.out = c.writtenOut + in->body.hips.x;
            c.along = c.writtenAlong + in->body.hips.y;
            c.y = kMat;
            handOut(c, o);
            o.facing = bodyFacing(c, in->body);
            o.landed = 1;
            *out = o;
            return 1;
        }
        break;
    case CAGE_PHASE_PULL_UP:
    case CAGE_PHASE_STRAIN:
        // ESCAPE on: the first bar, filled by taps while straining; full (or ESCAPE off, once pulled up) = onto the top;
        // down = back to the hang
        if (in->escape)
            countTap(c, *in);
        if (!started)
            break;
        if (in->down)
            startPhase(c, o, CAGE_PHASE_PULL_BACK, CAGE_CLIP_PULL_BACK, 1);
        else if (c.phase == CAGE_PHASE_PULL_UP && in->clipEnded)
        {
            if (!in->escape || c.taps >= n.tapsPart1)
                ontoTop(c, o, 0);
            else
                startPhase(c, o, CAGE_PHASE_STRAIN, CAGE_CLIP_STRAIN, 0);
        }
        else if (c.phase == CAGE_PHASE_STRAIN && c.taps >= n.tapsPart1)
            ontoTop(c, o, 1);
        break;
    case CAGE_PHASE_PULL_BACK:
    case CAGE_PHASE_OFF_TOP:
        // back to the hang (shaken: straight into the dangle there; the stages start again from the first)
        if (started && in->clipEnded)
        {
            if (c.knocked)
                slip(n, c, o, 1);
            else
                startPhase(c, o, CAGE_PHASE_HANG, hangClip(c.hold), 0);
        }
        break;
    case CAGE_PHASE_ONTO_TOP:
    case CAGE_PHASE_CROUCH:
        // ESCAPE on: the second bar, from climbing on through crouching there; full (or ESCAPE off, once on) = resting on
        // top (the crouch's loop plays on); down = back to the hang
        if (in->escape)
            countTap(c, *in);
        if (!started)
            break;
        if (in->escape && in->down)
            startPhase(c, o, CAGE_PHASE_OFF_TOP, CAGE_CLIP_OFF_TOP, 1);
        else if (c.phase == CAGE_PHASE_ONTO_TOP && in->clipEnded)
        {
            if (!in->escape || c.taps >= n.tapsPart2)
                ontoTruss(c, o);
            else
                startPhase(c, o, CAGE_PHASE_CROUCH, CAGE_CLIP_ON_TOP, 0);
        }
        else if (c.phase == CAGE_PHASE_CROUCH && c.taps >= n.tapsPart2)
            ontoTruss(c, o);
        break;
    case CAGE_PHASE_ONTO_TRUSS:
        // up onto the truss, turning to face the ring (the clip carries the body from the crouch's anchor); at its end
        // the anchor moved onto the truss and turned, the resting clip played from there, a cut (it starts on the last
        // frame's pose)
        if (started && in->clipEnded)
        {
            c.onTruss = 1;
            c.along += h.truss[0];
            c.y += h.truss[1];
            c.out += h.truss[2];
            startPhase(c, o, CAGE_PHASE_ON_TRUSS, CAGE_CLIP_TRUSS_REST, 0);
        }
        break;
    case CAGE_PHASE_ON_TRUSS:
        // resting on the truss, facing the ring: cross the dive (the caller's), triangle over the top (ESCAPE on), a tap
        // of down back off; getting up there was the pause, so the buttons count from its first picture
        if (!started)
            break;
        if (in->cross)
            o.dive = 1;
        else if (in->triangle && in->mayGoOver)
            offTruss(c, o, CAGE_PHASE_OVER_TRUSS, CAGE_CLIP_OVER_TRUSS);
        else if (in->down)
            offTruss(c, o, CAGE_PHASE_OFF_TRUSS, CAGE_CLIP_OFF_TRUSS);
        break;
    case CAGE_PHASE_OFF_TRUSS:
        // back off the truss to the crouch, then down from the top to the hang
        if (started && in->clipEnded)
            startPhase(c, o, CAGE_PHASE_OFF_TOP, CAGE_CLIP_OFF_TOP, 1);
        break;
    case CAGE_PHASE_ON_TOP:
        // resting on top: cross the dive (the caller's); down with triangle over the top; down held on its own back down
        // inside
        if (!started)
            break;
        {
            const bool settled = c.pictures > n.restPictures; // the second bar's taps (cross, triangle) don't count yet
            if (settled && in->cross)
                o.dive = 1;
            else if (in->down)
            {
                ++c.downHeld;
                if (settled && in->triangle && in->mayGoOver)
                {
                    startPhase(c, o, CAGE_PHASE_OVER, CAGE_CLIP_OVER, 1);
                    c.fromOut = c.out;
                }
                else if (c.downHeld >= n.downPictures)
                    startPhase(c, o, CAGE_PHASE_OFF_TOP, CAGE_CLIP_OFF_TOP, 1);
            }
            else
                c.downHeld = 0;
        }
        break;
    case CAGE_PHASE_OVER:
    case CAGE_PHASE_OVER_TRUSS:
        // (over from the truss: its clip carries the anchor's slide itself, so nothing is slid)
        // the clip carries the body over the top and down the far side, ending in the hang turned round (made for a top no
        // deeper than a bar): across the middle of it the anchor slides out over the top's depth (and the wires' room on
        // both faces), the hands crossing the top, so the body comes down on the outer face. At its end the escape is
        // told, the last frame held a picture (the caller ends the match); then the climber turns to face the cage from
        // outside, the anchor put where the hang's hips meet the body's
        if (started && !in->clipEnded && c.phase == CAGE_PHASE_OVER)
        {
            const float t = std::clamp((in->progress - 0.25f) / 0.6f, 0.0f, 1.0f);
            c.out = c.fromOut + (n.topDepth + 2.0f * (n.walls - n.wires)) * t * t * (3.0f - 2.0f * t);
        }
        if (!(started && in->clipEnded))
            break;
        if (!c.escaped)
        {
            c.escaped = 1;
            o.escaped = 1;
            break;
        }
        {
            const float hipsOut = c.writtenOut + body.hips.x, hipsAlong = c.writtenAlong + body.hips.y,
                        hipsUp = c.writtenY + body.hips.z;
            c.outside = 1; // facing in: the clip's forward is the side's inward way, its left the side's way back
            c.out = hipsOut + h.hangHips[2];
            c.along = hipsAlong + h.hangHips[0];
            c.y = hipsUp - h.hangHips[1];
            c.settle = 1;
            c.stepsOutside = std::max(0, static_cast<int32_t>(std::lround((c.y - dropStart(n, h)) / n.stepUp)));
            nextOutside(n, h, c, o);
        }
        break;
    case CAGE_PHASE_LOSE_GRIP:
    {
        // losing the grip where the climber was (the clip falls part of the way itself, faster and faster at its end);
        // at its end falling, the anchor moved so falling's pose starts where losing the grip left the body, at the speed
        // its hips were dropping (a fall from rest there made the body all but stop in the air for a moment). The speed
        // each picture from the hips (their height in the world: the anchor last handed out and the body's up from it),
        // the fastest once the blend into the clip is over (the clip drops faster and faster to its end; its last frame,
        // held, would read slow)
        const float hips = c.writtenY + body.hips.z;
        if (c.pictures > 13)
            c.fallSpeed = std::max(c.fallSpeed, (c.fallFromY - hips) / std::max(0.01f, in->step));
        c.fallFromY = hips;
        if (started && in->clipEnded)
        {
            c.along += h.grip[0];
            c.y += h.grip[1];
            c.out += h.grip[2];
            c.fallFromOut = c.out;
            c.fallFromY = c.y;
            startPhase(c, o, CAGE_PHASE_FALLING, CAGE_CLIP_FALLING, 0);
            // this picture already falls (a first picture of falling standing still showed as a stop in the air)
            fallStep();
        }
        break;
    }
    case CAGE_PHASE_FALLING:
        fallStep();
        break;
    case CAGE_PHASE_LAND:
        // flat on the mat; at its end lying there, face up, facing as the climb did (the caller: the lying state)
        if (started && in->clipEnded)
        {
            c.phase = CAGE_PHASE_NONE;
            handOut(c, o);
            o.landed = 1;
            o.lying = 1;
            *out = o;
            return 1;
        }
        break;
    case CAGE_PHASE_DOWN_OUTSIDE:
        c.y = c.fromY + (c.toY - c.fromY) * in->progress;
        if (started && in->clipEnded)
        {
            c.y = c.toY;
            nextOutside(n, h, c, o);
        }
        break;
    case CAGE_PHASE_DROP:
        // the clip drops the body to the floor; at its end standing there, outside the cage
        if (started && in->clipEnded)
        {
            c.phase = CAGE_PHASE_NONE;
            c.out = c.writtenOut + body.hips.x;
            c.along = c.writtenAlong + body.hips.y;
            c.y = kFloor;
            handOut(c, o);
            o.facing = bodyFacing(c, body);
            o.landed = 1;
            o.outside = 1;
            *out = o;
            return 1;
        }
        break;
    default:
        break;
    }
    if (!onTheTop(c.phase))
        againstCage(n, c, body, c.phase != CAGE_PHASE_ONTO && c.phase != CAGE_PHASE_LET_GO && c.phase != CAGE_PHASE_DROP);
    const bool part1 = c.phase == CAGE_PHASE_PULL_UP || c.phase == CAGE_PHASE_STRAIN;
    const bool part2 = c.phase == CAGE_PHASE_ONTO_TOP || c.phase == CAGE_PHASE_CROUCH;
    if (in->escape && (part1 || part2))
    {
        const int32_t needed = part1 ? n.tapsPart1 : n.tapsPart2;
        o.bar = 1;
        o.barNeeded = static_cast<float>(needed);
        o.barLeft = static_cast<float>(std::max(0, needed - c.taps));
    }
    handOut(c, o);
    o.clip = c.clip;
    o.blend = c.clipBlend;
    c.clip = 0;
    *out = o;
    return 1;
}

} // extern "C"
