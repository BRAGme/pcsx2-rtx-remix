// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixTransforms.h"
#include "GS/Remix/RemixRuntime.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

// Ported from rpcs3/Emu/RSX/Remix/RemixTransforms.cpp (branch remix-backend). Only the
// RSX-independent half transfers: the matrix algebra, the classifiers, and the fused
// view-projection splitter. The ucode walker has no VU1 equivalent and is not ported here.

namespace remix_ps2
{
	namespace
	{
		bool env_flag(const wchar_t* name)
		{
			const std::wstring value = read_env(name);
			return !value.empty() && value[0] != L'0';
		}

		float vec_length(const float (&v)[3])
		{
			return std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
		}

		bool normalize3(float (&v)[3])
		{
			const float len = vec_length(v);
			if (!std::isfinite(len) || len < 1e-8f)
				return false;

			v[0] /= len;
			v[1] /= len;
			v[2] /= len;
			return true;
		}

		void cross3(const float (&a)[3], const float (&b)[3], float (&out)[3])
		{
			out[0] = (a[1] * b[2]) - (a[2] * b[1]);
			out[1] = (a[2] * b[0]) - (a[0] * b[2]);
			out[2] = (a[0] * b[1]) - (a[1] * b[0]);
		}

		bool unproject(const mat4& inverse_m, float ndc_x, float ndc_y, float ndc_z, float (&out)[3])
		{
			const float clip[4] = {ndc_x, ndc_y, ndc_z, 1.f};
			float world[4]{};
			transform_point(inverse_m, clip, world);

			if (!std::isfinite(world[3]) || std::abs(world[3]) < 1e-9f)
				return false;

			out[0] = world[0] / world[3];
			out[1] = world[1] / world[3];
			out[2] = world[2] / world[3];
			return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
		}

		// The eye is the world point whose clip x, y and w all vanish.
		bool solve_camera_position(const mat4& m, float (&out)[3])
		{
			const u32 cols[3] = {0, 1, 3};

			double a[3][4]{};
			for (u32 r = 0; r < 3; ++r)
			{
				for (u32 k = 0; k < 3; ++k)
					a[r][k] = m.m[k][cols[r]];

				a[r][3] = -static_cast<double>(m.m[3][cols[r]]);
			}

			// Gauss-Jordan with partial pivoting.
			for (u32 col = 0; col < 3; ++col)
			{
				u32 pivot = col;
				for (u32 r = col + 1; r < 3; ++r)
				{
					if (std::abs(a[r][col]) > std::abs(a[pivot][col]))
						pivot = r;
				}

				if (std::abs(a[pivot][col]) < 1e-12)
					return false;

				if (pivot != col)
				{
					for (u32 k = 0; k < 4; ++k)
						std::swap(a[pivot][k], a[col][k]);
				}

				const double inv = 1.0 / a[col][col];
				for (u32 k = 0; k < 4; ++k)
					a[col][k] *= inv;

				for (u32 r = 0; r < 3; ++r)
				{
					if (r == col)
						continue;

					const double factor = a[r][col];
					for (u32 k = 0; k < 4; ++k)
						a[r][k] -= factor * a[col][k];
				}
			}

			for (u32 r = 0; r < 3; ++r)
			{
				if (!std::isfinite(a[r][3]))
					return false;

				out[r] = static_cast<float>(a[r][3]);
			}

			return true;
		}

		bool try_split_once(const mat4& fused, vp_split& out)
		{
			mat4 inverse_fused{};
			if (!mat4_invert(fused, inverse_fused))
				return false;

			float cam_pos[3]{};
			if (!solve_camera_position(fused, cam_pos))
				return false;

			float center[3]{};
			float above[3]{};
			if (!unproject(inverse_fused, 0.f, 0.f, 0.5f, center) ||
				!unproject(inverse_fused, 0.f, 1.f, 0.5f, above))
			{
				return false;
			}

			float forward[3] = {center[0] - cam_pos[0], center[1] - cam_pos[1], center[2] - cam_pos[2]};
			if (!normalize3(forward))
				return false;

			float up_hint[3] = {above[0] - center[0], above[1] - center[1], above[2] - center[2]};
			if (!normalize3(up_hint))
				return false;

			float right[3]{};
			cross3(up_hint, forward, right);
			if (!normalize3(right))
				return false;

			float up[3]{};
			cross3(forward, right, up);
			if (!normalize3(up))
				return false;

			// viewToWorld: rows are right / up / forward / position (Remix's own layout).
			mat4 view_to_world = mat4_identity();
			for (u32 k = 0; k < 3; ++k)
			{
				view_to_world.m[0][k] = right[k];
				view_to_world.m[1][k] = up[k];
				view_to_world.m[2][k] = forward[k];
				view_to_world.m[3][k] = cam_pos[k];
			}

			mat4 world_to_view{};
			if (!mat4_invert(view_to_world, world_to_view))
				return false;

			// M = V * P  =>  P = viewToWorld * M.
			const mat4 projection = mat4_multiply(view_to_world, fused);

			if (!mat4_is_finite(projection) || classify_perspective(projection) != 1)
				return false;

			out.view = world_to_view;
			out.projection = projection;
			out.l1_error = mat4_l1_error(mat4_multiply(world_to_view, projection), fused);
			out.used_transpose = false;
			return std::isfinite(out.l1_error);
		}
	} // namespace

	// -------------------------------------------------------------------------------------------
	// Matrix utilities
	// -------------------------------------------------------------------------------------------

	mat4 mat4_identity()
	{
		mat4 result{};
		result.m[0][0] = 1.f;
		result.m[1][1] = 1.f;
		result.m[2][2] = 1.f;
		result.m[3][3] = 1.f;
		return result;
	}

	mat4 mat4_zero()
	{
		return mat4{};
	}

	mat4 mat4_multiply(const mat4& a, const mat4& b)
	{
		mat4 result{};

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				float sum = 0.f;
				for (u32 k = 0; k < 4; ++k)
					sum += a.m[i][k] * b.m[k][j];

				result.m[i][j] = sum;
			}
		}

		return result;
	}

	mat4 mat4_transpose(const mat4& a)
	{
		mat4 result{};

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
				result.m[i][j] = a.m[j][i];
		}

		return result;
	}

	bool mat4_invert(const mat4& a, mat4& out)
	{
		const float* m = &a.m[0][0];
		float inv[16]{};

		inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
		inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
		inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
		inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
		inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
		inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
		inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
		inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
		inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
		inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
		inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
		inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

		float det = (m[0] * inv[0]) + (m[1] * inv[4]) + (m[2] * inv[8]) + (m[3] * inv[12]);

		if (!std::isfinite(det) || std::abs(det) < 1e-24f)
			return false;

		det = 1.f / det;

		for (u32 i = 0; i < 16; ++i)
			(&out.m[0][0])[i] = inv[i] * det;

		return mat4_is_finite(out);
	}

	float mat4_l1_error(const mat4& a, const mat4& b)
	{
		float sum = 0.f;

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
				sum += std::abs(a.m[i][j] - b.m[i][j]);
		}

		return sum;
	}

	bool mat4_is_finite(const mat4& a)
	{
		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				if (!std::isfinite(a.m[i][j]))
					return false;
			}
		}

		return true;
	}

	bool mat4_is_identity(const mat4& a, float tol)
	{
		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				const float expected = (i == j) ? 1.f : 0.f;
				if (std::abs(a.m[i][j] - expected) > tol)
					return false;
			}
		}

		return true;
	}

	void transform_point(const mat4& m, const float (&p)[4], float (&out)[4])
	{
		for (u32 j = 0; j < 4; ++j)
			out[j] = (p[0] * m.m[0][j]) + (p[1] * m.m[1][j]) + (p[2] * m.m[2][j]) + (p[3] * m.m[3][j]);
	}

	// -------------------------------------------------------------------------------------------
	// Converters
	// -------------------------------------------------------------------------------------------

	remixapi_Transform to_remix_transform(const mat4& m)
	{
		remixapi_Transform result{};

		for (u32 i = 0; i < 3; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				// remixapi_Transform is column-vector: out_i = sum_j matrix[i][j] * in_j.
				result.matrix[i][j] = m.m[j][i];
			}
		}

		return result;
	}

	void to_camera_matrix(const mat4& m, float (&out)[4][4])
	{
		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
				out[i][j] = m.m[i][j];
		}
	}

	mat4 fold_viewport_z(const mat4& p, float scale_z, float offset_z)
	{
		mat4 result = p;

		for (u32 k = 0; k < 4; ++k)
			result.m[k][2] = (scale_z * p.m[k][2]) + (offset_z * p.m[k][3]);

		return result;
	}

	mat4 make_perspective(float fov_y_degrees, float aspect, float near_plane, float far_plane)
	{
		// Left-handed, +Y up, z into [0,1] -- the convention the parameterized camera describes.
		const float fov_y = fov_y_degrees * (3.14159265358979323846f / 180.f);
		const float h = 1.f / std::tan(fov_y * 0.5f);
		const float w = (aspect > 1e-6f) ? (h / aspect) : h;
		const float range = far_plane - near_plane;

		mat4 result = mat4_zero();
		result.m[0][0] = w;
		result.m[1][1] = h;
		result.m[2][2] = (range > 1e-6f) ? (far_plane / range) : 1.f;
		result.m[2][3] = 1.f;
		result.m[3][2] = (range > 1e-6f) ? (-near_plane * far_plane / range) : 0.f;
		return result;
	}

	// -------------------------------------------------------------------------------------------
	// Classifiers
	// -------------------------------------------------------------------------------------------

	int classify_perspective(const mat4& mat)
	{
		constexpr float tol = 0.02f;
		constexpr float jitter_tol = 0.35f;

		const auto& m = mat.m;

		if (!mat4_is_finite(mat))
			return 0;

		if (std::abs(m[0][1]) > tol || std::abs(m[0][3]) > tol)
			return 0;

		if (std::abs(m[1][0]) > tol || std::abs(m[1][3]) > tol)
			return 0;

		if (std::abs(m[0][0]) < 0.1f || std::abs(m[1][1]) < 0.1f)
			return 0;

		if (std::abs(std::abs(m[2][3]) - 1.f) < tol && std::abs(m[3][3]) < tol)
		{
			if (std::abs(m[0][2]) > tol || std::abs(m[1][2]) > tol)
				return 0;

			if (std::abs(m[3][0]) > tol || std::abs(m[3][1]) > tol)
				return 0;

			return 1;
		}

		if (std::abs(std::abs(m[3][2]) - 1.f) < tol && std::abs(m[3][3]) < tol)
		{
			if (std::abs(m[0][2]) > jitter_tol || std::abs(m[1][2]) > jitter_tol)
				return 0;

			if (std::abs(m[2][0]) > tol || std::abs(m[2][1]) > tol)
				return 0;

			if (std::abs(m[3][0]) > tol || std::abs(m[3][1]) > tol)
				return 0;

			return 2;
		}

		return 0;
	}

	bool is_orthographic(const mat4& mat)
	{
		const auto& m = mat.m;

		return std::abs(m[0][3]) < 1e-5f &&
		       std::abs(m[1][3]) < 1e-5f &&
		       std::abs(m[2][3]) < 1e-5f &&
		       std::abs(m[3][3] - 1.f) < 1e-5f;
	}

	bool describe_projection(const mat4& mat, projection_params& out)
	{
		const auto& m = mat.m;

		const float x_scale = std::abs(m[0][0]);
		const float y_scale = std::abs(m[1][1]);

		if (!(x_scale > 1e-4f) || !(y_scale > 1e-4f))
			return false;

		out.fov_y_degrees = static_cast<float>(2.0 * std::atan(1.0 / static_cast<double>(y_scale)) * (180.0 / 3.14159265358979323846));
		out.aspect = y_scale / x_scale;

		out.near_plane = (std::abs(m[2][2]) > 1e-6f) ? std::abs(m[3][2] / m[2][2]) : 0.f;

		return std::isfinite(out.fov_y_degrees) && std::isfinite(out.aspect);
	}

	float score_perspective(const mat4& mat, float reference_aspect)
	{
		if (classify_perspective(mat) != 1)
			return 0.f;

		projection_params params{};
		if (!describe_projection(mat, params))
			return 0.f;

		if (params.fov_y_degrees < 15.f || params.fov_y_degrees > 150.f)
			return 0.f;

		// Cube-map faces and shadow cascades are square while the scene camera matches the display.
		if (reference_aspect > 1.1f && std::abs(params.aspect - 1.f) < 0.02f)
			return 0.f;

		float score = 1.f;

		score += (params.fov_y_degrees >= 30.f && params.fov_y_degrees <= 120.f) ? 2.f : 1.f;

		const float aspect_delta = std::abs(params.aspect - reference_aspect);
		if (aspect_delta < 0.15f)
			score += 2.f;
		else if (aspect_delta < 0.5f)
			score += 1.f;

		if (params.near_plane > 0.001f && params.near_plane < 100.f)
			score += 1.f;

		if (mat.m[0][0] < 0.f)
			score -= 0.5f;

		if (mat.m[1][1] < 0.f)
			score -= 1.f;

		return score;
	}

	bool is_affine(const mat4& mat, float tol)
	{
		const auto& m = mat.m;

		return std::abs(m[0][3]) < tol &&
		       std::abs(m[1][3]) < tol &&
		       std::abs(m[2][3]) < tol &&
		       std::abs(m[3][3] - 1.f) < tol;
	}

	bool split_view_projection_direct(const mat4& fused, vp_split& out)
	{
		if (!mat4_is_finite(fused))
			return false;

		return try_split_once(fused, out);
	}

	mat4 normalize_screen_clip(const mat4& fused, float scale_x, float offset_x, float scale_y, float offset_y)
	{
		mat4 result = fused;

		const float inv_sx = (std::abs(scale_x) > 1e-12f) ? (1.f / scale_x) : 1.f;
		const float inv_sy = (std::abs(scale_y) > 1e-12f) ? (1.f / scale_y) : 1.f;

		for (u32 i = 0; i < 4; ++i)
		{
			result.m[i][0] = (fused.m[i][0] - (offset_x * fused.m[i][3])) * inv_sx;
			result.m[i][1] = (fused.m[i][1] - (offset_y * fused.m[i][3])) * inv_sy;
		}

		return result;
	}

	bool make_clip_solver(const mat4& fused, clip_solver& out)
	{
		// Row-vector: clip_j = x*m[0][j] + y*m[1][j] + z*m[2][j] + m[3][j]. Take j in
		// {0, 1, 3} and the three equations are exactly determined in (x, y, z).
		constexpr u32 cols[3] = {0, 1, 3};

		float b[3][3]{};
		for (u32 k = 0; k < 3; ++k)
		{
			for (u32 i = 0; i < 3; ++i)
				b[k][i] = fused.m[i][cols[k]];

			out.bias[k] = fused.m[3][cols[k]];
			if (!std::isfinite(out.bias[k]))
				return false;
		}

		const float c00 = (b[1][1] * b[2][2]) - (b[1][2] * b[2][1]);
		const float c01 = (b[1][2] * b[2][0]) - (b[1][0] * b[2][2]);
		const float c02 = (b[1][0] * b[2][1]) - (b[1][1] * b[2][0]);

		const float det = (b[0][0] * c00) + (b[0][1] * c01) + (b[0][2] * c02);
		if (!std::isfinite(det) || std::abs(det) < 1e-20f)
			return false;

		const float inv_det = 1.f / det;

		out.inverse[0][0] = c00 * inv_det;
		out.inverse[1][0] = c01 * inv_det;
		out.inverse[2][0] = c02 * inv_det;

		out.inverse[0][1] = ((b[0][2] * b[2][1]) - (b[0][1] * b[2][2])) * inv_det;
		out.inverse[1][1] = ((b[0][0] * b[2][2]) - (b[0][2] * b[2][0])) * inv_det;
		out.inverse[2][1] = ((b[0][1] * b[2][0]) - (b[0][0] * b[2][1])) * inv_det;

		out.inverse[0][2] = ((b[0][1] * b[1][2]) - (b[0][2] * b[1][1])) * inv_det;
		out.inverse[1][2] = ((b[0][2] * b[1][0]) - (b[0][0] * b[1][2])) * inv_det;
		out.inverse[2][2] = ((b[0][0] * b[1][1]) - (b[0][1] * b[1][0])) * inv_det;

		for (u32 i = 0; i < 3; ++i)
		{
			for (u32 k = 0; k < 3; ++k)
			{
				if (!std::isfinite(out.inverse[i][k]))
					return false;
			}
		}

		return true;
	}

	void solve_world_position(const clip_solver& solver, float clip_x, float clip_y, float clip_w, float (&out)[3])
	{
		const float rhs[3] = {clip_x - solver.bias[0], clip_y - solver.bias[1], clip_w - solver.bias[2]};

		for (u32 i = 0; i < 3; ++i)
			out[i] = (solver.inverse[i][0] * rhs[0]) + (solver.inverse[i][1] * rhs[1]) + (solver.inverse[i][2] * rhs[2]);
	}

	mat4 rebuild_projection_z(const mat4& projection, float near_plane, float far_plane)
	{
		mat4 result = projection;

		const float range = far_plane - near_plane;
		if (!(range > 1e-6f))
			return result;

		// classify_perspective has already pinned |m[2][3]| to 1; its sign is the handedness.
		const float w_sign = (projection.m[2][3] < 0.f) ? -1.f : 1.f;

		result.m[0][2] = 0.f;
		result.m[1][2] = 0.f;
		result.m[2][2] = w_sign * (far_plane / range);
		result.m[3][2] = -w_sign * ((near_plane * far_plane) / range);

		return result;
	}

	bool split_view_projection(const mat4& fused, vp_split& out)
	{
		if (!mat4_is_finite(fused))
			return false;

		vp_split direct{};
		const bool have_direct = try_split_once(fused, direct);

		vp_split transposed{};
		bool have_transposed = try_split_once(mat4_transpose(fused), transposed);
		if (have_transposed)
			transposed.used_transpose = true;

		if (have_direct && have_transposed)
		{
			out = (direct.l1_error <= transposed.l1_error) ? direct : transposed;
			return true;
		}

		if (have_direct)
		{
			out = direct;
			return true;
		}

		if (have_transposed)
		{
			out = transposed;
			return true;
		}

		return false;
	}

	std::string format_matrix(const mat4& m)
	{
		std::string result = "[";

		for (u32 i = 0; i < 4; ++i)
		{
			if (i)
				result += " | ";

			for (u32 j = 0; j < 4; ++j)
			{
				if (j)
					result += ' ';

				fmt::format_to(std::back_inserter(result), "{:.5g}", m.m[i][j]);
			}
		}

		result += ']';
		return result;
	}

	bool dump_enabled()
	{
		static const bool value = env_flag(L"PCSX2_REMIX_DUMP");
		return value;
	}

	bool nocam_enabled()
	{
		static const bool value = env_flag(L"PCSX2_REMIX_NOCAM");
		return value;
	}
} // namespace remix_ps2
