// Frame dump for the 3D viewer. See frame_dump.h and the "Formato de datos"
// section of CLAUDE.md for the file format.
// Everything here only reads emulated memory, so it can't change game behavior.
#include "frame_dump.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "snes/ppu.h"
#include <stdio.h>
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
            "\"priority\": %d, \"hflip\": %s, \"vflip\": %s, \"size\": %d }",
            first ? "" : ",", i, x, y, w1 & 0xff, (w1 >> 8) & 1, (w1 >> 9) & 7, (w1 >> 12) & 3,
            (w1 & 0x4000) ? "true" : "false", (w1 & 0x8000) ? "true" : "false",
            kDumpSpriteSizes[ppu->objSize & 7][(high >> 1) & 1]);
    first = false;
  }
  fprintf(f, "\n  ]\n");
}

static bool WriteFrameJson(const char *filename, const char *warning, int width, int height, int render_scale) {
  FILE *f = fopen(filename, "w");
  if (!f)
    return false;
  const Ppu *ppu = g_zenv.ppu;
  int room = dungeon_room_index;

  fprintf(f, "{\n");
  fprintf(f, "  \"version\": 2,\n");
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
  fprintf(f, "  \"room_header\": { \"collision\": %d, \"bg2_properties\": %d, \"floor_offs\": [%d, %d], \"staircase\": %d },\n",
          dung_hdr_collision, dung_hdr_bg2_properties, (int16)dung_floor_x_offs, (int16)dung_floor_y_offs,
          kind_of_in_room_staircase);
  fprintf(f, "  \"cgram\": ");
  WriteJsonArray(f, ppu->cgram, 2, 256, 16, "  ");
  fprintf(f, ",\n");
  WriteJsonLayer(f, "bg1", dung_bg1, dung_bg1_attr_table, &ppu->bgLayer[0]);
  WriteJsonLayer(f, "bg2", dung_bg2, dung_bg2_attr_table, &ppu->bgLayer[1]);
  fprintf(f, "  \"tile_format\": \"bits 0-9 tile, 10-12 paleta, 13 prioridad, 14 hflip, 15 vflip\",\n");
  WriteJsonAtlas(f, "atlas", "tiles.png", kDumpBgTiles, kDumpBgTileVram, 0);
  WriteJsonAtlas(f, "sprites_atlas", "sprites.png", kDumpObjTiles, kDumpObjTileVram, 128);
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

  if (errors)
    fprintf(stderr, "*** Frame dump: %d file(s) could not be written to " DUMP_DIR "/\n", errors);
  else
    printf("*** Frame dump of frame %d written to " DUMP_DIR "/\n", frame_ctr_dbg);
}
