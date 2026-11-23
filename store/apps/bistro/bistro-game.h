// Bistro: the kitchen's rules. One kitchen, three recipes (salad, tomato soup, cheeseburger), chefs who
// carry one thing at a time. Plain C++17, no drawing and no device: bistro.cpp draws it and feeds it the buttons;
// bistro-test.cpp plays it. Positions are in kitchen squares (16 x 12, 40 px each on the top screen).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bistro {

constexpr int MW = 16, MH = 12;
// The kitchen. # counter (row 0-1 and 11: the walls), . floor. Crates (two squares, the capital first): T tomato,
// L lettuce (cabbage), M meat, H cheese, B bread. P plates, W the pass (dishes go out), K chopping board, S stove
// with a pot, F stove with a pan, X bin.
static const char* const KITCHEN1[MH] = {
	"################",
	"################",
	"#TtLlMmHhBb#PP##",
	"#..............W",
	"#..............W",
	"K....##KK##....W",
	"K....######....#",
	"#..............#",
	"#..............#",
	"#..............#",
	"#SS#FF##SS##X###",
	"################",
};

enum Food : uint8_t { NOFOOD, TOMATO, CABBAGE, MEAT, CHEESE, BREAD, SOUP };
enum Stage : uint8_t { RAW, CUT, COOKED, BURNT };
enum Recipe : uint8_t { SALAD, TOMATO_SOUP, BURGER, NRECIPES };

struct Bit { Food f = NOFOOD; Stage s = RAW; bool operator==(const Bit& o) const { return f == o.f && s == o.s; } };

// What a recipe is made of, on one plate
inline std::vector<Bit> recipeBits(Recipe r)
{
	switch (r) {
	case SALAD: return { { TOMATO, CUT }, { CABBAGE, CUT } };
	case TOMATO_SOUP: return { { SOUP, COOKED } };
	default: return { { BREAD, RAW }, { MEAT, COOKED }, { CHEESE, CUT } };
	}
}
inline const char* recipeName(Recipe r) { return r == SALAD ? "Salad" : r == TOMATO_SOUP ? "Tomato soup" : "Cheeseburger"; }

// Something on a counter or in a hand: a food, or a plate with up to three foods on it
struct Thing {
	enum Kind : uint8_t { NONE, FOOD, PLATE } kind = NONE;
	Bit food;
	float chop = 0;          // 0..1 on the way to CUT
	std::vector<Bit> on;     // a plate's
	bool empty() const { return kind == NONE; }
	static Thing ofFood(Food f, Stage s = RAW) { Thing t; t.kind = FOOD; t.food = { f, s }; return t; }
	static Thing plate() { Thing t; t.kind = PLATE; return t; }
};

// Whether these foods could still become one of the recipes (a plate takes nothing else)
inline bool fitsARecipe(const std::vector<Bit>& on)
{
	for (int r = 0; r < NRECIPES; ++r) {
		auto need = recipeBits((Recipe)r);
		bool ok = true;
		for (auto& b : on) {
			auto it = std::find(need.begin(), need.end(), b);
			if (it == need.end()) { ok = false; break; }
			need.erase(it);
		}
		if (ok) return true;
	}
	return false;
}
// The recipe a plate is, finished; -1 if it isn't one yet
inline int plateIs(const std::vector<Bit>& on)
{
	for (int r = 0; r < NRECIPES; ++r) {
		auto need = recipeBits((Recipe)r);
		if (need.size() != on.size()) continue;
		bool ok = true;
		for (auto& b : on) {
			auto it = std::find(need.begin(), need.end(), b);
			if (it == need.end()) { ok = false; break; }
			need.erase(it);
		}
		if (ok) return r;
	}
	return -1;
}

// A pot (three chopped tomatoes make soup) or a pan (one chopped meat makes a patty), on its stove
struct Vessel {
	enum Kind : uint8_t { POT, PAN } kind = POT;
	int n = 0;               // tomatoes in the pot, or 1 when the pan has meat
	float cook = 0;          // seconds cooking, once full
	float burnsAfter = BURNS_AFTER; // (the tutorial's never burn)
	static constexpr float POT_DONE = 10, PAN_DONE = 6, BURNS_AFTER = 9;  // (seconds after done)
	bool full() const { return kind == POT ? n >= 3 : n >= 1; }
	float doneAt() const { return kind == POT ? POT_DONE : PAN_DONE; }
	bool done() const { return full() && cook >= doneAt() && !burnt(); }
	bool burnt() const { return full() && cook >= doneAt() + burnsAfter; }
	bool warning() const { return full() && cook >= doneAt() + burnsAfter * 0.45f && !burnt(); }
};

struct Cell {
	char kind = '.';         // as in the map ('t' etc.: the crate's other square)
	Thing thing;             // on a counter or a board
	Vessel vessel;           // on a stove
	bool solid() const { return kind != '.'; }
};

struct Order {
	Recipe r = SALAD;
	float left = 60, total = 60; // seconds
	int id = 0;
	float age = 0;           // seconds since it came in
};

struct Chef {
	float x = 0, y = 0;      // centre, in squares
	float fx = 0, fy = 1;    // facing
	float dash = 0, dashWait = 0;
	Thing hand;
	int lastCX = -1, lastCY = -1;
	bool chopping = false;
};

// What happened in a step, for the sounds and the little pop-ups
struct Event {
	enum Kind : uint8_t { PICK, DROP, CHOP, CHOPPED, SIZZLE, DONE, BURNT, SERVED, WRONG, MISSED, NEWORDER, BIN, DASH, TICK, HEART } kind;
	float x = 0, y = 0;      // where, in squares
	int value = 0;           // tips for SERVED/MISSED
	int order = 0;           // the order's id (SERVED, MISSED, NEWORDER): its customer
};

// How a game is played. CALM: no clock, keep the customers happy (a customer who leaves without their dish takes a
// heart; the last one ends it). RUSH: three minutes, as many tips as you can. TUTORIAL: one chef's lessons, orders
// that wait, nothing burns.
enum Mode : uint8_t { CALM, RUSH, TUTORIAL };

struct Buttons { bool up = false, down = false, left = false, right = false, a = false, x = false, b = false; };

class Kitchen {
public:
	Cell cells[MH][MW];
	std::vector<Chef> chefs;
	std::vector<Order> orders;
	std::vector<Event> events;   // this step's
	Mode mode = RUSH;
	float time = 180;            // seconds left (RUSH)
	float roundLength = 180;
	float played = 0;            // seconds played
	int tips = 0, served = 0, missed = 0;
	int hearts = 3;              // (CALM)
	static constexpr int MAX_HEARTS = 3, HEART_BACK = 8;  // a heart back each 8 served, up to 3
	int stars[3] = { 40, 90, 140 };
	uint32_t rng = 12345;
	float nextOrder = 0;
	int orderIds = 0;
	bool over = false;
	static constexpr float SPEED = 3.6f, DASH_SPEED = 9.5f, R = 0.32f, CHOP_TIME = 1.6f;
	static constexpr int MAX_ORDERS = 4;

	explicit Kitchen(int nChefs = 2, uint32_t seed = 12345, Mode m = RUSH, const char* const* map = KITCHEN1)
	{
		mode = m;
		rng = seed ? seed : 1;
		for (int y = 0; y < MH; ++y)
			for (int x = 0; x < MW; ++x) {
				Cell& c = cells[y][x];
				c.kind = map[y][x];
				if (c.kind == 'S') c.vessel.kind = Vessel::POT;
				if (c.kind == 'F') c.vessel.kind = Vessel::PAN;
				if (m == TUTORIAL) c.vessel.burnsAfter = 1e9f;
			}
		static const float start[4][2] = { { 3.5f, 4.0f }, { 12.5f, 8.5f }, { 3.5f, 8.5f }, { 12.5f, 4.0f } };
		for (int i = 0; i < nChefs && i < 4; ++i) { Chef c; c.x = start[i][0]; c.y = start[i][1]; chefs.push_back(c); }
		if (m != TUTORIAL) { addOrder(); addOrder(); }
		nextOrder = 14;
	}
	bool autoOrders() const { return mode != TUTORIAL; }

	uint32_t rand() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

	// A customer in, with an order (a random one when r < 0); its id, or 0 when the room is full
	int addOrder(int r = -1)
	{
		if ((int)orders.size() >= MAX_ORDERS) return 0;
		Order o;
		// soup and salad more often than the burger (three steps more)
		uint32_t k = rand() % 10;
		o.r = r >= 0 ? (Recipe)r : k < 4 ? SALAD : k < 7 ? TOMATO_SOUP : BURGER;
		o.total = o.left = (o.r == BURGER ? 80 : o.r == TOMATO_SOUP ? 70 : 55) * patience();
		o.id = ++orderIds;
		orders.push_back(o);
		Event e{ Event::NEWORDER, 0, 0, o.id }; e.order = o.id;
		events.push_back(e);
		return o.id;
	}
	// CALM gets busier the longer it lasts: customers come more often (16 s apart, down to 9 after 5 minutes)
	// and wait less (down to 75% after 10 minutes)
	float patience() const { return mode == CALM ? std::max(0.75f, 1 - played / 2400) : 1; }
	float orderGap() const { return mode == CALM ? std::max(9.f, 16 - played / 43) : 16; }

	bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < MW && y < MH; }
	bool solidAt(int x, int y) const { return !inside(x, y) || cells[y][x].solid(); }
	// a counter a chef can use (not the walls' rows)
	bool usable(int x, int y) const { return inside(x, y) && y >= 2 && y <= MH - 2 && cells[y][x].solid(); }

	// The square a chef faces: the one in front, or the nearer of the two sides of a diagonal
	bool target(const Chef& c, int& tx, int& ty) const
	{
		int cx = (int)std::floor(c.x), cy = (int)std::floor(c.y);
		float ax = std::fabs(c.fx), ay = std::fabs(c.fy);
		int sx = c.fx > 0.3f ? 1 : c.fx < -0.3f ? -1 : 0, sy = c.fy > 0.3f ? 1 : c.fy < -0.3f ? -1 : 0;
		int cand[3][2] = { { cx + sx, cy + sy }, { ax >= ay ? cx + sx : cx, ax >= ay ? cy : cy + sy }, { ax >= ay ? cx : cx + sx, ax >= ay ? cy + sy : cy } };
		for (auto& p : cand) {
			if ((p[0] == cx && p[1] == cy)) continue;
			if (usable(p[0], p[1])) {
				// only if the chef is close to it (within 0.75 square of its edge)
				float nx = std::clamp(c.x, (float)p[0], (float)p[0] + 1), ny = std::clamp(c.y, (float)p[1], (float)p[1] + 1);
				if (std::hypot(c.x - nx, c.y - ny) < 0.75f) { tx = p[0]; ty = p[1]; return true; }
			}
		}
		return false;
	}

	// the crate square's food (either of its two squares)
	Food crateFood(int x, int y) const
	{
		char k = cells[y][x].kind;
		switch (k) {
		case 'T': case 't': return TOMATO;
		case 'L': case 'l': return CABBAGE;
		case 'M': case 'm': return MEAT;
		case 'H': case 'h': return CHEESE;
		case 'B': case 'b': return BREAD;
		default: return NOFOOD;
		}
	}

	static bool choppable(const Thing& t) { return t.kind == Thing::FOOD && t.food.s == RAW && t.food.f != BREAD && t.food.f != SOUP; }

	// A food onto a plate, if the plate could still become a dish with it
	static bool addToPlate(Thing& plate, const Thing& food)
	{
		if (plate.kind != Thing::PLATE || food.kind != Thing::FOOD) return false;
		auto on = plate.on; on.push_back(food.food);
		if (!fitsARecipe(on)) return false;
		plate.on = on;
		return true;
	}

	void ev(Event::Kind k, float x, float y, int v = 0) { events.push_back({ k, x, y, v }); }

	// A: pick up, put down, combine, serve
	void use(Chef& c)
	{
		int tx, ty;
		if (!target(c, tx, ty)) return;
		Cell& cell = cells[ty][tx];
		Thing& h = c.hand;
		float ex = tx + 0.5f, ey = ty + 0.5f;
		Food crate = crateFood(tx, ty);
		if (crate != NOFOOD) {
			if (h.empty()) { h = Thing::ofFood(crate); ev(Event::PICK, ex, ey); }
			return;
		}
		switch (cell.kind) {
		case 'P':
			if (h.empty()) { h = Thing::plate(); ev(Event::PICK, ex, ey); }
			return;
		case 'X':
			if (h.kind == Thing::FOOD) { h = Thing(); ev(Event::BIN, ex, ey); }
			else if (h.kind == Thing::PLATE && !h.on.empty()) { h.on.clear(); ev(Event::BIN, ex, ey); }
			return;
		case 'W': {
			if (h.kind != Thing::PLATE) return;
			int r = plateIs(h.on);
			int best = -1;
			for (int i = 0; i < (int)orders.size(); ++i)
				if (orders[i].r == r && (best < 0 || orders[i].left < orders[best].left)) best = i;
			if (r < 0 || best < 0) { ev(Event::WRONG, ex, ey); return; }
			int tip = 20 + (int)std::lround(10 * orders[best].left / orders[best].total);
			tips += tip; served++;
			int id = orders[best].id;
			orders.erase(orders.begin() + best);
			h = Thing();
			ev(Event::SERVED, ex, ey, tip); events.back().order = id;
			if (mode == CALM && served % HEART_BACK == 0 && hearts < MAX_HEARTS) { hearts++; ev(Event::HEART, ex, ey, hearts); }
			if (orders.empty()) nextOrder = std::min(nextOrder, 2.0f);
			return;
		}
		case 'S': case 'F': {
			Vessel& v = cell.vessel;
			if (h.kind == Thing::FOOD) {
				bool fits = v.kind == Vessel::POT ? h.food == Bit{ TOMATO, CUT } && v.n < 3 && v.cook == 0 && !v.burnt()
				                                   : h.food == Bit{ MEAT, CUT } && v.n == 0;
				if (fits) { v.n++; h = Thing(); ev(Event::DROP, ex, ey); }
				return;
			}
			if (h.kind == Thing::PLATE) {
				if (v.done()) {
					Thing f = Thing::ofFood(v.kind == Vessel::POT ? SOUP : MEAT, COOKED);
					if (addToPlate(h, f)) { v = Vessel{ v.kind }; ev(Event::PICK, ex, ey); }
				}
				return;
			}
			// empty-handed: a pan's patty (raw, cooked or burnt), or a burnt pot's mess, to carry to the bin
			if (v.kind == Vessel::PAN && v.n) {
				h = Thing::ofFood(MEAT, v.burnt() ? BURNT : v.done() ? COOKED : CUT);
				v = Vessel{ v.kind }; ev(Event::PICK, ex, ey);
			} else if (v.kind == Vessel::POT && v.burnt()) {
				h = Thing::ofFood(SOUP, BURNT);
				v = Vessel{ v.kind }; ev(Event::PICK, ex, ey);
			}
			return;
		}
		default: break;
		}
		// counters and boards
		Thing& t = cell.thing;
		if (t.empty() && !h.empty()) { t = h; h = Thing(); ev(Event::DROP, ex, ey); }
		else if (!t.empty() && h.empty()) { h = t; t = Thing(); ev(Event::PICK, ex, ey); }
		else if (t.kind == Thing::PLATE && h.kind == Thing::FOOD) { if (addToPlate(t, h)) { h = Thing(); ev(Event::DROP, ex, ey); } }
		else if (t.kind == Thing::FOOD && h.kind == Thing::PLATE) { if (addToPlate(h, t)) { t = Thing(); ev(Event::PICK, ex, ey); } }
	}

	// whether a chef standing here would overlap a counter
	bool blocked(float x, float y) const
	{
		for (int gy = (int)std::floor(y - R); gy <= (int)std::floor(y + R); ++gy)
			for (int gx = (int)std::floor(x - R); gx <= (int)std::floor(x + R); ++gx) {
				if (!solidAt(gx, gy)) continue;
				float nx = std::clamp(x, (float)gx, (float)gx + 1), ny = std::clamp(y, (float)gy, (float)gy + 1);
				if ((x - nx) * (x - nx) + (y - ny) * (y - ny) < R * R) return true;
			}
		return false;
	}

	void move(Chef& c, float dx, float dy, float dt)
	{
		float len = std::hypot(dx, dy);
		if (len > 0) { c.fx = dx / len; c.fy = dy / len; }
		float sp = c.dash > 0 ? DASH_SPEED : SPEED;
		if (len == 0 && c.dash <= 0) return;
		if (len == 0) { dx = c.fx; dy = c.fy; len = 1; }
		float vx = dx / len * sp * dt, vy = dy / len * sp * dt;
		// one axis at a time, so a chef slides along a counter
		if (!blocked(c.x + vx, c.y)) c.x += vx;
		if (!blocked(c.x, c.y + vy)) c.y += vy;
	}

	// One step of play: each chef's buttons (held), and the ones pressed since the last step
	void step(float dt, const std::vector<Buttons>& held, const std::vector<Buttons>& pressed)
	{
		events.clear();
		if (over) return;
		played += dt;
		if (mode == RUSH) {
			time -= dt;
			if (time <= 0) { time = 0; over = true; return; }
			if ((int)std::ceil(time) != (int)std::ceil(time + dt) && time <= 10) ev(Event::TICK, 0, 0, (int)std::ceil(time));
		}
		for (size_t i = 0; i < chefs.size(); ++i) {
			Chef& c = chefs[i];
			Buttons h = i < held.size() ? held[i] : Buttons(), p = i < pressed.size() ? pressed[i] : Buttons();
			c.dash = std::max(0.f, c.dash - dt); c.dashWait = std::max(0.f, c.dashWait - dt);
			if (p.b && c.dashWait <= 0) { c.dash = 0.16f; c.dashWait = 0.7f; ev(Event::DASH, c.x, c.y); }
			move(c, (h.right ? 1.f : 0.f) - (h.left ? 1.f : 0.f), (h.down ? 1.f : 0.f) - (h.up ? 1.f : 0.f), dt);
			if (p.a) use(c);
			// X held at a board: chopping
			c.chopping = false;
			int tx, ty;
			if (h.x && target(c, tx, ty) && cells[ty][tx].kind == 'K' && choppable(cells[ty][tx].thing) && c.hand.empty()) {
				Thing& t = cells[ty][tx].thing;
				float before = t.chop;
				t.chop = std::min(1.f, t.chop + dt / CHOP_TIME);
				c.chopping = true;
				if ((int)(before * 5) != (int)(t.chop * 5)) ev(Event::CHOP, tx + 0.5f, ty + 0.5f);
				if (t.chop >= 1) { t.food.s = CUT; t.chop = 0; ev(Event::CHOPPED, tx + 0.5f, ty + 0.5f); }
			}
		}
		// chefs don't walk through each other
		for (size_t i = 0; i < chefs.size(); ++i)
			for (size_t j = i + 1; j < chefs.size(); ++j) {
				Chef &a = chefs[i], &b = chefs[j];
				float dx = b.x - a.x, dy = b.y - a.y, d = std::hypot(dx, dy);
				if (d > 0 && d < 2 * R) {
					float push = (2 * R - d) / 2;
					float ox = dx / d * push, oy = dy / d * push;
					if (!blocked(a.x - ox, a.y - oy)) { a.x -= ox; a.y -= oy; }
					if (!blocked(b.x + ox, b.y + oy)) { b.x += ox; b.y += oy; }
				}
			}
		// the stoves
		for (int y = 0; y < MH; ++y)
			for (int x = 0; x < MW; ++x) {
				Vessel& v = cells[y][x].vessel;
				if ((cells[y][x].kind != 'S' && cells[y][x].kind != 'F') || !v.full() || v.burnt()) continue;
				bool wasDone = v.done();
				v.cook += dt;
				if (!wasDone && v.done()) ev(Event::DONE, x + 0.5f, y + 0.5f);
				if (v.burnt()) ev(Event::BURNT, x + 0.5f, y + 0.5f);
			}
		// the orders
		for (auto& o : orders) o.age += dt;
		for (size_t i = 0; i < orders.size() && mode != TUTORIAL;) {
			orders[i].left -= dt;
			if (orders[i].left <= 0) {
				missed++;
				int lost = 0;
				if (mode == RUSH) { lost = std::min(tips, 10); tips -= lost; }
				else if (hearts > 0) hearts--;
				ev(Event::MISSED, 0, 0, lost); events.back().order = orders[i].id;
				orders.erase(orders.begin() + i);
				if (mode == CALM && hearts <= 0) { over = true; return; }
			} else ++i;
		}
		if (!autoOrders()) return;
		nextOrder -= dt;
		if (nextOrder <= 0 || orders.size() < 2) {
			if ((int)orders.size() < MAX_ORDERS) addOrder();
			nextOrder = orderGap();
		}
	}

	// Anything in the kitchen (a hand, a counter, a pot's or pan's contents as a food) that pred likes
	template <class P> bool anywhere(P pred) const
	{
		for (auto& c : chefs) if (pred(c.hand)) return true;
		for (int y = 0; y < MH; ++y)
			for (int x = 0; x < MW; ++x) if (pred(cells[y][x].thing)) return true;
		return false;
	}

	// --- a CALM game kept to go on with later: one line of numbers per thing
	static void putThing(std::string& o, const Thing& t)
	{
		o += std::to_string((int)t.kind) + " " + std::to_string((int)t.food.f) + " " + std::to_string((int)t.food.s) + " " +
		     std::to_string((int)std::lround(t.chop * 1000)) + " " + std::to_string(t.on.size());
		for (auto& b : t.on) o += " " + std::to_string((int)b.f) + " " + std::to_string((int)b.s);
	}
	std::string save() const
	{
		std::string o = "bistro-save 1\n";
		auto num = [&](const char* k, double v) { char b[64]; snprintf(b, sizeof b, "%s %.3f\n", k, v); o += b; };
		num("mode", mode); num("time", time); num("played", played); num("tips", tips); num("served", served);
		num("missed", missed); num("hearts", hearts); num("rng", rng); num("next", nextOrder); num("ids", orderIds);
		for (auto& c : chefs) {
			char b[128]; snprintf(b, sizeof b, "chef %.3f %.3f %.3f %.3f ", c.x, c.y, c.fx, c.fy); o += b;
			putThing(o, c.hand); o += "\n";
		}
		for (auto& r : orders) { char b[96]; snprintf(b, sizeof b, "order %d %.3f %.3f %d\n", (int)r.r, r.left, r.total, r.id); o += b; }
		for (int y = 0; y < MH; ++y)
			for (int x = 0; x < MW; ++x) {
				const Cell& c = cells[y][x];
				if (!c.thing.empty()) { o += "thing " + std::to_string(x) + " " + std::to_string(y) + " "; putThing(o, c.thing); o += "\n"; }
				if (c.vessel.n) { char b[96]; snprintf(b, sizeof b, "vessel %d %d %d %.3f\n", x, y, c.vessel.n, c.vessel.cook); o += b; }
			}
		return o + "end\n";
	}
	// false (and the kitchen as it was) if s isn't a whole save
	bool load(const std::string& s)
	{
		Kitchen k(0, 1, CALM);
		k.orders.clear(); k.events.clear();
		const char* p = s.c_str();
		auto line = [&](std::string& out) { if (!*p) return false; const char* e = strchr(p, '\n'); out.assign(p, e ? e - p : strlen(p)); p = e ? e + 1 : p + strlen(p); return true; };
		std::string l;
		if (!line(l) || l != "bistro-save 1") return false;
		auto thing = [](const char*& q, Thing& t) {
			int kind, f, st, chop, n, used;
			if (sscanf(q, "%d %d %d %d %d%n", &kind, &f, &st, &chop, &n, &used) != 5 || n < 0 || n > 3) return false;
			q += used;
			t.kind = (Thing::Kind)kind; t.food = { (Food)f, (Stage)st }; t.chop = chop / 1000.f; t.on.clear();
			for (int i = 0; i < n; ++i) { int bf, bs; if (sscanf(q, "%d %d%n", &bf, &bs, &used) != 2) return false; q += used; t.on.push_back({ (Food)bf, (Stage)bs }); }
			return true;
		};
		bool ended = false;
		while (line(l)) {
			char key[16] = {}; int used = 0;
			if (sscanf(l.c_str(), "%15s%n", key, &used) != 1) continue;
			const char* q = l.c_str() + used;
			std::string kname = key;
			double v = 0;
			if (kname == "end") { ended = true; break; }
			if (kname == "chef") {
				Chef c; int u;
				if (sscanf(q, "%f %f %f %f%n", &c.x, &c.y, &c.fx, &c.fy, &u) != 4) return false;
				q += u;
				if (!thing(q, c.hand)) return false;
				k.chefs.push_back(c);
			} else if (kname == "order") {
				Order o; int r;
				if (sscanf(q, "%d %f %f %d", &r, &o.left, &o.total, &o.id) != 4 || r < 0 || r >= NRECIPES) return false;
				o.r = (Recipe)r; k.orders.push_back(o);
			} else if (kname == "thing") {
				int x, y, u;
				if (sscanf(q, "%d %d%n", &x, &y, &u) != 2 || !k.inside(x, y)) return false;
				q += u;
				if (!thing(q, k.cells[y][x].thing)) return false;
			} else if (kname == "vessel") {
				int x, y, n; float cook;
				if (sscanf(q, "%d %d %d %f", &x, &y, &n, &cook) != 4 || !k.inside(x, y)) return false;
				k.cells[y][x].vessel.n = n; k.cells[y][x].vessel.cook = cook;
			} else if (sscanf(q, "%lf", &v) == 1) {
				if (kname == "mode") k.mode = (Mode)(int)v;
				else if (kname == "time") k.time = (float)v;
				else if (kname == "played") k.played = (float)v;
				else if (kname == "tips") k.tips = (int)v;
				else if (kname == "served") k.served = (int)v;
				else if (kname == "missed") k.missed = (int)v;
				else if (kname == "hearts") k.hearts = (int)v;
				else if (kname == "rng") k.rng = (uint32_t)v ? (uint32_t)v : 1;
				else if (kname == "next") k.nextOrder = (float)v;
				else if (kname == "ids") k.orderIds = (int)v;
			}
		}
		if (!ended || k.chefs.empty()) return false;
		*this = k;
		return true;
	}

	int starsEarned() const { int n = 0; for (int s : stars) if (tips >= s) n++; return n; }
};

} // namespace bistro
