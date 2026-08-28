// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixTransforms.h"

#include <gtest/gtest.h>

namespace
{
	void expect_point(const float (&actual)[3], float x, float y, float z)
	{
		EXPECT_FLOAT_EQ(actual[0], x);
		EXPECT_FLOAT_EQ(actual[1], y);
		EXPECT_FLOAT_EQ(actual[2], z);
	}
}

TEST(RemixTransforms, WorldBasisOffDoesNotDoubleRotate)
{
	const remix_ps2::mat4 view = remix_ps2::mat4_identity();
	const float camera[3] = {10.f, 20.f, 30.f};
	const float point[3] = {13.f, 24.f, 35.f};
	float output[3]{};

	remix_ps2::apply_world_basis_rotation(view, camera, 0, point, output);

	expect_point(output, 13.f, 24.f, 35.f);
}

TEST(RemixTransforms, WorldBasisIdentityPreservesPointAndPivot)
{
	const remix_ps2::mat4 view = remix_ps2::mat4_identity();
	const float camera[3] = {10.f, 20.f, 30.f};
	const float point[3] = {13.f, 24.f, 35.f};
	float output[3]{};

	remix_ps2::apply_world_basis_rotation(view, camera, 1, point, output);
	expect_point(output, 13.f, 24.f, 35.f);

	remix_ps2::apply_world_basis_rotation(view, camera, 2, point, output);
	expect_point(output, 13.f, 24.f, 35.f);
}

TEST(RemixTransforms, WorldBasisYawDistinguishesRowsFromTranspose)
{
	remix_ps2::mat4 view = remix_ps2::mat4_identity();
	view.m[0][0] = 0.f;
	view.m[0][2] = 1.f;
	view.m[2][0] = -1.f;
	view.m[2][2] = 0.f;
	const float camera[3] = {10.f, 20.f, 30.f};
	const float point[3] = {11.f, 20.f, 30.f};
	float output[3]{};

	remix_ps2::apply_world_basis_rotation(view, camera, 1, point, output);
	expect_point(output, 10.f, 20.f, 29.f);

	remix_ps2::apply_world_basis_rotation(view, camera, 2, point, output);
	expect_point(output, 10.f, 20.f, 31.f);
}

TEST(RemixTransforms, WorldBasisPitchDistinguishesRowsFromTranspose)
{
	remix_ps2::mat4 view = remix_ps2::mat4_identity();
	view.m[1][1] = 0.f;
	view.m[1][2] = 1.f;
	view.m[2][1] = -1.f;
	view.m[2][2] = 0.f;
	const float camera[3] = {10.f, 20.f, 30.f};
	const float point[3] = {10.f, 21.f, 30.f};
	float output[3]{};

	remix_ps2::apply_world_basis_rotation(view, camera, 1, point, output);
	expect_point(output, 10.f, 20.f, 29.f);

	remix_ps2::apply_world_basis_rotation(view, camera, 2, point, output);
	expect_point(output, 10.f, 20.f, 31.f);
}
