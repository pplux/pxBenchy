#include "lua/bind_math.h"
#include <cmath>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// Only plain arrays live across luaL_error (it longjmps: Lua is built as C).

static constexpr int VEC_MAX = 16;

// Reads the array at arg into out (1..max numbers); returns its length.
static int read_array(lua_State* L, int arg, double* out, int max) {
  luaL_checktype(L, arg, LUA_TTABLE);
  lua_Integer n = (lua_Integer)lua_rawlen(L, arg);
  if (n < 1 || n > max) luaL_argerror(L, arg, lua_pushfstring(L, "expected 1..%d numbers, got %d", max, (int)n));
  for (lua_Integer i = 1; i <= n; i++) {
    lua_rawgeti(L, arg, i);
    int ok = 0;
    out[i - 1] = lua_tonumberx(L, -1, &ok);
    if (!ok) luaL_argerror(L, arg, lua_pushfstring(L, "element %d is not a number", (int)i));
    lua_pop(L, 1);
  }
  return (int)n;
}

static void read_exact(lua_State* L, int arg, double* out, int n) {
  if (read_array(L, arg, out, n) != n) luaL_argerror(L, arg, lua_pushfstring(L, "expected %d numbers", n));
}

static void push_array(lua_State* L, const double* v, int n) {
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_pushnumber(L, v[i]);
    lua_rawseti(L, -2, i + 1);
  }
}

// Two same-length vectors at args 1 and 2.
static int read_pair(lua_State* L, double* a, double* b) {
  int n = read_array(L, 1, a, VEC_MAX);
  if (read_array(L, 2, b, VEC_MAX) != n) luaL_argerror(L, 2, "length differs from argument #1");
  return n;
}

// ---- vectors ----

static int l_add(lua_State* L) {
  double a[VEC_MAX], b[VEC_MAX];
  int n = read_pair(L, a, b);
  for (int i = 0; i < n; i++) a[i] += b[i];
  push_array(L, a, n);
  return 1;
}

static int l_sub(lua_State* L) {
  double a[VEC_MAX], b[VEC_MAX];
  int n = read_pair(L, a, b);
  for (int i = 0; i < n; i++) a[i] -= b[i];
  push_array(L, a, n);
  return 1;
}

static int l_scale(lua_State* L) {
  double v[VEC_MAX];
  int n = read_array(L, 1, v, VEC_MAX);
  double s = luaL_checknumber(L, 2);
  for (int i = 0; i < n; i++) v[i] *= s;
  push_array(L, v, n);
  return 1;
}

static double dot(const double* a, const double* b, int n) {
  double s = 0;
  for (int i = 0; i < n; i++) s += a[i] * b[i];
  return s;
}

static int l_dot(lua_State* L) {
  double a[VEC_MAX], b[VEC_MAX];
  int n = read_pair(L, a, b);
  lua_pushnumber(L, dot(a, b, n));
  return 1;
}

static int l_length(lua_State* L) {
  double v[VEC_MAX];
  int n = read_array(L, 1, v, VEC_MAX);
  lua_pushnumber(L, std::sqrt(dot(v, v, n)));
  return 1;
}

static void normalize3(double* v) {
  double l = std::sqrt(dot(v, v, 3));
  for (int i = 0; i < 3; i++) v[i] /= l;
}

static int l_normalize(lua_State* L) {
  double v[VEC_MAX];
  int n = read_array(L, 1, v, VEC_MAX);
  double l = std::sqrt(dot(v, v, n));
  if (l == 0) return luaL_argerror(L, 1, "zero-length vector");
  for (int i = 0; i < n; i++) v[i] /= l;
  push_array(L, v, n);
  return 1;
}

static void cross(const double* a, const double* b, double* o) {
  o[0] = a[1] * b[2] - a[2] * b[1];
  o[1] = a[2] * b[0] - a[0] * b[2];
  o[2] = a[0] * b[1] - a[1] * b[0];
}

static int l_cross(lua_State* L) {
  double a[3], b[3], o[3];
  read_exact(L, 1, a, 3);
  read_exact(L, 2, b, 3);
  cross(a, b, o);
  push_array(L, o, 3);
  return 1;
}

// ---- mat4 (column-major: m[c * 4 + r]) ----

static void identity(double* m) {
  for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0 : 0.0;
}

static int l_identity(lua_State* L) {
  double m[16];
  identity(m);
  push_array(L, m, 16);
  return 1;
}

static int l_mul(lua_State* L) {
  double a[16], b[16], o[16];
  read_exact(L, 1, a, 16);
  read_exact(L, 2, b, 16);
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      double s = 0;
      for (int i = 0; i < 4; i++) s += a[i * 4 + r] * b[c * 4 + i];
      o[c * 4 + r] = s;
    }
  push_array(L, o, 16);
  return 1;
}

static int l_transform(lua_State* L) {
  double m[16], v[4], o[4];
  read_exact(L, 1, m, 16);
  read_exact(L, 2, v, 4);
  for (int r = 0; r < 4; r++) o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
  push_array(L, o, 4);
  return 1;
}

// perspective(fovy_radians, aspect, near, far): depth 0..1, Y flipped for the native Vulkan viewport.
static int l_perspective(lua_State* L) {
  double fovy = luaL_checknumber(L, 1), aspect = luaL_checknumber(L, 2);
  double n = luaL_checknumber(L, 3), f = luaL_checknumber(L, 4);
  double t = 1.0 / std::tan(fovy / 2), m[16] = {};
  m[0] = t / aspect;
  m[5] = -t;
  m[10] = f / (n - f);
  m[11] = -1;
  m[14] = n * f / (n - f);
  push_array(L, m, 16);
  return 1;
}

// look_at(eye, target, up): right-handed view matrix.
static int l_look_at(lua_State* L) {
  double eye[3], target[3], up[3], f[3], s[3], u[3];
  read_exact(L, 1, eye, 3);
  read_exact(L, 2, target, 3);
  read_exact(L, 3, up, 3);
  for (int i = 0; i < 3; i++) f[i] = target[i] - eye[i];
  normalize3(f);
  cross(f, up, s);
  normalize3(s);
  cross(s, f, u);
  double m[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
                  -dot(s, eye, 3), -dot(u, eye, 3), dot(f, eye, 3), 1};
  push_array(L, m, 16);
  return 1;
}

static int l_translation(lua_State* L) {
  double v[3], m[16];
  read_exact(L, 1, v, 3);
  identity(m);
  m[12] = v[0];
  m[13] = v[1];
  m[14] = v[2];
  push_array(L, m, 16);
  return 1;
}

static int l_scaling(lua_State* L) {
  double v[3], m[16];
  read_exact(L, 1, v, 3);
  identity(m);
  m[0] = v[0];
  m[5] = v[1];
  m[10] = v[2];
  push_array(L, m, 16);
  return 1;
}

// rotation(axis, radians): counter-clockwise looking down the axis (right-handed).
static int l_rotation(lua_State* L) {
  double a[3];
  read_exact(L, 1, a, 3);
  double angle = luaL_checknumber(L, 2);
  if (dot(a, a, 3) == 0) return luaL_argerror(L, 1, "zero-length axis");
  normalize3(a);
  double c = std::cos(angle), s = std::sin(angle), t = 1 - c, x = a[0], y = a[1], z = a[2];
  double m[16] = {t * x * x + c,     t * x * y + s * z, t * x * z - s * y, 0,
                  t * x * y - s * z, t * y * y + c,     t * y * z + s * x, 0,
                  t * x * z + s * y, t * y * z - s * x, t * z * z + c,     0,
                  0,                 0,                 0,                 1};
  push_array(L, m, 16);
  return 1;
}

static int l_transpose(lua_State* L) {
  double m[16], o[16];
  read_exact(L, 1, m, 16);
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) o[r * 4 + c] = m[c * 4 + r];
  push_array(L, o, 16);
  return 1;
}

// inverse(m): cofactor expansion; nil, "singular matrix" when the determinant is 0.
static int l_inverse(lua_State* L) {
  double m[16], inv[16];
  read_exact(L, 1, m, 16);
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
           m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
            m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (det == 0) {
    lua_pushnil(L);
    lua_pushstring(L, "singular matrix");
    return 2;
  }
  for (int i = 0; i < 16; i++) inv[i] /= det;
  push_array(L, inv, 16);
  return 1;
}

void bind_math_push(lua_State* L) {
  static const luaL_Reg fns[] = {
      {"add", l_add},           {"sub", l_sub},
      {"scale", l_scale},       {"dot", l_dot},
      {"length", l_length},     {"normalize", l_normalize},
      {"cross", l_cross},       {"identity", l_identity},
      {"mul", l_mul},           {"transform", l_transform},
      {"perspective", l_perspective}, {"look_at", l_look_at},
      {"translation", l_translation}, {"scaling", l_scaling},
      {"rotation", l_rotation}, {"transpose", l_transpose},
      {"inverse", l_inverse},   {nullptr, nullptr}};
  luaL_newlib(L, fns);
}
