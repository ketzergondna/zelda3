// Frame dump for the 3D viewer. See frame_dump.h and the "Formato de datos"
// section of CLAUDE.md for the file format.
// Everything here only reads emulated memory, so it can't change game behavior.
// The one exception is the room object replay, which runs Dungeon_LoadRoom on
// the live g_ram and then restores all of it (see FrameDump_CaptureRoomObjects).
#include "frame_dump.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "dungeon.h"
#include "snes/ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

// stb_image_write is third party code, keep its warnings out of our build.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "third_party/stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define DUMP_DIR "dump"

enum {
  kDumpBgTileVram = 0x2000,   // BG1/BG2 character data (BG12NBA = 0x22)
  kDumpObjTileVram = 0x4000,  // OBJ character data, both name tables (0x4000 and 0x5000)
  kDumpBgTiles = 1024,
  kDumpObjTiles = 512,
  kDumpAtlasColumns = 128,
  kDumpTilemapSize = 64,
};

static const uint8 kDumpSpriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static bool g_frame_dump_pending;
static uint8 *g_frame_dump_screen;
static size_t g_frame_dump_screen_size;

void FrameDump_Request() {
  g_frame_dump_pending = true;
}

bool FrameDump_IsPending() {
  return g_frame_dump_pending;
}

// ---- OAM owners (see frame_dump.h) ----
enum { kOamOwnerMaxDepth = 8 };
typedef struct OamOwnerScope {
  OamOwnerEntry owner;
  uint32 start;      // stamp when the scope began
  uint8 oam[0x200];  // copy of oam_buf (128 entries of 4 bytes)
  uint8 ext[0x80];   // copy of bytewise_extended_oam
} OamOwnerScope;
static bool g_oam_owner_enabled, g_oam_owner_valid;
static OamOwnerEntry g_oam_owner[128];
static uint32 g_oam_owner_stamp[128];
static uint32 g_oam_owner_seq;
static OamOwnerScope g_oam_owner_stack[kOamOwnerMaxDepth];
static int g_oam_owner_depth;

void OamOwner_SetEnabled(bool on) {
  g_oam_owner_enabled = on;
}

bool OamOwner_IsEnabled() {
  return g_oam_owner_enabled;
}

bool OamOwner_IsValid() {
  return g_oam_owner_valid;
}

const OamOwnerEntry *OamOwner_Entries() {
  return g_oam_owner;
}

void OamOwner_BeginFrame() {
  g_oam_owner_valid = g_oam_owner_enabled;
  if (!g_oam_owner_enabled)
    return;
  memset(g_oam_owner, 0, sizeof(g_oam_owner));  // kOamOwner_Other, body
  memset(g_oam_owner_stamp, 0, sizeof(g_oam_owner_stamp));
  g_oam_owner_seq = 0;
  g_oam_owner_depth = 0;
}

static void OamOwner_PushEntry(OamOwnerEntry owner) {
  if (!g_oam_owner_enabled)
    return;
  if (g_oam_owner_depth++ >= kOamOwnerMaxDepth)
    return;  // too deep: the outer scope gets these entries
  OamOwnerScope *sc = &g_oam_owner_stack[g_oam_owner_depth - 1];
  sc->owner = owner;
  sc->start = ++g_oam_owner_seq;
  memcpy(sc->oam, &g_ram[0x800], sizeof(sc->oam));
  memcpy(sc->ext, bytewise_extended_oam, sizeof(sc->ext));
}

void OamOwner_Push(int kind, int index, int type) {
  OamOwnerEntry owner = { (uint8)kind, (uint8)index, kOamPart_Body, (uint8)type };
  OamOwner_PushEntry(owner);
}

void OamOwner_PushPart(int part) {
  if (!g_oam_owner_enabled)
    return;
  int top = g_oam_owner_depth < kOamOwnerMaxDepth ? g_oam_owner_depth : kOamOwnerMaxDepth;
  OamOwnerEntry owner = { kOamOwner_Other, 0, kOamPart_Body, 0 };
  if (top > 0)
    owner = g_oam_owner_stack[top - 1].owner;
  owner.part = (uint8)part;
  OamOwner_PushEntry(owner);
}

void OamOwner_Pop() {
  if (!g_oam_owner_enabled || g_oam_owner_depth == 0)
    return;
  if (--g_oam_owner_depth >= kOamOwnerMaxDepth)
    return;
  const OamOwnerScope *sc = &g_oam_owner_stack[g_oam_owner_depth];
  for (int i = 0; i < 128; i++) {
    if (g_oam_owner_stamp[i] > sc->start)
      continue;  // claimed by a scope nested in this one
    if (memcmp(&g_ram[0x800 + i * 4], &sc->oam[i * 4], 4) == 0 && bytewise_extended_oam[i] == sc->ext[i])
      continue;  // this scope didn't write it
    g_oam_owner[i] = sc->owner;
    g_oam_owner_stamp[i] = ++g_oam_owner_seq;
  }
}

bool FrameDump_WantsOamOwners() {
  static int env = -1;
  if (env < 0) {
    const char *v = getenv("ZELDA3_OAM_OWNERS");
    env = !(v && v[0] == '0');
  }
  return g_frame_dump_pending && env;
}

uint8 *FrameDump_GetScreenBuffer(int width, int height) {
  size_t size = (size_t)width * height * 4;
  if (size > g_frame_dump_screen_size) {
    free(g_frame_dump_screen);
    g_frame_dump_screen = calloc(size, 1);
    g_frame_dump_screen_size = g_frame_dump_screen ? size : 0;
  }
  if (!g_frame_dump_screen) {
    fprintf(stderr, "Frame dump: out of memory\n");
    g_frame_dump_pending = false;
  }
  return g_frame_dump_screen;
}

// --- Room objects: sandbox replay of Dungeon_LoadRoom ---
// Layer index 0 is dung_bg1 ($7E4000, "bg1" in frame.json), 1 is dung_bg2
// ($7E2000, "bg2"). Note that dungeon.c's kDungeon_DrawObjectOffsets_BG1 points
// at $7E2000, so the log records the buffer that really changed.

enum {
  kRoomLogCells = 64 * 64,
  kRoomLogMaxEntries = 1024,
  kRoomLogMaxChanges = 65536,
};

typedef struct RoomLogEntry {
  uint8 kind, pass, len, raw[3];
  int change_start, change_count;  // range in RoomLog.changes
} RoomLogEntry;

typedef struct RoomLog {
  bool valid;
  bool overflow;     // too many entries or changed cells, the log is incomplete
  bool ram_restored; // g_ram is byte for byte what it was before the replay
  uint32 checksum_before, checksum_after;
  int room, layout;
  int pass;
  int open;          // entry between RoomLog_Begin and RoomLog_End, or -1
  int num_entries, num_changes;
  RoomLogEntry entries[kRoomLogMaxEntries];
  uint16 changes[kRoomLogMaxChanges];  // layer << 12 | cell, in drawing order
  int16 owner[2][kRoomLogCells];       // last entry that changed each cell, -1 = none
  uint16 before[2][kRoomLogCells];     // tilemaps before the open entry was drawn
  uint16 live[2][kRoomLogCells];       // tilemaps of the frame being dumped
  uint16 loaded[2][kRoomLogCells];     // tilemaps right after the replayed load
} RoomLog;

bool g_room_log_active;
static RoomLog g_room_log;
static uint8 g_room_log_saved_ram[sizeof(g_ram)];

static uint16 *RoomLogLayer(int layer) {
  return layer ? dung_bg2 : dung_bg1;
}

static uint32 Fnv1a(const uint8 *data, size_t size) {
  uint32 h = 2166136261u;
  for (size_t i = 0; i < size; i++)
    h = (h ^ data[i]) * 16777619u;
  return h;
}

void RoomLog_SetPass(int pass) {
  g_room_log.pass = pass;
}

void RoomLog_Begin(int kind, const uint8 *raw, int len) {
  RoomLog *log = &g_room_log;
  if (log->open >= 0)
    return;  // not expected: entries don't draw other entries
  if (log->num_entries == kRoomLogMaxEntries) {
    log->overflow = true;
    return;
  }
  RoomLogEntry *e = &log->entries[log->num_entries];
  e->kind = kind;
  e->pass = log->pass;
  e->len = len;
  memcpy(e->raw, raw, len);
  for (int layer = 0; layer < 2; layer++)
    memcpy(log->before[layer], RoomLogLayer(layer), sizeof(log->before[layer]));
  log->open = log->num_entries;
}

void RoomLog_End() {
  RoomLog *log = &g_room_log;
  if (log->open < 0)
    return;
  RoomLogEntry *e = &log->entries[log->open];
  e->change_start = log->num_changes;
  for (int layer = 0; layer < 2; layer++) {
    const uint16 *now = RoomLogLayer(layer);
    for (int i = 0; i < kRoomLogCells; i++) {
      if (now[i] == log->before[layer][i])
        continue;
      if (log->num_changes == kRoomLogMaxChanges) {
        log->overflow = true;
        continue;
      }
      log->changes[log->num_changes++] = layer << 12 | i;
      log->owner[layer][i] = log->open;
    }
  }
  e->change_count = log->num_changes - e->change_start;
  log->num_entries++;
  log->open = -1;
}

void FrameDump_CaptureRoomObjects() {
  RoomLog *log = &g_room_log;
  log->valid = false;
  if (!player_is_indoors)
    return;
  memcpy(g_room_log_saved_ram, g_ram, sizeof(g_ram));
  log->checksum_before = Fnv1a(g_ram, sizeof(g_ram));
  for (int layer = 0; layer < 2; layer++) {
    memcpy(log->live[layer], RoomLogLayer(layer), sizeof(log->live[layer]));
    for (int i = 0; i < kRoomLogCells; i++)
      log->owner[layer][i] = -1;
  }
  log->overflow = false;
  log->pass = kRoomLogPass_Layout;
  log->open = -1;
  log->num_entries = log->num_changes = 0;

  g_room_log_active = true;
  Dungeon_LoadRoom();
  g_room_log_active = false;

  for (int layer = 0; layer < 2; layer++)
    memcpy(log->loaded[layer], RoomLogLayer(layer), sizeof(log->loaded[layer]));
  log->room = dungeon_room_index;
  log->layout = dung_layout_and_starting_quadrant >> 2;

  memcpy(g_ram, g_room_log_saved_ram, sizeof(g_ram));
  log->checksum_after = Fnv1a(g_ram, sizeof(g_ram));
  log->ram_restored = memcmp(g_ram, g_room_log_saved_ram, sizeof(g_ram)) == 0;
  log->valid = true;
}

static void Bgr555ToRgba(uint16 c, uint8 *dst) {
  int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  dst[0] = r << 3 | r >> 2;
  dst[1] = g << 3 | g >> 2;
  dst[2] = b << 3 | b >> 2;
  dst[3] = 255;
}

static bool WriteBinary(const char *filename, const void *data, size_t size) {
  FILE *f = fopen(filename, "wb");
  if (!f)
    return false;
  bool ok = fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

static bool WriteScreenPng(const char *filename, const uint8 *pixels, int width, int height, int pitch) {
  uint8 *img = malloc((size_t)width * height * 4);
  if (!img)
    return false;
  for (int y = 0; y < height; y++) {
    const uint32 *src = (const uint32 *)(pixels + (size_t)y * pitch);
    uint8 *dst = img + (size_t)y * width * 4;
    for (int x = 0; x < width; x++, dst += 4) {
      // The PPU writes 0x00RRGGBB, force alpha to 255
      dst[0] = src[x] >> 16;
      dst[1] = src[x] >> 8;
      dst[2] = src[x];
      dst[3] = 255;
    }
  }
  bool ok = stbi_write_png(filename, width, height, 4, img, width * 4) != 0;
  free(img);
  return ok;
}

// Draws 4bpp tiles from VRAM into an atlas with kDumpAtlasColumns tiles per row.
// The tile at index palette * num_tiles + tile uses colors cgram[cgram_base + palette * 16 + c].
// Tiles are not flipped and color 0 is transparent.
static bool WriteTileAtlasPng(const char *filename, uint32 vram_base, int num_tiles, int cgram_base) {
  int width = kDumpAtlasColumns * 8;
  int height = num_tiles * 8 / kDumpAtlasColumns * 8;
  uint8 *img = calloc((size_t)width * height, 4);
  if (!img)
    return false;
  const uint16 *vram = g_zenv.vram, *cgram = g_zenv.ppu->cgram;
  for (int pal = 0; pal < 8; pal++) {
    for (int tile = 0; tile < num_tiles; tile++) {
      int index = pal * num_tiles + tile;
      uint8 *dst_tile = img + ((size_t)(index / kDumpAtlasColumns) * 8 * width + (index % kDumpAtlasColumns) * 8) * 4;
      const uint16 *src = &vram[(vram_base + tile * 16) & 0x7fff];
      for (int y = 0; y < 8; y++) {
        // Planes 0-1 are in the first 8 words, planes 2-3 in the next 8 (same as ppu.c)
        uint32 planes = src[y] | (uint32)src[y + 8] << 16;
        uint8 *dst = dst_tile + (size_t)y * width * 4;
        for (int x = 0; x < 8; x++, dst += 4) {
          int bit = 7 - x;
          int pixel = (planes >> bit) & 1 | (planes >> (bit + 7)) & 2 |
                      (planes >> (bit + 14)) & 4 | (planes >> (bit + 21)) & 8;
          if (pixel != 0)
            Bgr555ToRgba(cgram[cgram_base + pal * 16 + pixel], dst);
        }
      }
    }
  }
  bool ok = stbi_write_png(filename, width, height, 4, img, width * 4) != 0;
  free(img);
  return ok;
}

// VRAM word address of tilemap cell (tx, ty), computed the same way as the PPU does.
static uint32 TilemapVramAddr(const BgLayer *layer, int tx, int ty) {
  uint32 adr = layer->tilemapAdr + ((ty & 0x1f) << 5 | (tx & 0x1f));
  if ((tx & 0x20) && layer->tilemapWider)
    adr += 0x400;
  if ((ty & 0x20) && layer->tilemapHigher)
    adr += layer->tilemapWider ? 0x800 : 0x400;
  return adr & 0x7fff;
}

static int CountVramMismatch(const uint16 *wram_tiles, const BgLayer *layer) {
  int n = 0;
  for (int ty = 0; ty < kDumpTilemapSize; ty++) {
    for (int tx = 0; tx < kDumpTilemapSize; tx++)
      n += wram_tiles[ty * kDumpTilemapSize + tx] != g_zenv.vram[TilemapVramAddr(layer, tx, ty)];
  }
  return n;
}

static void WriteJsonArray(FILE *f, const void *data, int elem_size, int n, int per_line, const char *indent) {
  fputc('[', f);
  for (int i = 0; i < n; i++) {
    int v = (elem_size == 2) ? ((const uint16 *)data)[i] : ((const uint8 *)data)[i];
    if (i % per_line == 0)
      fprintf(f, "\n%s  ", indent);
    fprintf(f, (i + 1 < n) ? "%d," : "%d", v);
  }
  fprintf(f, "\n%s]", indent);
}

static void WriteJsonLayer(FILE *f, const char *name, const uint16 *tiles, const uint8 *attrs, const BgLayer *layer) {
  fprintf(f, "  \"%s\": {\n", name);
  fprintf(f, "    \"width\": %d, \"height\": %d,\n", kDumpTilemapSize, kDumpTilemapSize);
  fprintf(f, "    \"vram_addr\": %d,\n", layer->tilemapAdr);
  fprintf(f, "    \"vram_mismatch\": %d,\n", CountVramMismatch(tiles, layer));
  fprintf(f, "    \"tiles\": ");
  WriteJsonArray(f, tiles, 2, kDumpTilemapSize * kDumpTilemapSize, kDumpTilemapSize, "    ");
  fprintf(f, ",\n    \"attrs\": ");
  WriteJsonArray(f, attrs, 1, kDumpTilemapSize * kDumpTilemapSize, kDumpTilemapSize, "    ");
  fprintf(f, "\n  },\n");
}

static void WriteJsonAtlas(FILE *f, const char *name, const char *file, int num_tiles, int vram_word, int cgram_base) {
  fprintf(f, "  \"%s\": { \"file\": \"%s\", \"tile_size\": 8, \"columns\": %d, \"tiles\": %d, \"palettes\": 8,\n",
          name, file, kDumpAtlasColumns, num_tiles);
  fprintf(f, "    \"index\": \"palette*%d + tile\", \"vram_word\": %d, \"cgram_base\": %d, \"transparent_color0\": true },\n",
          num_tiles, vram_word, cgram_base);
}

static const char *const kOamOwnerNames[5] = { "other", "link", "sprite", "ancilla", "follower" };

// Live entities that can draw sprites, read from g_ram: Link, the 16 sprite
// slots and the 10 ancilla slots in use, and the follower. x and y are world
// pixels; z is the height above the floor (0-255 for sprites and ancillas).
static void WriteJsonActors(FILE *f) {
  fprintf(f, "  \"actors\": {\n");
  fprintf(f, "    \"link\": { \"x\": %d, \"y\": %d, \"z\": %d, \"floor\": %d },\n",
          link_x_coord, link_y_coord, (int16)link_z_coord, link_is_on_lower_level);
  fprintf(f, "    \"sprites\": [");
  bool first = true;
  for (int k = 0; k < 16; k++) {
    if (sprite_state[k] == 0)
      continue;
    fprintf(f, "%s\n      { \"index\": %d, \"type\": %d, \"state\": %d, \"x\": %d, \"y\": %d, \"z\": %d, \"floor\": %d }",
            first ? "" : ",", k, sprite_type[k], sprite_state[k], sprite_x_lo[k] | sprite_x_hi[k] << 8,
            sprite_y_lo[k] | sprite_y_hi[k] << 8, sprite_z[k], sprite_floor[k]);
    first = false;
  }
  fprintf(f, "%s],\n    \"ancillas\": [", first ? "" : "\n    ");
  first = true;
  for (int k = 0; k < 10; k++) {
    if (ancilla_type[k] == 0)
      continue;
    fprintf(f, "%s\n      { \"index\": %d, \"type\": %d, \"x\": %d, \"y\": %d, \"z\": %d, \"floor\": %d }",
            first ? "" : ",", k, ancilla_type[k], ancilla_x_lo[k] | ancilla_x_hi[k] << 8,
            ancilla_y_lo[k] | ancilla_y_hi[k] << 8, ancilla_z[k], ancilla_floor[k]);
    first = false;
  }
  fprintf(f, "%s],\n", first ? "" : "\n    ");
  if (follower_indicator)
    fprintf(f, "    \"follower\": { \"type\": %d }\n", follower_indicator);
  else
    fprintf(f, "    \"follower\": null\n");
  fprintf(f, "  },\n");
}

static void WriteJsonOam(FILE *f) {
  const Ppu *ppu = g_zenv.ppu;
  const uint16 *oam = ppu->oam;
  bool first = true;
  fprintf(f, "  \"oam\": [");
  for (int i = 0; i < 128; i++) {
    uint16 w0 = oam[i * 2], w1 = oam[i * 2 + 1];
    int y = w0 >> 8;
    if (y == 0xf0)
      continue;  // hidden
    int high = oam[0x100 + (i >> 3)] >> ((i & 7) * 2);
    int x = (w0 & 0xff) | (high & 1) << 8;
    if (x >= 256 + ppu->extraLeftRight)
      x -= 512;  // same wrap as the PPU
    fprintf(f, "%s\n    { \"i\": %d, \"x\": %d, \"y\": %d, \"tile\": %d, \"bank\": %d, \"palette\": %d, "
            "\"priority\": %d, \"hflip\": %s, \"vflip\": %s, \"size\": %d, \"owner\": ",
            first ? "" : ",", i, x, y, w1 & 0xff, (w1 >> 8) & 1, (w1 >> 9) & 7, (w1 >> 12) & 3,
            (w1 & 0x4000) ? "true" : "false", (w1 & 0x8000) ? "true" : "false",
            kDumpSpriteSizes[ppu->objSize & 7][(high >> 1) & 1]);
    if (OamOwner_IsValid()) {
      const OamOwnerEntry *o = &OamOwner_Entries()[i];
      fprintf(f, "{ \"kind\": \"%s\", \"index\": %d, \"type\": %d, \"part\": \"%s\" } }",
              kOamOwnerNames[o->kind < 5 ? o->kind : 0], o->index, o->type,
              o->part == kOamPart_Shadow ? "shadow" : "body");
    } else {
      fprintf(f, "null }");
    }
    first = false;
  }
  fprintf(f, "\n  ]\n");
}

// Writes the cells of |changes| (sorted by layer, then cell) that belong to
// |layer| as runs [row, first x, last x].
static void WriteJsonRuns(FILE *f, const uint16 *changes, int count, int layer) {
  bool first = true;
  fputc('[', f);
  for (int k = 0; k < count;) {
    int c = changes[k++];
    if ((c >> 12) != layer)
      continue;
    int y = (c >> 6) & 63, x0 = c & 63, x1 = x0;
    while (k < count && x1 + 1 < 64 && changes[k] == (layer << 12 | y << 6 | (x1 + 1)))
      x1++, k++;
    fprintf(f, "%s[%d, %d, %d]", first ? "" : ", ", y, x0, x1);
    first = false;
  }
  fputc(']', f);
}

static void WriteJsonRoomEntry(FILE *f, const RoomLog *log, int i) {
  static const char *const kKinds[] = { "object", "door", "block", "torch" };
  static const char *const kPasses[] = { "layout", "layer1", "layer2", "layer3", "extra" };
  const RoomLogEntry *e = &log->entries[i];
  const uint8 *raw = e->raw;
  fprintf(f, "\n      { \"i\": %d, \"kind\": \"%s\", \"pass\": \"%s\", \"raw\": \"", i, kKinds[e->kind], kPasses[e->pass]);
  for (int k = 0; k < e->len; k++)
    fprintf(f, k ? " %.2X" : "%.2X", raw[k]);
  fprintf(f, "\", ");
  uint16 r0 = raw[0] | raw[1] << 8;
  if (e->kind == kRoomLog_Object) {
    // Same decoding as RoomData_DrawObject. Ids follow assets/tables.py:
    // subtype 1 = 0x00-0xF7, subtype 2 = 0x100-0x13F, subtype 3 = 0xF80-0xFFF.
    uint8 idx = raw[2];
    if ((r0 & 0xfc) != 0xfc) {
      int x = (uint8)r0 >> 2, y = r0 >> 10, w = r0 & 3, h = (r0 >> 8) & 3;
      if (idx < 0xf8)
        fprintf(f, "\"subtype\": 1, \"id\": %d, \"x\": %d, \"y\": %d, \"size\": [%d, %d], ", idx, x, y, w, h);
      else
        fprintf(f, "\"subtype\": 3, \"id\": %d, \"x\": %d, \"y\": %d, \"size\": null, ",
                0xf80 + ((idx & 7) << 4 | h << 2 | w), x, y);
    } else {
      int x = (r0 & 3) << 4 | (r0 >> 12) & 0xf, y = ((r0 >> 8) & 0xf) << 2 | idx >> 6;
      fprintf(f, "\"subtype\": 2, \"id\": %d, \"x\": %d, \"y\": %d, \"size\": null, ", 0x100 + (idx & 0x3f), x, y);
    }
  } else if (e->kind == kRoomLog_Door) {
    // Same decoding as RoomData_DrawObject_Door. dir: 0 north, 1 south, 2 west, 3 east.
    fprintf(f, "\"door\": { \"type\": %d, \"pos\": %d, \"dir\": %d }, ", r0 >> 8, (r0 >> 4) & 0xf, r0 & 3);
  } else {
    // Blocks and torches store a tilemap offset * 2 (DstoPtr((v >> 1) & 0x1fff)).
    int cell = (r0 >> 1) & 0x1fff;
    fprintf(f, "\"x\": %d, \"y\": %d, ", cell & 63, (cell >> 6) & 63);
    if (e->kind == kRoomLog_Torch)
      fprintf(f, "\"lit\": %s, ", (r0 & 0x8000) ? "true" : "false");
  }
  const uint16 *changes = log->changes + e->change_start;
  int visible = 0;
  for (int k = 0; k < e->change_count; k++)
    visible += log->owner[changes[k] >> 12][changes[k] & 0xfff] == i;
  fprintf(f, "\"cells\": { \"bg1\": ");
  WriteJsonRuns(f, changes, e->change_count, 0);
  fprintf(f, ", \"bg2\": ");
  WriteJsonRuns(f, changes, e->change_count, 1);
  fprintf(f, " }, \"changed\": %d, \"visible\": %d }", e->change_count, visible);
}

// "room_objects": what the replayed Dungeon_LoadRoom drew, entry by entry, in
// drawing order. See "Formato de datos" in CLAUDE.md.
static void WriteJsonRoomObjects(FILE *f) {
  const RoomLog *log = &g_room_log;
  if (!log->valid || log->room != dungeon_room_index) {
    fprintf(f, "  \"room_objects\": null,\n");
    return;
  }
  fprintf(f, "  \"room_objects\": {\n");
  fprintf(f, "    \"room\": %d, \"layout\": %d, \"method\": \"replay\",\n", log->room, log->layout);
  fprintf(f, "    \"ram_checksum\": { \"before\": \"%.8X\", \"after\": \"%.8X\", \"restored\": %s },\n",
          log->checksum_before, log->checksum_after, log->ram_restored ? "true" : "false");
  fprintf(f, "    \"objects\": [");
  for (int i = 0; i < log->num_entries; i++) {
    WriteJsonRoomEntry(f, log, i);
    if (i + 1 < log->num_entries)
      fputc(',', f);
  }
  fprintf(f, "\n    ],\n");

  fprintf(f, "    \"warnings\": [");
  bool first = true;
  if (log->overflow)
    fprintf(f, "\"log overflow: some entries or cells are missing\""), first = false;
  if (!log->ram_restored)
    fprintf(f, "%s\"g_ram was not restored after the replay\"", first ? "" : ", "), first = false;
  for (int i = 0; i < log->num_entries; i++) {
    if (log->entries[i].change_count == 0)
      fprintf(f, "%s\"entry %d changed no cells\"", first ? "" : ", ", i), first = false;
  }
  fprintf(f, "],\n");

  // owner: per cell, the last entry that changed it (-1 = none, i.e. floor fill)
  fprintf(f, "    \"owner\": {");
  for (int layer = 0; layer < 2; layer++) {
    fprintf(f, "%s\n      \"bg%d\": [", layer ? "," : "", layer + 1);
    for (int i = 0; i < kRoomLogCells; i++)
      fprintf(f, "%s%s%d", i ? "," : "", (i & 63) ? "" : "\n        ", log->owner[layer][i]);
    fprintf(f, "\n      ]");
  }
  fprintf(f, "\n    },\n");

  // changed_since_load: cells of the dumped frame that differ from the replayed load
  // (lifted pots, opened chests and doors, overlays...). Empty right after entering a room.
  static uint16 diff[2 * kRoomLogCells];
  int n = 0;
  for (int layer = 0; layer < 2; layer++) {
    for (int i = 0; i < kRoomLogCells; i++) {
      if (log->live[layer][i] != log->loaded[layer][i])
        diff[n++] = layer << 12 | i;
    }
  }
  fprintf(f, "    \"changed_since_load\": { \"bg1\": ");
  WriteJsonRuns(f, diff, n, 0);
  fprintf(f, ", \"bg2\": ");
  WriteJsonRuns(f, diff, n, 1);
  fprintf(f, " }\n  },\n");
}

static bool WriteFrameJson(const char *filename, const char *warning, int width, int height, int render_scale) {
  FILE *f = fopen(filename, "w");
  if (!f)
    return false;
  const Ppu *ppu = g_zenv.ppu;
  int room = dungeon_room_index;

  fprintf(f, "{\n");
  fprintf(f, "  \"version\": 4,\n");
  fprintf(f, "  \"frame\": %d,\n", frame_ctr_dbg);
  fprintf(f, "  \"module\": %d, \"submodule\": %d,\n", main_module_index, submodule_index);
  fprintf(f, "  \"indoors\": %s,\n", player_is_indoors ? "true" : "false");
  if (warning[0])
    fprintf(f, "  \"warning\": \"%s\",\n", warning);
  else
    fprintf(f, "  \"warning\": null,\n");
  fprintf(f, "  \"room\": { \"id\": %d, \"origin\": [%d, %d] },\n",
          room, (room & 0xf) * 512, ((room & 0xff0) >> 4) * 512);
  // visible: extra space actually drawn this frame (ConfigurePpuSideSpace clips it to the
  // room's camera bounds); the rest of the extra_left strips and of the bottom stays black.
  fprintf(f, "  \"screen\": { \"file\": \"screen.png\", \"width\": %d, \"height\": %d, \"render_scale\": %d, \"extra_left\": %d,\n"
          "    \"visible\": { \"left\": %d, \"right\": %d, \"bottom\": %d } },\n",
          width, height, render_scale, ppu->extraLeftRight,
          ppu->extraLeftCur, ppu->extraRightCur, ppu->extraBottomCur);
  fprintf(f, "  \"camera\": {\n");
  fprintf(f, "    \"bg1\": { \"hofs\": %d, \"vofs\": %d },\n", BG1HOFS_copy, BG1VOFS_copy);
  fprintf(f, "    \"bg2\": { \"hofs\": %d, \"vofs\": %d },\n", BG2HOFS_copy, BG2VOFS_copy);
  fprintf(f, "    \"shake\": [%d, %d],\n", (int16)bg1_x_offset, (int16)bg1_y_offset);
  fprintf(f, "    \"main_screen\": %d, \"sub_screen\": %d\n", TM_copy, TS_copy);
  fprintf(f, "  },\n");
  fprintf(f, "  \"link\": { \"x\": %d, \"y\": %d, \"z\": %d, \"facing\": %d, \"layer\": \"%s\", \"lower_level\": %d },\n",
          link_x_coord, link_y_coord, (int16)link_z_coord, link_direction_facing,
          link_is_on_lower_level ? "bg1" : "bg2", link_is_on_lower_level);
  // blockset: byte 2 of the room header (aux_tile_theme_index, see Dungeon_LoadHeader).
  // Optional field added to version 3; null outdoors, where that byte means something else.
  fprintf(f, "  \"room_header\": { \"collision\": %d, \"bg2_properties\": %d, \"floor_offs\": [%d, %d], \"staircase\": %d, ",
          dung_hdr_collision, dung_hdr_bg2_properties, (int16)dung_floor_x_offs, (int16)dung_floor_y_offs,
          kind_of_in_room_staircase);
  if (player_is_indoors)
    fprintf(f, "\"blockset\": %d },\n", aux_tile_theme_index);
  else
    fprintf(f, "\"blockset\": null },\n");
  fprintf(f, "  \"cgram\": ");
  WriteJsonArray(f, ppu->cgram, 2, 256, 16, "  ");
  fprintf(f, ",\n");
  WriteJsonLayer(f, "bg1", dung_bg1, dung_bg1_attr_table, &ppu->bgLayer[0]);
  WriteJsonLayer(f, "bg2", dung_bg2, dung_bg2_attr_table, &ppu->bgLayer[1]);
  fprintf(f, "  \"tile_format\": \"bits 0-9 tile, 10-12 paleta, 13 prioridad, 14 hflip, 15 vflip\",\n");
  WriteJsonAtlas(f, "atlas", "tiles.png", kDumpBgTiles, kDumpBgTileVram, 0);
  WriteJsonAtlas(f, "sprites_atlas", "sprites.png", kDumpObjTiles, kDumpObjTileVram, 128);
  WriteJsonRoomObjects(f);
  WriteJsonActors(f);
  WriteJsonOam(f);
  fprintf(f, "}\n");
  return fclose(f) == 0;
}

void FrameDump_Write(const uint8 *pixels, int width, int height, int pitch, int render_scale) {
  g_frame_dump_pending = false;

  char warning[96] = "";
  if (main_module_index != 7)
    snprintf(warning, sizeof(warning), "module %d is not 7 (dungeon)", main_module_index);
  else if (submodule_index != 0)
    snprintf(warning, sizeof(warning), "submodule %d is not 0 (transition or special state)", submodule_index);
  if (warning[0])
    printf("*** Frame dump warning: %s\n", warning);

#if defined(_WIN32)
  _mkdir(DUMP_DIR);
#else
  mkdir(DUMP_DIR, 0755);
#endif

  int errors = 0;
  errors += !WriteBinary(DUMP_DIR "/wram.bin", g_ram, 0x20000);
  errors += !WriteBinary(DUMP_DIR "/vram.bin", g_zenv.vram, 0x10000);
  errors += !WriteBinary(DUMP_DIR "/cgram.bin", g_zenv.ppu->cgram, 0x200);
  errors += !WriteBinary(DUMP_DIR "/oam.bin", g_zenv.ppu->oam, 0x220);
  errors += !WriteScreenPng(DUMP_DIR "/screen.png", pixels, width, height, pitch);
  errors += !WriteTileAtlasPng(DUMP_DIR "/tiles.png", kDumpBgTileVram, kDumpBgTiles, 0);
  errors += !WriteTileAtlasPng(DUMP_DIR "/sprites.png", kDumpObjTileVram, kDumpObjTiles, 128);
  // Written last, so a complete frame.json means the other files are done too.
  errors += !WriteFrameJson(DUMP_DIR "/frame.json", warning, width, height, render_scale);
  g_room_log.valid = false;  // the next dump replays the room again

  if (errors)
    fprintf(stderr, "*** Frame dump: %d file(s) could not be written to " DUMP_DIR "/\n", errors);
  else
    printf("*** Frame dump of frame %d written to " DUMP_DIR "/\n", frame_ctr_dbg);
}
