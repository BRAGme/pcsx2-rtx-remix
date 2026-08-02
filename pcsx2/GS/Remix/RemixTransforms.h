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

	// Builds a synthetic row-vector perspective projection in Remix's left-handed, +Y-up
	// convention, mapping z to D3D-style [0,1]. This is the projection the view-space tier
	// un-projects through: the geometry it produces is exactly what the parameterized debug
	// camera (same fov/aspect/near/far) expects to see.
	mat4 make_perspective(float fov_y_degrees, float aspect, float near_plane, float far_plane);

	// Debug helper: "[a b c d | e f g h | ...]"
	std::string format_matrix(const mat4& m);

	// Env toggles, GetEnvironmentVariableW based like hardcoded_far_plane().
	// PCSX2_REMIX_DUMP=1   diagnostic dump file, one line per unique scanned matrix.
	// PCSX2_REMIX_NOCAM=1  force the parameterized fallback camera.
	bool dump_enabled();
	bool nocam_enabled();
} // namespace remix_ps2
