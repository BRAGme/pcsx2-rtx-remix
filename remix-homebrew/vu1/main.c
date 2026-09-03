/*
 * remixtest_vu1 -- the retail-style variant of remixtest.
 *
 * Same scene, same ground-truth block ("RMXT"), but the geometry is transformed on VU1 by
 * the ps2sdk draw_3D microprogram, with the world->clip matrix sitting in VU1 data memory
 * at qword 0 -- the shape a commercial PS2 title has and the shape the PCSX2 remix-backend
 * fork's VU1 capture scans for and back-slices (the matrix feeds the DIV and the CLIP).
 *
 * Conventions, all published in the truth block so nothing has to be trusted by eye:
 *   - VU1 qw 0..3 = world_view x view_screen (math3d, row-vector: p' = p * M), with the
 *     GS y-flip folded into the projection the way retail titles do it.
 *   - The microprogram divides by w, then does v = v * scale + scale with
 *     scale = (320, 224, Z_SCALE), and XYOFFSET is 0, so NDC [-1,1] spans the whole
 *     640x448 framebuffer on both axes. The matrix therefore carries the true aspect.
 *   - Lighting is flat per face (exact for cubes): colour = base * (ambient + sum of
 *     directional max(0, -dot(n, dir)) * colour), stored as GS 0x80 == 1.0.
 *   - The ground is unlit and textured with a procedural checker (16 px cells, 128x128
 *     RGB24), so its pixels are exactly the texture. It is wound so the right-hand cross
 *     product of its edges is +Y.
 *   - A far LANDMARK quad (96 verts, own texture, faces the camera, ~320 units away) exists
 *     so the fork's MESHTRACK -- which only latches a mesh >= 64 verts and > 250 units from
 *     the eye -- has something to latch. Its centroid names any mirrored axis and its
 *     `facing` sign is the handedness of the recovered world.
 *   - The microprogram drops any triangle with a vertex outside the frustum (it sets the
 *     ADC bit), so the ground is tessellated 8x8 and only edge cells can vanish.
 */

#include <kernel.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <malloc.h>
#include <math.h>
#include <tamtypes.h>
#include <gs_psm.h>
#include <dma.h>
#include <packet2.h>
#include <packet2_utils.h>
#include <graph.h>
#include <draw.h>
#include <math3d.h>

extern u32 VU1Draw3D_CodeStart __attribute__((section(".vudata")));
extern u32 VU1Draw3D_CodeEnd __attribute__((section(".vudata")));

/* ------------------------------------------------------------------------- */
/* Ground-truth block. Version 2 adds the landmark row before the tail magic. */
/* ------------------------------------------------------------------------- */

#define RMXT_MAGIC0   0x54584D52u   /* "RMXT" */
#define RMXT_MAGIC1   0x524D5854u   /* "TXMR" */
#define RMXT_VERSION  2u
#define RMXT_VARIANT  2u            /* 1 = EE/gsKit transform, 2 = VU1 microprogram */
#define MODE_FRAMES   600u
#define NUM_MODES     4u            /* 0 static, 1 dolly, 2 yaw sweep, 3 pitch sweep */

typedef struct {
	u32    magic0, version, size, frame;       /* 0x000 */
	u32    mode, width, height, light_count;   /* 0x010 */
	VECTOR cam_pos;                            /* 0x020 */
	VECTOR cam_rot;                            /* 0x030 */
	MATRIX world_view;                         /* 0x040 */
	MATRIX view_screen;                        /* 0x080 */
	MATRIX world_screen;                       /* 0x0C0  == the matrix at VU1 qword 0 */
	VECTOR light_dir[4];                       /* 0x100 */
	VECTOR light_col[4];                       /* 0x140 */
	u32    light_type[4];                      /* 0x180 */
	VECTOR cube_center[3];                     /* 0x190 */
	VECTOR cube_half;                          /* 0x1C0 */
	VECTOR probe_corners[8];                   /* 0x1D0 */
	VECTOR frustum;                            /* 0x250  left, right, bottom, top */
	VECTOR frustum2;                           /* 0x260  near, far, aspect, ground_y */
	VECTOR ground;                             /* 0x270  half-extent, cells per side, tex w, tex h */
	VECTOR landmark;                           /* 0x280  centre x,y,z, half-size (normal +Z, faces the camera) */
	u32    magic1, variant, vu1_matrix_qw, vu1_dbuf; /* 0x290  vu1_dbuf = base | offset << 16 */
} __attribute__((aligned(16))) RemixTruth;     /* 0x2A0 */

static RemixTruth truth __attribute__((aligned(64)));

/* ------------------------------------------------------------------------- */
/* Scene                                                                     */
/* ------------------------------------------------------------------------- */

#define FB_W 640
#define FB_H 448

static VECTOR cam_pos0 = {  0.00f, 4.00f, 18.00f, 1.00f };
static VECTOR cam_rot0 = { -0.18f, 0.00f,  0.00f, 1.00f };

#define FR_LEFT   -0.5f
#define FR_RIGHT   0.5f
#define FR_BOTTOM -0.35f            /* 0.5 * 448/640: square pixels on a 640x448 target */
#define FR_TOP     0.35f
#define FR_NEAR    1.0f
#define FR_FAR  1000.0f             /* the landmark sits ~320 units out */
#define FR_ASPECT  1.0f             /* aspect lives in left/right vs bottom/top */

static VECTOR light_dir[4] = {
	{  0.00f,  0.00f,  0.00f, 1.00f },
	{  0.40f, -0.80f, -0.45f, 1.00f },
	{ -0.70f, -0.30f,  0.60f, 1.00f },
	{  0.00f,  0.00f,  0.00f, 1.00f }
};
static VECTOR light_col[4] = {
	{ 0.15f, 0.15f, 0.15f, 1.00f },
	{ 1.00f, 0.95f, 0.85f, 1.00f },
	{ 0.25f, 0.30f, 0.40f, 1.00f },
	{ 0.00f, 0.00f, 0.00f, 1.00f }
};
static u32 light_type[4] = { LIGHT_AMBIENT, LIGHT_DIRECTIONAL, LIGHT_DIRECTIONAL, LIGHT_DIRECTIONAL };
static const int light_count = 3;

#define NUM_CUBES 3
static const float cube_center[NUM_CUBES][3] = { { 0.0f, 0.0f, 0.0f }, { 6.0f, 0.0f, -4.0f }, { -6.0f, 1.0f, 3.0f } };
static const float cube_half[NUM_CUBES]      = { 1.0f, 1.0f, 1.5f };
#define PROBE_CUBE 1

static const float face_n[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
static const float face_u[6][3] = { {0,1,0}, { 0,1,0}, {1,0,0}, {1, 0,0}, {1,0,0}, {1,0, 0} };
static const float face_v[6][3] = { {0,0,1}, { 0,0,-1},{0,0,-1},{0, 0,1}, {0,1,0}, {0,-1,0} };
static const float cube0_face_col[6][3] = {
	{ 1.00f, 0.10f, 0.10f }, { 0.50f, 0.05f, 0.05f },
	{ 0.10f, 1.00f, 0.10f }, { 0.05f, 0.50f, 0.05f },
	{ 0.10f, 0.10f, 1.00f }, { 0.05f, 0.05f, 0.50f }
};
static const float cube1_col[3] = { 1.00f, 1.00f, 1.00f };
static const float cube2_col[3] = { 1.00f, 0.85f, 0.20f };

#define GROUND_Y     -1.0f
#define GROUND_HALF  12.0f
#define GROUND_CELLS 8              /* 8x8 cells, 128 triangles, 4 draw batches of 32 */
#define TEX_W        128
#define TEX_H        128

/* Far landmark: a 4x4-cell quad in the XY plane facing +Z (toward the camera). */
#define LM_X         40.0f
#define LM_Y        -10.0f          /* low enough to stay inside the frustum through the pitch sweep */
#define LM_Z       -300.0f
#define LM_HALF      20.0f
#define LM_CELLS     4              /* 4x4 cells = 32 tris = 96 verts, one batch */

/* VU1 layout, as the ps2sdk sample: matrix at qw 0..3, double buffer base 8 / offset 496.
 * A batch of N verts needs 6 + 2N (input) + 5 + 3N (output) = 11 + 5N qwords <= 496. */
#define VU1_MATRIX_QW    0
#define VU1_DBUF_BASE    8
#define VU1_DBUF_OFFSET  496
#define MAX_BATCH_VERTS  96
#define Z_SCALE          (((float)0xFFFFFF) / 32.0f)   /* ftoi4 multiplies by 16: 24-bit z */

#define NUM_FACE_BATCHES   (NUM_CUBES * 6)
#define NUM_GROUND_BATCHES ((GROUND_CELLS * GROUND_CELLS * 2 * 3 + MAX_BATCH_VERTS - 1) / MAX_BATCH_VERTS)
#define NUM_LM_BATCHES     1
#define NUM_BATCHES        (NUM_FACE_BATCHES + NUM_GROUND_BATCHES + NUM_LM_BATCHES)

typedef struct {
	packet2_t *header;   /* 6 qwords: scale+count, GIF set, LOD, texbuf+clut, prim giftag, RGBA */
	VECTOR    *verts;    /* count qwords, world space */
	VECTOR    *sts;      /* count qwords: s, t, q=1, 0 */
	int        count;
	u32        rgba[4];
} DrawBatch;

static DrawBatch batches[NUM_BATCHES];
static packet2_t *vif_packets[2];
static u8 vif_ctx = 0;
static MATRIX world_view __attribute__((aligned(64)));
static MATRIX view_screen __attribute__((aligned(64)));
static MATRIX world_screen __attribute__((aligned(64)));
static prim_t prim;
static clutbuffer_t clut;
static lod_t lod;
static texbuffer_t tex_checker, tex_white, tex_landmark;
static u8 *checker_rgb, *white_rgb, *landmark_rgb;

/* ------------------------------------------------------------------------- */

static void vnorm3(VECTOR v)
{
	float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
	if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

/* Flat lighting for one face normal, math3d semantics: intensity = -dot(n, dir), clamped. */
static void lit_rgba(const float *n, const float *base, u32 *out)
{
	float I[3] = { 0.f, 0.f, 0.f };
	int k, c;
	for (k = 0; k < light_count; k++) {
		float in;
		if (light_type[k] == LIGHT_AMBIENT) in = 1.0f;
		else {
			in = -(n[0]*light_dir[k][0] + n[1]*light_dir[k][1] + n[2]*light_dir[k][2]);
			if (in < 0.0f) in = 0.0f;
		}
		for (c = 0; c < 3; c++) I[c] += light_col[k][c] * in;
	}
	for (c = 0; c < 3; c++) {
		float v = base[c] * I[c] * 128.0f + 0.5f;
		if (v < 0.0f) v = 0.0f;
		if (v > 255.0f) v = 255.0f;
		out[c] = (u32)v;
	}
	out[3] = 128;
}

static packet2_t *build_header(int count, texbuffer_t *tb, const u32 *rgba)
{
	packet2_t *p = packet2_create(8, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	int i;
	packet2_add_float(p, (float)(FB_W / 2));     /* scale.x: ndc -1..1 -> 0..640  */
	packet2_add_float(p, (float)(FB_H / 2));     /* scale.y: ndc -1..1 -> 0..448  */
	packet2_add_float(p, Z_SCALE);
	packet2_add_s32(p, count);                    /* vert count, read with ilw.w   */
	packet2_utils_gif_add_set(p, 1);
	packet2_utils_gs_add_lod(p, &lod);
	packet2_utils_gs_add_texbuff_clut(p, tb, &clut);
	packet2_utils_gs_add_prim_giftag(p, &prim, count, DRAW_STQ2_REGLIST, 3, 0);
	for (i = 0; i < 4; i++) packet2_add_u32(p, rgba[i]);
	return p;
}

static void alloc_batch(DrawBatch *b, int count)
{
	b->count = count;
	b->verts = memalign(128, sizeof(VECTOR) * count);
	b->sts   = memalign(128, sizeof(VECTOR) * count);
}

/* A cells x cells grid over a quad spanned by (origin, u, v), wound so the right-hand cross
 * product of the triangle edges is u x v. sts run 0..1 across the quad. */
static void build_grid(DrawBatch *b, const float *origin, const float *u, const float *v, int cells)
{
	static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
	int cnt = cells * cells * 6, k;
	alloc_batch(b, cnt);
	for (k = 0; k < cnt; k++) {
		int tri_i = k / 3, corner = tri[(tri_i & 1) * 3 + (k % 3)];
		int cellid = tri_i / 2, cu = cellid % cells, cv = cellid / cells;
		float fu = (cu + ((corner == 1 || corner == 2) ? 1.0f : 0.0f)) / (float)cells;
		float fv = (cv + ((corner == 2 || corner == 3) ? 1.0f : 0.0f)) / (float)cells;
		int c;
		for (c = 0; c < 3; c++) b->verts[k][c] = origin[c] + fu * u[c] + fv * v[c];
		b->verts[k][3] = 1.0f;
		b->sts[k][0] = fu; b->sts[k][1] = fv; b->sts[k][2] = 1.0f; b->sts[k][3] = 0.0f;
	}
}

static void build_scene(void)
{
	int c, f, k, n = 0;
	static const int su[4] = { -1, 1, 1, -1 };
	static const int sv[4] = { -1, -1, 1, 1 };
	static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
	static const u32 white[4] = { 128, 128, 128, 128 };

	/* 18 cube faces, one flat-lit batch each (6 verts). Corners (-u-v)(+u-v)(+u+v)(-u+v)
	 * with tris (0,1,2)(0,2,3): edge cross product = u x v = the outward normal. */
	for (c = 0; c < NUM_CUBES; c++) {
		const float h = cube_half[c];
		const float *ctr = cube_center[c];
		for (f = 0; f < 6; f++) {
			DrawBatch *b = &batches[n++];
			const float *base = (c == 0) ? cube0_face_col[f] : (c == 1) ? cube1_col : cube2_col;
			alloc_batch(b, 6);
			for (k = 0; k < 6; k++) {
				int q = tri[k];
				b->verts[k][0] = ctr[0] + h * (face_n[f][0] + su[q]*face_u[f][0] + sv[q]*face_v[f][0]);
				b->verts[k][1] = ctr[1] + h * (face_n[f][1] + su[q]*face_u[f][1] + sv[q]*face_v[f][1]);
				b->verts[k][2] = ctr[2] + h * (face_n[f][2] + su[q]*face_u[f][2] + sv[q]*face_v[f][2]);
				b->verts[k][3] = 1.0f;
				b->sts[k][0] = 0.5f; b->sts[k][1] = 0.5f; b->sts[k][2] = 1.0f; b->sts[k][3] = 0.0f;
			}
			lit_rgba(face_n[f], base, b->rgba);
			b->header = build_header(6, &tex_white, b->rgba);
		}
	}

	/* Ground: 8x8 cells over +-GROUND_HALF at GROUND_Y, unlit. Spanned by u = +Z, v = +X so
	 * u x v = +Y (up) under the right-hand rule. Split into batches of <= 96 verts. */
	{
		static const float gu[3] = { 0.0f, 0.0f, 2.0f * GROUND_HALF };
		static const float gv[3] = { 2.0f * GROUND_HALF, 0.0f, 0.0f };
		static const float go[3] = { -GROUND_HALF, GROUND_Y, -GROUND_HALF };
		DrawBatch whole;
		int total, done = 0;
		build_grid(&whole, go, gu, gv, GROUND_CELLS);
		total = whole.count;
		while (done < total) {
			DrawBatch *b = &batches[n++];
			int cnt = (total - done < MAX_BATCH_VERTS) ? (total - done) : MAX_BATCH_VERTS;
			alloc_batch(b, cnt);
			memcpy(b->verts, whole.verts + done, sizeof(VECTOR) * cnt);
			memcpy(b->sts,   whole.sts + done,   sizeof(VECTOR) * cnt);
			memcpy(b->rgba, white, sizeof(white));
			b->header = build_header(cnt, &tex_checker, white);
			done += cnt;
		}
	}

	/* Landmark: XY quad centred at (LM_X, LM_Y, LM_Z), u = +X, v = +Y so u x v = +Z, which
	 * faces the camera. One batch of 96 verts with its own texture (own material, own mesh). */
	{
		static const float lu[3] = { 2.0f * LM_HALF, 0.0f, 0.0f };
		static const float lv[3] = { 0.0f, 2.0f * LM_HALF, 0.0f };
		static const float lo[3] = { LM_X - LM_HALF, LM_Y - LM_HALF, LM_Z };
		DrawBatch *b = &batches[n++];
		build_grid(b, lo, lu, lv, LM_CELLS);
		memcpy(b->rgba, white, sizeof(white));
		b->header = build_header(b->count, &tex_landmark, white);
	}
}

static void build_textures(void)
{
	int x, y;
	checker_rgb  = memalign(128, TEX_W * TEX_H * 3);
	white_rgb    = memalign(128, TEX_W * TEX_H * 3);
	landmark_rgb = memalign(128, TEX_W * TEX_H * 3);
	memset(white_rgb, 0xFF, TEX_W * TEX_H * 3);
	/* Landmark: white with a 2x2 grey corner so its content hash differs from the cube texture. */
	memset(landmark_rgb, 0xFF, TEX_W * TEX_H * 3);
	for (y = 0; y < 2; y++) for (x = 0; x < 2; x++) {
		landmark_rgb[(y * TEX_W + x) * 3 + 0] = 0xC0;
		landmark_rgb[(y * TEX_W + x) * 3 + 1] = 0xC0;
		landmark_rgb[(y * TEX_W + x) * 3 + 2] = 0xC0;
	}
	for (y = 0; y < TEX_H; y++)
		for (x = 0; x < TEX_W; x++) {
			int c = ((x >> 4) ^ (y >> 4)) & 1;
			u8 v = c ? 200 : 60, r = v, g = v, b = v;
			if (y < 2) { r = 220; g = 40;  b = 40;  }   /* red band along t = 0 */
			if (x < 2) { r = 40;  g = 60;  b = 220; }   /* blue band along s = 0 */
			checker_rgb[(y * TEX_W + x) * 3 + 0] = r;
			checker_rgb[(y * TEX_W + x) * 3 + 1] = g;
			checker_rgb[(y * TEX_W + x) * 3 + 2] = b;
		}
}

/* ---- GS / VU1 plumbing, following the ps2sdk vu1 sample ---------------------------------- */

static void init_gs(framebuffer_t *fb, zbuffer_t *z)
{
	fb->width = FB_W; fb->height = FB_H; fb->mask = 0; fb->psm = GS_PSM_32;
	fb->address = graph_vram_allocate(fb->width, fb->height, fb->psm, GRAPH_ALIGN_PAGE);

	z->enable = DRAW_ENABLE; z->mask = 0; z->method = ZTEST_METHOD_GREATER_EQUAL; z->zsm = GS_ZBUF_32;
	z->address = graph_vram_allocate(fb->width, fb->height, z->zsm, GRAPH_ALIGN_PAGE);

	tex_checker.width = TEX_W; tex_checker.psm = GS_PSM_24;
	tex_checker.address = graph_vram_allocate(TEX_W, TEX_H, GS_PSM_24, GRAPH_ALIGN_BLOCK);
	tex_checker.info.width = draw_log2(TEX_W); tex_checker.info.height = draw_log2(TEX_H);
	tex_checker.info.components = TEXTURE_COMPONENTS_RGB; tex_checker.info.function = TEXTURE_FUNCTION_MODULATE;

	tex_white = tex_checker;
	tex_white.address = graph_vram_allocate(TEX_W, TEX_H, GS_PSM_24, GRAPH_ALIGN_BLOCK);
	tex_landmark = tex_checker;
	tex_landmark.address = graph_vram_allocate(TEX_W, TEX_H, GS_PSM_24, GRAPH_ALIGN_BLOCK);

	graph_initialize(fb->address, fb->width, fb->height, fb->psm, 0, 0);
}

static void init_drawing_environment(framebuffer_t *fb, zbuffer_t *z)
{
	packet2_t *p = packet2_create(20, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
	packet2_update(p, draw_setup_environment(p->next, 0, fb, z));
	packet2_update(p, draw_primitive_xyoffset(p->next, 0, 0, 0));   /* XYOFFSET 0: NDC spans the framebuffer */
	packet2_update(p, draw_finish(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_wait_fast();
	packet2_free(p);
}

static void send_texture(texbuffer_t *tb, void *rgb)
{
	packet2_t *p = packet2_create(50, P2_TYPE_NORMAL, P2_MODE_CHAIN, 0);
	packet2_update(p, draw_texture_transfer(p->next, rgb, TEX_W, TEX_H, GS_PSM_24, tb->address, tb->width));
	packet2_update(p, draw_texture_flush(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_wait_fast();
	packet2_free(p);
}

static void clear_screen(framebuffer_t *fb, zbuffer_t *z)
{
	packet2_t *p = packet2_create(35, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
	packet2_update(p, draw_disable_tests(p->next, 0, z));
	packet2_update(p, draw_clear(p->next, 0, 0.0f, 0.0f, fb->width, fb->height, 0x20, 0x30, 0x50));
	packet2_update(p, draw_enable_tests(p->next, 0, z));
	packet2_update(p, draw_finish(p->next));
	dma_wait_fast();
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	packet2_free(p);
	draw_wait_finish();
}

static void set_prim_state(void)
{
	lod.calculation = LOD_USE_K; lod.max_level = 0;
	lod.mag_filter = LOD_MAG_NEAREST; lod.min_filter = LOD_MIN_NEAREST; lod.l = 0; lod.k = 0;

	clut.storage_mode = CLUT_STORAGE_MODE1; clut.start = 0; clut.psm = 0;
	clut.load_method = CLUT_NO_LOAD; clut.address = 0;

	prim.type = PRIM_TRIANGLE; prim.shading = PRIM_SHADE_GOURAUD;
	prim.mapping = DRAW_ENABLE; prim.fogging = DRAW_DISABLE;
	prim.blending = DRAW_DISABLE;            /* opaque world geometry */
	prim.antialiasing = DRAW_DISABLE;
	prim.mapping_type = PRIM_MAP_ST; prim.colorfix = PRIM_UNFIXED;
}

static void vu1_upload_program(void)
{
	u32 sz = packet2_utils_get_packet_size_for_program(&VU1Draw3D_CodeStart, &VU1Draw3D_CodeEnd) + 1;
	packet2_t *p = packet2_create(sz, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_vif_add_micro_program(p, 0, &VU1Draw3D_CodeStart, &VU1Draw3D_CodeEnd);
	packet2_utils_vu_add_end_tag(p);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(p);
}

static void vu1_set_double_buffer(void)
{
	packet2_t *p = packet2_create(1, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_utils_vu_add_double_buffer(p, VU1_DBUF_BASE, VU1_DBUF_OFFSET);
	packet2_utils_vu_add_end_tag(p);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(p);
}

/* One draw: matrix to VU1 qw 0 (absolute), then header/verts/sts into the TOP buffer, MSCAL 0. */
static void send_batch(DrawBatch *b)
{
	packet2_t *vp = vif_packets[vif_ctx];
	packet2_reset(vp, 0);
	packet2_utils_vu_add_unpack_data(vp, VU1_MATRIX_QW, world_screen, 4, 0);
	packet2_utils_vu_add_unpack_data(vp, 0, b->header->base, packet2_get_qw_count(b->header), 1);
	packet2_utils_vu_add_unpack_data(vp, 6, b->verts, b->count, 1);
	packet2_utils_vu_add_unpack_data(vp, 6 + b->count, b->sts, b->count, 1);
	packet2_utils_vu_add_start_program(vp, 0);
	packet2_utils_vu_add_end_tag(vp);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	dma_channel_send_packet2(vp, DMA_CHANNEL_VIF1, 1);
	vif_ctx ^= 1;
}

/* ---- truth block ------------------------------------------------------------------------- */

static void publish(u32 frame, u32 mode, const VECTOR cp, const VECTOR cr, const VECTOR *probe)
{
	int i;
	truth.magic0 = RMXT_MAGIC0; truth.version = RMXT_VERSION;
	truth.size = sizeof(truth); truth.frame = frame;
	truth.mode = mode; truth.width = FB_W; truth.height = FB_H; truth.light_count = light_count;
	vector_copy(truth.cam_pos, (float *)cp); vector_copy(truth.cam_rot, (float *)cr);
	matrix_copy(truth.world_view, world_view);
	matrix_copy(truth.view_screen, view_screen);
	matrix_copy(truth.world_screen, world_screen);
	for (i = 0; i < 4; i++) {
		vector_copy(truth.light_dir[i], light_dir[i]);
		vector_copy(truth.light_col[i], light_col[i]);
		truth.light_type[i] = light_type[i];
	}
	for (i = 0; i < NUM_CUBES; i++) {
		truth.cube_center[i][0] = cube_center[i][0]; truth.cube_center[i][1] = cube_center[i][1];
		truth.cube_center[i][2] = cube_center[i][2]; truth.cube_center[i][3] = 1.0f;
		truth.cube_half[i] = cube_half[i];
	}
	truth.cube_half[3] = 0.0f;
	for (i = 0; i < 8; i++) vector_copy(truth.probe_corners[i], (float *)probe[i]);
	truth.frustum[0] = FR_LEFT; truth.frustum[1] = FR_RIGHT; truth.frustum[2] = FR_BOTTOM; truth.frustum[3] = FR_TOP;
	truth.frustum2[0] = FR_NEAR; truth.frustum2[1] = FR_FAR; truth.frustum2[2] = FR_ASPECT; truth.frustum2[3] = GROUND_Y;
	truth.ground[0] = GROUND_HALF; truth.ground[1] = GROUND_CELLS; truth.ground[2] = TEX_W; truth.ground[3] = TEX_H;
	truth.landmark[0] = LM_X; truth.landmark[1] = LM_Y; truth.landmark[2] = LM_Z; truth.landmark[3] = LM_HALF;
	truth.magic1 = RMXT_MAGIC1; truth.variant = RMXT_VARIANT;
	truth.vu1_matrix_qw = VU1_MATRIX_QW; truth.vu1_dbuf = VU1_DBUF_BASE | (VU1_DBUF_OFFSET << 16);
}

static void print_matrix(const char *name, const MATRIX m)
{
	int r;
	printf("RMXT %s\n", name);
	for (r = 0; r < 4; r++)
		printf("RMXT   %9.4f %9.4f %9.4f %9.4f\n", m[r*4+0], m[r*4+1], m[r*4+2], m[r*4+3]);
}

/* ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	framebuffer_t fb;
	zbuffer_t z;
	VECTOR probe[8];
	VECTOR cam_pos, cam_rot;
	u32 frame = 0;
	int i;

	(void)argc; (void)argv;

	/* stdout goes through the IOP fileio RPC in ps2sdk; bind SIF before the first printf. */
	SifInitRpc(0);

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	dma_channel_fast_waits(DMA_CHANNEL_VIF1);

	vif_packets[0] = packet2_create(16, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	vif_packets[1] = packet2_create(16, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);

	vu1_upload_program();
	vu1_set_double_buffer();

	init_gs(&fb, &z);
	init_drawing_environment(&fb, &z);
	build_textures();
	send_texture(&tex_checker, checker_rgb);
	send_texture(&tex_white, white_rgb);
	send_texture(&tex_landmark, landmark_rgb);
	set_prim_state();

	vnorm3(light_dir[1]); vnorm3(light_dir[2]);
#ifdef SUN_ONLY
	/* Calibration build: one directional light plus ambient, so a single-sun fit is well posed. */
	light_col[2][0] = light_col[2][1] = light_col[2][2] = 0.0f;
	printf("RMXT SUN_ONLY build: fill light colour zeroed\n");
#endif
	build_scene();

	{
		const float *ctr = cube_center[PROBE_CUBE]; const float h = cube_half[PROBE_CUBE];
		for (i = 0; i < 8; i++) {
			probe[i][0] = ctr[0] + ((i & 1) ? h : -h);
			probe[i][1] = ctr[1] + ((i & 2) ? h : -h);
			probe[i][2] = ctr[2] + ((i & 4) ? h : -h);
			probe[i][3] = 1.0f;
		}
	}

	create_view_screen(view_screen, FR_ASPECT, FR_LEFT, FR_RIGHT, FR_BOTTOM, FR_TOP, FR_NEAR, FR_FAR);
	/* math3d's projection leaves +y up in NDC, but GS y grows downward. Fold the flip into the
	 * projection the way retail titles do, so view-up is screen-up and the VU1 matrix carries it. */
#ifndef NO_YFLIP
	view_screen[0x05] = -view_screen[0x05];
	view_screen[0x09] = -view_screen[0x09];
#else
	printf("RMXT NO_YFLIP build: projection keeps math3d +y up (PS2 picture is upside-down by design)\n");
#endif

	printf("\nRMXT remixtest_vu1 boot: %dx%d fb PSM32 z32 | VU1 matrix @ qw %d | dbuf base %d offset %d | %d batches\n",
	       FB_W, FB_H, VU1_MATRIX_QW, VU1_DBUF_BASE, VU1_DBUF_OFFSET, NUM_BATCHES);
	printf("RMXT truth block @ 0x%08x size 0x%x version %u variant %u\n", (unsigned)&truth, (unsigned)sizeof(truth), RMXT_VERSION, RMXT_VARIANT);
	printf("RMXT offsets: cam_pos 0x%x world_view 0x%x view_screen 0x%x world_screen 0x%x light_dir 0x%x light_col 0x%x probe 0x%x landmark 0x%x magic1 0x%x\n",
	       (unsigned)offsetof(RemixTruth, cam_pos), (unsigned)offsetof(RemixTruth, world_view),
	       (unsigned)offsetof(RemixTruth, view_screen), (unsigned)offsetof(RemixTruth, world_screen),
	       (unsigned)offsetof(RemixTruth, light_dir), (unsigned)offsetof(RemixTruth, light_col),
	       (unsigned)offsetof(RemixTruth, probe_corners), (unsigned)offsetof(RemixTruth, landmark),
	       (unsigned)offsetof(RemixTruth, magic1));
	printf("RMXT landmark centre %.1f %.1f %.1f half %.1f normal +0 +0 +1 (faces the camera), %d verts, own texture\n",
	       LM_X, LM_Y, LM_Z, LM_HALF, batches[NUM_BATCHES-1].count);
	print_matrix("view_screen", view_screen);
	printf("RMXT sun  dir %.5f %.5f %.5f  col %.2f %.2f %.2f\n", light_dir[1][0], light_dir[1][1], light_dir[1][2], light_col[1][0], light_col[1][1], light_col[1][2]);
	printf("RMXT fill dir %.5f %.5f %.5f  col %.2f %.2f %.2f\n", light_dir[2][0], light_dir[2][1], light_dir[2][2], light_col[2][0], light_col[2][1], light_col[2][2]);
	printf("RMXT ambient col %.2f %.2f %.2f\n", light_col[0][0], light_col[0][1], light_col[0][2]);
	for (i = 0; i < 6; i++)
		printf("RMXT probe face %d normal %+.0f %+.0f %+.0f -> rgba %u %u %u %u\n", i,
		       face_n[i][0], face_n[i][1], face_n[i][2],
		       batches[PROBE_CUBE*6+i].rgba[0], batches[PROBE_CUBE*6+i].rgba[1], batches[PROBE_CUBE*6+i].rgba[2], batches[PROBE_CUBE*6+i].rgba[3]);

	for (;;) {
		u32 mode = (frame / MODE_FRAMES) % NUM_MODES;
		float t = (float)(frame % MODE_FRAMES) / (float)MODE_FRAMES;
		float ph = 2.0f * 3.14159265f * t;

		vector_copy(cam_pos, cam_pos0);
		vector_copy(cam_rot, cam_rot0);
		if (mode == 1) cam_pos[2] = cam_pos0[2] + 6.0f * (1.0f - cosf(ph));   /* dolly z 18 -> 30 -> 18 */
		if (mode == 2) cam_rot[1] = 0.15f * sinf(ph);                         /* yaw sweep +-0.15 rad */
		if (mode == 3) cam_rot[0] = cam_rot0[0] + 0.10f * sinf(ph);           /* pitch sweep +-0.10 rad about -0.18 */

		create_world_view(world_view, cam_pos, cam_rot);
		matrix_multiply(world_screen, world_view, view_screen);

		publish(frame, mode, cam_pos, cam_rot, probe);

		clear_screen(&fb, &z);
		for (i = 0; i < NUM_BATCHES; i++) send_batch(&batches[i]);
		/* No draw_wait_finish() here: nothing in the VU1 batches raises GS FINISH, so waiting on the
		 * CSR spins forever (it froze frame 0 on the first run). clear_screen() already synced. */
		dma_channel_wait(DMA_CHANNEL_VIF1, 0);

		if ((frame % 300) == 0) {
			printf("RMXT frame %u mode %u cam_pos %.3f %.3f %.3f cam_rot %.3f %.3f %.3f\n",
			       frame, mode, cam_pos[0], cam_pos[1], cam_pos[2], cam_rot[0], cam_rot[1], cam_rot[2]);
			if (frame == 0) { print_matrix("world_view", world_view); print_matrix("world_screen (VU1 qw0-3)", world_screen); }
		}

		graph_wait_vsync();
		frame++;
	}

	return 0;
}
