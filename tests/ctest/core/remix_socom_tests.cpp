// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixSocomCamera.h"
#include <gtest/gtest.h>
#include <cmath>

namespace
{
using namespace remix_ps2;
using namespace remix_ps2::socom_camera;

mat4 RasterProjection()
{
	mat4 raster{};
	raster.m[0][0] = 342.730f;
	raster.m[1][1] = 426.509f;
	raster.m[2][0] = raster.m[2][1] = 2048.f;
	raster.m[2][2] = -3.10117f;
	raster.m[2][3] = 1.f;
	raster.m[3][2] = 262152.4f;
	return raster;
}

void RecoverThroughGS(const mat4& raw, const Camera& camera, const Viewport& viewport,
	const float (&point)[3], float (&out)[3])
{
	const float homogeneous[] = {point[0], point[1], point[2], 1.f};
	float projected[4];
	transform_point(raw, homogeneous, projected);
	const float sx = 2.f / (viewport.width * 16.f);
	const float sy = 2.f / (viewport.height * 16.f);
	const float nx = (projected[0] * 16.f - .05f * projected[3]) * sx -
		(viewport.ofx * sx - 1.f / viewport.width + 1.f) * projected[3];
	const float ny = -((projected[1] * 16.f - .05f * projected[3]) * sy -
		(viewport.ofy * sy - 1.f / viewport.height + 1.f) * projected[3]);
	solve_world_position(camera.solver, nx, ny, projected[3], out);
}
}

TEST(RemixSocomCamera, GSPositionsStayFixedAcrossCameraYaw)
{
	const Viewport viewport{27648.f, 29184.f, 640, 448};
	const float eye[] = {1930.f, 210.f, 3920.f};
	const float landmark[] = {1700.f, 170.f, 3600.f};
	for (int degrees = -180; degrees <= 180; degrees += 10)
	{
		const float yaw = degrees * 3.14159265359f / 180.f;
		mat4 view = mat4_identity();
		view.m[0][0] = view.m[2][2] = std::cos(yaw);
		view.m[0][2] = -std::sin(yaw);
		view.m[2][0] = std::sin(yaw);
		for (u32 column = 0; column < 3; ++column)
			for (u32 row = 0; row < 3; ++row)
				view.m[3][column] -= eye[row] * view.m[row][column];
		const mat4 raw = mat4_multiply(view, RasterProjection());
		Camera camera;
		ASSERT_TRUE(BuildRaster(raw, viewport, camera));
		float recovered[3];
		RecoverThroughGS(raw, camera, viewport, landmark, recovered);
		for (u32 axis = 0; axis < 3; ++axis)
			EXPECT_NEAR(recovered[axis], landmark[axis], .005f) << degrees;
	}
}

TEST(RemixSocomCamera, UsesCurrentRenderTargetViewport)
{
	const mat4 raw = RasterProjection();
	const float point[] = {25.f, -12.f, 90.f};
	const Viewport viewports[] = {{27648.f, 29184.f, 640, 448}, {32768.f, 32768.f, 128, 128}};
	for (const auto& viewport : viewports)
	{
		Camera camera;
		ASSERT_TRUE(BuildRaster(raw, viewport, camera));
		float recovered[3];
		RecoverThroughGS(raw, camera, viewport, point, recovered);
		for (u32 axis = 0; axis < 3; ++axis)
			EXPECT_NEAR(recovered[axis], point[axis], .001f);
	}
}

TEST(RemixSocomCamera, RefusesCullingAndReflectedTransforms)
{
	mat4 culling{};
	culling.m[0][0] = .535516f;
	culling.m[1][1] = .666420f;
	culling.m[2][2] = 1.000399f;
	culling.m[2][3] = 1.f;
	culling.m[3][2] = -8.00178f;
	EXPECT_FALSE(IsRaster(culling));
	mat4 reflected = RasterProjection();
	reflected.m[0][0] = -reflected.m[0][0];
	EXPECT_FALSE(IsRaster(reflected));
	EXPECT_TRUE(IsRaster(RasterProjection()));
}
