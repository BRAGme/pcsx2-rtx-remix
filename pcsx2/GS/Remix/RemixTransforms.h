// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// See RemixRuntime.h: NOMINMAX must be in effect before remix_c.h pulls <windows.h>.
#include "common/RedtapeWindows.h"

#include "GS/Remix/remix_c.h"

#include <string>

namespace remix_ps2
{
	// ---------------------------------------------------------------------------------------
	// 4x4 matrix, ROW-VECTOR convention throughout this file: p' = p * M, translation in row 3.
	// That is what remixapi_CameraInfo.view / .projection expect (verified against the runtime
	// by the RPCS3 port this is taken from). remixapi_Transform is the opposite
	// (column-vector); the two converters below are the only places that difference is allowed
	// to exist.
	// ---------------------------------------------------------------------------------------
	struct mat4
	{
		float m[4][4];
	};

	mat4 mat4_identity();
	mat4 mat4_zero();
	mat4 mat4_multiply(const mat4& a, const mat4& b);
	mat4 mat4_transpose(const mat4& a);
	bool mat4_invert(const mat4& a, mat4& out);
	float mat4_l1_error(const mat4& a, const mat4& b);
	bool mat4_is_finite(const mat4& a);
	bool mat4_is_identity(const mat4& a, float tol = 1e-5f);

	// Row-vector transform of a homogeneous point: out = p * m.
	void transform_point(const mat4& m, const float (&p)[4], float (&out)[4]);

	// ---------------------------------------------------------------------------------------
	// Converters -- the only two places the row/column-vector difference exists
	// ---------------------------------------------------------------------------------------

	// Row-vector affine matrix to remixapi_Transform (column-vector, translation in column 3).
	remixapi_Transform to_remix_transform(const mat4& m);

	// Row-vector matrix into remixapi_CameraInfo.view / .projection (also row-vector).
	void to_camera_matrix(const mat4& m, float (&out)[4][4]);

	// depth = (z/w)*scale_z + offset_z, folded into the projection's z output column so the
	// matrix handed to Remix produces D3D-style [0,1] depth whatever convention the title used.
	mat4 fold_viewport_z(const mat4& p, float scale_z, float offset_z);

	// ---------------------------------------------------------------------------------------
	// Classifiers
	// ---------------------------------------------------------------------------------------

	// 0 = not perspective, 1 = already row-vector, 2 = looks like the transpose.
	int classify_perspective(const mat4& m);

	// m[3][3] ~= 1 with no perspective column.
	bool is_orthographic(const mat4& m);

	struct projection_params
	{
		float fov_y_degrees = 0.f;
		float aspect = 0.f;
		float near_plane = 0.f;
	};

	bool describe_projection(const mat4& m, projection_params& out);

	// Aspect-vs-viewport main camera pick. Higher is better; <= 0 means reject.
	float score_perspective(const mat4& m, float reference_aspect);

	// Perspective row (column 3) is 0,0,0,1 - i.e. the matrix is a plain affine transform.
	bool is_affine(const mat4& m, float tol = 1e-3f);

	// ---------------------------------------------------------------------------------------
	// Split a fused view-projection into a view and a projection by unprojecting NDC points.
	// Port of dxvk-remix-mirrorsedge's d3d9_rtx.cpp:3910 by way of the RPCS3 backend.
	// ---------------------------------------------------------------------------------------
	struct vp_split
	{
		mat4 view = mat4_identity();
		mat4 projection = mat4_identity();
		float l1_error = 0.f;
		bool used_transpose = false;
	};

	bool split_view_projection(const mat4& fused, vp_split& out);

	// Single-orientation split: does NOT retry the transpose. The PS2 screen-clip normaliser
	// has to be applied in a known majorness, so the caller resolves row-vs-column itself and
	// asks for the split of each hypothesis separately -- split_view_projection's internal
	// retry would transpose a matrix the normaliser had already been composed into, which
	// mixes conventions and can accept a meaningless answer.
	// Which step of the split refused, so the stats line can name the stage rather than report
	// one opaque count. Every candidate on Rainbow Six 3 fails here, and "split-reject 4066432"
	// does not distinguish "the matrix was not invertible" from "the recovered projection was
	// not a perspective" -- which are different bugs with different fixes.
	enum class split_stage : u32
	{
		accepted = 0,
		not_finite,      // the fused matrix had a NaN or an infinity
		invert_fused,    // fused matrix is singular
		camera_position, // no solution for the eye point
		unproject,       // could not unproject the two reference clip points
		forward,         // eye-to-centre vector is degenerate
		up_hint,         // centre-to-above vector is degenerate
		basis,           // right/up cross products are degenerate
		invert_view,     // the assembled viewToWorld is singular
		not_perspective, // the recovered projection failed classify_perspective
		error_not_finite,// the reconstruction error came back non-finite
		count
	};

	const char* split_stage_name(split_stage stage);

	bool split_view_projection_direct(const mat4& fused, vp_split& out, split_stage* stage = nullptr);

	// ---------------------------------------------------------------------------------------
	// PS2 joint: screen-clip normalisation and the world un-projection
	// ---------------------------------------------------------------------------------------

	// Composes a matrix that emits the guest's own post-divide output space with the inverse
	// of the x/y viewport map, so its clip x/w and y/w become the same NDC the per-vertex
	// un-projection produces: result = fused * inverse(S), with
	//     S = [[sx 0 0 0] [0 sy 0 0] [0 0 1 0] [ox oy 0 1]].
	// The z column is deliberately left alone -- see make_clip_solver.
	mat4 normalize_screen_clip(const mat4& fused, float scale_x, float offset_x, float scale_y, float offset_y);

	// The OTHER half of the normalisation, and the half the hypothesis set structurally could
	// not reach.
	//
	// normalize_screen_clip rescales columns 0 and 1 only. It cannot change the recovered
	// world's UNIT, and the reason is exact rather than empirical: make_clip_solver builds B
	// from columns {0, 1, 3}, so world-per-unit-of-clip-w is fixed by column 3 alone -- for
	// mutually orthogonal rows it is exactly 1 / |column 3 xyz| -- and column 3 is precisely
	// what normalize_screen_clip leaves alone. A guest whose w is not eye-space depth in its
	// own units therefore lands outside PCSX2_REMIX_CAMSCALE with no hypothesis able to bring
	// it back. SOCOM: Combined Assault's register-resident candidate measures 7.232e-08, i.e.
	// a column 3 of magnitude 1.38e7.
	//
	//   scale_w  divides ALL SIXTEEN terms, not column 3 alone. Dividing column 3 by itself
	//            would multiply every post-divide NDC by the same factor and undo the screen
	//            normalisation composed before it. Scaling the whole matrix is projectively
	//            invariant -- the NDC it emits is bit-identical -- and moves exactly one thing:
	//            the recovered world is scaled about the recovered eye, which is the free
	//            parameter being fixed. It is therefore a GAUGE change for any scale-free
	//            reading (CAMTEST's alpha divides drift by range, and both carry it), and a
	//            REAL change for anything that reads absolute recovered coordinates -- the
	//            depth-scale gate, scene radius, light placement, the picture.
	//   swap_zw  exchanges columns 2 and 3: the assertion that the value the guest divided by
	//            is the one this code has been calling depth. NOT a gauge change -- it feeds a
	//            different third equation into make_clip_solver and recovers a different world.
	mat4 normalize_clip_depth(const mat4& fused, float scale_w, bool swap_zw);

	// |column 3 xyz| of a fused matrix: the factor normalize_clip_depth has to divide by for the
	// recovered world unit to become the guest's own. 1 when the column is degenerate.
	float clip_w_scale(const mat4& fused);

	// The nearest power of two when `value` is within `tolerance` (relative) of one, `value`
	// otherwise. Both are reported by the caller, so "this convention IS 2^-24" can be told
	// apart from "this convention is merely near 2^-24".
	float snap_power_of_two(float value, float tolerance);

	// Recovers world positions from a normalised fused matrix using clip x, y and w ONLY.
	// A PS2 vertex's GS Z is a raw integer in a per-title convention (usually reversed, and
	// scaled by the ZBUF format's max depth), so the fused matrix's z column cannot be
	// trusted. Dropping it costs nothing: three equations in three unknowns is exactly
	// determined, because w = 1/Q already carries the absolute depth.
	struct clip_solver
	{
		float inverse[3][3]; // of B, where B[k][i] = fused.m[i][c] for c in {0, 1, 3}
		float bias[3]; // fused.m[3][0], fused.m[3][1], fused.m[3][3]
	};

	bool make_clip_solver(const mat4& fused, clip_solver& out);
	void solve_world_position(const clip_solver& solver, float clip_x, float clip_y, float clip_w, float (&out)[3]);

	// Replaces a recovered projection's z output column with a synthetic one spanning
	// near..far. fold_viewport_z would be the RPCS3 move, but a PS2 fused matrix's z column
	// emits raw GS Z in an unknown convention and the world un-projection never reads it, so
	// there is nothing meaningful to fold. Rebuilding is correct-by-construction for Remix's
	// D3D-style [0,1] depth and stays consistent with geometry that has no z dependence.
	// The sign of m[2][3] (already pinned to +-1 by classify_perspective) carries handedness.
	mat4 rebuild_projection_z(const mat4& projection, float near_plane, float far_plane);

	// Builds a synthetic row-vector perspective projection in Remix's left-handed, +Y-up
	// convention, mapping z to D3D-style [0,1]. This is the projection the view-space tier
	// un-projects through: the geometry it produces is exactly what the parameterized debug
	// camera (same fov/aspect/near/far) expects to see.
	mat4 make_perspective(float fov_y_degrees, float aspect, float near_plane, float far_plane);

	// ---------------------------------------------------------------------------------------
	// Rigid registration (Kabsch), used for stable mesh identity
	// ---------------------------------------------------------------------------------------
	//
	// Finds the proper rotation R minimising sum |R*a_i - b_i|^2 for two mean-centred point
	// sets, and reports the residual. Horn's unit-quaternion method: build the 4x4 symmetric
	// matrix from the cross-covariance and take the eigenvector of its largest eigenvalue by
	// Jacobi rotation. Chosen over an SVD because it yields a proper rotation by construction
	// (no reflection to detect and undo) and stays well defined for the coplanar and collinear
	// point sets that dominate real geometry -- walls, floors and billboards are all rank 2.
	//
	// 'a' and 'b' are 3*count floats, both already relative to their own centroid. 'out' is
	// row-major R[i][j] with b ~= R*a. Returns false only if the input is empty or non-finite.
	bool kabsch_rotation(const float* a, const float* b, size_t count, float (&out)[3][3]);

	// RMS of |R*a_i - b_i| over the set. Reported separately from the solve so a caller can
	// decide what residual means "the same object moved" versus "a different object".
	float rigid_residual(const float* a, const float* b, size_t count, const float (&rotation)[3][3]);

	// Debug helper: "[a b c d | e f g h | ...]"
	std::string format_matrix(const mat4& m);

	// Env toggles, GetEnvironmentVariableW based like hardcoded_far_plane().
	// PCSX2_REMIX_DUMP=1   diagnostic dump file, one line per unique scanned matrix.
	// PCSX2_REMIX_NOCAM=1  force the parameterized fallback camera.
	bool dump_enabled();
	bool nocam_enabled();
} // namespace remix_ps2
