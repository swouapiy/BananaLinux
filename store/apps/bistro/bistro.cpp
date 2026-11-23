/*
 * Bistro: a co-op cooking game for the RG DS. The kitchen on the top screen (its customers along the top), the orders
 * on the bottom one. One kitchen, three recipes; on one handheld you run two chefs and Y switches between them.
 * Three ways to play: Play (no clock: keep the customers happy, saved when you leave), Rush (three minutes, tips) and
 * the Tutorial (lessons, one step at a time).
 *
 * Everything is in this binary: the pictures and fonts (bistro-assets.h, Kenney's CC0 Food Kit and fonts, Inter),
 * the sounds and the music (bistro-sounds.h, Kenney's CC0 Interface Sounds and Doge's CC0 "Catchy Swing"). Plain C++17 with DRM/KMS and evdev ioctls (the same
 * shim as Activity) and ALSA for the sound; no SDL.
 *
 *   banana-bistro [--data DIR] [--top DSI-2]           play (both panels; the menu has let go of them)
 *   banana-bistro --smoke PREFIX                        draw each screen to PREFIX-<screen>.png (no device)
 *   banana-bistro --bench                               what drawing a frame costs
 */
#include "bistro-game.h"
#include "bistro-tutorial.h"
#include "bistro-assets.h"
#include "bistro-sounds.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#if defined(__linux__) && !defined(BISTRO_NO_DEVICE)
#define BISTRO_DEVICE 1
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef BISTRO_NO_ALSA
#include <alsa/asoundlib.h>
#endif
#endif

using namespace bistro;
static const int W = 640, H = 480, T = 40;

/* ------------------------------------------------------------------------------------------------ drawing */

struct Canvas {
	int w, h;
	std::vector<uint32_t> px;
	Canvas(int w_ = W, int h_ = H) : w(w_), h(h_), px((size_t)w_ * h_, 0xff000000) {}

	inline void blend(int i, uint32_t c, float a)
	{
		if (a <= 0.002f) return;
		if (a >= 0.998f) { px[i] = 0xff000000 | c; return; }
		uint32_t d = px[i]; int ia = (int)(a * 256), na = 256 - ia;
		uint32_t r = (((c >> 16) & 255) * ia + ((d >> 16) & 255) * na) >> 8;
		uint32_t g = (((c >> 8) & 255) * ia + ((d >> 8) & 255) * na) >> 8;
		uint32_t b = ((c & 255) * ia + (d & 255) * na) >> 8;
		px[i] = 0xff000000 | r << 16 | g << 8 | b;
	}
	void fill(uint32_t c) { std::fill(px.begin(), px.end(), 0xff000000 | c); }
	void rect(float x0, float y0, float x1, float y1, uint32_t c, float a = 1)
	{
		int ix0 = std::max(0, (int)std::lround(x0)), iy0 = std::max(0, (int)std::lround(y0));
		int ix1 = std::min(w, (int)std::lround(x1)), iy1 = std::min(h, (int)std::lround(y1));
		for (int y = iy0; y < iy1; ++y)
			for (int x = ix0; x < ix1; ++x) blend(y * w + x, c, a);
	}
	// A rounded rectangle, anti-aliased; soft > 0 blurs its edge that far (shadows)
	void rrect(float x0, float y0, float x1, float y1, float r, uint32_t c, float a = 1, float soft = 0)
	{
		if (x1 <= x0 || y1 <= y0) return;
		float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
		r = std::min(r, std::min(hx, hy));
		float e = soft + 1;
		int ix0 = std::max(0, (int)std::floor(x0 - e)), iy0 = std::max(0, (int)std::floor(y0 - e));
		int ix1 = std::min(w, (int)std::ceil(x1 + e)), iy1 = std::min(h, (int)std::ceil(y1 + e));
		for (int y = iy0; y < iy1; ++y) {
			float py = std::fabs(y + 0.5f - cy) - hy + r;
			for (int x = ix0; x < ix1; ++x) {
				float qx = std::fabs(x + 0.5f - cx) - hx + r;
				float d = (qx > 0 || py > 0 ? std::hypot(std::max(qx, 0.f), std::max(py, 0.f)) : std::max(qx, py)) - r;
				float cov;
				if (soft > 0) { float t = std::clamp((d + soft) / (2 * soft), 0.f, 1.f); cov = 1 - t * t * (3 - 2 * t); }
				else cov = std::clamp(0.5f - d, 0.f, 1.f);
				blend(y * w + x, c, cov * a);
			}
		}
	}
	// The edge of a rounded rectangle, width wide, inside it
	void rring(float x0, float y0, float x1, float y1, float r, float width, uint32_t c, float a = 1)
	{
		float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
		r = std::min(r, std::min(hx, hy));
		int ix0 = std::max(0, (int)std::floor(x0 - 1)), iy0 = std::max(0, (int)std::floor(y0 - 1));
		int ix1 = std::min(w, (int)std::ceil(x1 + 1)), iy1 = std::min(h, (int)std::ceil(y1 + 1));
		for (int y = iy0; y < iy1; ++y)
			for (int x = ix0; x < ix1; ++x) {
				float qx = std::fabs(x + 0.5f - cx) - hx + r, qy = std::fabs(y + 0.5f - cy) - hy + r;
				float d = (qx > 0 || qy > 0 ? std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) : std::max(qx, qy)) - r;
				float cov = std::clamp(0.5f - d, 0.f, 1.f) - std::clamp(0.5f - (d + width), 0.f, 1.f);
				blend(y * w + x, c, cov * a);
			}
	}
	void ellipse(float cx, float cy, float rx, float ry, uint32_t c, float a = 1, float soft = 0)
	{
		float m = std::min(rx, ry), e = soft + 1;
		int ix0 = std::max(0, (int)std::floor(cx - rx - e)), iy0 = std::max(0, (int)std::floor(cy - ry - e));
		int ix1 = std::min(w, (int)std::ceil(cx + rx + e)), iy1 = std::min(h, (int)std::ceil(cy + ry + e));
		for (int y = iy0; y < iy1; ++y)
			for (int x = ix0; x < ix1; ++x) {
				float nx = (x + 0.5f - cx) / rx, ny = (y + 0.5f - cy) / ry;
				float d = (std::sqrt(nx * nx + ny * ny) - 1) * m;
				float cov;
				if (soft > 0) { float t = std::clamp((d + soft) / (2 * soft), 0.f, 1.f); cov = 1 - t * t * (3 - 2 * t); }
				else cov = std::clamp(0.5f - d, 0.f, 1.f);
				blend(y * w + x, c, cov * a);
			}
	}
	// A filled polygon (the stars), 4x4 samples a pixel
	void poly(const std::vector<std::pair<float, float>>& p, uint32_t c, float a = 1)
	{
		float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
		for (auto& q : p) { minx = std::min(minx, q.first); maxx = std::max(maxx, q.first); miny = std::min(miny, q.second); maxy = std::max(maxy, q.second); }
		auto inside = [&](float x, float y) {
			bool in = false;
			for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
				if ((p[i].second > y) != (p[j].second > y) && x < (p[j].first - p[i].first) * (y - p[i].second) / (p[j].second - p[i].second) + p[i].first) in = !in;
			return in;
		};
		for (int y = std::max(0, (int)minx * 0 + (int)miny); y <= std::min(h - 1, (int)maxy); ++y)
			for (int x = std::max(0, (int)minx); x <= std::min(w - 1, (int)maxx); ++x) {
				int n = 0;
				for (int sy = 0; sy < 4; ++sy)
					for (int sx = 0; sx < 4; ++sx) n += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
				blend(y * w + x, c, n / 16.f * a);
			}
	}
	void star(float cx, float cy, float r, uint32_t c, float a = 1)
	{
		std::vector<std::pair<float, float>> p;
		for (int i = 0; i < 10; ++i) {
			float ang = (float)M_PI / 2 + i * (float)M_PI / 5, rr = i % 2 ? r * 0.45f : r;
			p.push_back({ cx + std::cos(ang) * rr, cy - std::sin(ang) * rr });
		}
		poly(p, c, a);
	}
	void copy(const Canvas& o) { px = o.px; }
};

// --- the baked pictures
struct SpriteRef { const bistro_assets::Sprite* big = nullptr; const bistro_assets::Sprite* small = nullptr; };
static std::unordered_map<std::string, SpriteRef> g_sprites;
static void loadSprites()
{
	for (auto& s : bistro_assets::sprites) {
		std::string n = s.name;
		auto u = n.rfind('_');
		std::string base = n.substr(0, u);
		if (n.substr(u + 1) == "64") g_sprites[base].big = &s; else g_sprites[base].small = &s;
	}
}
// A picture centred on (cx, cy), its longer side `size` px; dark > 0 darkens it (0..1)
static void sprite(Canvas& cv, const char* name, float cx, float cy, float size, float alpha = 1, float dark = 0)
{
	auto it = g_sprites.find(name);
	if (it == g_sprites.end()) return;
	const bistro_assets::Sprite* s = size <= 36 && it->second.small ? it->second.small : it->second.big;
	float sc = size / std::max(s->w, s->h);
	float dw = s->w * sc, dh = s->h * sc, x0 = cx - dw / 2, y0 = cy - dh / 2;
	int ix0 = std::max(0, (int)std::floor(x0)), iy0 = std::max(0, (int)std::floor(y0));
	int ix1 = std::min(cv.w, (int)std::ceil(x0 + dw)), iy1 = std::min(cv.h, (int)std::ceil(y0 + dh));
	int ia = (int)(alpha * 256), id = (int)((1 - dark) * 256);
	for (int y = iy0; y < iy1; ++y) {
		float sy = (y + 0.5f - y0) / sc - 0.5f;
		int y_0 = (int)std::floor(sy); float fy = sy - y_0;
		for (int x = ix0; x < ix1; ++x) {
			float sx = (x + 0.5f - x0) / sc - 0.5f;
			int x_0 = (int)std::floor(sx); float fx = sx - x_0;
			float acc[4] = { 0, 0, 0, 0 };
			for (int k = 0; k < 4; ++k) {
				int xx = x_0 + (k & 1), yy = y_0 + (k >> 1);
				if (xx < 0 || yy < 0 || xx >= s->w || yy >= s->h) continue;
				float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
				uint32_t p = s->px[yy * s->w + xx];
				acc[0] += wgt * (p >> 24); acc[1] += wgt * ((p >> 16) & 255); acc[2] += wgt * ((p >> 8) & 255); acc[3] += wgt * (p & 255);
			}
			int sa = (int)acc[0] * ia >> 8;
			if (sa <= 0) continue;
			int r = (int)acc[1] * ia >> 8, g = (int)acc[2] * ia >> 8, b = (int)acc[3] * ia >> 8;
			r = r * id >> 8; g = g * id >> 8; b = b * id >> 8;
			uint32_t& d = cv.px[y * cv.w + x];
			int na = 255 - sa;
			int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
			d = 0xff000000 | std::min(255, r + dr * na / 255) << 16 | std::min(255, g + dg * na / 255) << 8 | std::min(255, b + db * na / 255);
		}
	}
}

// --- the baked fonts
enum FontId { F11, F13, F16, F24, F32, F48, N13, N15, K13 };  // Kenney Future; N: Inter SemiBold; K: Inter Bold
struct FontIdx { std::array<int16_t, 256> at; };
static FontIdx g_fidx[9];
static void loadFonts()
{
	for (int f = 0; f < 9; ++f) {
		g_fidx[f].at.fill(-1);
		auto& F = bistro_assets::fonts[f];
		for (int i = 0; i < F.n; ++i)
			if (F.g[i].cp < 256) g_fidx[f].at[F.g[i].cp] = (int16_t)i;
	}
}
static std::vector<uint32_t> utf8(const std::string& s)
{
	std::vector<uint32_t> out;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = s[i];
		if (c < 0x80) { out.push_back(c); i++; }
		else if ((c & 0xe0) == 0xc0 && i + 1 < s.size()) { out.push_back((c & 0x1f) << 6 | (s[i + 1] & 0x3f)); i += 2; }
		else if ((c & 0xf0) == 0xe0 && i + 2 < s.size()) { out.push_back((c & 0x0f) << 12 | (s[i + 1] & 0x3f) << 6 | (s[i + 2] & 0x3f)); i += 3; }
		else { out.push_back('?'); i++; }
	}
	return out;
}
static const bistro_assets::Glyph* glyph(int f, uint32_t cp)
{
	auto& F = bistro_assets::fonts[f];
	if (cp < 256 && g_fidx[f].at[cp] >= 0) return &F.g[g_fidx[f].at[cp]];
	// a letter the font lacks (an accent): its plain letter
	static const char* plain = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
	if (cp >= 0xC0 && cp < 0x100 && g_fidx[f].at[(unsigned char)plain[cp - 0xC0]] >= 0) return &F.g[g_fidx[f].at[(unsigned char)plain[cp - 0xC0]]];
	if (g_fidx[f].at['?'] >= 0) return &F.g[g_fidx[f].at['?']];
	return nullptr;
}
static float textW(int f, const std::string& s, float track = 0)
{
	float w = 0;
	for (uint32_t cp : utf8(s)) if (auto g = glyph(f, cp)) w += g->adv + track;
	return w - (s.empty() ? 0 : track);
}
// align: 0 left, 1 centre, 2 right. y is the middle of the capitals.
static float text(Canvas& cv, int f, float x, float y, const std::string& s, uint32_t c, int align = 0, float a = 1, float track = 0)
{
	auto& F = bistro_assets::fonts[f];
	float wdt = textW(f, s, track);
	if (align == 1) x -= wdt / 2; else if (align == 2) x -= wdt;
	const bistro_assets::Glyph* H_ = glyph(f, 'H');
	float top = y - (H_ ? H_->oy + H_->h / 2.f : F.ascent / 2.f);
	float pen = std::round(x);
	top = std::round(top);
	for (uint32_t cp : utf8(s)) {
		auto g = glyph(f, cp);
		if (!g) continue;
		const uint8_t* bits = F.bits + g->at;
		for (int gy = 0; gy < g->h; ++gy) {
			int yy = (int)top + g->oy + gy;
			if (yy < 0 || yy >= cv.h) continue;
			for (int gx = 0; gx < g->w; ++gx) {
				int xx = (int)pen + g->ox + gx;
				if (xx < 0 || xx >= cv.w) continue;
				uint8_t v = bits[gy * g->w + gx];
				if (v) cv.blend(yy * cv.w + xx, c, v / 255.f * a);
			}
		}
		pen += g->adv + track;
	}
	return wdt;
}

/* ------------------------------------------------------------------------------------------------ the look */

static const uint32_t FLOOR_A = 0xf6ecd6, FLOOR_B = 0xecdec2, WALL = 0x5c465c;
static const uint32_t DINING = 0xe0ae78, DINING_LINE = 0xd09c66, DOOR = 0x6e4a3a, DOOR_IN = 0x3e2a24;
static const uint32_t COUNTER_TOP = 0xeccea0, COUNTER_EDGE = 0x967046, SHADOW = 0x1e140a;
static const uint32_t YELLOW = 0xffd640, INK = 0x422c1e, CARD = 0xfff8ec, CARD_LINE = 0xe2d6c4;
static const uint32_t BG = 0x2c2234, BG_STRIPE = 0x32273a, PANEL = 0x3c3046, LIGHT = 0xf0e8f6, MUTED = 0xbeb0cc;
static const uint32_t GREEN = 0x34c759, ORANGE = 0xff9f0a, RED = 0xff453a, WHITE = 0xffffff;
static const uint32_t CHEF_COL[4] = { 0xf5a623, 0x409cff, 0x34c759, 0xff6b9a };

static const char* foodSprite(Bit b)
{
	switch (b.f) {
	case TOMATO: return b.s == RAW ? "tomato" : "tomato_cut";
	case CABBAGE: return b.s == RAW ? "cabbage" : "cabbage_cut";
	case MEAT: return b.s == RAW ? "meat" : b.s == CUT ? "patty_raw" : b.s == COOKED ? "patty" : "patty_burnt";
	case CHEESE: return b.s == RAW ? "cheese" : "cheese_cut";
	case BREAD: return "bread";
	case SOUP: return b.s == BURNT ? "pot_burnt" : "soup";
	default: return "plate";
	}
}
static const char* dishSprite(int r) { return r == SALAD ? "salad" : r == TOMATO_SOUP ? "soup" : "burger"; }

static void drawThing(Canvas& cv, const Thing& t, float cx, float cy, float size)
{
	if (t.kind == Thing::FOOD) { sprite(cv, foodSprite(t.food), cx, cy, size * (t.food.s == RAW ? 0.9f : 0.85f)); return; }
	if (t.kind != Thing::PLATE) return;
	int r = plateIs(t.on);
	if (r >= 0) { sprite(cv, dishSprite(r), cx, cy, size * 1.05f); return; }
	sprite(cv, "plate", cx, cy, size);
	static const float spots[3][3][2] = { { { 0, 0 } }, { { -0.18f, 0 }, { 0.18f, 0 } }, { { -0.2f, 0.06f }, { 0.2f, 0.06f }, { 0, -0.14f } } };
	int n = (int)std::min<size_t>(3, t.on.size());
	for (int i = 0; i < n; ++i)
		sprite(cv, foodSprite(t.on[i]), cx + spots[n - 1][i][0] * size, cy - 2 + spots[n - 1][i][1] * size, size * 0.5f);
}

static uint32_t mixc(uint32_t a, uint32_t b, float t)
{
	auto ch = [&](int sh) { return (uint32_t)std::lround(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
	return ch(16) | ch(8) | ch(0);
}
static void heart(Canvas& cv, float cx, float cy, float r, uint32_t c, float a = 1)
{
	cv.ellipse(cx - r * 0.48f, cy - r * 0.25f, r * 0.55f, r * 0.55f, c, a);
	cv.ellipse(cx + r * 0.48f, cy - r * 0.25f, r * 0.55f, r * 0.55f, c, a);
	cv.poly({ { cx - r * 1.0f, cy - r * 0.1f }, { cx + r * 1.0f, cy - r * 0.1f }, { cx, cy + r * 0.95f } }, c, a);
}
static void bar(Canvas& cv, float x, float y, float wdt, float p, uint32_t c)
{
	cv.rrect(x, y, x + wdt, y + 7, 3.5f, 0x281e28);
	cv.rrect(x + 1, y + 1, x + 1 + (wdt - 2) * std::clamp(p, 0.f, 1.f), y + 6, 2.5f, c);
}
static void badge(Canvas& cv, float cx, float cy, const char* s, uint32_t c)
{
	cv.ellipse(cx, cy, 10, 10, c);
	text(cv, F13, cx + 0.5f, cy, s, WHITE, 1);
}

/* ------------------------------------------------------------------------------------------------ the kitchen */

// What doesn't move: the floor, the walls, the counters and what's built into them. Drawn once.
static void drawKitchenBase(Canvas& cv, const Kitchen& k)
{
	for (int gy = 0; gy < MH; ++gy)
		for (int gx = 0; gx < MW; ++gx) cv.rect(gx * T, gy * T, (gx + 1) * T, (gy + 1) * T, (gx + gy) % 2 ? FLOOR_B : FLOOR_A);
	// the dining room along the top (rows 0-1), where the customers come in by the left door and leave by the right
	cv.rect(0, 0, W, 2 * T, DINING);
	for (int i = 0; i < 8; ++i) cv.rect(0, 14 + i * 9, W, 15 + i * 9, DINING_LINE);
	cv.rect(0, 0, W, 10, WALL);
	cv.rrect(-12, 4, 30, 78, 6, DOOR); cv.rrect(-12, 8, 24, 78, 4, DOOR_IN);
	cv.rrect(610, 4, 652, 78, 6, DOOR); cv.rrect(616, 8, 652, 78, 4, DOOR_IN);
	cv.rect(0, 11 * T, W, H, WALL);
	auto counter = [&](int gx, int gy) { return gy >= 2 && gy <= MH - 2 && k.cells[gy][gx].solid(); };
	for (int gy = 0; gy < MH; ++gy)
		for (int gx = 0; gx < MW; ++gx)
			if (counter(gx, gy)) cv.rrect(gx * T, gy * T + 5, gx * T + T, gy * T + T + 5, 4, SHADOW, 0.28f, 3);
	for (int gy = 0; gy < MH; ++gy)
		for (int gx = 0; gx < MW; ++gx) {
			if (!counter(gx, gy)) continue;
			float x = gx * T, y = gy * T, cx = x + T / 2.f, cy = y + T / 2.f - 3;
			cv.rrect(x, y, x + T, y + T, 3, COUNTER_EDGE);
			cv.rrect(x + 1, y + 1, x + T - 1, y + T - 6, 3, COUNTER_TOP);
			char ch = k.cells[gy][gx].kind;
			Food crate = k.crateFood(gx, gy);
			if (crate != NOFOOD && ch >= 'A' && ch <= 'Z') {
				cv.rrect(x + 3, y + 3, x + 2 * T - 3, y + T - 9, 5, 0xa06838);
				cv.rrect(x + 6, y + 6, x + 2 * T - 6, y + T - 12, 3, 0xc4884e);
				for (int i = 0; i < 4; ++i)
					sprite(cv, foodSprite({ crate, RAW }), x + 18 + i * 16, cy + (i % 2 ? 2 : -2), 22);
			} else if (ch == 'P') {
				for (int i = 0; i < 3; ++i) sprite(cv, "plate", cx, cy + 2 - i * 3, 30);
			} else if (ch == 'W') {
				cv.rrect(x + 4, y + 2, x + T - 2, y + T - 8, 4, YELLOW);
				cv.rect(x + 9, y + 8, x + 11, y + T - 14, 0xc8961e);
			} else if (ch == 'K') {
				sprite(cv, "board", cx, cy, 34);
			} else if (ch == 'S' || ch == 'F') {
				cv.rrect(x + 3, y + 3, x + T - 3, y + T - 9, 4, 0x464854);
				for (int i = 0; i < 4; ++i) {
					float bx = x + (i % 2 ? 28 : 12), by = y + (i / 2 ? 22 : 11);
					cv.ellipse(bx, by, 5, 4, 0x787c8c);
					cv.ellipse(bx, by, 3, 2, 0x464854);
				}
			} else if (ch == 'X') {
				cv.rrect(x + 9, y + 3, x + T - 9, y + T - 9, 5, 0x58966e);
				cv.rrect(x + 6, y + 3, x + T - 6, y + 9, 3, 0x407856);
				for (int i = 0; i < 3; ++i) cv.rect(x + 14 + i * 5, y + 13, x + 16 + i * 5, y + T - 13, 0x407856);
			}
		}
}

struct Floater { float x, y; std::string s; uint32_t c; float life = 1.2f, age = 0; int font = F16; };

static void drawChef(Canvas& cv, const Chef& c, int i, bool active, bool marker, float clock, bool moving)
{
	float cx = c.x * T, cy = c.y * T;
	float bob = moving ? std::fabs(std::sin(clock * 14 + i)) * -2.5f : 0;
	uint32_t body = CHEF_COL[i % 4];
	cv.ellipse(cx, cy + 15, 15, 5, SHADOW, 0.35f, 2);
	bool back = c.fy < -0.4f;
	auto held = [&] {
		if (c.hand.empty()) return;
		drawThing(cv, c.hand, cx + c.fx * 17, cy + 4 + c.fy * 6 + bob, 26);
	};
	if (back) held();
	cy += bob;
	cv.ellipse(cx, cy + 5, 15, 11, body);
	cv.ellipse(cx, cy + 9, 10, 6, WHITE, 0.85f);                    // apron
	cv.ellipse(cx, cy - 8, 11, 10, 0xffdcba);                       // head
	cv.ellipse(cx, cy - 21, 12, 9, WHITE);                          // hat
	cv.rrect(cx - 10, cy - 16, cx + 10, cy - 11, 2, 0xebebf0);
	if (!back) {
		float ex = c.fx * 4, ey = c.fy * 1.5f;
		cv.ellipse(cx - 4 + ex, cy - 6 + ey, 1.7f, 2, 0x32221e);
		cv.ellipse(cx + 4 + ex, cy - 6 + ey, 1.7f, 2, 0x32221e);
	}
	if (!back) held();
	if (c.chopping) sprite(cv, "knife", cx + c.fx * 26, cy - 6 + c.fy * 18 - std::fabs(std::sin(clock * 22)) * 6, 20);
	if (marker) {
		// the chef you're running: a little arrow in their colour, bobbing
		float ay = cy - 40 + std::sin(clock * 5) * 2.5f;
		std::vector<std::pair<float, float>> p = { { cx - 8, ay - 6 }, { cx + 8, ay - 6 }, { cx, ay + 4 } };
		cv.poly({ { cx - 10, ay - 8 }, { cx + 10, ay - 8 }, { cx, ay + 6 } }, WHITE);
		cv.poly(p, body);
	}
	(void)active;
}

// A customer: one for each order, in the dining room along the top. In by the left door to a seat, their dish in a
// bubble; out by the right door, hearts when served, cross when they gave up waiting.
struct Customer {
	enum State { COMING, WAITING, HAPPY, MAD } state = COMING;
	int id = 0, seat = 0, recipe = 0;
	float x = -30, t = 0;
	uint32_t look = 0;
};
static const int SEATS = 4;
static float seatX(int i) { return 80 + i * 135.f; }

struct View {
	int active = 0;          // the chef this handheld runs
	bool markers = true;     // the arrow over that chef
	float clock = 0;
	std::vector<Floater> floaters;
	std::vector<std::pair<float, float>> lastPos;
	std::vector<Customer> customers;
	uint32_t looks = 7;

	// after each step of the kitchen shown: its events, then everyone's walk
	void updateCustomers(const Kitchen& k, float dt)
	{
		for (auto& e : k.events) {
			if (e.kind != Event::SERVED && e.kind != Event::MISSED) continue;
			for (auto& c : customers)
				if (c.id == e.order && c.state < Customer::HAPPY) { c.state = e.kind == Event::SERVED ? Customer::HAPPY : Customer::MAD; c.t = 0; }
		}
		// a waiting customer whose order is gone without a word (the kitchen started again): gone too
		customers.erase(std::remove_if(customers.begin(), customers.end(), [&](const Customer& c) {
			if (c.state >= Customer::HAPPY) return c.x > W + 40;
			for (auto& o : k.orders) if (o.id == c.id) return false;
			return true;
		}), customers.end());
		// each order its customer (a new one, the first two, a saved game's): in by the door
		for (auto& o : k.orders) {
			bool has = false;
			for (auto& c : customers) has = has || c.id == o.id;
			if (has) continue;
			bool taken[SEATS] = {};
			for (auto& c : customers) if (c.state < Customer::HAPPY && c.seat < SEATS) taken[c.seat] = true;
			Customer c; c.id = o.id; c.recipe = o.r;
			c.seat = 0; while (c.seat < SEATS - 1 && taken[c.seat]) c.seat++;
			looks = looks * 1103515245u + 12345u; c.look = looks >> 8;
			c.x = -30 - 30.f * (float)customers.size();
			customers.push_back(c);
		}
		for (auto& c : customers) {
			c.t += dt;
			if (c.state == Customer::COMING) {
				c.x = std::min(seatX(c.seat), c.x + 130 * dt);
				if (c.x >= seatX(c.seat)) { c.state = Customer::WAITING; c.t = 0; }
			} else if (c.state >= Customer::HAPPY && c.t > 0.9f) c.x += (c.state == Customer::MAD ? 170 : 110) * dt;
		}
	}
};

static const uint32_t SHIRTS[6] = { 0x5b8def, 0xe85d75, 0x47b881, 0x9b6bdf, 0x3fb8c8, 0xf08a3c };
static const uint32_t SKINS[4] = { 0xffdcba, 0xf0c090, 0xc68c5c, 0x8d5a3a };
static const uint32_t HAIRS[5] = { 0x3a2a20, 0x7a4a24, 0xe8c060, 0x222222, 0xb85030 };

static void drawCustomer(Canvas& cv, const Customer& c, const Order* o, bool patient, float clock)
{
	bool walking = c.state == Customer::COMING || (c.state >= Customer::HAPPY && c.t > 0.9f);
	float x = c.x, y = 46 + (walking ? -std::fabs(std::sin(clock * 12 + c.id)) * 2.5f : 0);
	cv.ellipse(x, 70, 13, 4, SHADOW, 0.3f, 2);
	uint32_t shirt = SHIRTS[c.look % 6];
	if (c.state == Customer::MAD) shirt = mixc(shirt, RED, 0.4f);
	cv.ellipse(x, y + 12, 13, 10, shirt);
	cv.ellipse(x, y - 4, 10, 9.5f, SKINS[(c.look / 6) % 4]);
	cv.ellipse(x, y - 10, 10.5f, 5.5f, HAIRS[(c.look / 24) % 5]);
	cv.ellipse(x - 3.5f, y - 2, 1.5f, 1.8f, 0x32221e);
	cv.ellipse(x + 3.5f, y - 2, 1.5f, 1.8f, 0x32221e);
	if (c.state == Customer::WAITING && o) {
		// the dish they want, in a bubble; under it, how long they'll still wait
		float p = o->left / o->total, shake = !patient && p < 0.25f ? std::sin(clock * 40) * 1.2f : 0;
		float bx = x + 34 + shake, by = 24;
		cv.rrect(bx - 19, by - 17, bx + 19, by + 15, 11, SHADOW, 0.25f, 2);
		cv.poly({ { bx - 16, by + 4 }, { bx - 6, by + 12 }, { bx - 24, by + 16 } }, WHITE);
		cv.rrect(bx - 19, by - 18, bx + 19, by + 13, 11, WHITE);
		sprite(cv, dishSprite(o->r), bx, by - 3, 25);
		if (!patient) {
			uint32_t col = p > 0.5f ? GREEN : p > 0.25f ? ORANGE : RED;
			bar(cv, bx - 18, by + 18, 36, p, col);
		}
	} else if (c.state >= Customer::HAPPY) {
		float rise = std::min(1.f, c.t * 3);
		sprite(cv, c.state == Customer::HAPPY ? "emo_hearts" : "emo_anger", x + 14, y - 22 - rise * 6, 24, std::min(1.f, c.t * 4));
	}
}

static void drawKitchen(Canvas& cv, const Canvas& base, const Kitchen& k, View& v)
{
	cv.copy(base);
	// the target square of the chef you run
	int tx, ty;
	if (v.active < (int)k.chefs.size() && k.target(k.chefs[v.active], tx, ty))
		cv.rring(tx * T - 1, ty * T - 1, tx * T + T + 1, ty * T + T - 4, 5, 2.5f, WHITE, 0.85f);
	for (int gy = 2; gy < MH - 1; ++gy)
		for (int gx = 0; gx < MW; ++gx) {
			const Cell& c = k.cells[gy][gx];
			float x = gx * T, y = gy * T, cx = x + T / 2.f, cy = y + T / 2.f - 4;
			if (c.kind == 'S' || c.kind == 'F') {
				const Vessel& ve = c.vessel;
				if (ve.kind == Vessel::POT) {
					const char* spr = ve.burnt() ? "pot_burnt" : ve.full() ? "pot_soup" : "pot";
					sprite(cv, spr, cx, cy, 34);
					if (!ve.full() && ve.n)
						for (int i = 0; i < 3; ++i) cv.ellipse(cx - 9 + i * 9, y - 4, 3.2f, 3.2f, i < ve.n ? RED : 0x281e28, i < ve.n ? 1 : 0.5f);
				} else {
					sprite(cv, "pan", cx, cy, 34);
					if (ve.n) sprite(cv, ve.burnt() ? "patty_burnt" : ve.done() ? "patty" : "patty_raw", cx - 6, cy - 1, 18);
				}
				if (ve.full() && !ve.burnt()) {
					if (!ve.done() && !ve.warning()) bar(cv, x + 4, y - 9, T - 8, ve.cook / ve.doneAt(), GREEN);
					else if (ve.warning()) {
						bar(cv, x + 4, y - 9, T - 8, (ve.cook - ve.doneAt()) / Vessel::BURNS_AFTER, RED);
						if (std::fmod(v.clock, 0.5f) < 0.3f) badge(cv, x + T - 4, y - 14, "!", RED);
					} else badge(cv, x + T - 4, y - 12, "", GREEN), text(cv, F11, x + T - 4, y - 12, "OK", WHITE, 1);
					for (int i = 0; i < 3; ++i) {       // steam (or sizzle)
						float ph = std::fmod(v.clock * 0.8f + i / 3.f, 1.f);
						cv.ellipse(cx - 8 + i * 8 + std::sin(ph * 6 + i) * 2, y + 2 - ph * 18, 4 + ph * 2, 4 + ph * 2, WHITE, 0.55f * (1 - ph));
					}
				}
				if (ve.burnt())
					for (int i = 0; i < 4; ++i) {
						float ph = std::fmod(v.clock * 0.6f + i / 4.f, 1.f);
						cv.ellipse(cx - 9 + i * 6, y - ph * 26, 5 + ph * 4, 5 + ph * 4, 0x3c3c3c, 0.6f * (1 - ph));
					}
				continue;
			}
			if (!c.thing.empty()) {
				drawThing(cv, c.thing, cx, cy, 30);
				if (c.kind == 'K' && c.thing.chop > 0) bar(cv, x + 4, y - 9, T - 8, c.thing.chop, GREEN);
			}
		}
	// the customers (the leaving ones over the others)
	for (int pass = 0; pass < 2; ++pass)
		for (auto& cu : v.customers) {
			if ((cu.state >= Customer::HAPPY) != (pass == 1)) continue;
			const Order* o = nullptr;
			for (auto& r : k.orders) if (r.id == cu.id) o = &r;
			drawCustomer(cv, cu, o, k.mode == TUTORIAL, v.clock);
		}
	// chefs, the farther ones first
	std::vector<int> order(k.chefs.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
	std::sort(order.begin(), order.end(), [&](int a, int b) { return k.chefs[a].y < k.chefs[b].y; });
	v.lastPos.resize(k.chefs.size(), { -1, -1 });
	for (int i : order) {
		const Chef& c = k.chefs[i];
		bool moving = std::fabs(v.lastPos[i].first - c.x) + std::fabs(v.lastPos[i].second - c.y) > 0.001f;
		drawChef(cv, c, i, i == v.active, v.markers && i == v.active && k.chefs.size() > 1, v.clock, moving);
		v.lastPos[i] = { c.x, c.y };
	}
	for (auto& f : v.floaters) {
		float p = f.age / f.life;
		text(cv, f.font, f.x + 1, f.y - p * 30 + 2, f.s, SHADOW, 1, 0.4f * (1 - p));
		text(cv, f.font, f.x, f.y - p * 30, f.s, f.c, 1, 1 - p * p);
	}
}

/* ------------------------------------------------------------------------------------------------ the orders */

static void stripes(Canvas& cv)
{
	cv.fill(BG);
	for (int i = -20; i < 40; ++i)
		cv.poly({ { (float)i * 40, 0 }, { (float)i * 40 + 20, 0 }, { (float)i * 40 + 20 - 480, (float)H }, { (float)i * 40 - 480, (float)H } }, BG_STRIPE);
}

static void keycap(Canvas& cv, float& x, float y, const char* key, const char* what)
{
	float kw = std::max(26.f, textW(K13, key) + 14);
	cv.rrect(x, y - 13, x + kw, y + 13, 13, WHITE);
	text(cv, K13, x + kw / 2, y, key, INK, 1);
	float ww = text(cv, N15, x + kw + 7, y, what, LIGHT);
	x += kw + 7 + ww + 16;
}

static void drawTicket(Canvas& cv, const Order& o, float x, float y, float cw, float clock)
{
	float p = o.left / o.total;
	float shake = p < 0.25f ? std::sin(clock * 40) * 1.2f : 0;
	x += shake;
	cv.rrect(x, y + 6, x + cw, y + 256, 14, 0x000000, 0.45f, 6);
	cv.rrect(x, y, x + cw, y + 250, 14, CARD);
	uint32_t c = p > 0.5f ? GREEN : p > 0.25f ? ORANGE : RED;
	cv.rrect(x + 10, y + 10, x + cw - 10, y + 20, 5, CARD_LINE);
	cv.rrect(x + 10, y + 10, x + 10 + (cw - 20) * p, y + 20, 5, c);
	cv.ellipse(x + cw / 2, y + 80, 50, 50, 0xffecc8);
	sprite(cv, dishSprite(o.r), x + cw / 2, y + 80, 64);
	std::string name = recipeName(o.r);
	for (auto& ch : name) ch = (char)toupper(ch);
	text(cv, N15, x + cw / 2, y + 146, name, INK, 1);
	cv.rect(x + 14, y + 159, x + cw - 14, y + 161, CARD_LINE);
	struct Ing { Bit raw; bool chop; const char* cook; };
	static const Ing salad[] = { { { TOMATO, RAW }, true, nullptr }, { { CABBAGE, RAW }, true, nullptr } };
	static const Ing soup[] = { { { TOMATO, RAW }, true, "pot" }, { { TOMATO, RAW }, true, "pot" }, { { TOMATO, RAW }, true, "pot" } };
	static const Ing burger[] = { { { BREAD, RAW }, false, nullptr }, { { MEAT, RAW }, true, "pan" }, { { CHEESE, RAW }, true, nullptr } };
	const Ing* ings = o.r == SALAD ? salad : o.r == TOMATO_SOUP ? soup : burger;
	int ni = o.r == SALAD ? 2 : 3;
	float iw = (cw - 20) / ni;
	for (int j = 0; j < ni; ++j) {
		float ix = x + 10 + iw * j + iw / 2;
		cv.rrect(ix - 19, y + 170, ix + 19, y + 208, 10, 0xf6ead6);
		sprite(cv, foodSprite(ings[j].raw), ix, y + 189, 28);
		int n = (ings[j].chop ? 1 : 0) + (ings[j].cook ? 1 : 0);
		float sx = ix - (n - 1) * 8;
		if (ings[j].chop) { sprite(cv, "knife", sx, y + 224, 15); sx += 16; }
		if (ings[j].cook) sprite(cv, ings[j].cook, sx, y + 224, 16);
	}
	if (p < 0.25f) {
		cv.rrect(x + cw - 40, y - 10, x + cw + 6, y + 14, 12, RED);
		text(cv, F11, x + cw - 17, y + 2, "HURRY", WHITE, 1);
	}
}

// a sentence on as many lines as it needs (at spaces), each `lineH` below the last; the lines' count
static int wrapText(Canvas& cv, int f, float x, float y, float maxW, const std::string& s, uint32_t c, float lineH, int align = 0)
{
	std::vector<std::string> lines;
	std::string cur;
	size_t i = 0;
	while (i <= s.size()) {
		size_t sp = s.find(' ', i);
		if (sp == std::string::npos) sp = s.size();
		std::string word = s.substr(i, sp - i);
		std::string tryLine = cur.empty() ? word : cur + " " + word;
		if (!cur.empty() && textW(f, tryLine) > maxW) { lines.push_back(cur); cur = word; }
		else cur = tryLine;
		i = sp + 1;
	}
	if (!cur.empty()) lines.push_back(cur);
	for (size_t l = 0; l < lines.size(); ++l) text(cv, f, x, y + l * lineH, lines[l], c, align);
	return (int)lines.size();
}

// the tutorial's lesson, over the bottom screen's top
static void drawLesson(Canvas& cv, const Tutorial& t, float clock)
{
	const Lesson& l = LESSONS[std::min(t.step, NLESSONS - 1)];
	cv.rrect(16, 18, 624, 166, 16, 0, 0.4f, 6);
	cv.rrect(16, 12, 624, 160, 16, CARD);
	char buf[48];
	snprintf(buf, sizeof buf, "LESSON %d OF %d", t.step + 1, NLESSONS);
	text(cv, F11, 36, 32, buf, 0x9a8a78);
	for (int i = 0; i < NLESSONS; ++i)
		cv.ellipse(604 - (NLESSONS - 1 - i) * 11, 32, 3.5f, 3.5f, i < t.step ? GREEN : i == t.step ? ORANGE : CARD_LINE);
	text(cv, F24, 36, 62, l.title, INK);
	wrapText(cv, N15, 36, 100, 568, l.text, INK, 22);
	// the buttons it's about, right of the title
	std::string keys = l.keys;
	float x = 604;
	{
		size_t a2 = 0;
		while (a2 < keys.size()) {
			size_t sp = keys.find(' ', a2);
			if (sp == std::string::npos) sp = keys.size();
			x -= std::max(28.f, textW(K13, keys.substr(a2, sp - a2)) + 16) + 10;
			a2 = sp + 1;
		}
		x += 10;
	}
	size_t at = 0;
	while (at < keys.size()) {
		size_t sp = keys.find(' ', at);
		if (sp == std::string::npos) sp = keys.size();
		std::string k = keys.substr(at, sp - at);
		float kw = std::max(28.f, textW(K13, k) + 16);
		float pulse = 0.5f + 0.5f * std::sin(clock * 6);
		cv.rrect(x - 3, 47, x + kw + 3, 77, 15, ORANGE, pulse);
		cv.rrect(x, 50, x + kw, 74, 12, INK);
		text(cv, K13, x + kw / 2, 62, k, WHITE, 1);
		x += kw + 10;
		at = sp + 1;
	}
}

static void pill(Canvas& cv, float x0, float y0, float x1, float y1, const char* s, uint32_t bg, uint32_t fg)
{
	cv.rrect(x0, y0 + 3, x1, y1 + 3, (y1 - y0) / 2, 0, 0.35f, 3);
	cv.rrect(x0, y0, x1, y1, (y1 - y0) / 2, bg);
	text(cv, F13, (x0 + x1) / 2, (y0 + y1) / 2, s, fg, 1);
}

// The bottom screen's buttons while cooking (touch)
static const float PAUSE_X0 = 510, PAUSE_X1 = 616, SKIP_X0 = 24, SKIP_X1 = 220, TUTOR_Y0 = 436, TUTOR_Y1 = 470;

static void drawOrders(Canvas& cv, const Canvas& bg, const Kitchen& k, const View& v, const Tutorial& tutor, float missFlash)
{
	cv.copy(bg);
	char buf[32];
	int nChefs = (int)k.chefs.size();
	float ticketsY = 92;
	if (k.mode == TUTORIAL) {
		drawLesson(cv, tutor, v.clock);
		ticketsY = 176;
	} else if (k.mode == RUSH) {
		bool low = k.time <= 10 && k.time > 0;
		uint32_t tb = low && std::fmod(v.clock, 1.f) < 0.5f ? RED : YELLOW;
		cv.rrect(16, 14, 200, 70, 16, tb);
		text(cv, F11, 34, 27, "TIME", low ? WHITE : 0x785014);
		int secs = (int)std::ceil(k.time);
		snprintf(buf, sizeof buf, "%d:%02d", secs / 60, secs % 60);
		text(cv, F24, 34, 52, buf, low && tb == RED ? WHITE : INK);
		for (int i = 0; i < 3; ++i) {
			float sx = 268 + i * 52;
			bool got = k.tips >= k.stars[i];
			cv.star(sx, 36, 17, got ? YELLOW : 0x5a4c64);
			snprintf(buf, sizeof buf, "%d", k.stars[i]);
			text(cv, F11, sx, 64, buf, 0xe6dcf0, 1);
		}
	} else {
		// CALM: the hearts left, and the customers served
		bool hurt = missFlash > 0;
		cv.rrect(16, 14, 200, 70, 16, hurt ? RED : YELLOW);
		text(cv, F11, 34, 27, "HAPPY CUSTOMERS", hurt ? WHITE : 0x785014);
		for (int i = 0; i < Kitchen::MAX_HEARTS; ++i)
			heart(cv, 46 + i * 34, 51, 12, i < k.hearts ? (hurt ? WHITE : RED) : (hurt ? 0xffb0a8 : 0xd8b040), i < k.hearts ? 1 : 0.6f);
		text(cv, F11, 320, 27, "SERVED", MUTED, 1);
		snprintf(buf, sizeof buf, "%d", k.served);
		text(cv, F32, 320, 54, buf, WHITE, 1);
	}
	if (k.mode != TUTORIAL) {
		bool tipFlash = missFlash > 0 && k.mode == RUSH;
		cv.rrect(440, 14, 624, 70, 16, tipFlash ? RED : YELLOW);
		text(cv, F11, 458, 27, "TIPS", tipFlash ? WHITE : 0x785014);
		sprite(cv, "cookie", 476, 51, 24);
		snprintf(buf, sizeof buf, "%d", k.tips);
		text(cv, F24, 496, 52, buf, tipFlash ? WHITE : INK);
		if (tipFlash) text(cv, F16, 600, 52, "-10", WHITE, 2, std::min(1.f, missFlash * 2));
	}
	int n = (int)k.orders.size();
	float cw = 142, gap = 10, x0 = (W - (n * cw + (n - 1) * gap)) / 2;
	for (int i = 0; i < n; ++i) {
		const Order& o = k.orders[i];
		float slide = o.age < 0.35f ? (1 - o.age / 0.35f) * 300 : 0;
		Order shown = o;
		if (k.mode == TUTORIAL) shown.left = shown.total;  // (they wait)
		drawTicket(cv, shown, x0 + i * (cw + gap) + slide, ticketsY, cw, v.clock);
	}
	if (n == 0) text(cv, N15, W / 2, ticketsY + 118, k.mode == TUTORIAL ? "NO CUSTOMER YET" : "NO ORDERS RIGHT NOW", MUTED, 1);
	if (k.mode == TUTORIAL) {
		pill(cv, SKIP_X0, TUTOR_Y0, SKIP_X1, TUTOR_Y1, "SKIP LESSON", PANEL, LIGHT);
		pill(cv, PAUSE_X0 - 40, TUTOR_Y0, PAUSE_X1, TUTOR_Y1, "PAUSE", PANEL, LIGHT);
		text(cv, F11, 345, 453, tutor.step >= 17 ? "Y: SWITCH CHEF" : "SELECT: SKIP", MUTED, 1);
		return;
	}
	cv.rrect(16, 362, 624, 410, 16, PANEL);
	for (int i = 0; i < nChefs; ++i) {
		float cx = 40 + i * 150;
		if (i == v.active) cv.rrect(cx - 18, 370, cx + 116, 402, 16, 0x56486a);
		cv.ellipse(cx, 386, 10, 10, CHEF_COL[i % 4]);
		snprintf(buf, sizeof buf, "CHEF %d", i + 1);
		text(cv, F13, cx + 18, 386, buf, LIGHT);
	}
	pill(cv, PAUSE_X0, 370, PAUSE_X1, 402, "PAUSE", 0x56486a, LIGHT);
	float x = 20;
	keycap(cv, x, 441, "A", "Take / put");
	keycap(cv, x, 441, "X", "Chop (hold)");
	keycap(cv, x, 441, "B", "Dash");
	if (nChefs > 1) keycap(cv, x, 441, "Y", "Switch chef");
	keycap(cv, x, 441, "START", "Pause");
}

/* ------------------------------------------------------------------------------------------------ the game */

enum class Btn { UP, DOWN, LEFT, RIGHT, A, B, X, Y, L, R, SELECT, START, NONE };

struct Button { float x0, y0, x1, y1; std::string label; };

struct Game {
	enum Scene { TITLE, HOWTO, PLAY, PAUSE, RESULTS, OPTIONS } scene = TITLE;
	Scene optionsFrom = TITLE;
	Mode mode = CALM;
	Kitchen k{ 2 };
	Kitchen demo{ 2, 1, TUTORIAL };   // the title's kitchen: customers come and go
	float demoT = 0;
	Tutorial tutor;
	View view;
	Canvas base, bottomBg, top, bottom;
	int sel = 0;
	float sceneT = 0, missFlash = 0;
	bool quit = false, redrawBase = true, newBest = false;
	int bestTips = 0, bestStars = 0, bestServed = 0;
	bool musicOn = true, soundsOn = true, hasSave = false;
	std::string dataDir;
	bool held[12] = {};
	Buttons pressed;
	std::function<void(const char*)> sound = [](const char*) {};
	std::function<void()> optionsChanged = [] {};
	uint32_t seed = 0;

	Game()
	{
		stripes(bottomBg);
		drawKitchenBase(base, k);
	}
	void play(const char* s) { if (soundsOn) sound(s); }

	// --- what's kept in dataDir: the best scores, the options, a Play game left to go on with
	std::string file(const char* n) const { return dataDir + "/" + n; }
	void loadBest()
	{
		if (dataDir.empty()) return;
		if (FILE* f = fopen(file("best.txt").c_str(), "r")) {
			char line[96];
			bool first = true;
			while (fgets(line, sizeof line, f)) {
				int a, b;
				if (first && sscanf(line, "%d %d", &a, &b) == 2) { bestTips = a; bestStars = b; }   // (the first version's)
				else if (sscanf(line, "rush %d %d", &a, &b) == 2) { bestTips = a; bestStars = b; }
				else if (sscanf(line, "play %d", &a) == 1) bestServed = a;
				first = false;
			}
			fclose(f);
		}
		if (FILE* f = fopen(file("options.txt").c_str(), "r")) {
			char line[64];
			int v;
			while (fgets(line, sizeof line, f)) {
				if (sscanf(line, "music %d", &v) == 1) musicOn = v;
				else if (sscanf(line, "sounds %d", &v) == 1) soundsOn = v;
			}
			fclose(f);
		}
		FILE* f = fopen(file("save.txt").c_str(), "r");
		hasSave = f != nullptr;
		if (f) fclose(f);
	}
	void saveBest()
	{
		if (dataDir.empty()) return;
		if (FILE* f = fopen(file("best.txt").c_str(), "w")) { fprintf(f, "rush %d %d\nplay %d\n", bestTips, bestStars, bestServed); fclose(f); }
	}
	void saveOptions()
	{
		optionsChanged();
		if (dataDir.empty()) return;
		if (FILE* f = fopen(file("options.txt").c_str(), "w")) { fprintf(f, "music %d\nsounds %d\n", musicOn, soundsOn); fclose(f); }
	}
	// a Play game, kept (written beside, then put in place: a power cut leaves the old one or the new one)
	void saveGame()
	{
		if (mode != CALM || k.over || dataDir.empty()) return;
		std::string tmp = file("save.txt.new");
		if (FILE* f = fopen(tmp.c_str(), "w")) {
			std::string s = k.save();
			bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
			ok = fflush(f) == 0 && ok;
			fsync(fileno(f));
			fclose(f);
			if (ok && rename(tmp.c_str(), file("save.txt").c_str()) == 0) hasSave = true;
		}
	}
	void dropSave() { if (!dataDir.empty()) remove(file("save.txt").c_str()); hasSave = false; }
	bool loadGame()
	{
		FILE* f = fopen(file("save.txt").c_str(), "r");
		if (!f) return false;
		std::string s; char b[4096]; size_t n;
		while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
		fclose(f);
		return k.load(s);
	}

	void go(Scene s) { scene = s; sel = 0; sceneT = 0; }
	void fresh()
	{
		view.floaters.clear(); view.customers.clear(); view.active = 0; missFlash = 0; newBest = false;
		go(PLAY);
		play("start");
	}
	void startRound(Mode m)
	{
		mode = m;
		k = Kitchen(2, seed ? seed : (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count(), m);
		if (m == TUTORIAL) { tutor = Tutorial(); tutor.begin(k); }
		if (m == CALM) dropSave();
		fresh();
	}
	void continueGame()
	{
		if (!loadGame()) { dropSave(); startRound(CALM); return; }
		mode = CALM;
		fresh();
	}
	void leave()
	{
		if (mode == CALM) saveGame();
		go(TITLE);
	}

	std::vector<std::string> menuItems() const
	{
		if (scene == TITLE) {
			std::vector<std::string> v;
			if (hasSave) v.push_back("CONTINUE");
			for (auto s : { "PLAY", "RUSH: 3 MINUTES", "TUTORIAL", "HOW TO COOK", "OPTIONS", "QUIT" }) v.push_back(s);
			return v;
		}
		if (scene == PAUSE) {
			if (mode == TUTORIAL) return { "RESUME", "SKIP THIS LESSON", "OPTIONS", "LEAVE THE TUTORIAL" };
			if (mode == CALM) return { "RESUME", "OPTIONS", "SAVE AND LEAVE", "START AGAIN" };
			return { "RESUME", "START AGAIN", "OPTIONS", "LEAVE THE KITCHEN" };
		}
		if (scene == RESULTS) return mode == TUTORIAL ? std::vector<std::string>{ "PLAY", "MENU" } : std::vector<std::string>{ "PLAY AGAIN", "MENU" };
		if (scene == OPTIONS) return { musicOn ? "MUSIC: ON" : "MUSIC: OFF", soundsOn ? "SOUNDS: ON" : "SOUNDS: OFF", "BACK" };
		return {};
	}
	void choose(int i)
	{
		auto items = menuItems();
		if (i < 0 || i >= (int)items.size()) return;
		const std::string& c = items[i];
		if (c.rfind("SOUNDS", 0) != 0) play("ok");
		if (c == "CONTINUE") continueGame();
		else if (c == "PLAY") startRound(CALM);
		else if (c == "RUSH: 3 MINUTES") startRound(RUSH);
		else if (c == "TUTORIAL") startRound(TUTORIAL);
		else if (c == "HOW TO COOK") go(HOWTO);
		else if (c == "OPTIONS") { optionsFrom = scene; go(OPTIONS); }
		else if (c == "QUIT") quit = true;
		else if (c == "RESUME") scene = PLAY;
		else if (c == "START AGAIN" || c == "PLAY AGAIN") startRound(mode);
		else if (c == "SKIP THIS LESSON") { tutor.skip(k); scene = PLAY; if (tutor.finished) go(RESULTS); }
		else if (c == "SAVE AND LEAVE" || c == "LEAVE THE KITCHEN" || c == "LEAVE THE TUTORIAL" || c == "MENU") leave();
		else if (c.rfind("MUSIC", 0) == 0) { musicOn = !musicOn; saveOptions(); }
		else if (c.rfind("SOUNDS", 0) == 0) { soundsOn = !soundsOn; saveOptions(); play("ok"); }
		else if (c == "BACK") { scene = optionsFrom; sel = 0; }
	}
	void back()
	{
		play("back");
		if (scene == PAUSE) scene = PLAY;
		else if (scene == RESULTS || scene == HOWTO) go(TITLE);
		else if (scene == OPTIONS) { scene = optionsFrom; sel = 0; }
		else if (scene == TITLE) quit = true;
	}
	void key(Btn b, bool down)
	{
		if (b == Btn::NONE) return;
		held[(int)b] = down;
		if (!down) return;
		auto items = menuItems();
		if (scene == PLAY) {
			if (b == Btn::START) { scene = PAUSE; sel = 0; play("back"); return; }
			if (b == Btn::SELECT && mode == TUTORIAL) { skipLesson(); return; }
			if (b == Btn::Y && k.chefs.size() > 1) {
				if (mode == TUTORIAL && tutor.step < 17) { play("wrong"); return; }   // (one chef till the lesson on two)
				view.active = (view.active + 1) % (int)k.chefs.size(); play("switch");
				tutor.switched = true;
				return;
			}
			if (b == Btn::A) { pressed.a = true; if (mode == TUTORIAL && !tutor.finished && LESSONS[tutor.step].pressA) tutor.aPressed = true; }
			if (b == Btn::B) pressed.b = true;
			if (b == Btn::X) pressed.x = true;
			return;
		}
		if (scene == HOWTO) { if (b == Btn::B || b == Btn::A || b == Btn::START) back(); return; }
		if (items.empty()) return;
		if (b == Btn::UP) { sel = (sel + (int)items.size() - 1) % (int)items.size(); play("move"); }
		else if (b == Btn::DOWN) { sel = (sel + 1) % (int)items.size(); play("move"); }
		else if (b == Btn::A) choose(sel);
		else if (b == Btn::B) back();
		else if (b == Btn::START && scene == PAUSE) { scene = PLAY; play("back"); }
	}
	void skipLesson()
	{
		tutor.skip(k);
		play("chopped");
		if (tutor.finished) { play("end"); go(RESULTS); }
	}
	std::vector<Button> buttons() const
	{
		std::vector<Button> out;
		auto items = menuItems();
		float h = 44, step = 50;
		float y = scene == TITLE ? H - 14 - items.size() * step : scene == RESULTS ? 350 : scene == OPTIONS ? 150 : 130;
		for (size_t i = 0; i < items.size(); ++i) out.push_back({ 170, y + i * step, 470, y + i * step + h, items[i] });
		return out;
	}
	void touch(int x, int y)
	{
		if (scene == HOWTO) { back(); return; }
		if (scene == PLAY) {
			if (mode == TUTORIAL) {
				if (y >= TUTOR_Y0 - 6 && y < TUTOR_Y1 + 6 && x >= SKIP_X0 && x < SKIP_X1) skipLesson();
				else if (y >= TUTOR_Y0 - 6 && y < TUTOR_Y1 + 6 && x >= PAUSE_X0 - 40 && x < PAUSE_X1) { scene = PAUSE; sel = 0; play("back"); }
				return;
			}
			if (x >= PAUSE_X0 && x < PAUSE_X1 && y >= 362 && y < 410) { scene = PAUSE; sel = 0; play("back"); return; }
			// the chef chips switch chef
			for (size_t i = 0; i < k.chefs.size(); ++i)
				if (x >= 22 + (int)i * 150 && x < 156 + (int)i * 150 && y >= 362 && y < 410 && view.active != (int)i) { view.active = (int)i; play("switch"); }
			return;
		}
		auto bs = buttons();
		for (size_t i = 0; i < bs.size(); ++i)
			if (x >= bs[i].x0 && x < bs[i].x1 && y >= bs[i].y0 && y < bs[i].y1) { sel = (int)i; choose((int)i); return; }
	}

	void events()
	{
		for (auto& e : k.events) {
			float ex = e.x * T, ey = e.y * T - 10;
			char buf[16];
			switch (e.kind) {
			case Event::PICK: play("pick"); break;
			case Event::DROP: play("drop"); break;
			case Event::CHOP: play("chop"); break;
			case Event::CHOPPED: play("chopped"); break;
			case Event::DONE: play("done"); break;
			case Event::BURNT: play("burnt"); view.floaters.push_back({ ex, ey - 16, "BURNT!", RED }); break;
			case Event::SERVED:
				play("served");
				snprintf(buf, sizeof buf, "+%d", e.value);
				view.floaters.push_back({ ex - 30, ey, buf, GREEN, 1.4f, 0, F24 });
				break;
			case Event::WRONG: play("wrong"); view.floaters.push_back({ ex - 50, ey, "NOT ORDERED", RED, 1.2f, 0, F13 }); break;
			case Event::MISSED: play("missed"); missFlash = 1.2f; break;
			case Event::NEWORDER: if (sceneT > 1) play("neworder"); break;
			case Event::BIN: play("bin"); break;
			case Event::DASH: play("dash"); break;
			case Event::TICK: play("tick"); break;
			case Event::HEART: play("done"); view.floaters.push_back({ ex - 40, ey, "+1 HEART", RED, 1.4f, 0, F16 }); break;
			case Event::SIZZLE: break;
			}
		}
	}

	// the title's kitchen: a customer in every few seconds, served (or not) a while after
	void stepDemo(float dt)
	{
		demo.step(dt, {}, {});
		demoT += dt;
		if (demoT > 2.6f) {
			demoT = 0;
			if (demo.orders.size() < 3 || (demo.rand() % 3 == 0 && demo.orders.size() < 4)) demo.addOrder();
			else {
				Event e{ demo.rand() % 4 ? Event::SERVED : Event::MISSED, 0, 0, 0 };
				e.order = demo.orders.front().id;
				demo.orders.erase(demo.orders.begin());
				demo.events.push_back(e);
			}
		}
		view.updateCustomers(demo, dt);
	}

	// one 60th of a second
	void update(float dt)
	{
		view.clock += dt; sceneT += dt;
		missFlash = std::max(0.f, missFlash - dt);
		for (auto& f : view.floaters) f.age += dt;
		view.floaters.erase(std::remove_if(view.floaters.begin(), view.floaters.end(), [](const Floater& f) { return f.age >= f.life; }), view.floaters.end());
		bool inMenus = scene == TITLE || scene == HOWTO || (scene == OPTIONS && optionsFrom == TITLE);
		if (inMenus) { stepDemo(dt); pressed = Buttons(); return; }
		if (scene == RESULTS) { k.events.clear(); view.updateCustomers(k, dt); }
		if (scene != PLAY) { pressed = Buttons(); return; }
		if (sceneT < 1.6f) { pressed = Buttons(); k.events.clear(); view.updateCustomers(k, dt); return; }   // READY... GO
		std::vector<Buttons> h(k.chefs.size()), p(k.chefs.size());
		Buttons& mine = h[view.active];
		mine.up = held[(int)Btn::UP]; mine.down = held[(int)Btn::DOWN]; mine.left = held[(int)Btn::LEFT]; mine.right = held[(int)Btn::RIGHT];
		mine.x = held[(int)Btn::X];
		p[view.active] = pressed;
		pressed = Buttons();
		k.step(dt, h, p);
		events();
		view.updateCustomers(k, dt);
		if (mode == TUTORIAL && tutor.check(k, view.active)) {
			play(tutor.finished ? "end" : "start");
			if (tutor.finished) go(RESULTS);
			return;
		}
		if (k.over) {
			play("end");
			if (mode == RUSH) {
				int st = k.starsEarned();
				if (k.tips > bestTips) { bestTips = k.tips; newBest = true; }
				bestStars = std::max(bestStars, st);
			} else {
				if (k.served > bestServed) { bestServed = k.served; newBest = true; }
				dropSave();
			}
			saveBest();
			go(RESULTS);
		}
	}

	void drawMenuButtons(Canvas& cv)
	{
		auto bs = buttons();
		for (size_t i = 0; i < bs.size(); ++i) {
			auto& b = bs[i];
			bool on = (int)i == sel;
			cv.rrect(b.x0, b.y0 + 5, b.x1, b.y1 + 5, 20, 0, 0.4f, 4);
			cv.rrect(b.x0, b.y0, b.x1, b.y1, 20, on ? YELLOW : PANEL);
			if (on) cv.rring(b.x0 - 4, b.y0 - 4, b.x1 + 4, b.y1 + 4, 24, 3, WHITE);
			text(cv, F16, (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, b.label, on ? INK : LIGHT, 1);
		}
	}

	// the tutorial's squares to go to: lit up, an arrow over the nearest
	void drawLessonMarks(Canvas& cv)
	{
		if (tutor.finished) return;
		std::string where = LESSONS[tutor.step].where;
		if (where.empty()) return;
		float pulse = 0.55f + 0.45f * std::sin(view.clock * 6);
		const Chef& c = k.chefs[view.active];
		float bestD = 1e9, ax = 0, ay = 0;
		for (int gy = 2; gy < MH - 1; ++gy)
			for (int gx = 0; gx < MW; ++gx) {
				if (where.find(k.cells[gy][gx].kind) == std::string::npos) continue;
				cv.rring(gx * T - 3, gy * T - 3, gx * T + T + 3, gy * T + T - 1, 7, 4, ORANGE, pulse);
				cv.rring(gx * T + 1, gy * T + 1, gx * T + T - 1, gy * T + T - 5, 4, 2, WHITE, pulse * 0.8f);
				float d = std::hypot(gx + 0.5f - c.x, gy + 0.5f - c.y);
				if (d < bestD) { bestD = d; ax = gx * T + T / 2.f; ay = gy * T; }
			}
		if (bestD > 1.6f && bestD < 1e9) {
			float bob = std::sin(view.clock * 6) * 4;
			bool below = ay < 3 * T;   // (the top row: the arrow under it, pointing up)
			float dir = below ? -1 : 1, tip = below ? ay + T + 2 - bob : ay - 2 + bob, base = tip - dir * 22;
			cv.poly({ { ax - 15, base - dir * 2 }, { ax + 15, base - dir * 2 }, { ax, tip + dir * 3 } }, INK);
			cv.poly({ { ax - 11, base }, { ax + 11, base }, { ax, tip } }, ORANGE);
			cv.rect(ax - 5, base - dir * 12 - (below ? 0 : 0), ax + 5, base, ORANGE);
		}
	}

	void drawTop()
	{
		if (redrawBase) { drawKitchenBase(base, k); redrawBase = false; }
		bool inMenus = scene == TITLE || scene == HOWTO || (scene == OPTIONS && optionsFrom == TITLE);
		if (inMenus) {
			// the kitchen, its two chefs waiting, customers coming and going
			View iv = view; iv.markers = false; iv.active = -1; iv.floaters.clear();
			drawKitchen(top, base, demo, iv);
			if (scene == HOWTO) howtoLabels(top);
			return;
		}
		drawKitchen(top, base, k, view);
		if (mode == TUTORIAL && (scene == PLAY || scene == PAUSE)) drawLessonMarks(top);
		if (scene == PLAY && sceneT < 2.2f) {
			const char* s = sceneT < 1.6f ? "READY?" : "GO!";
			float a = sceneT < 1.6f ? std::min(1.f, sceneT * 3) : 1 - (sceneT - 1.6f) / 0.6f;
			top.rrect(190, 200, 450, 280, 24, 0, 0.45f * a);
			text(top, F48, 320, 240, s, sceneT < 1.6f ? WHITE : YELLOW, 1, a);
		}
		if (scene == PAUSE || scene == OPTIONS) {
			top.rect(0, 0, W, H, 0x000000, 0.5f);
			text(top, F48, 320, 240, "PAUSED", WHITE, 1);
		}
		if (scene == RESULTS) {
			top.rect(0, 0, W, H, 0x000000, 0.45f);
			text(top, F48, 320, 230, mode == TUTORIAL ? "WELL DONE!" : mode == RUSH ? "TIME'S UP!" : "CLOSED!", YELLOW, 1);
		}
	}

	void howtoLabels(Canvas& cv)
	{
		struct L { float x, y; const char* s; };
		static const L ls[] = {
			{ 80, 128, "TOMATOES" }, { 160, 128, "LETTUCE" }, { 240, 128, "MEAT" }, { 320, 128, "CHEESE" }, { 400, 128, "BREAD" },
			{ 520, 128, "PLATES" }, { 560, 160, "SERVE" }, { 72, 230, "CHOP" }, { 320, 196, "CHOP" },
			{ 60, 394, "POTS" }, { 200, 394, "PANS" }, { 360, 394, "POTS" }, { 500, 394, "BIN" },
		};
		cv.rect(0, 80, W, H, 0x000000, 0.25f);
		for (auto& l : ls) {
			float wdt = textW(F11, l.s) + 14;
			cv.rrect(l.x - wdt / 2, l.y - 10, l.x + wdt / 2, l.y + 10, 10, INK, 0.92f);
			text(cv, F11, l.x, l.y, l.s, WHITE, 1);
		}
	}

	void drawBottom()
	{
		char buf[96];
		if (scene == PLAY) { drawOrders(bottom, bottomBg, k, view, tutor, missFlash); return; }
		bottom.copy(bottomBg);
		if (scene == TITLE) {
			text(bottom, F32, 320, 34, "BISTRO", YELLOW, 1);
			int y = 70;
			if (bestServed || bestTips) {
				snprintf(buf, sizeof buf, "BEST: %d SERVED IN PLAY  -  %d TIPS IN RUSH", bestServed, bestTips);
				text(bottom, F11, 320, y, buf, MUTED, 1);
			} else text(bottom, N15, 320, y, "New here? Start with the tutorial.", MUTED, 1);
			drawMenuButtons(bottom);
		} else if (scene == HOWTO) {
			text(bottom, F24, 320, 36, "HOW TO COOK", YELLOW, 1);
			const char* rows[3][2] = {
				{ "salad", "Chop a tomato and some lettuce, put both on a plate." },
				{ "soup", "Chop three tomatoes, put them in a pot. When it's cooked, scoop it with a plate." },
				{ "burger", "Chop the meat and fry it. Chop the cheese. Bread, meat and cheese on a plate." },
			};
			for (int i = 0; i < 3; ++i) {
				float y = 76 + i * 78;
				bottom.rrect(16, y, 624, y + 68, 14, CARD);
				sprite(bottom, rows[i][0], 56, y + 34, 52);
				std::string s = rows[i][1];
				bool two = textW(N15, s) > 500;
				wrapText(bottom, N15, 100, two ? y + 24 : y + 34, 500, s, INK, 22);
			}
			text(bottom, N15, 320, 330, "Take the plate to the yellow counter to serve it. Hurry: each order has a time.", MUTED, 1);
			text(bottom, N15, 320, 352, "Don't leave a pan or pot too long: it burns! Throw burnt food in the bin.", MUTED, 1);
			float x = 20;
			keycap(bottom, x, 400, "A", "Take / put");
			keycap(bottom, x, 400, "X", "Chop (hold)");
			keycap(bottom, x, 400, "B", "Dash");
			keycap(bottom, x, 400, "Y", "Switch chef");
			text(bottom, F11, 320, 450, "PRESS B TO GO BACK", MUTED, 1);
		} else if (scene == PAUSE) {
			text(bottom, F24, 320, 60, "PAUSED", YELLOW, 1);
			if (mode == CALM) text(bottom, N15, 320, 96, "Leave whenever you like: your kitchen is kept for next time.", MUTED, 1);
			drawMenuButtons(bottom);
		} else if (scene == OPTIONS) {
			text(bottom, F24, 320, 60, "OPTIONS", YELLOW, 1);
			text(bottom, N15, 320, 100, "Kept for next time.", MUTED, 1);
			drawMenuButtons(bottom);
		} else if (scene == RESULTS) {
			if (mode == TUTORIAL) {
				sprite(bottom, "burger", 200, 90, 64); sprite(bottom, "soup", 320, 84, 64); sprite(bottom, "salad", 440, 90, 64);
				text(bottom, F32, 320, 170, "YOU'RE READY!", WHITE, 1);
				text(bottom, N15, 320, 214, "Play: no clock, keep your customers happy.", LIGHT, 1);
				text(bottom, N15, 320, 238, "Each one who leaves without their dish takes a heart.", LIGHT, 1);
				text(bottom, N15, 320, 270, "Rush: three minutes, as many tips as you can.", MUTED, 1);
			} else if (mode == RUSH) {
				int st = k.starsEarned();
				for (int i = 0; i < 3; ++i) {
					float appear = std::clamp((sceneT - 0.4f - i * 0.45f) * 4, 0.f, 1.f);
					float r = 34 * (i < st ? 0.6f + 0.4f * appear + (appear < 1 ? 0.25f * std::sin(appear * 3.14f) : 0) : 1);
					bottom.star(220 + i * 100, 70, r, i < st && appear > 0 ? YELLOW : 0x5a4c64);
				}
				snprintf(buf, sizeof buf, "%d TIPS", k.tips);
				text(bottom, F32, 320, 150, buf, WHITE, 1);
				snprintf(buf, sizeof buf, "SERVED %d    MISSED %d", k.served, k.missed);
				text(bottom, F13, 320, 190, buf, MUTED, 1);
				if (newBest) { bottom.rrect(250, 220, 390, 248, 14, GREEN); text(bottom, F13, 320, 234, "NEW BEST!", WHITE, 1); }
				else { snprintf(buf, sizeof buf, "BEST: %d", bestTips); text(bottom, F13, 320, 234, buf, MUTED, 1); }
				text(bottom, N15, 320, 290, st == 3 ? "A perfect kitchen!" : st == 2 ? "Great cooking!" : st == 1 ? "Good start! Try for the next star." : "Keep at it: serve the oldest order first.", LIGHT, 1);
			} else {
				for (int i = 0; i < Kitchen::MAX_HEARTS; ++i) heart(bottom, 280 + i * 40, 60, 15, 0x5a4c64);
				snprintf(buf, sizeof buf, "%d SERVED", k.served);
				text(bottom, F32, 320, 140, buf, WHITE, 1);
				int mins = (int)(k.played / 60);
				snprintf(buf, sizeof buf, "%d TIPS    %d MINUTE%s OPEN", k.tips, mins, mins == 1 ? "" : "S");
				text(bottom, F13, 320, 180, buf, MUTED, 1);
				if (newBest) { bottom.rrect(250, 210, 390, 238, 14, GREEN); text(bottom, F13, 320, 224, "NEW BEST!", WHITE, 1); }
				else { snprintf(buf, sizeof buf, "BEST: %d SERVED", bestServed); text(bottom, F13, 320, 224, buf, MUTED, 1); }
				text(bottom, N15, 320, 290, k.served >= 30 ? "What a bistro! The whole town eats here." : k.served >= 12 ? "Lots of happy customers!" : "Tip: start the soup first, it cooks while you chop.", LIGHT, 1);
			}
			drawMenuButtons(bottom);
		}
	}
};

/* ------------------------------------------------------------------------------------------------ PNG (--smoke) */

static uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0)
{
	static uint32_t t[256];
	if (!t[1]) for (uint32_t i = 0; i < 256; ++i) { uint32_t x = i; for (int k = 0; k < 8; ++k) x = x & 1 ? 0xedb88320 ^ (x >> 1) : x >> 1; t[i] = x; }
	c = ~c;
	for (size_t i = 0; i < n; ++i) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
	return ~c;
}
static bool writePng(const std::string& path, const uint32_t* px, int w, int h)
{
	std::vector<uint8_t> raw;
	for (int y = 0; y < h; ++y) {
		raw.push_back(0);
		for (int x = 0; x < w; ++x) { uint32_t p = px[y * w + x]; raw.push_back(p >> 16); raw.push_back(p >> 8); raw.push_back(p); }
	}
	std::vector<uint8_t> z = { 0x78, 0x01 };
	uint32_t a = 1, b = 0;
	for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
	for (size_t i = 0; i < raw.size(); i += 65535) {
		size_t n = std::min<size_t>(65535, raw.size() - i);
		z.push_back(i + n == raw.size()); z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
		z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
	}
	uint32_t ad = b << 16 | a;
	for (int i = 3; i >= 0; --i) z.push_back(ad >> (i * 8));
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) return false;
	auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
		uint8_t len[4] = { (uint8_t)(d.size() >> 24), (uint8_t)(d.size() >> 16), (uint8_t)(d.size() >> 8), (uint8_t)d.size() };
		fwrite(len, 1, 4, f);
		std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), d.begin(), d.end());
		fwrite(td.data(), 1, td.size(), f);
		uint32_t c = crc32(td.data(), td.size());
		uint8_t cb[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
		fwrite(cb, 1, 4, f);
	};
	static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	std::vector<uint8_t> ihdr = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
	chunk("IHDR", ihdr); chunk("IDAT", z); chunk("IEND", {});
	fclose(f);
	return true;
}
// both screens, one above the other as on the handheld
static void writeShot(const std::string& path, Game& g)
{
	g.drawTop(); g.drawBottom();
	int pad = 16, gap = 24, w = W + 2 * pad, h = 2 * H + gap + 2 * pad;
	std::vector<uint32_t> px((size_t)w * h, 0xff18161c);
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			px[(size_t)(pad + y) * w + pad + x] = g.top.px[y * W + x];
			px[(size_t)(pad + H + gap + y) * w + pad + x] = g.bottom.px[y * W + x];
		}
	writePng(path, px.data(), w, h);
	printf("%s\n", path.c_str());
}

// Each screen, drawn to PNGs: the title, how to cook, a moment of each way to play (made, not played), pause,
// options, the results
static void settle(Game& g, float secs) { for (float t = 0; t < secs; t += 1 / 60.f) g.update(1 / 60.f); }
static void busyKitchen(Kitchen& k)
{
	auto order = [&](Recipe r, float left, float total) { Order o; o.r = r; o.left = left; o.total = total; o.id = ++k.orderIds; k.orders.push_back(o); };
	k.orders.clear();
	order(BURGER, 16, 80); order(TOMATO_SOUP, 40, 70); order(SALAD, 50, 55); order(SALAD, 54, 55);
	k.chefs[0].x = 6.3f; k.chefs[0].y = 3.6f; k.chefs[0].fx = 1; k.chefs[0].fy = 0; k.chefs[0].hand = Thing::ofFood(TOMATO);
	k.chefs[1].x = 9.5f; k.chefs[1].y = 9.4f; k.chefs[1].fx = 0; k.chefs[1].fy = 1; k.chefs[1].hand = Thing::plate();
	k.cells[5][0].thing = Thing::ofFood(TOMATO, CUT);
	k.cells[5][7].thing = Thing::ofFood(CHEESE); k.cells[5][7].thing.chop = 0.6f;
	k.cells[6][6].thing = Thing::plate(); k.cells[6][6].thing.on = { { BREAD, RAW } };
	k.cells[2][14].thing = Thing::plate(); k.cells[2][14].thing.on = { { TOMATO, CUT }, { CABBAGE, CUT } };
	k.cells[10][1].vessel.n = 2;
	k.cells[10][8].vessel.n = 3; k.cells[10][8].vessel.cook = 6;
	k.cells[10][9].vessel.n = 3; k.cells[10][9].vessel.cook = 11;
	k.cells[10][4].vessel.n = 1; k.cells[10][4].vessel.cook = 12;
	k.cells[10][5].vessel.n = 1; k.cells[10][5].vessel.cook = 2;
}
static int smoke(const std::string& prefix)
{
	Game g;
	g.view.clock = 0.3f;
	g.bestTips = 96; g.bestStars = 2; g.bestServed = 23; g.hasSave = true;
	settle(g, 9);
	writeShot(prefix + "-1-title.png", g);
	g.go(Game::HOWTO); writeShot(prefix + "-2-howto.png", g);
	// Play: customers along the top, one leaving happy and one cross
	g.seed = 7; g.startRound(CALM);
	settle(g, 2.5f);
	Kitchen& k = g.k;
	busyKitchen(k);
	k.hearts = 2; k.served = 14; k.tips = 312;
	settle(g, 1 / 60.f); g.sceneT = 5;
	settle(g, 6);
	{
		Customer happy; happy.state = Customer::HAPPY; happy.x = 290; happy.t = 0.5f; happy.id = 9001; happy.look = 40;
		Customer cross; cross.state = Customer::MAD; cross.x = 585; cross.t = 1.2f; cross.id = 9002; cross.look = 77;
		g.view.customers.push_back(happy); g.view.customers.push_back(cross);
	}
	g.view.floaters.push_back({ 600, 150, "+27", GREEN, 1.4f, 0.4f, F24 });
	writeShot(prefix + "-3-play.png", g);
	g.scene = Game::PAUSE; g.sel = 0; writeShot(prefix + "-4-pause.png", g);
	g.optionsFrom = Game::PAUSE; g.go(Game::OPTIONS); writeShot(prefix + "-5-options.png", g);
	k.served = 31; k.played = 1260; k.over = true; g.newBest = true;
	g.go(Game::RESULTS); g.sceneT = 3; writeShot(prefix + "-6-results-play.png", g);
	// Rush
	g.startRound(RUSH); settle(g, 2.5f); busyKitchen(g.k); g.k.time = 151; g.k.tips = 64; settle(g, 3); g.sceneT = 5;
	writeShot(prefix + "-7-rush.png", g);
	g.k.tips = 112; g.k.served = 6; g.k.missed = 1; g.k.time = 0; g.newBest = true;
	g.go(Game::RESULTS); g.sceneT = 3; writeShot(prefix + "-8-results-rush.png", g);
	// the tutorial: its first lesson, then with a customer (lesson 10)
	g.startRound(TUTORIAL); settle(g, 2.5f);
	g.k.chefs[0].x = 3.5f; g.k.chefs[0].y = 6.5f;
	writeShot(prefix + "-9-tutorial-1.png", g);
	for (int i = 0; i < 9; ++i) g.tutor.skip(g.k);
	g.k.chefs[0].hand = Thing::plate(); g.k.chefs[0].hand.on = { { TOMATO, CUT }, { CABBAGE, CUT } };
	g.k.chefs[0].x = 11.5f; g.k.chefs[0].y = 7.5f; g.k.chefs[0].fx = 1; g.k.chefs[0].fy = 0;
	settle(g, 3);
	writeShot(prefix + "-10-tutorial-serve.png", g);
	g.tutor.finished = true; g.go(Game::RESULTS); g.sceneT = 3;
	writeShot(prefix + "-11-tutorial-done.png", g);
	return 0;
}

/* ------------------------------------------------------------------------------------------------ the handheld */
#ifdef BISTRO_DEVICE
#define DRM_IOCTL_BASE 'd'
#define DRM_IO(nr) _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
struct drm_mode_modeinfo { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew; uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct drm_mode_card_res { uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr; uint32_t count_fbs, count_crtcs, count_connectors, count_encoders; uint32_t min_width, max_width, min_height, max_height; };
struct drm_mode_get_connector { uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr; uint32_t count_modes, count_props, count_encoders, encoder_id, connector_id; uint32_t connector_type, connector_type_id, connection, mm_width, mm_height, subpixel, pad; };
struct drm_mode_get_encoder { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; };
struct drm_mode_crtc { uint64_t set_connectors_ptr; uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct drm_mode_modeinfo mode; };
struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb { uint32_t handle; };
struct drm_mode_fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct drm_mode_crtc_page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct drm_event { uint32_t type, length; };
struct drm_event_vblank { struct drm_event base; uint64_t user_data; uint32_t tv_sec, tv_usec, sequence, crtc_id; };
#define DRM_IOCTL_SET_MASTER DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER DRM_IO(0x1f)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_PAGE_FLIP DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_CREATE_DUMB DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

static int xioctl(int fd, unsigned long req, void* arg)
{
	int r;
	do r = ioctl(fd, req, arg); while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

// Both panels, two buffers each; the top one is the connector named (DSI-2 on the RG DS). Frames go by page flips,
// so each panel shows a whole frame at its refresh.
class Display {
public:
	struct Panel {
		uint32_t conn = 0, crtc = 0; drm_mode_modeinfo mode = {}; drm_mode_crtc saved = {}; bool hasSaved = false, on = false;
		int w = 640, h = 480, back = 0; bool pending = false, started = false;
		struct { uint32_t fb = 0, handle = 0, pitch = 0; uint64_t size = 0; uint32_t* map = nullptr; } b[2];
	} p[2];
	int fd = -1;

	bool alloc(Panel& q, int i)
	{
		drm_mode_create_dumb c = { (uint32_t)q.h, (uint32_t)q.w, 32, 0, 0, 0, 0 };
		if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) return false;
		q.b[i].handle = c.handle; q.b[i].pitch = c.pitch; q.b[i].size = c.size;
		drm_mode_fb_cmd f = { 0, (uint32_t)q.w, (uint32_t)q.h, c.pitch, 32, 24, c.handle };
		if (xioctl(fd, DRM_IOCTL_MODE_ADDFB, &f) < 0) return false;
		q.b[i].fb = f.fb_id;
		drm_mode_map_dumb m = { c.handle, 0, 0 };
		if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) return false;
		void* mp = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
		if (mp == MAP_FAILED) return false;
		q.b[i].map = (uint32_t*)mp;
		memset(mp, 0, c.size);
		return true;
	}
	bool open(const std::string& top)
	{
		fd = ::open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
		if (fd < 0) return false;
		xioctl(fd, DRM_IOCTL_SET_MASTER, nullptr);
		drm_mode_card_res r = {};
		if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0) return false;
		std::vector<uint32_t> conns(r.count_connectors), crtcs(r.count_crtcs), fbs(r.count_fbs), encs(r.count_encoders);
		r.connector_id_ptr = (uintptr_t)conns.data(); r.crtc_id_ptr = (uintptr_t)crtcs.data();
		r.fb_id_ptr = (uintptr_t)fbs.data(); r.encoder_id_ptr = (uintptr_t)encs.data();
		if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0) return false;
		uint32_t used = 0;
		for (uint32_t i = 0; i < r.count_connectors; ++i) {
			drm_mode_get_connector c = {}; c.connector_id = conns[i];
			if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || c.connection != 1 || !c.count_modes) continue;
			std::vector<drm_mode_modeinfo> modes(c.count_modes); std::vector<uint32_t> ce(c.count_encoders), props(c.count_props); std::vector<uint64_t> pv(c.count_props);
			c.modes_ptr = (uintptr_t)modes.data(); c.encoders_ptr = (uintptr_t)ce.data(); c.props_ptr = (uintptr_t)props.data(); c.prop_values_ptr = (uintptr_t)pv.data();
			if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) continue;
			char name[32];
			snprintf(name, sizeof name, "%s-%u", c.connector_type == 16 ? "DSI" : c.connector_type == 15 ? "Virtual" : c.connector_type == 14 ? "eDP" : "Other", c.connector_type_id);
			Panel* q = top == name && !p[0].on ? &p[0] : !p[1].on ? &p[1] : !p[0].on ? &p[0] : nullptr;
			if (!q) continue;
			q->conn = c.connector_id; q->mode = modes[0]; q->w = modes[0].hdisplay; q->h = modes[0].vdisplay;
			uint32_t crtc = 0;
			if (c.encoder_id) { drm_mode_get_encoder e = {}; e.encoder_id = c.encoder_id; if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0) crtc = e.crtc_id; }
			for (uint32_t e = 0; e < c.count_encoders && !crtc; ++e) {
				drm_mode_get_encoder en = {}; en.encoder_id = ce[e];
				if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &en) < 0) continue;
				for (uint32_t k = 0; k < r.count_crtcs; ++k)
					if ((en.possible_crtcs & (1u << k)) && !(used & (1u << k))) { crtc = crtcs[k]; used |= 1u << k; break; }
			}
			if (!crtc) continue;
			q->crtc = crtc; q->saved.crtc_id = crtc;
			if (xioctl(fd, DRM_IOCTL_MODE_GETCRTC, &q->saved) == 0) q->hasSaved = true;
			if (alloc(*q, 0) && alloc(*q, 1)) q->on = true;
		}
		if (!p[0].on && p[1].on) std::swap(p[0], p[1]);
		return p[0].on;
	}
	~Display()
	{
		if (fd < 0) return;
		waitFlips(200);
		for (auto& q : p) {
			if (!q.on) continue;
			if (q.hasSaved) { q.saved.set_connectors_ptr = (uintptr_t)&q.conn; q.saved.count_connectors = 1; xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &q.saved); }
			for (auto& b : q.b) {
				if (b.map) munmap(b.map, b.size);
				if (b.fb) xioctl(fd, DRM_IOCTL_MODE_RMFB, &b.fb);
				if (b.handle) { drm_mode_destroy_dumb d = { b.handle }; xioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
			}
		}
		xioctl(fd, DRM_IOCTL_DROP_MASTER, nullptr);
		close(fd);
	}
	bool ready(int i) const { return p[i].on && !p[i].pending; }
	// the canvas onto panel i's back buffer (scaled if the panel isn't 640x480), then shown
	void show(int i, const Canvas& cv)
	{
		Panel& q = p[i];
		if (!q.on || q.pending) return;
		uint32_t* dst = q.b[q.back].map; int stride = q.b[q.back].pitch / 4;
		if (q.w == cv.w && q.h == cv.h)
			for (int y = 0; y < q.h; ++y) memcpy(dst + (size_t)y * stride, &cv.px[(size_t)y * cv.w], (size_t)cv.w * 4);
		else
			for (int y = 0; y < q.h; ++y) {
				int sy = y * cv.h / q.h;
				for (int x = 0; x < q.w; ++x) dst[(size_t)y * stride + x] = cv.px[(size_t)sy * cv.w + x * cv.w / q.w];
			}
		if (!q.started) {
			drm_mode_crtc s = {}; s.crtc_id = q.crtc; s.fb_id = q.b[q.back].fb; s.set_connectors_ptr = (uintptr_t)&q.conn;
			s.count_connectors = 1; s.mode = q.mode; s.mode_valid = 1;
			xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &s);
			q.started = true;
		} else {
			drm_mode_crtc_page_flip f = { q.crtc, q.b[q.back].fb, DRM_MODE_PAGE_FLIP_EVENT, 0, (uint64_t)i };
			if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &f) == 0) q.pending = true;
			else { drm_mode_crtc s = {}; s.crtc_id = q.crtc; s.fb_id = q.b[q.back].fb; s.set_connectors_ptr = (uintptr_t)&q.conn; s.count_connectors = 1; s.mode = q.mode; s.mode_valid = 1; xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &s); }
		}
		q.back ^= 1;
	}
	// the flips that have happened
	void readEvents()
	{
		char buf[1024];
		ssize_t n = read(fd, buf, sizeof buf);
		for (ssize_t at = 0; n > 0 && at + (ssize_t)sizeof(drm_event) <= n;) {
			drm_event* e = (drm_event*)(buf + at);
			if (e->type == DRM_EVENT_FLIP_COMPLETE && e->length >= sizeof(drm_event_vblank)) {
				uint64_t i = ((drm_event_vblank*)e)->user_data;
				if (i < 2) p[i].pending = false;
			}
			if (!e->length) break;
			at += e->length;
		}
	}
	void waitFlips(int ms)
	{
		while ((p[0].pending || p[1].pending) && ms > 0) {
			pollfd pf = { fd, POLLIN, 0 };
			if (poll(&pf, 1, 20) > 0) readEvents();
			ms -= 20;
		}
	}
};

// The pad and the touchscreen (the bottom panel's), as the menu finds them (ui/src/input.cpp)
class Input {
	struct Dev { int fd = -1; bool touch = false; int slot = 0, tx[10] = {}, ty[10] = {}, tid[10]; int minx = 0, maxx = 0, miny = 0, maxy = 0; };
	std::vector<Dev> devs;
	static bool bit(const unsigned long* b, int n) { return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }
public:
	static Btn map(int code)
	{
		switch (code) {
		case BTN_DPAD_LEFT: case KEY_LEFT: return Btn::LEFT;
		case BTN_DPAD_RIGHT: case KEY_RIGHT: return Btn::RIGHT;
		case BTN_DPAD_UP: case KEY_UP: return Btn::UP;
		case BTN_DPAD_DOWN: case KEY_DOWN: return Btn::DOWN;
		case BTN_EAST: case KEY_ENTER: return Btn::A;        // the handhelds' layout: A east, B south, X north, Y west
		case BTN_SOUTH: case KEY_BACKSPACE: case KEY_ESC: return Btn::B;
		case BTN_NORTH: case KEY_X: return Btn::X;
		case BTN_WEST: case KEY_Y: return Btn::Y;
		case BTN_TL: case BTN_TL2: return Btn::L;
		case BTN_TR: case BTN_TR2: return Btn::R;
		case BTN_START: return Btn::START;
		case BTN_SELECT: return Btn::SELECT;
		default: return Btn::NONE;
		}
	}
	~Input() { for (auto& d : devs) close(d.fd); }
	void open()
	{
		DIR* dir = opendir("/dev/input");
		if (!dir) return;
		while (dirent* e = readdir(dir)) {
			if (strncmp(e->d_name, "event", 5)) continue;
			int fd = ::open((std::string("/dev/input/") + e->d_name).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0) continue;
			unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {}, abs[ABS_MAX / (8 * sizeof(long)) + 1] = {};
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
			Dev d; d.fd = fd; std::fill(d.tid, d.tid + 10, -1);
			bool mt = bit(abs, ABS_MT_POSITION_X) && bit(abs, ABS_MT_POSITION_Y);
			bool pad = bit(abs, ABS_HAT0X);
			for (int k = KEY_ESC; k < KEY_MAX && !pad; ++k) if (bit(keys, k) && map(k) != Btn::NONE) pad = true;
			if (mt) {
				d.touch = true;
				input_absinfo ax = {}, ay = {};
				ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax); ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
				d.minx = ax.minimum; d.maxx = ax.maximum; d.miny = ay.minimum; d.maxy = ay.maximum;
			} else if (!pad) { close(fd); continue; }
			devs.push_back(d);
		}
		closedir(dir);
	}
	std::vector<int> fds() const { std::vector<int> v; for (auto& d : devs) v.push_back(d.fd); return v; }
	void read(int fd, Game& g)
	{
		for (auto& d : devs) {
			if (d.fd != fd) continue;
			input_event ev;
			while (::read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
				if (d.touch) {
					if (ev.type != EV_ABS) continue;
					if (ev.code == ABS_MT_SLOT && ev.value >= 0 && ev.value < 10) d.slot = ev.value;
					else if (ev.code == ABS_MT_POSITION_X) d.tx[d.slot] = ev.value;
					else if (ev.code == ABS_MT_POSITION_Y) d.ty[d.slot] = ev.value;
					else if (ev.code == ABS_MT_TRACKING_ID) {
						if (ev.value >= 0) d.tid[d.slot] = ev.value;
						else if (d.tid[d.slot] >= 0) {
							d.tid[d.slot] = -1;
							int x = d.maxx > d.minx ? (d.tx[d.slot] - d.minx) * W / (d.maxx - d.minx + 1) : d.tx[d.slot];
							int y = d.maxy > d.miny ? (d.ty[d.slot] - d.miny) * H / (d.maxy - d.miny + 1) : d.ty[d.slot];
							g.touch(x, y);
						}
					}
				} else if (ev.type == EV_KEY && ev.value != 2) {
					g.key(map(ev.code), ev.value != 0);
				} else if (ev.type == EV_ABS && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)) {
					if (ev.code == ABS_HAT0X) { g.key(Btn::LEFT, ev.value < 0); g.key(Btn::RIGHT, ev.value > 0); }
					else { g.key(Btn::UP, ev.value < 0); g.key(Btn::DOWN, ev.value > 0); }
				}
			}
		}
	}
};

// The sounds and the music, mixed in a thread of their own. ALSA's "default", 44.1 kHz stereo (each 22.05 kHz
// sample twice). What happens to the device goes in the log (bistro.log): a game with no sound says why there.
class Audio {
	struct Voice { const bistro_sounds::Clip* c; int at; };
	std::vector<Voice> voices;
	std::mutex m;
	std::thread th;
	std::atomic<bool> run{ true }, musicOn{ true };
	std::atomic<float> musicVol{ 0.42f };
	std::unordered_map<std::string, const bistro_sounds::Clip*> clips;
	// the music: IMA ADPCM, decoded as it plays, from the start again at its end
	int mAt = 0, mPred = 0, mIdx = 0;
	float mGain = 0;
	int musicSample()
	{
		static const int IDX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
		int code = (bistro_sounds::music[mAt >> 1] >> ((mAt & 1) * 4)) & 15;
		int step = bistro_sounds::ADPCM_STEPS[mIdx], d = step >> 3;
		if (code & 4) d += step;
		if (code & 2) d += step >> 1;
		if (code & 1) d += step >> 2;
		mPred = std::clamp(code & 8 ? mPred - d : mPred + d, -32768, 32767);
		mIdx = std::clamp(mIdx + IDX[code & 7], 0, 88);
		if (++mAt >= bistro_sounds::MUSIC_SAMPLES) mAt = mPred = mIdx = 0;
		return mPred;
	}
public:
	Audio()
	{
		for (auto& c : bistro_sounds::clips) clips[c.name] = &c;
#ifndef BISTRO_NO_ALSA
		th = std::thread([this] {
			snd_pcm_t* pcm = nullptr;
			// (the menu lets go of the device once its launch chime has played: a few tries, half a second apart)
			int err = 0;
			for (int i = 0; run && i < 20; ++i) {
				err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
				if (err >= 0) break;
				if (i == 0) fprintf(stderr, "bistro: sound: the device isn't free yet (%s), trying again\n", snd_strerror(err));
				pcm = nullptr;
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
			if (!pcm) { fprintf(stderr, "bistro: sound: no device (%s): playing without sound\n", snd_strerror(err)); return; }
			snd_pcm_nonblock(pcm, 0);
			err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, bistro_sounds::RATE * 2, 1, 50000);
			if (err < 0) { fprintf(stderr, "bistro: sound: the device refused 44.1 kHz stereo (%s)\n", snd_strerror(err)); snd_pcm_close(pcm); return; }
			fprintf(stderr, "bistro: sound: playing (%s)\n", snd_pcm_name(pcm));
			std::vector<int16_t> buf(256 * 2);
			int recovered = 0;
			while (run) {
				{
					std::lock_guard<std::mutex> l(m);
					float want = musicOn ? musicVol.load() : 0;
					for (int i = 0; i < 128; ++i) {
						int s = 0;
						for (auto& v : voices) if (v.at < v.c->n) s += v.c->s[v.at++];
						mGain += (want - mGain) * 0.0005f;   // (fades in and out)
						if (mGain > 0.001f) s += (int)(musicSample() * mGain);
						int16_t o = (int16_t)std::clamp(s * 3 / 4, -32768, 32767);
						for (int k = 0; k < 4; ++k) buf[i * 4 + k] = o;   // two frames, two channels
					}
					voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice& v) { return v.at >= v.c->n; }), voices.end());
				}
				snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf.data(), 256);
				if (w < 0) {
					if (recovered++ < 5) fprintf(stderr, "bistro: sound: %s, recovering\n", snd_strerror((int)w));
					if (snd_pcm_recover(pcm, (int)w, 1) < 0) { fprintf(stderr, "bistro: sound: lost the device\n"); break; }
				}
			}
			snd_pcm_drop(pcm);
			snd_pcm_close(pcm);
		});
#endif
	}
	~Audio() { run = false; if (th.joinable()) th.join(); }
	void music(bool on) { musicOn = on; }
	void musicVolume(float v) { musicVol = v; }
	void play(const char* name)
	{
		auto it = clips.find(name);
		if (it == clips.end()) return;
		std::lock_guard<std::mutex> l(m);
		// the same sound again soon after: started over, not doubled (chopping)
		for (auto& v : voices) if (v.c == it->second && v.at < 2000) { v.at = 0; return; }
		if (voices.size() < 8) voices.push_back({ it->second, 0 });
	}
};

static volatile sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

static int playOnDevice(const std::string& data, const std::string& top)
{
	signal(SIGTERM, onSignal); signal(SIGINT, onSignal);
	Display disp;
	if (!disp.open(top)) { fprintf(stderr, "bistro: no panel\n"); return 1; }
	Input in; in.open();
	Audio audio;
	Game g;
	g.dataDir = data; g.loadBest();
	g.sound = [&](const char* s) { audio.play(s); };
	g.optionsChanged = [&] { audio.music(g.musicOn); };
	audio.music(g.musicOn);
	auto last = std::chrono::steady_clock::now();
	double acc = 0;
	int frame = 0;
	while (!g.quit && !g_stop) {
		std::vector<pollfd> pf;
		for (int fd : in.fds()) pf.push_back({ fd, POLLIN, 0 });
		pf.push_back({ disp.fd, POLLIN, 0 });
		poll(pf.data(), pf.size(), 4);
		for (auto& p : pf) {
			if (!(p.revents & POLLIN)) continue;
			if (p.fd == disp.fd) disp.readEvents(); else in.read(p.fd, g);
		}
		auto now = std::chrono::steady_clock::now();
		acc += std::chrono::duration<double>(now - last).count();
		last = now;
		acc = std::min(acc, 0.1);
		while (acc >= 1 / 60.0) { g.update(1 / 60.f); acc -= 1 / 60.0; }
		audio.musicVolume(g.scene == Game::PAUSE || g.scene == Game::OPTIONS ? 0.18f : 0.42f);
		if (disp.ready(0)) {
			g.drawTop(); disp.show(0, g.top);
			// the orders: every other frame is plenty
			if (disp.p[1].on && (frame++ % 2 == 0 || g.scene != Game::PLAY) && disp.ready(1)) { g.drawBottom(); disp.show(1, g.bottom); }
		}
	}
	// closed while cooking (the home button, the power): a Play kitchen is kept
	if (g.mode == CALM && (g.scene == Game::PLAY || g.scene == Game::PAUSE || (g.scene == Game::OPTIONS && g.optionsFrom == Game::PAUSE))) g.saveGame();
	return 0;
}
#endif

int main(int argc, char** argv)
{
	loadSprites(); loadFonts();
	bool bench = false;
	std::string smokePrefix, data = getenv("BISTRO_DATA") ? getenv("BISTRO_DATA") : "", top = getenv("BANANA_TOP") ? getenv("BANANA_TOP") : "DSI-2";
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--smoke" && i + 1 < argc) smokePrefix = argv[++i];
		else if (a == "--data" && i + 1 < argc) data = argv[++i];
		else if (a == "--top" && i + 1 < argc) top = argv[++i];
		else if (a == "--bench") bench = true;
	}
	if (!smokePrefix.empty()) return smoke(smokePrefix);
	if (bench) {
		// what a frame of play costs to draw: the kitchen each frame, the orders every other one
		Game g; g.seed = 3; g.startRound(CALM);
		for (int i = 0; i < 200; ++i) g.update(1 / 60.f);
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < 300; ++i) { g.update(1 / 60.f); g.drawTop(); if (i % 2 == 0) g.drawBottom(); }
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 300;
		printf("bistro: %.2f ms a frame (the budget at 60 fps: 16.7)\n", ms);
		return 0;
	}
#ifdef BISTRO_DEVICE
	return playOnDevice(data, top);
#else
	fprintf(stderr, "bistro: this build only draws (--smoke PREFIX)\n");
	return 1;
#endif
}
