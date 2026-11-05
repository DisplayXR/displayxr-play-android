// core/profile.h — what a per-game profile tells the GLES stereo core.
//
// The core (hooks, twins, replay, camera patch, transport) is engine-agnostic.
// Everything it must know about ONE game lives behind this interface, so a new
// game is a new profiles/<game>/ directory, not a fork of the core.
// Proposal — see docs/track4-core.md §2. Not wired into the core yet.
#pragma once

#include <cstdint>

#include <GLES3/gl3.h>

namespace dxr::core {

// How a camera uniform is patched per eye.
enum class CameraKind : uint8_t {
	None,      // not a camera; never touched
	Proj,      // bare projection: shift + off-axis directly (safe to test values)
	ViewProj,  // combined P*V (or P*V*M): patched with the profile's stored P,
	           // never classified by testing the matrix values (C-fit bug 1)
};

struct UniformInfo {
	const char *name;  // as returned by glGetActiveUniform, "[0]" stripped
	GLenum type;       // GL_FLOAT_MAT4, GL_FLOAT_VEC4, ...
	GLint size;        // array size
};

struct TargetInfo {
	int w, h;
	GLenum internal_format;
	int samples;
	bool layered;  // array/3D/cube attachment
};

struct Profile {
	const char *name = "generic";

	// Camera locator: called once per uniform when a program is first seen.
	CameraKind (*classify_uniform)(const UniformInfo &u) = nullptr;

	// Optional: value-pattern or UBO-based locator for engines without usable
	// names (packed arrays, uniform buffers). Returns true and the byte offset
	// of a 4x4 camera matrix inside a buffer upload. nullptr = names only.
	bool (*locate_in_buffer)(GLuint buffer, GLintptr offset, GLsizeiptr size, const void *data,
	                         GLintptr *matrix_offset, CameraKind *kind) = nullptr;

	// Should a render target get a right-eye twin? Default rule: non-square,
	// not layered, >= 16 px (shadow maps and cube faces stay mono).
	bool (*is_stereo_target)(const TargetInfo &t) = nullptr;

	// World units per real-world metre at the convergence plane. Turns the
	// tracked eyes (display space, metres) into a camera offset in game units:
	// half separation = |eR.x - eL.x| / 2 * world_per_meter.
	float world_per_meter = 1.0f;

	// Convergence distance in world units when no tracking is available.
	float default_convergence = 3.0f;

	// Optional: true when the frame is 2D-only (menus, inventory, loading).
	// The core then renders mono and asks the runtime for hardware 2D.
	bool (*frame_is_2d)() = nullptr;
};

// The profile the host registered (profiles/<game>/ calls set_profile at load).
void set_profile(const Profile *p);
const Profile &profile();

} // namespace dxr::core
