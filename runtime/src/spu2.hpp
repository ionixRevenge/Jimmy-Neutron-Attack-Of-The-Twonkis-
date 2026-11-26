// SPU2: the PS2's sound processor, as libsd presents it.
//
// Two cores of 24 voices each play SPU2 ADPCM from 2 MB of sound RAM with pitch,
// ADSR envelopes and volume; each core also takes a stream of 16-bit stereo PCM
// from IOP RAM ("block transfer", used for movie audio). The mix is played
// through XAudio2 at 48 kHz, and the audio clock drives the SPU2: voices and
// block transfers advance only as samples are played.
//
// The libsd calls run on the game thread (from the IRX interpreter); mixing runs
// on the audio thread. Completion callbacks are handed back to the interpreter.
#pragma once
#include <cstdint>

namespace ps2::spu2 {

void start();   // open the audio device (once); silent if there is none

// libsd (sceSd*). `entry` is the libsd parameter word: core in bit 0, voice in
// bits 1-5, register in bits 8-15.
void sd_init(int flag);
void set_param(uint16_t entry, uint16_t value);
uint16_t get_param(uint16_t entry);
void set_switch(uint16_t entry, uint32_t value);
uint32_t get_switch(uint16_t entry);
void set_addr(uint16_t entry, uint32_t value);
uint32_t get_addr(uint16_t entry);
int voice_trans(int ch, uint16_t mode, uint32_t iop_addr, uint32_t spu_addr, uint32_t size);
int block_trans(int ch, uint16_t mode, uint32_t iop_addr, uint32_t size, uint32_t start);
uint32_t block_trans_status(int ch, int flag);
void set_trans_handler(int ch, uint32_t func, uint32_t arg);   // IRX function called when a transfer ends

void set_output_volume(float v);   // host-side master volume, 0..1

} // namespace ps2::spu2
