/*
 * remixtest -- a deterministic PS2 scene that publishes its own ground truth.
 *
 * Purpose: give the PCSX2 remix-backend fork a controlled input. Every frame the
 * program writes the exact camera, matrices, lights and geometry it used into a
 * magic-tagged block in EE RAM ("RMXT"), so whatever the backend un-projects can
 * be checked against a known answer instead of against a commercial game.
 *
 * Built with ps2dev (ps2sdk + gsKit). All geometry is authored directly in world
 * space and transformed by a single world->screen matrix, so there is no per-object
 * transform to reason about. Lighting follows the ps2sdk math3d convention:
 * one ambient plus directional lights, intensity = -dot(normal, dir).
 */

#include <kernel.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <malloc.h>
#include <math.h>
#include <tamtypes.h>
#include <math3d.h>
#include <gsKit.h>
#include <gsInline.h>
#include <dmaKit.h>
#include <draw.h>
#include <draw3d.h>

/* ------------------------------------------------------------------------- */
/* Ground-truth block. Fixed layout, 16-byte fields, magic at head and tail.  */
/* Offsets are printed at boot so nothing here has to be trusted by eye.      */
/* ------------------------------------------------------------------------- */

#define RMXT_MAGIC0   0x54584D52u   /* bytes "RMXT" */
#define RMXT_MAGIC1   0x524D5854u   /* bytes "TXMR" */
#define RMXT_VERSION  1u
#define MODE_FRAMES   600u          /* 10 s per camera mode at 60 Hz */
#define NUM_MODES     3u

typedef struct {
	u32    magic0, version, size, frame;       /* 0x000 */
	u32    mode, width, height, light_count;   /* 0x010 */
	VECTOR cam_pos;                            /* 0x020  as fed to create_world_view */
	VECTOR cam_rot;                            /* 0x030  radians, as fed to create_world_view */
	MATRIX world_view;                         /* 0x040  create_world_view() */
	MATRIX view_screen;                        /* 0x080  create_view_screen() */
	MATRIX world_screen;                       /* 0x0C0  matrix_multiply(world_view, view_screen) */
	VECTOR light_dir[4];                       /* 0x100  exactly what calculate_lights() saw */
	VECTOR light_col[4];                       /* 0x140 */
	u32    light_type[4];                      /* 0x180  0 = ambient, 1 = directional */
	VECTOR cube_center[3];                     /* 0x190 */
	VECTOR cube_half;                          /* 0x1C0  x,y,z = half-size of cube 0,1,2 */
	VECTOR probe_corners[8];                   /* 0x1D0  world corners of the white probe cube */
	VECTOR frustum;                            /* 0x250  left, right, bottom, top (at near) */
	VECTOR frustum2;                           /* 0x260  near, far, aspect, ground_y */
	VECTOR ground;                             /* 0x270  half-extent, uv span in texels, tex w, tex h */
	u32    magic1, variant, vu1_matrix_qw, vu1_dbuf; /* 0x280  variant 1 = EE transform (no VU1 matrix) */
} __attribute__((aligned(16))) RemixTruth;     /* 0x290 */
#define RMXT_VARIANT 1u

static RemixTruth truth __attribute__((aligned(64)));

/* ------------------------------------------------------------------------- */
/* Scene definition                                                          */
/* ------------------------------------------------------------------------- */

static VECTOR cam_pos0 = {  0.00f, 4.00f, 18.00f, 1.00f };
static VECTOR cam_rot0 = { -0.18f, 0.00f,  0.00f, 1.00f };   /* slight pitch toward the ground */

#define FR_LEFT   -0.5f
#define FR_RIGHT   0.5f
#define FR_BOTTOM -0.5f
#define FR_TOP     0.5f
#define FR_NEAR    1.0f
#define FR_FAR   200.0f
#define FR_ASPECT (4.0f / 3.0f)

static VECTOR light_dir[4] = {
	{  0.00f,  0.00f,  0.00f, 1.00f },   /* ambient: direction unused */
	{  0.40f, -0.80f, -0.45f, 1.00f },   /* sun  (normalised at init) */
	{ -0.70f, -0.30f,  0.60f, 1.00f },   /* fill (normalised at init) */
	{  0.00f,  0.00f,  0.00f, 1.00f }    /* unused slot, kept for layout */
};
static VECTOR light_col[4] = {
	{ 0.15f, 0.15f, 0.15f, 1.00f },
	{ 1.00f, 0.95f, 0.85f, 1.00f },
	{ 0.25f, 0.30f, 0.40f, 1.00f },
	{ 0.00f, 0.00f, 0.00f, 1.00f }
};
static int light_type[4] = { LIGHT_AMBIENT, LIGHT_DIRECTIONAL, LIGHT_DIRECTIONAL, LIGHT_DIRECTIONAL };
static const int light_count = 3;

#define NUM_CUBES 3
static const float cube_center[NUM_CUBES][3] = { { 0.0f, 0.0f, 0.0f }, { 6.0f, 0.0f, -4.0f }, { -6.0f, 1.0f, 3.0f } };
static const float cube_half[NUM_CUBES]      = { 1.0f, 1.0f, 1.5f };
#define PROBE_CUBE 1

/* Face order: +X -X +Y -Y +Z -Z. Cube 0 is colour-coded per face; 1 is the
 * white lighting probe; 2 is a single warm colour. */
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
#define TEX_W        64
#define TEX_H        64
#define GROUND_UV    512.0f   /* texels spanned across the plane: 8 repeats of a 64px checker */

#define CUBE_VERTS   (6 * 2 * 3)             /* 6 faces, 2 tris, 3 verts, unindexed */
#define NV           (NUM_CUBES * CUBE_VERTS) /* 108 */

/* ------------------------------------------------------------------------- */

static void vnorm3(VECTOR v)
{
	float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
	if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void build_cube(int c, VECTOR *pos, VECTOR *nrm, VECTOR *col)
{
	int f, k, n = 0;
	const float h = cube_half[c];
	const float *ctr = cube_center[c];
	for (f = 0; f < 6; f++) {
		/* quad corners: (-u-v) (+u-v) (+u+v) (-u+v) */
		static const int su[4] = { -1, 1, 1, -1 };
		static const int sv[4] = { -1, -1, 1, 1 };
		static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
		const float *fc = (c == 0) ? cube0_face_col[f] : (c == 1) ? cube1_col : cube2_col;
		for (k = 0; k < 6; k++) {
			int q = tri[k];
			pos[n][0] = ctr[0] + h * (face_n[f][0] + su[q]*face_u[f][0] + sv[q]*face_v[f][0]);
			pos[n][1] = ctr[1] + h * (face_n[f][1] + su[q]*face_u[f][1] + sv[q]*face_v[f][1]);
			pos[n][2] = ctr[2] + h * (face_n[f][2] + su[q]*face_u[f][2] + sv[q]*face_v[f][2]);
			pos[n][3] = 1.0f;
			nrm[n][0] = face_n[f][0]; nrm[n][1] = face_n[f][1]; nrm[n][2] = face_n[f][2]; nrm[n][3] = 1.0f;
			col[n][0] = fc[0]; col[n][1] = fc[1]; col[n][2] = fc[2]; col[n][3] = 1.0f;
			n++;
		}
	}
}

/* Same NDC -> GS conversion the gsKit cube example uses. */
static int ndc_to_gs(vertex_f_t *out, GSGLOBAL *g, int count, const vertex_f_t *in)
{
	int zbits;
	unsigned int max_z;
	switch (g->PSMZ) {
		case GS_PSMZ_32:  zbits = 32; break;
		case GS_PSMZ_24:  zbits = 24; break;
		case GS_PSMZ_16:
		case GS_PSMZ_16S: zbits = 16; break;
		default: return -1;
	}
	{
		float cx = g->Width / 2, cy = g->Height / 2;
		int i;
		max_z = 1u << (zbits - 1);
		for (i = 0; i < count; i++) {
			out[i].x = (in[i].x + 1.0f) * cx;
			out[i].y = (in[i].y + 1.0f) * cy;
			out[i].z = (unsigned int)((in[i].z + 1.0f) * max_z);
		}
	}
	return 0;
}

static void build_checker(u32 *px)
{
	int x, y;
	for (y = 0; y < TEX_H; y++)
		for (x = 0; x < TEX_W; x++) {
			int c = ((x >> 3) ^ (y >> 3)) & 1;
			u8 v = c ? 200 : 60;
			u8 r = v, gg = v, b = v;
			if (y < 2)  { r = 220; gg = 40;  b = 40;  }   /* red band along v = 0  */
			if (x < 2)  { r = 40;  gg = 60;  b = 220; }   /* blue band along u = 0 */
			px[y * TEX_W + x] = (0x80u << 24) | ((u32)b << 16) | ((u32)gg << 8) | r;
		}
}

static void publish(GSGLOBAL *g, u32 frame, u32 mode, const VECTOR cp, const VECTOR cr,
                    const MATRIX wv, const MATRIX vs, const MATRIX ws, const VECTOR *probe)
{
	int i;
	truth.magic0 = RMXT_MAGIC0; truth.version = RMXT_VERSION;
	truth.size = sizeof(truth); truth.frame = frame;
	truth.mode = mode; truth.width = g->Width; truth.height = g->Height; truth.light_count = light_count;
	vector_copy(truth.cam_pos, (float *)cp); vector_copy(truth.cam_rot, (float *)cr);
	matrix_copy(truth.world_view, (float *)wv);
	matrix_copy(truth.view_screen, (float *)vs);
	matrix_copy(truth.world_screen, (float *)ws);
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
	truth.ground[0] = GROUND_HALF; truth.ground[1] = GROUND_UV; truth.ground[2] = TEX_W; truth.ground[3] = TEX_H;
	truth.magic1 = RMXT_MAGIC1; truth.variant = RMXT_VARIANT; truth.vu1_matrix_qw = 0; truth.vu1_dbuf = 0;
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
	GSGLOBAL *g;
	GSTEXTURE tex;
	u32 *texpx;
	VECTOR *wpos, *wnrm, *wcol, *tv, *tl, *tc, *gv, *gtv;
	vertex_f_t *gsv, *ggsv;
	color_t *gcol;
	GSPRIMPOINT *pts;
	VECTOR probe[8];
	MATRIX world_view, view_screen, world_screen;
	VECTOR cam_pos, cam_rot;
	u32 frame = 0;
	int i, c;

	(void)argc; (void)argv;

	/* stdout goes through the IOP fileio RPC in ps2sdk; bind SIF before the first printf. */
	SifInitRpc(0);

	g = gsKit_init_global();               /* NTSC 640x448 interlaced, like a retail title */
	g->PrimAlphaEnable = GS_SETTING_OFF;   /* opaque world geometry, no blending */
	g->PrimAAEnable    = GS_SETTING_OFF;
	g->ZBuffering      = GS_SETTING_ON;
	g->DoubleBuffering = GS_SETTING_ON;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);
	gsKit_set_clamp(g, GS_CMODE_REPEAT);
	gsKit_vram_clear(g);
	gsKit_init_screen(g);
	gsKit_mode_switch(g, GS_ONESHOT);
	gsKit_set_test(g, GS_ZTEST_ON);

	printf("\nRMXT remixtest boot: %dx%d PSM=%d PSMZ=%d interlace=%d field=%d\n",
	       g->Width, g->Height, g->PSM, g->PSMZ, g->Interlace, g->Field);
	printf("RMXT truth block @ 0x%08x size 0x%x\n", (unsigned)&truth, (unsigned)sizeof(truth));
	printf("RMXT offsets: cam_pos 0x%x world_view 0x%x view_screen 0x%x world_screen 0x%x light_dir 0x%x light_col 0x%x probe 0x%x magic1 0x%x\n",
	       (unsigned)offsetof(RemixTruth, cam_pos), (unsigned)offsetof(RemixTruth, world_view),
	       (unsigned)offsetof(RemixTruth, view_screen), (unsigned)offsetof(RemixTruth, world_screen),
	       (unsigned)offsetof(RemixTruth, light_dir), (unsigned)offsetof(RemixTruth, light_col),
	       (unsigned)offsetof(RemixTruth, probe_corners), (unsigned)offsetof(RemixTruth, magic1));

	/* Lights: normalise the directional ones so intensity is a clean cosine. */
	vnorm3(light_dir[1]); vnorm3(light_dir[2]);

	/* Geometry, authored directly in world space. */
	wpos = memalign(128, sizeof(VECTOR) * NV);
	wnrm = memalign(128, sizeof(VECTOR) * NV);
	wcol = memalign(128, sizeof(VECTOR) * NV);
	tv   = memalign(128, sizeof(VECTOR) * NV);
	tl   = memalign(128, sizeof(VECTOR) * NV);
	tc   = memalign(128, sizeof(VECTOR) * NV);
	gsv  = memalign(128, sizeof(vertex_f_t) * NV);
	gcol = memalign(128, sizeof(color_t) * NV);
	pts  = memalign(128, sizeof(GSPRIMPOINT) * NV);
	for (c = 0; c < NUM_CUBES; c++) build_cube(c, wpos + c*CUBE_VERTS, wnrm + c*CUBE_VERTS, wcol + c*CUBE_VERTS);

	/* Probe cube corners for the truth block (unique corners of the unindexed mesh). */
	{
		const float *ctr = cube_center[PROBE_CUBE]; const float h = cube_half[PROBE_CUBE];
		for (i = 0; i < 8; i++) {
			probe[i][0] = ctr[0] + ((i & 1) ? h : -h);
			probe[i][1] = ctr[1] + ((i & 2) ? h : -h);
			probe[i][2] = ctr[2] + ((i & 4) ? h : -h);
			probe[i][3] = 1.0f;
		}
	}

	/* Ground: two triangles, unlit, textured with a procedural checker. */
	gv   = memalign(128, sizeof(VECTOR) * 6);
	gtv  = memalign(128, sizeof(VECTOR) * 6);
	ggsv = memalign(128, sizeof(vertex_f_t) * 6);
	{
		static const float gc[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
		static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
		for (i = 0; i < 6; i++) {
			gv[i][0] = GROUND_HALF * gc[tri[i]][0]; gv[i][1] = GROUND_Y;
			gv[i][2] = GROUND_HALF * gc[tri[i]][1]; gv[i][3] = 1.0f;
		}
	}
	texpx = memalign(128, TEX_W * TEX_H * 4);
	build_checker(texpx);
	memset(&tex, 0, sizeof(tex));
	tex.Width = TEX_W; tex.Height = TEX_H; tex.PSM = GS_PSM_CT32;
	tex.Mem = texpx; tex.Filter = GS_FILTER_NEAREST; tex.Delayed = 0;
	tex.Vram = gsKit_vram_alloc(g, gsKit_texture_size(tex.Width, tex.Height, tex.PSM), GSKIT_ALLOC_USERBUFFER);
	FlushCache(0);
	gsKit_texture_upload(g, &tex);
	printf("RMXT checker texture %dx%d CT32 uploaded to VRAM 0x%x\n", TEX_W, TEX_H, (unsigned)tex.Vram);

	create_view_screen(view_screen, FR_ASPECT, FR_LEFT, FR_RIGHT, FR_BOTTOM, FR_TOP, FR_NEAR, FR_FAR);
	/* math3d leaves +y up in NDC but GS y grows downward; fold the flip into the projection
	 * (retail convention) so view-up is screen-up. */
	view_screen[0x05] = -view_screen[0x05];
	view_screen[0x09] = -view_screen[0x09];
	print_matrix("view_screen", view_screen);
	printf("RMXT sun  dir %.5f %.5f %.5f  col %.2f %.2f %.2f\n", light_dir[1][0], light_dir[1][1], light_dir[1][2], light_col[1][0], light_col[1][1], light_col[1][2]);
	printf("RMXT fill dir %.5f %.5f %.5f  col %.2f %.2f %.2f\n", light_dir[2][0], light_dir[2][1], light_dir[2][2], light_col[2][0], light_col[2][1], light_col[2][2]);
	printf("RMXT ambient col %.2f %.2f %.2f\n", light_col[0][0], light_col[0][1], light_col[0][2]);

	for (;;) {
		u32 mode = (frame / MODE_FRAMES) % NUM_MODES;
		float t = (float)(frame % MODE_FRAMES) / (float)MODE_FRAMES;
		float ph = 2.0f * 3.14159265f * t;

		vector_copy(cam_pos, cam_pos0);
		vector_copy(cam_rot, cam_rot0);
		if (mode == 1) cam_pos[2] = cam_pos0[2] + 6.0f * (1.0f - cosf(ph));   /* dolly: z 18 -> 30 -> 18 */
		if (mode == 2) cam_rot[1] = 0.30f * sinf(ph);                         /* yaw sweep +-0.3 rad */

		create_world_view(world_view, cam_pos, cam_rot);
		matrix_multiply(world_screen, world_view, view_screen);

		/* Cubes: transform, light, convert. Normals are already world-space. */
		calculate_vertices(tv, NV, wpos, world_screen);
		calculate_lights(tl, NV, wnrm, light_dir, light_col, light_type, light_count);
		calculate_colours(tc, NV, wcol, tl);
		ndc_to_gs(gsv, g, NV, (vertex_f_t *)tv);
		draw_convert_rgbq(gcol, NV, (vertex_f_t *)tv, (color_f_t *)tc, 0x80);
		for (i = 0; i < NV; i++) {
			pts[i].rgbaq = color_to_RGBAQ(gcol[i].r, gcol[i].g, gcol[i].b, gcol[i].a, 0.0f);
			pts[i].xyz2  = vertex_to_XYZ2(g, gsv[i].x, gsv[i].y, gsv[i].z);
		}

		/* Ground */
		calculate_vertices(gtv, 6, gv, world_screen);
		ndc_to_gs(ggsv, g, 6, (vertex_f_t *)gtv);

		gsKit_clear(g, GS_SETREG_RGBAQ(0x20, 0x30, 0x50, 0x80, 0x00));

		{
			/* u,v in texels; corners (-1,-1)(1,-1)(1,1)(-1,1) -> (0,0)(UV,0)(UV,UV)(0,UV) */
			static const float uv[4][2] = { {0,0}, {GROUND_UV,0}, {GROUND_UV,GROUND_UV}, {0,GROUND_UV} };
			static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
			u64 white = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00);
			for (i = 0; i < 6; i += 3)
				gsKit_prim_triangle_texture_3d(g, &tex,
					ggsv[i+0].x, ggsv[i+0].y, ggsv[i+0].z, uv[tri[i+0]][0], uv[tri[i+0]][1],
					ggsv[i+1].x, ggsv[i+1].y, ggsv[i+1].z, uv[tri[i+1]][0], uv[tri[i+1]][1],
					ggsv[i+2].x, ggsv[i+2].y, ggsv[i+2].z, uv[tri[i+2]][0], uv[tri[i+2]][1],
					white);
		}

		gsKit_prim_list_triangle_gouraud_3d(g, NV, pts);

		publish(g, frame, mode, cam_pos, cam_rot, world_view, view_screen, world_screen, probe);

		if ((frame % 300) == 0) {
			printf("RMXT frame %u mode %u cam_pos %.3f %.3f %.3f cam_rot %.3f %.3f %.3f\n",
			       frame, mode, cam_pos[0], cam_pos[1], cam_pos[2], cam_rot[0], cam_rot[1], cam_rot[2]);
			if (frame == 0) {
				print_matrix("world_view", world_view);
				print_matrix("world_screen", world_screen);
				/* One probe vertex end to end, so the pipeline can be checked by hand. */
				i = PROBE_CUBE * CUBE_VERTS;
				printf("RMXT probe v0 world %.3f %.3f %.3f -> ndc %.4f %.4f %.4f w %.4f -> gs %.1f %.1f z %u rgb %u %u %u\n",
				       wpos[i][0], wpos[i][1], wpos[i][2], tv[i][0], tv[i][1], tv[i][2], tv[i][3],
				       gsv[i].x, gsv[i].y, (unsigned)gsv[i].z, gcol[i].r, gcol[i].g, gcol[i].b);
			}
		}

		gsKit_queue_exec(g);
		gsKit_sync_flip(g);
		frame++;
	}

	return 0;
}
