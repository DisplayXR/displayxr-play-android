#include <regex>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include "StereoGL.hpp"

#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <dlfcn.h>

#include <pl/memory/Hook.hpp>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "displayxr3d", __VA_ARGS__)

namespace dxr3d {

std::atomic<float> g_half_ipd{0.10f};
std::atomic<bool> g_flat_now{false};
std::atomic<bool> g_flat_menus{true};
std::atomic<float> g_convergence{3.0f};
// Convergence plane distance (blocks): the clip-space shift in every camera patch.
float conv_eff() { return g_convergence.load(std::memory_order_relaxed); }
// Look-around: per-eye camera offsets in blocks (0 = left, 1 = right), set from
// the tracked eye positions each frame. g_eye_tracked false -> fixed +-g_half_ipd.
std::atomic<float> g_eye_off[2][2] = {{{0.f}, {0.f}}, {{0.f}, {0.f}}};
std::atomic<bool> g_eye_tracked{false};

namespace {

constexpr int kUnits = 32;
constexpr int kAtt = 10;  // colour 0..7, depth, stencil

// ---- per-thread GL state (GL bindings are per context = per thread here) ----
thread_local int t_bypass = 0;
thread_local bool t_stereo = false;  // this thread renders the current frame twice
thread_local GLuint t_draw_fbo = 0, t_read_fbo = 0, t_rb = 0, t_program = 0;
thread_local int t_active = 0, t_units_hw = 0;
thread_local GLuint t_unit_tex[kUnits] = {};
thread_local GLuint t_unit_vol[kUnits] = {};   // GL_TEXTURE_3D / GL_TEXTURE_2D_ARRAY per unit
thread_local GLenum t_unit_vol_tgt[kUnits] = {};

// Menus: a frame with almost no 3D-camera draws renders one eye only, which
// avoids a framebuffer switch for every UI draw. Go back to two eyes at once.
constexpr int kMinWorldDraws = 16;   // the title panorama is 6, a world is hundreds
constexpr int kFlatAfterFrames = 30;
constexpr int kWorldAfterFrames = 3;  // don't flicker on one busy menu frame
constexpr int kTwinWarmFrames = 3;    // twins hold stale pictures after one-eye frames
// g_programs belongs to the thread that swaps. Other GL threads (Minecraft's loaders)
// must not touch it; their relinks are queued and applied at the frame boundary.
thread_local bool t_render = false;
std::mutex g_relink_mu;
std::vector<GLuint> g_relinked;
thread_local int t_camera_draws = 0, t_flat_run = 0, t_world_run = 0, t_warm = 0;

// ---- stats --------------------------------------------------------------------
std::atomic<uint64_t> s_draws{0}, s_doubled{0}, s_proj{0}, s_targets{0}, s_fbos{0}, s_programs{0},
    s_programs_cam{0}, s_fbo_fail{0}, s_draw_win{0}, s_draw_mono{0}, s_blits{0}, s_blits_win{0}, s_frames{0};
std::atomic<bool> s_hooks_ok{false};

// ---- originals ------------------------------------------------------------------
#define GLFN(ret, name, ...) ret (*o_##name)(__VA_ARGS__) = nullptr;
GLFN(void, glUseProgram, GLuint)
GLFN(void, glLinkProgram, GLuint)
GLFN(void, glUniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat *)
GLFN(void, glUniform4fv, GLint, GLsizei, const GLfloat *)
GLFN(void, glClear, GLbitfield)
GLFN(void, glClearBufferfv, GLenum, GLint, const GLfloat *)
GLFN(void, glClearBufferiv, GLenum, GLint, const GLint *)
GLFN(void, glClearBufferuiv, GLenum, GLint, const GLuint *)
GLFN(void, glClearBufferfi, GLenum, GLint, GLfloat, GLint)
GLFN(void, glBindFramebuffer, GLenum, GLuint)
GLFN(void, glDeleteFramebuffers, GLsizei, const GLuint *)
GLFN(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)
GLFN(void, glFramebufferTextureLayer, GLenum, GLenum, GLuint, GLint, GLint)
GLFN(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)
GLFN(void, glFramebufferTexture2DMultisampleEXT, GLenum, GLenum, GLenum, GLuint, GLint, GLsizei)
GLFN(void, glDrawBuffers, GLsizei, const GLenum *)
GLFN(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
GLFN(void, glActiveTexture, GLenum)
GLFN(void, glBindTexture, GLenum, GLuint)
GLFN(void, glDeleteTextures, GLsizei, const GLuint *)
GLFN(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)
GLFN(void, glTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)
GLFN(void, glTexStorage2D, GLenum, GLsizei, GLenum, GLsizei, GLsizei)
GLFN(void, glTexParameteri, GLenum, GLenum, GLint)
GLFN(void, glTexParameterf, GLenum, GLenum, GLfloat)
GLFN(void, glGenerateMipmap, GLenum)
GLFN(void, glBindRenderbuffer, GLenum, GLuint)
GLFN(void, glDeleteRenderbuffers, GLsizei, const GLuint *)
GLFN(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)
GLFN(void, glRenderbufferStorageMultisample, GLenum, GLsizei, GLenum, GLsizei, GLsizei)
GLFN(void, glDrawArrays, GLenum, GLint, GLsizei)
GLFN(void, glDrawElements, GLenum, GLsizei, GLenum, const void *)
GLFN(void, glDrawArraysInstanced, GLenum, GLint, GLsizei, GLsizei)
GLFN(void, glDrawElementsInstanced, GLenum, GLsizei, GLenum, const void *, GLsizei)
GLFN(void, glDrawRangeElements, GLenum, GLuint, GLuint, GLsizei, GLenum, const void *)
GLFN(void, glDrawElementsBaseVertex, GLenum, GLsizei, GLenum, const void *, GLint)
GLFN(void, glDrawRangeElementsBaseVertex, GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint)
GLFN(void, glDrawElementsInstancedBaseVertex, GLenum, GLsizei, GLenum, const void *, GLsizei, GLint)
GLFN(void, glDrawArraysIndirect, GLenum, const void *)
GLFN(void, glDrawElementsIndirect, GLenum, GLenum, const void *)
GLFN(void, glDispatchCompute, GLuint, GLuint, GLuint)
GLFN(void, glBindImageTexture, GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum)
GLFN(void, glBindBufferBase, GLenum, GLuint, GLuint)
GLFN(void, glCopyImageSubData, GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei)
GLFN(void, glCopyTexSubImage2D, GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)
GLFN(void, glBindBufferRange, GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)
GLFN(void, glShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *)
#undef GLFN

// ============================ camera matrices ===================================
enum Kind : uint8_t { kProj, kCombined, kInverse /* M * E^-1 */, kPrevForward /* E * M */, kCamPos /* vec4 xyz += eye along camera axes */, kViewTrack /* remembered, not changed */, kView /* T(-e) * V: eye view matrix (programs without a forward camera) */, kInvView /* V^-1 * T(+e): eye-view -> world */, kViewIn /* M * T(+-e): consumes view-space positions (shadow projections) */, kViewportScale /* vec4 (scale.xy, offset.zw) of a pixel->ray mapping: offset by the eye's clip shift */, kProbe /* log-only numeric classification */ };
struct Target {
	GLint loc;
	Kind kind;
	bool vec4;  // uploaded as vec4[4]
	bool has = false;
	float orig[16];
	int count = 1;             // mat4 arrays (count > 1): originals live in arr
	std::vector<float> arr;
	std::string probe_name;  // kProbe only
};
// Shaders / programs of the deferred specular pass (image-based reflections from
// the sky probe, screen-space): their inverse camera matrices stay the game's.
// The pass only needs far-field directions, and the per-eye shift moves its
// cube-face choice (a hard boundary at the probe's lowest mip) across walls
// and sky; the centre camera gives both eyes vanilla's result.
std::mutex g_spec_mu;
std::unordered_set<GLuint> g_spec_shaders, g_spec_programs;  // guarded by g_spec_mu
bool spec_program(GLuint p)
{
	static thread_local GLuint last_p = 0;
	static thread_local bool last_v = false;
	if (p == last_p && p) return last_v;
	std::lock_guard<std::mutex> l(g_spec_mu);
	last_p = p;
	last_v = g_spec_programs.count(p) != 0;
	return last_v;
}
struct Program {
	std::vector<Target> targets;
	std::vector<int> image_units;  // units of the image uniforms this program declares
	std::string blocks;            // debug: uniform / storage blocks (name@binding size)
	std::vector<int> ssbo_bindings;  // binding points of the storage blocks this program declares
};
std::unordered_map<GLuint, Program> g_programs;  // render thread
float g_last_p00 = 0.0f;

bool is_proj_name(const std::string &n)
{
	return n == "u_proj" || n == "PROJ" || n == "u_projection" || n == "PROJECTION";
}
// Deferred lighting / shadows rebuild world positions with these; temporal passes
// reproject with the previous frame's. They must follow the eye too.
bool is_inverse_name(const std::string &n)
{
	return n == "u_invViewProj" || n == "u_invProj" || n == "PrevInvProj";
}
bool is_prev_forward_name(const std::string &n) { return n == "u_prevViewProj"; }
// Camera world position (xyz) packed with time: view vectors for specular / fog.
bool is_campos_name(const std::string &n) { return n == "ViewPositionAndTime"; }
// Shadow projections: if they take VIEW-space positions they carry the eye offset.
bool is_viewin_name(const std::string &n)
{
	return n == "CascadesShadowProj" || n == "PlayerShadowProj" || n == "CloudShadowProj";
}
bool is_combined_name(const std::string &n)
{
	return n == "u_viewProj" || n == "u_modelViewProj" || n == "VIEWPROJ" || n == "WORLDVIEWPROJ" ||
	       n == "VIEW_PROJ" || n == "WORLD_VIEW_PROJ" || n == "u_viewProjection";
}

Program &program(GLuint p)
{
	auto it = g_programs.find(p);
	if (it != g_programs.end()) return it->second;
	Program info;
	GLint count = 0;
	glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &count);
	std::vector<Target> proj, comb, extra;
	for (GLint i = 0; i < count && i < 256; ++i) {
		char buf[128] = {};
		GLsizei len = 0;
		GLint size = 0;
		GLenum type = 0;
		glGetActiveUniform(p, i, sizeof buf, &len, &size, &type, buf);
		std::string name(buf, len > 0 ? len : 0);
		if (name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0) name.resize(name.size() - 3);
		const bool mat = type == GL_FLOAT_MAT4 && size == 1, v4 = type == GL_FLOAT_VEC4 && size == 4;
		const bool campos = type == GL_FLOAT_VEC4 && size == 1 && is_campos_name(name);
		const bool vpscale = type == GL_FLOAT_VEC4 && size == 1 && name == "ViewportScale";
		const bool viewin = type == GL_FLOAT_MAT4 && size >= 1 && size <= 8 && is_viewin_name(name);
		if (!mat && !v4 && !campos && !viewin && !vpscale) continue;
		const GLint loc = glGetUniformLocation(p, buf);
		if (loc < 0) continue;
		Target t{loc, kProj, v4};
		if (viewin) {
			t.kind = kViewIn;
			t.count = size;
			t.arr.assign((size_t)size * 16, 0.f);
			extra.push_back(t);
			continue;
		}
		if (campos) {
			t.kind = kCamPos;
			extra.push_back(t);
			continue;
		}
		if (vpscale) {
			t.kind = kViewportScale;
			extra.push_back(t);
			continue;
		}
		if (mat && name == "u_view") {
			t.kind = kView;
			extra.push_back(t);
			continue;
		}
		if (mat && name == "u_invView") {
			t.kind = kInvView;
			extra.push_back(t);
			continue;
		}
		if (is_proj_name(name)) proj.push_back(t);
		else if (is_combined_name(name)) {
			t.kind = kCombined;
			comb.push_back(t);
		} else if (!v4 && is_inverse_name(name)) {
			t.kind = kInverse;
			extra.push_back(t);
		} else if (!v4 && is_prev_forward_name(name)) {
			t.kind = kPrevForward;
			extra.push_back(t);
		} else if (mat) {
			t.kind = kProbe;  // unnamed matrix: classify numerically, log once
			t.probe_name = name;
			extra.push_back(t);
		}
	}
	for (GLint i = 0; i < count && i < 256; ++i) {  // image uniforms -> their units
		char buf[128] = {};
		GLsizei len = 0;
		GLint size = 0;
		GLenum type = 0;
		glGetActiveUniform(p, i, sizeof buf, &len, &size, &type, buf);
		if (type < 0x904D || type > 0x906C) continue;  // GL_IMAGE_2D .. GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY
		std::string base(buf, len > 0 ? len : 0);
		if (base.size() > 3 && base.compare(base.size() - 3, 3, "[0]") == 0) base.resize(base.size() - 3);
		for (GLint e = 0; e < size && e < 8; ++e) {  // arrays: every element has its own unit
			const std::string el = size > 1 ? base + "[" + std::to_string(e) + "]" : std::string(buf, len > 0 ? len : 0);
			const GLint loc = glGetUniformLocation(p, el.c_str());
			GLint unit = -1;
			if (loc >= 0) glGetUniformiv(p, loc, &unit);
			if (unit >= 0 && unit < 16) info.image_units.push_back(unit);
		}
	}
	for (GLenum iface : {GL_UNIFORM_BLOCK, GL_SHADER_STORAGE_BLOCK}) {  // debug description
		GLint nb = 0;
		glGetProgramInterfaceiv(p, iface, GL_ACTIVE_RESOURCES, &nb);
		for (GLint i = 0; i < nb && i < 16; ++i) {
			char nm[96] = {};
			GLsizei len = 0;
			glGetProgramResourceName(p, iface, i, sizeof nm, &len, nm);
			const GLenum props[] = {GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE};
			GLint vals[2] = {-1, -1};
			glGetProgramResourceiv(p, iface, i, 2, props, 2, nullptr, vals);
			char b[160];
			snprintf(b, sizeof b, "%s %s@%d %dB; ", iface == GL_UNIFORM_BLOCK ? "UBO" : "SSBO", nm, vals[0], vals[1]);
			info.blocks += b;
			if (iface == GL_SHADER_STORAGE_BLOCK && vals[0] >= 0 && vals[0] < 16) info.ssbo_bindings.push_back(vals[0]);
		}
	}
	while (glGetError() != GL_NO_ERROR) {
	}
	info.targets = !proj.empty() ? proj : comb;  // separate projection wins
	// A program with a forward camera carries the eye in its projection; its
	// u_view must stay the game's, or the eye would be applied twice.
	if (!info.targets.empty())
		for (auto &t : extra)
			if (t.kind == kView) t.kind = kViewTrack;
	info.targets.insert(info.targets.end(), extra.begin(), extra.end());
	s_programs++;
	if (!info.targets.empty()) s_programs_cam++;
	if (g_programs.size() > 4096) g_programs.clear();
	return g_programs.emplace(p, std::move(info)).first->second;
}

Target *find_target(GLint loc)
{
	if (!t_render || !t_program || loc < 0) return nullptr;
	for (auto &t : program(t_program).targets)
		if (t.loc == loc) return &t;
	return nullptr;
}

bool is_perspective(const float *m, Kind kind)
{
	if (!std::isfinite(m[0]) || !std::isfinite(m[12])) return false;
	if (kind == kProj) return std::fabs(m[11]) > 0.5f && std::fabs(m[15]) < 0.01f && std::fabs(m[0]) > 0.01f;
	// Combined view-projection: the bottom row is the camera's forward axis, whatever
	// way it points; an orthographic matrix has (0,0,0,1) there.
	return m[3] * m[3] + m[7] * m[7] + m[11] * m[11] > 0.25f;
}

// Off-axis eye: camera moved (eye_x, eye_y) blocks along its x / y axes, picture
// shifted so that g_convergence blocks away stays put. Column-major, clip = M * v.
float g_last_p11 = 0.f;
float g_last_proj[16];       // last original projection (column-major)
bool g_have_proj = false;

void mul4(const float *a, const float *b, float *out)  // out = a * b, column-major
{
	float r[16];
	for (int c = 0; c < 4; ++c)
		for (int row = 0; row < 4; ++row) {
			float v = 0.f;
			for (int k = 0; k < 4; ++k) v += a[k * 4 + row] * b[c * 4 + k];
			r[c * 4 + row] = v;
		}
	std::memcpy(out, r, sizeof r);
}
bool inv4(const float *m, float *out)
{
	float inv[16];
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
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (!std::isfinite(det) || std::fabs(det) < 1e-20f) return false;
	for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
	return true;
}

// The eye as a clip-space transform of the original camera:
// E = Shift * P * T(-e) * P^-1 (what apply_eye does to a projection). With
// inverse = true, E^-1 = P * T(+e) * P^-1 * Shift^-1.
float g_scene_proj[16];      // inverse(u_invProj): the projection the depth buffer was rendered with
bool g_have_scene_proj = false;


float g_last_view[16];
bool g_have_view = false;
float g_last_combined[16];
bool g_have_combined = false;

bool near_identity(const float *m, float tol)
{
	for (int i = 0; i < 16; ++i) {
		const float want = (i % 5 == 0) ? 1.f : 0.f;
		if (!(std::fabs(m[i] - want) <= tol)) return false;
	}
	return true;
}

// Generic, name-free classification of a matrix against the frame's camera
// (log only for now): is it the inverse camera, inverse projection, a camera
// of a previous frame, or unrelated (model, shadow, bones)?
void probe_matrix(const Target &t, const float *m)
{
	static std::unordered_map<std::string, int> done;
	if (done.count(t.probe_name) || done.size() > 200) return;
	if (!g_have_proj) return;
	float vp[16], r[16];
	const char *cls = "unrelated";
	if (g_have_view) mul4(g_last_proj, g_last_view, vp);
	else if (g_have_combined) std::memcpy(vp, g_last_combined, sizeof vp);
	else return;
	mul4(m, vp, r);
	float rv[16];
	if (g_have_view) mul4(m, g_last_view, rv);
	if (near_identity(r, 2e-3f)) cls = "INVERSE CAMERA (M*VP=I)";
	else if (g_have_view && near_identity(rv, 2e-3f)) cls = "INVERSE VIEW (M*V=I)";
	else {
		mul4(m, g_last_proj, r);
		if (near_identity(r, 2e-3f)) cls = "INVERSE PROJECTION (M*P=I)";
		else {
			float d = 0.f, n = 0.f;
			for (int i = 0; i < 16; ++i) d += std::fabs(m[i] - vp[i]), n += std::fabs(vp[i]);
			if (d < 0.05f * n) cls = "CAMERA-LIKE (near VP: previous frame / jittered)";
			else if (std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11]) > 0.25f && std::fabs(m[15]) < 0.5f) cls = "OTHER PERSPECTIVE";
		}
	}
	done.emplace(t.probe_name, 1);
}

bool apply_eye(float *m, Kind kind, float eye_x, float eye_y = 0.f)
{
	if (kind == kProbe) return false;
	if (kind == kViewTrack || kind == kView || kind == kInvView || kind == kViewIn || kind == kViewportScale) return false;
	if (kind == kCamPos) {
		if (!g_have_view) return false;
		const float *V = g_last_view;  // world -> view: rows of the 3x3 are the camera axes
		const float R[3] = {V[0], V[4], V[8]}, U[3] = {V[1], V[5], V[9]};
		for (int i = 0; i < 3; ++i) m[i] += eye_x * R[i] + eye_y * U[i];
		return true;
	}
	if (kind == kInverse) {
		// An inverse projection hands us the scene's own projection exactly.
		float Mi[16];
		if (inv4(m, Mi) && std::fabs(Mi[11]) > 0.5f && std::fabs(Mi[15]) < 0.01f && std::fabs(Mi[3]) < 1e-3f &&
		    std::fabs(Mi[7]) < 1e-3f) {
			std::memcpy(g_scene_proj, Mi, sizeof g_scene_proj);
			g_have_scene_proj = true;
		}
		// The deferred specular pass (image-based reflections) keeps the game's own
		// camera: its reflected directions sit on cube-face boundaries of the sky
		// probe, and a per-eye shift flips the face in one eye.
		if (spec_program(t_program)) return false;
		// Closed form of M * E^-1 with E^-1 = P T(e) P^-1 S^-1:
		// P T(e) P^-1 = I + (e.x p00, e.y p11, 0, 0) (x) wrow(P^-1), and the w row
		// of any inverse camera matrix M is wrow(P^-1), so
		//   M' = (M + (e.x p00 M.col0 + e.y p11 M.col1) (x) M.roww) * S^-1.
		// No inversion, no products: the z and w rows of a P^-1 stay bit-exact.
		if (!g_have_proj && !g_have_scene_proj) return false;
		const float *P = g_have_scene_proj ? g_scene_proj : g_last_proj;
		const float p00 = P[0], p11 = P[5];
		const float conv = conv_eff();
		const float sx = eye_x * p00 / conv, sy = eye_y * p11 / conv;
		const float wrow[4] = {m[3], m[7], m[11], m[15]};
		float v[4];
		for (int r = 0; r < 4; ++r) v[r] = eye_x * p00 * m[r] + eye_y * p11 * m[4 + r];
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r) m[c * 4 + r] += v[r] * wrow[c];
		for (int r = 0; r < 4; ++r) m[12 + r] -= sx * m[r] + sy * m[4 + r];  // * S^-1: col3 -= sx col0 + sy col1
		return true;
	}
	if (kind == kPrevForward) {
		// E * M with E = S P T(-e) P^-1 = S (I + (-e.x p00, -e.y p11, 0, 0) (x) wrow(P^-1)).
		if (!g_have_proj && !g_have_scene_proj) return false;
		const float *P = g_have_scene_proj ? g_scene_proj : g_last_proj;
		float Pi[16];
		if (!inv4(P, Pi)) return false;
		const float p00 = P[0], p11 = P[5];
		const float conv = conv_eff();
		const float sx = eye_x * p00 / conv, sy = eye_y * p11 / conv;
		const float wrow[4] = {Pi[3], Pi[7], Pi[11], Pi[15]};
		for (int c = 0; c < 4; ++c) {
			const float wm = wrow[0] * m[c * 4 + 0] + wrow[1] * m[c * 4 + 1] + wrow[2] * m[c * 4 + 2] + wrow[3] * m[c * 4 + 3];
			m[c * 4 + 0] += -eye_x * p00 * wm;
			m[c * 4 + 1] += -eye_y * p11 * wm;
		}
		for (int c = 0; c < 4; ++c) {  // S: rows x,y += s * row w
			m[c * 4 + 0] += sx * m[c * 4 + 3];
			m[c * 4 + 1] += sy * m[c * 4 + 3];
		}
		return true;
	}
	if (!is_perspective(m, kind)) return false;
	if (kind == kProj) {
		std::memcpy(g_last_proj, m, sizeof g_last_proj);
		g_have_proj = true;
	} else if (kind == kCombined) {
		std::memcpy(g_last_combined, m, sizeof g_last_combined);
		g_have_combined = true;
	}
	float p00, p11;
	if (kind == kProj) {
		p00 = m[0];
		p11 = m[5];
		g_last_p00 = p00;
		g_last_p11 = p11;
		for (int r = 0; r < 4; ++r) m[12 + r] += -eye_x * m[r] - eye_y * m[4 + r];
	} else {
		p00 = g_last_p00 > 0.01f ? g_last_p00 : 0.9f;
		p11 = g_last_p11 > 0.01f ? g_last_p11 : p00 * 1.6f;
		m[12] += -eye_x * p00;
		m[13] += -eye_y * p11;
	}
	const float conv = conv_eff();
	const float sx = eye_x * p00 / conv, sy = eye_y * p11 / conv;
	for (int c = 0; c < 4; ++c) {
		m[c * 4 + 0] += sx * m[c * 4 + 3];
		m[c * 4 + 1] += sy * m[c * 4 + 3];
	}
	return true;
}

// Camera offset of one eye (-1 left, +1 right), tracked or fixed.
float eye_off_x(int eye)
{
	if (g_eye_tracked.load(std::memory_order_relaxed)) return g_eye_off[eye > 0][0].load(std::memory_order_relaxed);
	return eye * g_half_ipd.load(std::memory_order_relaxed);
}
float eye_off_y(int eye)
{
	if (g_eye_tracked.load(std::memory_order_relaxed)) return g_eye_off[eye > 0][1].load(std::memory_order_relaxed);
	return 0.f;
}

void upload(const Target &t, const float *m)
{
	if (t.kind == kCamPos || t.kind == kViewportScale) o_glUniform4fv(t.loc, 1, m);
	else if (t.vec4) o_glUniform4fv(t.loc, 4, m);
	else o_glUniformMatrix4fv(t.loc, t.count, GL_FALSE, m);
}

bool program_has_3d_camera()
{
	if (!t_program) return false;
	for (auto &t : program(t_program).targets)
		if (t.has && (t.kind == kProj || t.kind == kCombined) && is_perspective(t.orig, t.kind)) return true;
	return false;
}

// Re-upload the current program's camera exactly as the game set it.
void upload_center()
{
	if (!t_program) return;
	for (auto &t : program(t_program).targets)
		if (t.has && t.kind != kViewTrack && t.kind != kProbe) upload(t, t.count > 1 ? t.arr.data() : t.orig);
}

// Apply the eye to a mat4 array (count matrices in a row); true if any changed.
bool apply_eye_n(float *m, int count, Kind kind, float ex, float ey)
{
	bool any = false;
	for (int i = 0; i < count; ++i) any |= apply_eye(m + i * 16, kind, ex, ey);
	return any;
}

// Re-upload the current program's camera for one eye (-1 left, +1 right).
void upload_eye(int eye)
{
	if (!t_program) return;
	const float ex = eye_off_x(eye), ey = eye_off_y(eye);
	for (auto &t : program(t_program).targets) {
		if (!t.has) continue;
		if (t.count > 1) {
			std::vector<float> m(t.arr);
			if (apply_eye_n(m.data(), t.count, t.kind, ex, ey)) upload(t, m.data());
			continue;
		}
		float m[16];
		std::memcpy(m, t.orig, sizeof m);
		if (apply_eye(m, t.kind, ex, ey)) upload(t, m);
	}
}

// ============================ render targets ====================================
struct Att {
	uint8_t kind = 0;  // 0 none, 1 texture 2D, 2 renderbuffer, 3 other (layer / cube) → mono
	GLuint name = 0;
	GLint level = 0;
	GLsizei msaa = 0;  // EXT_multisampled_render_to_texture
};
struct Fbo {
	Att att[kAtt];
	GLenum bufs[8];
	int nbufs = -1;
	GLuint twin = 0;
	int state = 0;  // 0 unknown, 1 stereo, 2 mono
	int fails = 0;  // consecutive failed twin builds (retried a few times, then mono)
	unsigned retry_at = 0;
};
std::mutex g_mu;  // guards the maps below
std::unordered_map<GLuint, Fbo> g_fbos;
std::unordered_map<GLuint, GLuint> g_tex_twin;  // texture → twin (0 = checked, mono)
std::unordered_map<GLuint, GLuint> g_rb_twin;   // renderbuffer → twin (0 = mono)

// Window (framebuffer 0) twin.
GLuint g_win_fbo = 0, g_win_tex = 0, g_win_rb = 0;
int g_win_w = 0, g_win_h = 0;

GLenum att_enum(int i)
{
	return i < 8 ? GL_COLOR_ATTACHMENT0 + i : i == 8 ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT;
}

void drop_twin_fbos_locked()
{
	for (auto &kv : g_fbos) {
		if (kv.second.twin) o_glDeleteFramebuffers(1, &kv.second.twin);
		kv.second.twin = 0;
		kv.second.state = 0;
	}
}

void set_att(GLenum target, GLenum attachment, Att a)
{
	const GLuint fbo = target == GL_READ_FRAMEBUFFER ? t_read_fbo : t_draw_fbo;
	if (fbo == 0) return;
	std::lock_guard<std::mutex> l(g_mu);
	Fbo &f = g_fbos[fbo];
	auto put = [&](int i) { f.att[i] = a; };
	if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + 8) put(attachment - GL_COLOR_ATTACHMENT0);
	else if (attachment == GL_DEPTH_ATTACHMENT) put(8);
	else if (attachment == GL_STENCIL_ATTACHMENT) put(9);
	else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
		put(8);
		put(9);
	}
	if (f.twin) o_glDeleteFramebuffers(1, &f.twin);
	f.twin = 0;
	f.state = 0;
}

GLenum sized(GLint f)
{
	switch (f) {
	case GL_RGBA: return GL_RGBA8;
	case GL_RGB: return GL_RGB8;
	case GL_DEPTH_COMPONENT: return GL_DEPTH_COMPONENT24;
	case GL_DEPTH_STENCIL: return GL_DEPTH24_STENCIL8;
	default: return (GLenum)f;
	}
}

// Per-eye targets: screen-shaped. Small ones (tile grids such as 19x12 depth
// bounds) count too when their aspect follows the screen's; squares (shadow
// maps, mip pyramids, LUTs) stay shared.
bool stereo_size(GLint w, GLint h)
{
	if (w == h || w < 2 || h < 2) return false;
	if (w >= 16 && h >= 16) return true;
	const float sa = g_win_h > 0 ? (float)g_win_w / g_win_h : 1.6f;
	return std::fabs(((float)w / h) / sa - 1.f) < 0.15f;
}

struct SeedJob { GLuint src, dst; GLint w, h; GLenum fmt; bool rb; };
std::vector<SeedJob> g_seed_pending;  // guarded by g_mu; drained at the frame boundary
bool is_depth_fmt(GLenum f)
{
	return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24 || f == GL_DEPTH_COMPONENT32F || f == GL_DEPTH24_STENCIL8 ||
	       f == GL_DEPTH32F_STENCIL8 || f == GL_DEPTH_COMPONENT || f == GL_DEPTH_STENCIL;
}
GLuint tex_twin_locked(GLuint tex)
{
	auto it = g_tex_twin.find(tex);
	if (it != g_tex_twin.end()) return it->second;
	GLint prev = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
	o_glBindTexture(GL_TEXTURE_2D, tex);
	GLint w = 0, h = 0, fmt = 0, imm = 0, levels = 1;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &imm);
	if (imm) glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
	GLuint twin = 0;
	if (stereo_size(w, h) && fmt) {
		const GLenum pn[] = {GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T,
		                     GL_TEXTURE_COMPARE_MODE, GL_TEXTURE_COMPARE_FUNC};
		GLint pv[6];
		for (int i = 0; i < 6; ++i) glGetTexParameteriv(GL_TEXTURE_2D, pn[i], &pv[i]);
		while (glGetError() != GL_NO_ERROR) {
		}
		glGenTextures(1, &twin);
		o_glBindTexture(GL_TEXTURE_2D, twin);
		o_glTexStorage2D(GL_TEXTURE_2D, levels > 0 ? levels : 1, sized(fmt), w, h);
		for (int i = 0; i < 6; ++i) o_glTexParameteri(GL_TEXTURE_2D, pn[i], pv[i]);
		if (glGetError() != GL_NO_ERROR) {
			o_glDeleteTextures(1, &twin);
			twin = 0;
		} else {
			s_targets++;
			// Seed the twin from the original at the end of this frame (see
			// seed_pending_twins): a buffer the game clears only once, its second
			// depth-stencil texture (terrain depth, sampled by the specular pass),
			// would otherwise leave the right eye with uninitialised memory where
			// the left eye has the clear value: sky treated as geometry in one eye.
			g_seed_pending.push_back({tex, twin, w, h, (GLenum)fmt, false});
		}
	}
	o_glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
	g_tex_twin[tex] = twin;
	return twin;
}

// Right-eye twin of a 3D / array texture (camera-aligned volumes: froxel fog,
// clustered lights). Created when a compute pass writes the volume.
std::unordered_map<GLuint, std::pair<GLuint, GLenum>> g_vol_twin;  // tex -> (twin, target)
struct VolDims { GLint w = 0, h = 0, d = 0; unsigned seeded_frame = ~0u; };
std::unordered_map<GLuint, VolDims> g_vol_dims;  // twin -> extent + last frame it was seeded
using PFN_CopyImage = void (*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
PFN_CopyImage g_copy_image = nullptr;
std::atomic<unsigned> g_frame_no{0};
GLuint vol_twin_locked(GLuint tex, bool create)
{
	auto it = g_vol_twin.find(tex);
	if (it != g_vol_twin.end()) return it->second.first;
	if (!create) return 0;
	GLuint twin = 0;
	GLenum tgt_found = 0;
	for (GLenum tgt : {GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY}) {
		while (glGetError() != GL_NO_ERROR) {
		}
		const GLenum binding = tgt == GL_TEXTURE_3D ? GL_TEXTURE_BINDING_3D : GL_TEXTURE_BINDING_2D_ARRAY;
		GLint prev = 0;
		glGetIntegerv(binding, &prev);
		o_glBindTexture(tgt, tex);
		if (glGetError() != GL_NO_ERROR) continue;  // wrong target for this name
		GLint w = 0, h = 0, d = 0, fmt = 0, imm = 0, levels = 1;
		glGetTexLevelParameteriv(tgt, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(tgt, 0, GL_TEXTURE_HEIGHT, &h);
		glGetTexLevelParameteriv(tgt, 0, GL_TEXTURE_DEPTH, &d);
		glGetTexLevelParameteriv(tgt, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
		glGetTexParameteriv(tgt, GL_TEXTURE_IMMUTABLE_FORMAT, &imm);
		if (imm) glGetTexParameteriv(tgt, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
		const GLenum pn[] = {GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R};
		GLint pv[5];
		for (int i = 0; i < 5; ++i) glGetTexParameteriv(tgt, pn[i], &pv[i]);
		// Only camera-aligned volumes are per-eye: their x/y grid follows the
		// screen's aspect (froxels, light clusters). LUTs (sky, atmosphere) don't.
		const float sa = g_win_h > 0 ? (float)g_win_w / g_win_h : 1.6f, va = h > 0 ? (float)w / h : 0.f;
		const float tol = 0.05f + 1.0f / (float)std::max(1, std::min(w, h));  // tile rounding of small grids
		const bool camera_aligned = w >= 4 && h >= 4 && std::fabs(va / sa - 1.f) < tol;
		if (!camera_aligned)
		if (camera_aligned && w > 0 && h > 0 && d > 0 && fmt) {
			glGenTextures(1, &twin);
			o_glBindTexture(tgt, twin);
			glTexStorage3D(tgt, levels > 0 ? levels : 1, sized(fmt), w, h, d);
			for (int i = 0; i < 5; ++i) o_glTexParameteri(tgt, pn[i], pv[i]);
			if (glGetError() != GL_NO_ERROR) {
				o_glDeleteTextures(1, &twin);
				twin = 0;
			} else {
				tgt_found = tgt;
				s_targets++;
				g_vol_dims[twin] = {w, h, d, ~0u};
			}
		}
		o_glBindTexture(tgt, (GLuint)prev);
		break;
	}
	g_vol_twin[tex] = {twin, tgt_found};
	return twin;
}

// First right-eye use in a frame: the twin starts as a copy of the left
// volume, so anything the right eye doesn't recompute matches the left eye
// instead of being stale or empty.
void seed_vol_twin_locked(GLuint tex)
{
	auto it = g_vol_twin.find(tex);
	if (it == g_vol_twin.end() || !it->second.first) return;
	VolDims &d = g_vol_dims[it->second.first];
	const unsigned f = g_frame_no.load(std::memory_order_relaxed);
	if (d.seeded_frame == f || d.w <= 0) return;
	d.seeded_frame = f;
	if (!g_copy_image) {
		g_copy_image = (PFN_CopyImage)eglGetProcAddress("glCopyImageSubData");
		if (!g_copy_image) g_copy_image = (PFN_CopyImage)eglGetProcAddress("glCopyImageSubDataEXT");
		if (!g_copy_image) return;
	}
	g_copy_image(tex, it->second.second, 0, 0, 0, 0, it->second.first, it->second.second, 0, 0, 0, 0, d.w, d.h, d.d);
}
GLuint vol_lookup(GLuint tex)
{
	std::lock_guard<std::mutex> l(g_mu);
	const GLuint tw = vol_twin_locked(tex, false);
	if (tw) {
		glMemoryBarrier(GL_ALL_BARRIER_BITS);
		seed_vol_twin_locked(tex);
	}
	return tw;
}

// Swap the bound volumes to their right-eye twins (on) or back (off).
int vol_mode() { return 2; }  // 0 mono volumes, 1 compute doubled only, 2 full
void swap_volumes(bool on)
{
	if (vol_mode() < 2) return;
	bool touched = false;
	for (int u = 0; u < t_units_hw; ++u) {
		if (!t_unit_vol[u]) continue;
		const GLuint tw = vol_lookup(t_unit_vol[u]);
		if (!tw) continue;
		o_glActiveTexture(GL_TEXTURE0 + u);
		o_glBindTexture(t_unit_vol_tgt[u], on ? tw : t_unit_vol[u]);
		touched = true;
	}
	if (touched) o_glActiveTexture(GL_TEXTURE0 + t_active);
}

GLuint rb_twin_locked(GLuint rb)
{
	auto it = g_rb_twin.find(rb);
	if (it != g_rb_twin.end()) return it->second;
	GLint prev = 0;
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev);
	o_glBindRenderbuffer(GL_RENDERBUFFER, rb);
	GLint w = 0, h = 0, fmt = 0, samples = 0;
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &fmt);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
	GLuint twin = 0;
	if (stereo_size(w, h) && fmt) {
		while (glGetError() != GL_NO_ERROR) {
		}
		glGenRenderbuffers(1, &twin);
		o_glBindRenderbuffer(GL_RENDERBUFFER, twin);
		o_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, sized(fmt), w, h);
		if (glGetError() != GL_NO_ERROR) {
			o_glDeleteRenderbuffers(1, &twin);
			twin = 0;
		} else {
			s_targets++;
			if (samples == 0) g_seed_pending.push_back({rb, twin, w, h, (GLenum)fmt, true});
		}
	}
	o_glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev);
	g_rb_twin[rb] = twin;
	return twin;
}

void build_locked(Fbo &f)
{
	f.state = 2;
	int primary = -1;
	for (int i = 0; i < kAtt && primary < 0; ++i)
		if (f.att[i].kind) primary = i;
	if (primary < 0) return;
	GLuint tw[kAtt] = {};
	for (int i = 0; i < kAtt; ++i) {
		const Att &a = f.att[i];
		if (a.kind == 3) return;  // layered / cube attachment: leave mono
		if (a.kind == 1) tw[i] = tex_twin_locked(a.name);
		else if (a.kind == 2) tw[i] = rb_twin_locked(a.name);
	}
	if (!tw[primary]) return;
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	for (int i = 0; i < kAtt; ++i) {
		const Att &a = f.att[i];
		if (!a.kind) continue;
		const GLuint name = tw[i] ? tw[i] : a.name;
		if (a.kind == 2) o_glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att_enum(i), GL_RENDERBUFFER, name);
		else if (a.msaa > 0 && o_glFramebufferTexture2DMultisampleEXT)
			o_glFramebufferTexture2DMultisampleEXT(GL_DRAW_FRAMEBUFFER, att_enum(i), GL_TEXTURE_2D, name, a.level, a.msaa);
		else o_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att_enum(i), GL_TEXTURE_2D, name, a.level);
	}
	if (f.nbufs >= 0) o_glDrawBuffers(f.nbufs, f.bufs);
	const bool ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t_draw_fbo);
	if (!ok) {
		o_glDeleteFramebuffers(1, &fbo);
		s_fbo_fail++;
		// Right after the game rebuilt its resources (resume), attachments can be
		// incomplete for a frame: try again later instead of settling for mono.
		f.state = ++f.fails < 8 ? 0 : 2;
		if (f.fails < 8) f.retry_at = g_frame_no.load(std::memory_order_relaxed) + 2;
		return;
	}
	f.twin = fbo;
	f.state = 1;
	f.fails = 0;
	s_fbos++;
}

// Right-eye twin of a framebuffer in the current stereo frame, or 0 (mono).
GLuint twin_of(GLuint fbo)
{
	if (fbo == 0) return g_win_fbo;
	std::lock_guard<std::mutex> l(g_mu);
	auto it = g_fbos.find(fbo);
	if (it == g_fbos.end()) return 0;
	Fbo &f = it->second;
	if (f.state == 0 && (f.fails == 0 || g_frame_no.load(std::memory_order_relaxed) >= f.retry_at)) build_locked(f);
	return f.state == 1 ? f.twin : 0;
}

GLuint tex_lookup(GLuint tex)
{
	std::lock_guard<std::mutex> l(g_mu);
	auto it = g_tex_twin.find(tex);
	return it == g_tex_twin.end() ? 0 : it->second;
}

void forget_tex(GLuint tex)
{
	if (!tex) return;
	std::lock_guard<std::mutex> l(g_mu);
	auto it = g_tex_twin.find(tex);
	if (it == g_tex_twin.end()) return;
	if (it->second) o_glDeleteTextures(1, &it->second);
	g_tex_twin.erase(it);
	drop_twin_fbos_locked();
}

void forget_rb(GLuint rb)
{
	if (!rb) return;
	std::lock_guard<std::mutex> l(g_mu);
	auto it = g_rb_twin.find(rb);
	if (it == g_rb_twin.end()) return;
	if (it->second) o_glDeleteRenderbuffers(1, &it->second);
	g_rb_twin.erase(it);
	drop_twin_fbos_locked();
}

int swap_ssbos(bool on);  // defined with the storage-buffer twins below
// The convergence shift of one eye in NDC (what the matrices would carry).


inline bool active() { return t_stereo && t_bypass == 0; }

// ---- each stereo draw, once per eye ---------------------------------------------
template <class F>
void stereo_draw(F &&draw)
{
	s_draws.fetch_add(1, std::memory_order_relaxed);
	if (t_render && !t_bypass && program_has_3d_camera()) ++t_camera_draws;
	if (!active()) return draw();
	if (t_draw_fbo == 0) s_draw_win.fetch_add(1, std::memory_order_relaxed);
	const GLuint twin = twin_of(t_draw_fbo);
	if (!twin) {
		s_draw_mono.fetch_add(1, std::memory_order_relaxed);
		// A mono target (shadow map, LUT, probe) is not an eye view: its camera
		// must be the game's own, not the left eye's.
		bool cam = false;
		if (t_program)
			for (auto &t : program(t_program).targets) cam |= t.has && t.kind != kViewTrack && t.kind != kProbe;
		if (!cam) return draw();
		upload_center();
		draw();
		upload_eye(-1);
		return;
	}
	draw();  // left eye: uniforms already hold the left camera
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, twin);
	int subs[kUnits];
	int n = 0;
	for (int u = 0; u < t_units_hw; ++u) {
		if (!t_unit_tex[u]) continue;
		if (GLuint tw = tex_lookup(t_unit_tex[u])) {
			o_glActiveTexture(GL_TEXTURE0 + u);
			o_glBindTexture(GL_TEXTURE_2D, tw);
			subs[n++] = u;
		}
	}
	swap_volumes(true);
	swap_ssbos(true);
	upload_eye(+1);
	draw();
	upload_eye(-1);
	swap_ssbos(false);
	swap_volumes(false);
	for (int i = 0; i < n; ++i) {
		o_glActiveTexture(GL_TEXTURE0 + subs[i]);
		o_glBindTexture(GL_TEXTURE_2D, t_unit_tex[subs[i]]);
	}
	if (n) o_glActiveTexture(GL_TEXTURE0 + t_active);
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t_draw_fbo);
	s_doubled.fetch_add(1, std::memory_order_relaxed);
}

template <class F>
void mirror_on_twin(F &&op)
{
	op();
	if (!active()) return;
	if (const GLuint twin = twin_of(t_draw_fbo)) {
		o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, twin);
		op();
		o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t_draw_fbo);
	}
}

// Run op on the twin of the texture bound to the active unit's 2D target.
template <class F>
void mirror_on_tex_twin(GLenum target, F &&op)
{
	op();
	if (t_bypass || target != GL_TEXTURE_2D || t_active >= kUnits || !t_unit_tex[t_active]) return;
	if (const GLuint tw = tex_lookup(t_unit_tex[t_active])) {
		o_glBindTexture(GL_TEXTURE_2D, tw);
		op();
		o_glBindTexture(GL_TEXTURE_2D, t_unit_tex[t_active]);
	}
}

// ================================ hooks ==========================================
void h_glUseProgram(GLuint p)
{
	if (!t_bypass) t_program = p;
	o_glUseProgram(p);
}
// Screen-space passes rebuild the view-space position from depth with a
// hand-written inverse projection that reads only four elements of u_invProj
// (symmetric-frustum shortcut). The eye's translation and convergence shift live
// in the elements it skips, so those passes (shadows, fog, reflections) placed
// everything for the centre camera. Rewrite the shortcut into the full
// multiply at compile time; the per-eye u_invProj then applies to them too.

int patch_fast_inverse_projection(std::string &src);
int patch_fast_inverse_projection(std::string &src)
{
	static const std::regex re(
	    R"(vec4\(([A-Za-z0-9_]+) \* ([A-Za-z0-9_]+)\[0\]\.x, ([A-Za-z0-9_]+) \* ([A-Za-z0-9_]+)\[1\]\.y, ([A-Za-z0-9_]+) \* ([A-Za-z0-9_]+)\[3\]\.z, \(([A-Za-z0-9_]+) \* ([A-Za-z0-9_]+)\[2\]\.w\) \+ \(([A-Za-z0-9_]+) \* ([A-Za-z0-9_]+)\[3\]\.w\)\))");
	int n = 0;
	std::string out;
	auto begin = std::sregex_iterator(src.begin(), src.end(), re), end = std::sregex_iterator();
	size_t last = 0;
	for (auto it = begin; it != end; ++it) {
		const std::smatch &m = *it;
		// Only when the matrix copies come from u_invProj.
		bool ok = true;
		for (int g : {2, 4, 6, 8, 10}) {
			const std::string decl = "mat4 " + m[g].str() + " = u_invProj;";
			if (src.find(decl) == std::string::npos) ok = false;
		}
		if (!ok) continue;
		out.append(src, last, (size_t)m.position() - last);
		// input vec4 was (x, y, z, w): x=g1, y=g3, w=g5, z=g7
		out += "(u_invProj * vec4(" + m[1].str() + ", " + m[3].str() + ", " + m[7].str() + ", " + m[5].str() + "))";
		last = (size_t)m.position() + (size_t)m.length();
		++n;
	}
	if (n) {
		out.append(src, last, std::string::npos);
		src.swap(out);
	}
	return n;
}

void h_glShaderSource(GLuint sh, GLsizei n, const GLchar *const *str, const GLint *len)
{
	std::string src;
	for (GLsizei i = 0; i < n && str && str[i]; ++i) {
		if (len && len[i] >= 0) src.append(str[i], (size_t)len[i]);
		else src.append(str[i]);
	}
	if (src.find("s_SpecularIBLRecords") != std::string::npos && src.find("s_SceneDepth") != std::string::npos) {
		std::lock_guard<std::mutex> l(g_spec_mu);
		g_spec_shaders.insert(sh);
	}
	if (src.find("u_invProj") != std::string::npos && patch_fast_inverse_projection(src)) {
		const char *one = src.c_str();
		const GLint one_len = (GLint)src.size();
		o_glShaderSource(sh, 1, &one, &one_len);
		return;
	}
	o_glShaderSource(sh, n, str, len);
}

void h_glLinkProgram(GLuint p)
{
	o_glLinkProgram(p);
	if (t_render) {
		g_programs.erase(p);
	} else {
		std::lock_guard<std::mutex> l(g_relink_mu);
		g_relinked.push_back(p);
	}
	// The deferred specular pass is recognised by its shaders (see spec_program).
	GLuint ids[8] = {};
	GLsizei cnt = 0;
	glGetAttachedShaders(p, 8, &cnt, ids);
	{
		std::lock_guard<std::mutex> l(g_spec_mu);
		bool spec = false;
		for (GLsizei i = 0; i < cnt; ++i) spec |= g_spec_shaders.count(ids[i]) != 0;
		if (spec) g_spec_programs.insert(p);
		else g_spec_programs.erase(p);
	}
}
void h_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean tr, const GLfloat *v)
{
	if (!t_bypass && count > 1 && !tr && v) {
		if (Target *t = find_target(loc); t && t->count == count) {
			std::memcpy(t->arr.data(), v, (size_t)count * 16 * sizeof(float));
			t->has = true;
			if (t_stereo) {
				std::vector<float> m(t->arr);
				if (apply_eye_n(m.data(), count, t->kind, eye_off_x(-1), eye_off_y(-1)))
					return o_glUniformMatrix4fv(loc, count, GL_FALSE, m.data());
			}
		}
	}
	if (!t_bypass && count == 1 && !tr && v) {
		if (Target *t = find_target(loc); t && !t->vec4 && t->kind != kCamPos && t->kind != kViewportScale && t->count == 1) {
			std::memcpy(t->orig, v, sizeof t->orig);
			t->has = true;
			if (t->kind == kViewTrack || t->kind == kView) {
				std::memcpy(g_last_view, v, sizeof g_last_view);
				g_have_view = true;
			} else if (t->kind == kProbe) {
				probe_matrix(*t, v);
			}
			if (t_stereo) {
				float m[16];
				std::memcpy(m, v, sizeof m);
				if (apply_eye(m, t->kind, eye_off_x(-1), eye_off_y(-1))) {
					s_proj.fetch_add(1, std::memory_order_relaxed);
					return o_glUniformMatrix4fv(loc, 1, GL_FALSE, m);
				}
			}
		}
	}
	o_glUniformMatrix4fv(loc, count, tr, v);
}
void h_glUniform4fv(GLint loc, GLsizei count, const GLfloat *v)
{
	if (!t_bypass && count == 1 && v) {
		if (Target *t = find_target(loc); t && (t->kind == kCamPos || t->kind == kViewportScale)) {
			std::memcpy(t->orig, v, 4 * sizeof(float));
			t->has = true;
			if (t_stereo) {
				float m[16] = {v[0], v[1], v[2], v[3]};
				if (apply_eye(m, t->kind, eye_off_x(-1), eye_off_y(-1))) return o_glUniform4fv(loc, 1, m);
			}
		}
	}
	if (!t_bypass && count == 4 && v) {
		if (Target *t = find_target(loc); t && t->vec4) {
			std::memcpy(t->orig, v, sizeof t->orig);
			t->has = true;
			if (t_stereo) {
				float m[16];
				std::memcpy(m, v, sizeof m);
				if (apply_eye(m, t->kind, eye_off_x(-1), eye_off_y(-1))) {
					s_proj.fetch_add(1, std::memory_order_relaxed);
					return o_glUniform4fv(loc, 4, m);
				}
			}
		}
	}
	o_glUniform4fv(loc, count, v);
}

void h_glClear(GLbitfield m)
{
	mirror_on_twin([&] { o_glClear(m); });
}
void h_glClearBufferfv(GLenum b, GLint d, const GLfloat *v)
{
	mirror_on_twin([&] { o_glClearBufferfv(b, d, v); });
}
void h_glClearBufferiv(GLenum b, GLint d, const GLint *v) { mirror_on_twin([&] { o_glClearBufferiv(b, d, v); }); }
void h_glClearBufferuiv(GLenum b, GLint d, const GLuint *v) { mirror_on_twin([&] { o_glClearBufferuiv(b, d, v); }); }
void h_glClearBufferfi(GLenum b, GLint d, GLfloat f, GLint s)
{
	mirror_on_twin([&] { o_glClearBufferfi(b, d, f, s); });
}

void h_glBindFramebuffer(GLenum target, GLuint fbo)
{
	o_glBindFramebuffer(target, fbo);
	if (t_bypass) return;
	if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) t_draw_fbo = fbo;
	if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) t_read_fbo = fbo;
}
void h_glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
	if (!t_bypass) {
		std::lock_guard<std::mutex> l(g_mu);
		for (GLsizei i = 0; i < n; ++i) {
			auto it = g_fbos.find(ids[i]);
			if (it == g_fbos.end()) continue;
			if (it->second.twin) o_glDeleteFramebuffers(1, &it->second.twin);
			g_fbos.erase(it);
		}
		for (GLsizei i = 0; i < n; ++i) {
			if (ids[i] && ids[i] == t_draw_fbo) t_draw_fbo = 0;
			if (ids[i] && ids[i] == t_read_fbo) t_read_fbo = 0;
		}
	}
	o_glDeleteFramebuffers(n, ids);
}
void h_glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level)
{
	o_glFramebufferTexture2D(target, att, textarget, tex, level);
	if (t_bypass) return;
	Att a;
	if (tex) a = Att{uint8_t(textarget == GL_TEXTURE_2D ? 1 : 3), tex, level, 0};
	set_att(target, att, a);
}
void h_glFramebufferTexture2DMultisampleEXT(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level,
                                            GLsizei samples)
{
	o_glFramebufferTexture2DMultisampleEXT(target, att, textarget, tex, level, samples);
	if (t_bypass) return;
	Att a;
	if (tex) a = Att{uint8_t(textarget == GL_TEXTURE_2D ? 1 : 3), tex, level, samples};
	set_att(target, att, a);
}
void h_glFramebufferTextureLayer(GLenum target, GLenum att, GLuint tex, GLint level, GLint layer)
{
	o_glFramebufferTextureLayer(target, att, tex, level, layer);
	if (t_bypass) return;
	Att a;
	if (tex) a = Att{3, tex, level, 0};
	set_att(target, att, a);
}
void h_glFramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbt, GLuint rb)
{
	o_glFramebufferRenderbuffer(target, att, rbt, rb);
	if (t_bypass) return;
	Att a;
	if (rb) a = Att{2, rb, 0, 0};
	set_att(target, att, a);
}
void h_glDrawBuffers(GLsizei n, const GLenum *bufs)
{
	o_glDrawBuffers(n, bufs);
	if (t_bypass || t_draw_fbo == 0 || n > 8) return;
	std::lock_guard<std::mutex> l(g_mu);
	Fbo &f = g_fbos[t_draw_fbo];
	f.nbufs = n;
	std::memcpy(f.bufs, bufs, sizeof(GLenum) * n);
	if (f.twin) {
		o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f.twin);
		o_glDrawBuffers(n, bufs);
		o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t_draw_fbo);
	}
}
// Texture-to-texture copies and framebuffer-to-texture copies: the game uses them
// to snapshot a buffer (scene depth for the screen-space passes). A per-eye
// destination gets the copy from the per-eye source too, or it stays stale.
void h_glCopyImageSubData(GLuint sn, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint dn, GLenum dt, GLint dl, GLint dx,
                          GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei d)
{
	o_glCopyImageSubData(sn, st, sl, sx, sy, sz, dn, dt, dl, dx, dy, dz, w, h, d);
	if (!active()) return;
	const GLuint stw = st == GL_TEXTURE_2D ? tex_lookup(sn) : 0, dtw = dt == GL_TEXTURE_2D ? tex_lookup(dn) : 0;
	if (!dtw) return;
	o_glCopyImageSubData(stw ? stw : sn, st, sl, sx, sy, sz, dtw, dt, dl, dx, dy, dz, w, h, d);
}
void h_glCopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
	o_glCopyTexSubImage2D(target, level, xo, yo, x, y, w, h);
	if (!active() || target != GL_TEXTURE_2D) return;
	const GLuint tex = t_unit_tex[t_active];
	const GLuint tw = tex ? tex_lookup(tex) : 0;
	const GLuint rt = twin_of(t_read_fbo);
	if (!tw) return;
	o_glBindFramebuffer(GL_READ_FRAMEBUFFER, rt ? rt : t_read_fbo);
	o_glBindTexture(GL_TEXTURE_2D, tw);
	o_glCopyTexSubImage2D(target, level, xo, yo, x, y, w, h);
	o_glBindTexture(GL_TEXTURE_2D, tex);
	o_glBindFramebuffer(GL_READ_FRAMEBUFFER, t_read_fbo);
}
void h_glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
	o_glBlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
	if (!active()) return;
	s_blits.fetch_add(1, std::memory_order_relaxed);
	if (t_draw_fbo == 0) s_blits_win.fetch_add(1, std::memory_order_relaxed);
	const GLuint dt = twin_of(t_draw_fbo);
	if (!dt) return;  // mono destination: the left eye is all it gets
	const GLuint rt = twin_of(t_read_fbo);
	o_glBindFramebuffer(GL_READ_FRAMEBUFFER, rt ? rt : t_read_fbo);
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dt);
	o_glBlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
	o_glBindFramebuffer(GL_READ_FRAMEBUFFER, t_read_fbo);
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t_draw_fbo);
}

void h_glActiveTexture(GLenum u)
{
	o_glActiveTexture(u);
	if (!t_bypass) t_active = int(u - GL_TEXTURE0);
}
void h_glBindTexture(GLenum target, GLuint tex)
{
	o_glBindTexture(target, tex);
	if (!t_bypass && (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY) && t_active >= 0 && t_active < kUnits) {
		t_unit_vol[t_active] = tex;
		t_unit_vol_tgt[t_active] = target;
		if (t_active + 1 > t_units_hw) t_units_hw = t_active + 1;
		return;
	}
	if (t_bypass || target != GL_TEXTURE_2D || t_active < 0 || t_active >= kUnits) return;
	t_unit_tex[t_active] = tex;
	if (t_active + 1 > t_units_hw) t_units_hw = t_active + 1;
}
void h_glDeleteTextures(GLsizei n, const GLuint *ids)
{
	if (!t_bypass)
		for (GLsizei i = 0; i < n; ++i) {
			forget_tex(ids[i]);
			for (int u = 0; u < t_units_hw; ++u)
				if (t_unit_tex[u] == ids[i]) t_unit_tex[u] = 0;
		}
	o_glDeleteTextures(n, ids);
}
void h_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type,
                    const void *data)
{
	o_glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
	if (!t_bypass && target == GL_TEXTURE_2D && level == 0 && t_active < kUnits) forget_tex(t_unit_tex[t_active]);
}
// CPU uploads into a per-eye texture (per-tile light lists and the like are
// rebuilt on the CPU each frame): the right eye's twin gets the same upload, or
// it reads stale data.
void h_glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                       const void *data)
{
	o_glTexSubImage2D(target, level, xo, yo, w, h, fmt, type, data);
	if (t_bypass || target != GL_TEXTURE_2D || t_active >= kUnits) return;
	const GLuint tex = t_unit_tex[t_active];
	const GLuint tw = tex ? tex_lookup(tex) : 0;
	if (!tw) return;
	o_glBindTexture(GL_TEXTURE_2D, tw);
	o_glTexSubImage2D(target, level, xo, yo, w, h, fmt, type, data);
	o_glBindTexture(GL_TEXTURE_2D, tex);
}
void h_glTexStorage2D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h)
{
	o_glTexStorage2D(target, levels, ifmt, w, h);
	if (!t_bypass && target == GL_TEXTURE_2D && t_active < kUnits) forget_tex(t_unit_tex[t_active]);
}
void h_glTexParameteri(GLenum target, GLenum pn, GLint v)
{
	mirror_on_tex_twin(target, [&] { o_glTexParameteri(target, pn, v); });
}
void h_glTexParameterf(GLenum target, GLenum pn, GLfloat v)
{
	mirror_on_tex_twin(target, [&] { o_glTexParameterf(target, pn, v); });
}
void h_glGenerateMipmap(GLenum target)
{
	mirror_on_tex_twin(target, [&] { o_glGenerateMipmap(target); });
}
void h_glBindRenderbuffer(GLenum target, GLuint rb)
{
	o_glBindRenderbuffer(target, rb);
	if (!t_bypass) t_rb = rb;
}
void h_glDeleteRenderbuffers(GLsizei n, const GLuint *ids)
{
	if (!t_bypass)
		for (GLsizei i = 0; i < n; ++i) forget_rb(ids[i]);
	o_glDeleteRenderbuffers(n, ids);
}
void h_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{
	o_glRenderbufferStorage(t, f, w, h);
	if (!t_bypass) forget_rb(t_rb);
}
void h_glRenderbufferStorageMultisample(GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h)
{
	o_glRenderbufferStorageMultisample(t, s, f, w, h);
	if (!t_bypass) forget_rb(t_rb);
}

void h_glDrawArrays(GLenum m, GLint f, GLsizei c) { stereo_draw([&] { o_glDrawArrays(m, f, c); }); }
void h_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { stereo_draw([&] { o_glDrawElements(m, c, t, i); }); }
void h_glDrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n)
{
	stereo_draw([&] { o_glDrawArraysInstanced(m, f, c, n); });
}
void h_glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n)
{
	stereo_draw([&] { o_glDrawElementsInstanced(m, c, t, i, n); });
}
void h_glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i)
{
	stereo_draw([&] { o_glDrawRangeElements(m, s, e, c, t, i); });
}

void h_glDrawElementsBaseVertex(GLenum m, GLsizei c, GLenum t, const void *i, GLint bv)
{
	stereo_draw([&] { o_glDrawElementsBaseVertex(m, c, t, i, bv); });
}
void h_glDrawRangeElementsBaseVertex(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint bv)
{
	stereo_draw([&] { o_glDrawRangeElementsBaseVertex(m, s, e, c, t, i, bv); });
}
void h_glDrawElementsInstancedBaseVertex(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint bv)
{
	stereo_draw([&] { o_glDrawElementsInstancedBaseVertex(m, c, t, i, n, bv); });
}
void h_glDrawArraysIndirect(GLenum m, const void *i) { stereo_draw([&] { o_glDrawArraysIndirect(m, i); }); }
// Compute passes run twice too: screen-space passes (shadow masks, light
// culling, AO) and camera-aligned volumes must see each eye's inputs and write
// each eye's outputs, exactly like draws.
// ---- shader storage buffers: per-eye twins -------------------------------------
// A compute pass that writes a storage buffer (exposure grids, light lists) and
// the passes that read it must see the right eye's data: the twin is seeded from
// the left buffer at its first right-eye use in a frame, then written / read by
// the right eye's passes only.
bool ssbo_mode()  // debug.mc3d.ssbotwin=1: per-eye storage buffers (default off: costs a copy per buffer per frame)
{
	static int n = 0;
	static bool on = false;
	if ((n++ % 500) == 0) {
		char v[PROP_VALUE_MAX] = {0};
		__system_property_get("debug.mc3d.ssbotwin", v);
		on = v[0] == '1';
	}
	return on;
}
struct SsboBind {
	GLuint buf = 0;
	GLintptr off = 0;
	GLsizeiptr size = 0;  // 0 = whole buffer (BindBufferBase)
};
thread_local SsboBind t_ssbo[16];
struct SsboTwin {
	GLuint twin = 0;
	GLsizeiptr size = 0;
	unsigned seeded_frame = ~0u;
};
std::unordered_map<GLuint, SsboTwin> g_ssbo_twin;  // guarded by g_mu
void h_glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
	if (target == GL_SHADER_STORAGE_BUFFER && index < 16) t_ssbo[index] = {buffer, 0, 0};
	o_glBindBufferBase(target, index, buffer);
}
void h_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr off, GLsizeiptr size)
{
	if (target == GL_SHADER_STORAGE_BUFFER && index < 16) t_ssbo[index] = {buffer, off, size};
	o_glBindBufferRange(target, index, buffer, off, size);
}
// Twin of a storage buffer (created on first use, seeded once per frame).
GLuint ssbo_twin(GLuint buf)
{
	if (!buf) return 0;
	std::lock_guard<std::mutex> l(g_mu);
	SsboTwin &t = g_ssbo_twin[buf];
	if (!t.twin) {
		GLint prev = 0;
		glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prev);
		glBindBuffer(GL_COPY_READ_BUFFER, buf);
		GLint64 sz = 0;
		glGetBufferParameteri64v(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &sz);
		glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)prev);
		if (sz <= 0 || sz > (64 << 20)) {
			t.size = -1;
			return 0;
		}
		glGenBuffers(1, &t.twin);
		GLint prevw = 0;
		glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &prevw);
		glBindBuffer(GL_COPY_WRITE_BUFFER, t.twin);
		glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)sz, nullptr, GL_DYNAMIC_COPY);
		glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)prevw);
		t.size = (GLsizeiptr)sz;
	}
	if (t.size < 0) return 0;
	const unsigned f = g_frame_no.load(std::memory_order_relaxed);
	if (t.seeded_frame != f) {
		t.seeded_frame = f;
		GLint pr = 0, pw = 0;
		glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &pr);
		glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &pw);
		glBindBuffer(GL_COPY_READ_BUFFER, buf);
		glBindBuffer(GL_COPY_WRITE_BUFFER, t.twin);
		glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, t.size);
		glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)pr);
		glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)pw);
	}
	return t.twin;
}
// Bind the right-eye twins (on) or the originals (off) of the program's storage blocks.
int swap_ssbos(bool on)
{
	if (!t_program || !ssbo_mode()) return 0;
	int n = 0;
	for (int idx : program(t_program).ssbo_bindings) {
		const SsboBind &b = t_ssbo[idx];
		if (!b.buf) continue;
		const GLuint tw = on ? ssbo_twin(b.buf) : b.buf;
		if (!tw) continue;
		if (b.size > 0) o_glBindBufferRange(GL_SHADER_STORAGE_BUFFER, idx, tw, b.off, b.size);
		else o_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, idx, tw);
		++n;
	}
	return n;
}

struct ImgBind {
	GLuint tex = 0;
	GLint level = 0;
	GLboolean layered = GL_FALSE;
	GLint layer = 0;
	GLenum access = 0, fmt = 0;
};
constexpr int kImgUnits = 16;
thread_local ImgBind t_img[kImgUnits];
void h_glBindImageTexture(GLuint unit, GLuint tex, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum fmt)
{
	if (unit < (GLuint)kImgUnits) t_img[unit] = {tex, level, layered, layer, access, fmt};
	o_glBindImageTexture(unit, tex, level, layered, layer, access, fmt);
}
std::atomic<int> s_compute_doubled{0}, s_compute_mono{0};
void h_glDispatchCompute(GLuint x, GLuint y, GLuint z)
{
	if (!active()) return o_glDispatchCompute(x, y, z);
	// Right-eye images: existing twins, or new ones for images this pass writes.
	GLuint img_tw[kImgUnits] = {};
	bool any = false;
	if (t_program) {
		const std::vector<int> units = program(t_program).image_units;
		std::lock_guard<std::mutex> l(g_mu);
		for (int u : units) {
			const ImgBind &b = t_img[u];
			if (!b.tex) continue;
			if (b.layered || g_vol_twin.count(b.tex)) {
				if (vol_mode() >= 1) {
					img_tw[u] = vol_twin_locked(b.tex, b.access != GL_READ_ONLY);
				}
			} else {
				auto it = g_tex_twin.find(b.tex);
				if (it != g_tex_twin.end()) img_tw[u] = it->second;
				else if (b.access != GL_READ_ONLY) img_tw[u] = tex_twin_locked(b.tex);
			}
			any |= img_tw[u] != 0;
		}
	}
	int subs[kUnits];
	int n = 0;
	for (int u = 0; u < t_units_hw; ++u)
		if (t_unit_tex[u] && tex_lookup(t_unit_tex[u])) subs[n++] = u;
	{
		static std::unordered_map<GLuint, int> seen;
	}
	bool ssbo_any = false;
	if (t_program && ssbo_mode())
		for (int idx : program(t_program).ssbo_bindings)
			if (t_ssbo[idx].buf) ssbo_any = true;  // twin made at swap time
	any = any || ssbo_any;
	if (!any) {  // writes nothing per-eye (buffers, layered volumes, small LUTs): once
		s_compute_mono.fetch_add(1, std::memory_order_relaxed);
		static std::unordered_map<GLuint, int> seen;
		return o_glDispatchCompute(x, y, z);
	}
	o_glDispatchCompute(x, y, z);  // left eye
	{	// seed right-eye volumes from what the left pass just produced
		bool vol = false;
		for (int u = 0; u < kImgUnits; ++u) vol |= img_tw[u] && (t_img[u].layered || g_vol_twin.count(t_img[u].tex));
		if (vol) {
			glMemoryBarrier(GL_ALL_BARRIER_BITS);
			std::lock_guard<std::mutex> l(g_mu);
			for (int u = 0; u < kImgUnits; ++u)
				if (img_tw[u] && g_vol_twin.count(t_img[u].tex)) seed_vol_twin_locked(t_img[u].tex);
		}
	}
	// Right eye: the left pass's writes must land before the right pass reads shared inputs.
	for (int i = 0; i < n; ++i) {
		o_glActiveTexture(GL_TEXTURE0 + subs[i]);
		o_glBindTexture(GL_TEXTURE_2D, tex_lookup(t_unit_tex[subs[i]]));
	}
	for (int u = 0; u < kImgUnits; ++u)
		if (img_tw[u]) {
			const ImgBind &b = t_img[u];
			o_glBindImageTexture(u, img_tw[u], b.level, b.layered, b.layer, b.access, b.fmt);
		}
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);  // left pass's buffer writes land before the seed copy
	swap_volumes(true);
	swap_ssbos(true);
	upload_eye(+1);
	o_glDispatchCompute(x, y, z);
	upload_eye(-1);
	swap_ssbos(false);
	swap_volumes(false);
	for (int u = 0; u < kImgUnits; ++u)
		if (img_tw[u]) {
			const ImgBind &b = t_img[u];
			o_glBindImageTexture(u, b.tex, b.level, b.layered, b.layer, b.access, b.fmt);
		}
	for (int i = 0; i < n; ++i) {
		o_glActiveTexture(GL_TEXTURE0 + subs[i]);
		o_glBindTexture(GL_TEXTURE_2D, t_unit_tex[subs[i]]);
	}
	if (n) o_glActiveTexture(GL_TEXTURE0 + t_active);
	s_compute_doubled.fetch_add(1, std::memory_order_relaxed);
}
void h_glDrawElementsIndirect(GLenum m, GLenum t, const void *i)
{
	stereo_draw([&] { o_glDrawElementsIndirect(m, t, i); });
}

void destroy_window_twin()
{
	if (g_win_fbo) o_glDeleteFramebuffers(1, &g_win_fbo);
	if (g_win_tex) o_glDeleteTextures(1, &g_win_tex);
	if (g_win_rb) o_glDeleteRenderbuffers(1, &g_win_rb);
	g_win_fbo = g_win_tex = g_win_rb = 0;
	g_win_w = g_win_h = 0;
}

bool ensure_window_twin(int w, int h)
{
	if (g_win_fbo && g_win_w == w && g_win_h == h) return true;
	destroy_window_twin();
	GLint tex = 0, rb = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
	glGenTextures(1, &g_win_tex);
	o_glBindTexture(GL_TEXTURE_2D, g_win_tex);
	o_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	glGenRenderbuffers(1, &g_win_rb);
	o_glBindRenderbuffer(GL_RENDERBUFFER, g_win_rb);
	o_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glGenFramebuffers(1, &g_win_fbo);
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_win_fbo);
	o_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_win_tex, 0);
	o_glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_win_rb);
	const bool ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t_draw_fbo);
	o_glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
	o_glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)rb);
	if (!ok) {
		destroy_window_twin();
		return false;
	}
	g_win_w = w;
	g_win_h = h;
	return true;
}

} // namespace

GLBypass::GLBypass() { ++t_bypass; }
GLBypass::~GLBypass() { --t_bypass; }

void stereo_install_hooks()
{
	void *gles = dlopen("libGLESv2.so", RTLD_NOW);
	if (!gles) return;
	struct H {
		const char *name;
		void *detour;
		void **orig;
		bool required;
	};
#define HK(n, req) {#n, (void *)h_##n, (void **)&o_##n, req}
	const H hooks[] = {
	    HK(glUseProgram, true), HK(glLinkProgram, true), HK(glUniformMatrix4fv, true), HK(glUniform4fv, true),
	    HK(glClear, true), HK(glClearBufferfv, true), HK(glClearBufferiv, true), HK(glClearBufferuiv, true),
	    HK(glClearBufferfi, true), HK(glBindFramebuffer, true), HK(glDeleteFramebuffers, true),
	    HK(glFramebufferTexture2D, true), HK(glFramebufferTextureLayer, true), HK(glFramebufferRenderbuffer, true),
	    HK(glFramebufferTexture2DMultisampleEXT, false), HK(glDrawBuffers, true), HK(glBlitFramebuffer, true),
	    HK(glActiveTexture, true), HK(glBindTexture, true), HK(glDeleteTextures, true), HK(glTexImage2D, true),
	    HK(glTexStorage2D, true), HK(glTexParameteri, true), HK(glTexParameterf, true), HK(glGenerateMipmap, true),
	    HK(glBindRenderbuffer, true), HK(glDeleteRenderbuffers, true), HK(glRenderbufferStorage, true),
	    HK(glRenderbufferStorageMultisample, true), HK(glDrawArrays, true), HK(glDrawElements, true),
	    HK(glDrawArraysInstanced, true), HK(glDrawElementsInstanced, true), HK(glDrawRangeElements, true),
	    HK(glDrawElementsBaseVertex, false), HK(glDrawRangeElementsBaseVertex, false),
	    HK(glDrawElementsInstancedBaseVertex, false), HK(glDrawArraysIndirect, false),
	    HK(glDrawElementsIndirect, false), HK(glDispatchCompute, false), HK(glBindImageTexture, false),
	    HK(glBindBufferBase, false), HK(glBindBufferRange, false), HK(glShaderSource, false),
	    HK(glCopyImageSubData, false), HK(glCopyTexSubImage2D, false), HK(glTexSubImage2D, true),
	};
#undef HK
	bool ok = true;
	int n = 0;
	for (const H &h : hooks) {
		void *sym = dlsym(gles, h.name);
		if (!sym) sym = (void *)eglGetProcAddress(h.name);
		const bool hooked = sym && pl::memory::hook(sym, h.detour, h.orig) == 0;
		if (hooked) ++n;
		else {
			LOGI("GL hook FAILED: %s", h.name);
			if (h.required) ok = false;
		}
	}
	s_hooks_ok = ok;
	LOGI("stereo GL hooks: %d/%d", n, (int)(sizeof hooks / sizeof hooks[0]));
}

bool stereo_hooks_ok() { return s_hooks_ok.load(); }
bool stereo_is_flat() { return g_flat_now.load(); }

bool stereo_frame_was_stereo() { return t_stereo && g_win_fbo != 0 && t_warm == 0; }

unsigned stereo_window_twin_fbo() { return g_win_fbo; }





// Copy each new twin's original into it, once, between frames (never mid-pass).
void seed_pending_twins()
{
	std::vector<SeedJob> jobs;
	{
		std::lock_guard<std::mutex> l(g_mu);
		jobs.swap(g_seed_pending);
	}
	if (jobs.empty() || !o_glBindFramebuffer) return;
	GLint prev_read = 0, prev_draw = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	// glBlitFramebuffer honours the scissor test, and the game leaves a UI scissor
	// box (1024x1024) enabled at the frame boundary: with it on, only the left part
	// of each twin was seeded and the right eye showed a lighter sky with dark
	// clouds right of a vertical line at pixel 1024 of the render target.
	const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	if (scissor) glDisable(GL_SCISSOR_TEST);
	GLuint fb[2] = {};
	glGenFramebuffers(2, fb);
	for (const SeedJob &j : jobs) {
		const bool depth = is_depth_fmt(j.fmt);
		const bool stencil = j.fmt == GL_DEPTH24_STENCIL8 || j.fmt == GL_DEPTH32F_STENCIL8 || j.fmt == GL_DEPTH_STENCIL;
		const GLenum att = depth ? (stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT) : GL_COLOR_ATTACHMENT0;
		const GLbitfield mask = depth ? (GL_DEPTH_BUFFER_BIT | (stencil ? GL_STENCIL_BUFFER_BIT : 0)) : GL_COLOR_BUFFER_BIT;
		while (glGetError() != GL_NO_ERROR) {
		}
		o_glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
		o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
		if (j.rb) {
			o_glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, att, GL_RENDERBUFFER, j.src);
			o_glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att, GL_RENDERBUFFER, j.dst);
		} else {
			o_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, att, GL_TEXTURE_2D, j.src, 0);
			o_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, j.dst, 0);
		}
		const GLenum rs = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), ds = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
		if (rs == GL_FRAMEBUFFER_COMPLETE && ds == GL_FRAMEBUFFER_COMPLETE)
			o_glBlitFramebuffer(0, 0, j.w, j.h, 0, 0, j.w, j.h, mask, GL_NEAREST);
		while (glGetError() != GL_NO_ERROR) {
		}
		// detach so the scratch framebuffers hold no references
		if (j.rb) {
			o_glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
			o_glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
		} else {
			o_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
			o_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
		}
	}
	o_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
	o_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
	glDeleteFramebuffers(2, fb);
	if (scissor) glEnable(GL_SCISSOR_TEST);
}

void stereo_context_reset()
{
	std::lock_guard<std::mutex> l(g_mu);
	// The game rebuilt its framebuffers and programs on the new context before
	// this swap: their attachment records and specular marks are current and must
	// survive. Only the twins (names of the dead context, or built pre-swap and
	// now untracked) are forgotten; they come back lazily. Stale records of old
	// ids are overwritten when the game attaches / relinks those ids again.
	for (auto &kv : g_fbos) {
		kv.second.twin = 0;
		kv.second.state = 0;
		kv.second.fails = 0;
	}
	g_tex_twin.clear();
	g_rb_twin.clear();
	g_vol_twin.clear();
	g_vol_dims.clear();
	g_ssbo_twin.clear();
	g_seed_pending.clear();
	g_programs.clear();  // uniform locations are re-read at next use
	g_win_fbo = g_win_tex = g_win_rb = 0;
	g_win_w = g_win_h = 0;
	__android_log_print(ANDROID_LOG_WARN, "displayxr3d", "GL context replaced: twins dropped (framebuffer records kept); rebuilding lazily");
}

void stereo_frame_boundary(bool on, int w, int h)
{
	seed_pending_twins();
	if (!s_hooks_ok.load() || !o_glBindFramebuffer) {
		t_stereo = false;
		return;
	}
	t_render = true;
	{
		std::lock_guard<std::mutex> l(g_relink_mu);
		for (GLuint p : g_relinked) g_programs.erase(p);
		g_relinked.clear();
	}
	if (t_stereo) s_frames.fetch_add(1, std::memory_order_relaxed);
	g_frame_no.fetch_add(1, std::memory_order_relaxed);
	const bool world = t_camera_draws >= kMinWorldDraws;
	t_camera_draws = 0;
	t_flat_run = world ? 0 : t_flat_run + 1;
	t_world_run = world ? t_world_run + 1 : 0;
	if (!g_flat_menus.load()) g_flat_now = false;
	else if (t_flat_run >= kFlatAfterFrames) g_flat_now = true;
	else if (t_world_run >= kWorldAfterFrames) g_flat_now = false;
	if (on && w > 0 && h > 0 && !g_flat_now.load()) {
		const bool was = t_stereo;
		t_stereo = ensure_window_twin(w, h);
		if (t_stereo && !was) t_warm = kTwinWarmFrames;
		else if (t_warm > 0) --t_warm;
	} else {
		t_stereo = false;
	}
}

StereoStats stereo_stats()
{
	StereoStats s;
	s.draws = s_draws.load();
	s.doubled = s_doubled.load();
	s.proj_patches = s_proj.load();
	s.stereo_targets = s_targets.load();
	s.stereo_fbos = s_fbos.load();
	s.programs = s_programs.load();
	s.programs_with_camera = s_programs_cam.load();
	s.fbo_fail = s_fbo_fail.load();
	s.draws_window = s_draw_win.load();
	s.draws_mono = s_draw_mono.load();
	s.blits = s_blits.load();
	s.blits_window = s_blits_win.load();
	s.stereo_frames = s_frames.load();
	return s;
}

} // namespace dxr3d
