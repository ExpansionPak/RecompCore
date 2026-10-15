// SPDX-License-Identifier: GPL-3.0-or-later
// Fastmem fault path: accesses that land on unmapped pages of the view must
// reach external_read/external_write exactly as the checked path would
// (same address, size and value), and register results must be right.
#include "core/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static u8 *view;
static u32 last_ea; static u64 last_val; static u8 last_size; static int nwrites, nreads;

static u64 ext_read(CPUState *c, u32 ea, u8 size) {
  (void)c; nreads++; last_ea = ea; last_size = size;
  u64 v = (u64)ea * 0x9E3779B97F4A7C15ull ^ size;
  return size == 8 ? v : v & ((1ull << (8 * size)) - 1);
}
static void ext_write(CPUState *c, u32 ea, u64 v, u8 size) { (void)c; nwrites++; last_ea = ea; last_val = v; last_size = size; }
static void *ext_pointer(CPUState *c, u32 ea, u32 size) { (void)c; return (ea == 0xFFFFF000u && size == 0) ? view : NULL; }

static u64 rng = 88172645463325252ull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)(rng >> 16); }

static u32 pick(void) {
  switch (rnd() % 5) {
  case 0: return 0x90000000u + (rnd() & 0xFFFFFu);           // unmapped
  case 1: return 0x817FFFF8u + (rnd() % 8);                   // straddles end of RAM
  case 2: return 0x00000000u + (rnd() & 0xFFFu);              // null page
  case 3: return 0xD0000000u + (rnd() & 0xFFFFu);             // unmapped high
  default: return 0x81000000u + (rnd() & 0xFFFFu);            // RAM (direct)
  }
}

int main(void) {
  CPUState *c = calloc(1, sizeof *c);
  view = VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
  if (!c || !view || !VirtualAlloc(view + 0x80000000u, GC_MAIN_RAM_SIZE, MEM_COMMIT, PAGE_READWRITE))
    return 2;
  c->ram = view + 0x80000000u; c->ram_size = GC_MAIN_RAM_SIZE;
  c->external_read = ext_read; c->external_write = ext_write; c->external_pointer = ext_pointer;
  (void)mem_read32(c, 0xCC000000u);  // MMIO: slow path, which asks for the view
  if (g_dolrecomp_fm_min != 0x06000000u) { printf("fastmem did not come on\n"); return 1; }
  /* Even a mapped MMIO page must use the external hooks, never raw memory. */
  if (!VirtualAlloc(view + 0xCC000000u, 4096, MEM_COMMIT, PAGE_READWRITE))
    return 4;
  nreads = nwrites = 0;
  mem_write32(c, 0xCC000000u, 0x87654321u);
  u64 mmio = mem_read32(c, 0xCC000000u);
  u64 expected_mmio = ext_read(c, 0xCC000000u, 4);
  if (nwrites != 1 || nreads != 2 || mmio != expected_mmio ||
      read_be32(view + 0xCC000000u) != 0)
    return 5;
  /* The uncached alias is intentionally not mapped: recovery must use RAM,
     rather than sending this ordinary boot-time store to external_write. */
  nreads = nwrites = 0;
  mem_write32(c, 0xC00000D0u, 0x12345678u);
  if (read_be32(c->ram + 0xD0) != 0x12345678u ||
      mem_read32(c, 0xC00000D0u) != 0x12345678u || nreads || nwrites)
    return 3;
  long bad = 0, faulted = 0, direct = 0;
  for (int t = 0; t < 400000; t++) {
    u32 a = pick();
    int size = 1 << (rnd() % 4);
    int ram = (a & ~0x40000000u) - 0x80000000u <= GC_MAIN_RAM_SIZE - (u32)size;
    int store = rnd() & 1;
    u64 v = ((u64)rnd() << 32) | rnd();
    int nr = nreads, nw = nwrites;
    if (store) {
      switch (size) {
      case 1: mem_write8(c, a, (u8)v); break;
      case 2: mem_write16(c, a, (u16)v); break;
      case 4: mem_write32(c, a, (u32)v); break;
      default: mem_write64(c, a, v); break;
      }
      u64 want = size == 8 ? v : v & ((1ull << (8 * size)) - 1);
      if (ram) {
        u64 got = size == 1 ? mem_read8(c, a) : size == 2 ? mem_read16(c, a) : size == 4 ? mem_read32(c, a) : mem_read64(c, a);
        if (got != want || nwrites != nw) { if (bad++ < 10) printf("ram store %08X size %d\n", a, size); }
        direct++;
      } else if (nwrites != nw + 1 || last_ea != a || last_size != size || last_val != want) {
        if (bad++ < 10) printf("store %08X size %d: writes %d ea %08X size %u val %llx want %llx\n", a, size,
                               nwrites - nw, last_ea, last_size, (unsigned long long)last_val, (unsigned long long)want);
      } else faulted++;
    } else {
      u64 got = size == 1 ? mem_read8(c, a) : size == 2 ? mem_read16(c, a) : size == 4 ? mem_read32(c, a) : mem_read64(c, a);
      if (ram) { direct++; continue; }
      u64 want = ext_read(c, a, (u8)size); nreads--;
      if (nreads != nr + 1 || last_ea != a || got != want) {
        if (bad++ < 10) printf("load %08X size %d: reads %d got %llx want %llx\n", a, size, nreads - nr,
                               (unsigned long long)got, (unsigned long long)want);
      } else faulted++;
    }
  }
  printf("faulted+emulated %ld, direct %ld, mismatches %ld\n", faulted, direct, bad);
  VirtualFree(view, 0, MEM_RELEASE);
  free(c);
  return bad != 0;
}
