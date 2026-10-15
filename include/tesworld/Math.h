#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace tesworld
{
	struct Vec2
	{
		float x{ 0.0f };
		float y{ 0.0f };
	};

	struct Vec3
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };

		friend Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		friend Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		friend Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	};

	struct Vec4
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
		float w{ 0.0f };
	};

	[[nodiscard]] inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	[[nodiscard]] inline Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	[[nodiscard]] inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
	[[nodiscard]] inline Vec3 Normalize(Vec3 a, Vec3 fallback = { 0.0f, 0.0f, 1.0f })
	{
		const auto length = Length(a);
		return length > 1e-12f ? a * (1.0f / length) : fallback;
	}

	// A similarity transform in the game's Z-up space: world = translation + rotation * (scale * local). Rotation is
	// row-major.
	struct Transform
	{
		std::array<std::array<float, 3>, 3> rotation{ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
		Vec3 translation{};
		float scale{ 1.0f };

		[[nodiscard]] Vec3 Apply(Vec3 a_point) const
		{
			return translation + RotateVector(a_point) * scale;
		}

		[[nodiscard]] Vec3 RotateVector(Vec3 a_vector) const
		{
			return {
				rotation[0][0] * a_vector.x + rotation[0][1] * a_vector.y + rotation[0][2] * a_vector.z,
				rotation[1][0] * a_vector.x + rotation[1][1] * a_vector.y + rotation[1][2] * a_vector.z,
				rotation[2][0] * a_vector.x + rotation[2][1] * a_vector.y + rotation[2][2] * a_vector.z,
			};
		}

		// This transform after a_inner: (this * inner)(p) = this(inner(p)).
		[[nodiscard]] Transform operator*(const Transform& a_inner) const
		{
			Transform result;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					result.rotation[i][j] =
						rotation[i][0] * a_inner.rotation[0][j] + rotation[i][1] * a_inner.rotation[1][j] + rotation[i][2] * a_inner.rotation[2][j];
				}
			}
			result.scale = scale * a_inner.scale;
			result.translation = Apply(a_inner.translation);
			return result;
		}

		// A placed reference's transform from its DATA position and rotation (radians) and XSCL scale. Bethesda's
		// rotations turn clockwise about X, then Y, then Z, so the matrix is (Rz Ry Rx) transposed.
		[[nodiscard]] static Transform FromPlacement(const float a_position[3], const float a_rotation[3], float a_scale)
		{
			const float sx = std::sin(a_rotation[0]), sy = std::sin(a_rotation[1]), sz = std::sin(a_rotation[2]);
			const float cx = std::cos(a_rotation[0]), cy = std::cos(a_rotation[1]), cz = std::cos(a_rotation[2]);
			Transform result;
			result.rotation = { {
				{ cy * cz, cy * sz, -sy },
				{ sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy },
				{ cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy },
			} };
			result.translation = { a_position[0], a_position[1], a_position[2] };
			result.scale = a_scale;
			return result;
		}
	};

	[[nodiscard]] constexpr std::int32_t FloorDiv(std::int32_t a_value, std::int32_t a_divisor)
	{
		const auto quotient = a_value / a_divisor;
		return (a_value % a_divisor != 0 && ((a_value < 0) != (a_divisor < 0))) ? quotient - 1 : quotient;
	}
}
