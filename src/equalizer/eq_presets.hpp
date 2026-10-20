#pragma once
#include "eq_config.hpp"

namespace eqb {

struct EqPreset { const wchar_t* name; int band[EQ_BANDS]; };
static const EqPreset EQ_PRESETS[] = {
    {L"Flat",      { 0,  0,  0,  0,  0,  0,  0}},
    {L"Bass",      { 8,  6,  3,  0,  0,  0,  0}},
    {L"Treble",    { 0,  0,  0,  0,  3,  6,  8}},
    {L"Vocal",     {-2, -1,  2,  4,  4,  2,  0}},
    {L"Rock",      { 5,  3, -1, -2,  1,  4,  6}},
    {L"Pop",       {-1,  2,  4,  4,  2,  0, -1}},
    {L"Jazz",      { 4,  3,  1,  2, -1,  2,  4}},
    {L"Classical", { 4,  3,  0,  0,  0,  2,  4}},
    {L"Dance",     { 7,  5,  1,  0,  2,  5,  6}},
    {L"Acoustic",  { 4,  3,  1,  1,  2,  3,  3}},
};
static const int NUM_EQ_PRESETS = (int)(sizeof(EQ_PRESETS) / sizeof(EQ_PRESETS[0]));

}
