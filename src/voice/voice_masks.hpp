#pragma once

namespace voice {

enum FxKind {
    FX_NONE, FX_RING, FX_TREMOLO, FX_RADIO, FX_VIBRATO,
    FX_ANONYMOUS, FX_ALIEN, FX_ASTRONAUT, FX_DRUNK, FX_DUCK, FX_GHOST, FX_GLITCH, FX_MONSTER, FX_CROWD, FX_UNDERWATER,
    FX_MOUSE, FX_BABY, FX_GOBLIN, FX_GIANT, FX_ZOMBIE, FX_DARK_LORD, FX_CYBORG, FX_MEGAPHONE, FX_PHONE, FX_BEE, FX_WHISPER,
    FX_STRANGER
};

struct Anchor {
    bool on = false;
    double spread = 1.0, follow = 0.0, rough = 0.0;
};

struct Mask {
    const wchar_t* name;
    int  ratio;
    int  size;
    int  fx;
    double a, b;
    Anchor anchor = {};
};

static const Mask MASKS[] = {
    {L"Alien Brute",     850, 132, FX_RING,     90.0, 0.60},
    {L"Alien Infiltrator",1000,100, FX_RING,   150.0, 0.50},
    {L"Deep Voice",      790, 116, FX_NONE,      0.0, 0.0},
    {L"Demon",           600, 148, FX_NONE,      0.0, 0.0},
    {L"Dwarf",           820, 130, FX_NONE,      0.0, 0.0},
    {L"Elderly",         880, 112, FX_TREMOLO,   5.0, 0.55},
    {L"Elf",            1300,  84, FX_NONE,      0.0, 0.0},
    {L"Emo",             850, 114, FX_NONE,      0.0, 0.0},
    {L"Female to Male",  750, 122, FX_NONE,      0.0, 0.0},
    {L"From Up North",   880, 118, FX_NONE,      0.0, 0.0},
    {L"Male to Female", 1430,  84, FX_NONE,      0.0, 0.0},
    {L"Marine",          970, 100, FX_RADIO,     5.5, 0.0},
    {L"Munchkin",       1950,  68, FX_NONE,      0.0, 0.0},
    {L"Neutral",        1000, 100, FX_NONE,      0.0, 0.0},
    {L"Orc",             700, 134, FX_NONE,      0.0, 0.0},
    {L"Robot",          1000, 100, FX_RING,     75.0, 1.00},
    {L"Scrappy Kid",    1600,  78, FX_TREMOLO,   6.5, 0.28},
    {L"Unstable",       1850,  82, FX_VIBRATO,   5.5, 0.65},

    {L"Anonymous",      1000, 100, FX_ANONYMOUS, 0.0, 0.0},
    {L"Alien",          1220,  86, FX_ALIEN,     0.0, 0.0},
    {L"Astronaut",      1000, 100, FX_ASTRONAUT, 0.0, 0.0},
    {L"Drunk",           940, 106, FX_DRUNK,     0.0, 0.0},
    {L"Duck",           1450,  64, FX_DUCK,      0.0, 0.0},
    {L"Ghost",          1000, 100, FX_GHOST,     0.0, 0.0},
    {L"Glitch",         1000, 100, FX_GLITCH,    0.0, 0.0},
    {L"Monster",         560, 148, FX_MONSTER,   0.0, 0.0},
    {L"Three of Me",    1000, 100, FX_CROWD,     0.0, 0.0},
    {L"Underwater",      960, 108, FX_UNDERWATER, 0.0, 0.0},
    {L"Mickey Mouse",   2250,  72, FX_MOUSE,     0.0, 0.0},
    {L"Chipmunk",       2450,  60, FX_NONE,      0.0, 0.0},
    {L"Helium",         1100,  60, FX_NONE,      0.0, 0.0},
    {L"Baby",           1700,  70, FX_BABY,      0.0, 0.0},
    {L"Goblin",         1450, 126, FX_GOBLIN,    0.0, 0.0},
    {L"Giant",           560, 150, FX_GIANT,     0.0, 0.0},
    {L"Zombie",          760, 122, FX_ZOMBIE,    0.0, 0.0},
    {L"Dark Lord",       790, 134, FX_DARK_LORD, 0.0, 0.0},
    {L"Cyborg",         1000, 100, FX_CYBORG,    0.0, 0.0},
    {L"Megaphone",      1000, 100, FX_MEGAPHONE, 0.0, 0.0},
    {L"Phone Call",     1000, 100, FX_PHONE,     0.0, 0.0},
    {L"Bumblebee",      1650,  80, FX_BEE,       0.0, 0.0},
    {L"Whisper",        1000, 100, FX_WHISPER,   0.0, 0.0},
    {L"Stranger",        720, 120, FX_STRANGER,  0.0, 0.0, {true, 0.6, 0.3, 0.15}},
};
static const int NUM_MASKS = (int)(sizeof(MASKS) / sizeof(MASKS[0]));
static const int NUM_XFI_MASKS = 18;
static const int NUM_OWN_MASKS = NUM_MASKS - NUM_XFI_MASKS;
static const int MASK_NEUTRAL = 13;

static const int OWN_ORDER[] = {
    19, 18, 20, 31, 39, 29, 36, 35, 21, 22, 23, 33, 24, 32, 30, 37, 28, 25, 38, 41, 26, 27, 40, 34,
};
static_assert(sizeof(OWN_ORDER) / sizeof(OWN_ORDER[0]) == NUM_OWN_MASKS, "every own mask needs a place in the list");

}
