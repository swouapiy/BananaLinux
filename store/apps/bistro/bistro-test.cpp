// Bistro: the kitchen's rules, played. Each recipe made and served, burning, the bin, a wrong dish, a missed
// order, chefs and counters. g++ -std=c++17 -O1 -o bistro-test bistro-test.cpp && ./bistro-test
#include "bistro-game.h"
#include "bistro-tutorial.h"
#include <deque>
#include <cstdio>

using namespace bistro;
static int fails = 0, passes = 0;
#define CHECK(c, what) do { if (c) passes++; else { fails++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

// Stand the chef on the floor next to square (tx, ty), facing it
static void standAt(Kitchen& k, Chef& c, int tx, int ty)
{
	static const int d[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
	for (auto& v : d) {
		int fx = tx + v[0], fy = ty + v[1];
		if (k.inside(fx, fy) && !k.cells[fy][fx].solid()) {
			c.x = fx + 0.5f; c.y = fy + 0.5f; c.fx = (float)-v[0]; c.fy = (float)-v[1];
			return;
		}
	}
	printf("no floor beside %d,%d\n", tx, ty);
}
static void press(Kitchen& k, int chef, Buttons p, float dt = 1 / 60.f)
{
	std::vector<Buttons> held(k.chefs.size()), pr(k.chefs.size());
	pr[chef] = p;
	k.step(dt, held, pr);
}
static void hold(Kitchen& k, int chef, Buttons h, float secs)
{
	std::vector<Buttons> held(k.chefs.size()), pr(k.chefs.size());
	held[chef] = h;
	for (float t = 0; t < secs; t += 1 / 60.f) k.step(1 / 60.f, held, pr);
}
static void wait(Kitchen& k, float secs) { hold(k, 0, Buttons(), secs); }
static Buttons A() { Buttons b; b.a = true; return b; }
static Buttons X() { Buttons b; b.x = true; return b; }
static bool hasEvent(const Kitchen& k, Event::Kind e) { for (auto& v : k.events) if (v.kind == e) return true; return false; }

// put a fresh order of this recipe first, with plenty of time
static void want(Kitchen& k, Recipe r) { Order o; o.r = r; o.left = o.total = 999; o.id = 900 + r; k.orders.insert(k.orders.begin(), o); }

// carry food from a crate to a board, chop it, take it up
static void fetchAndChop(Kitchen& k, Chef& c, int crateX, int boardX, int boardY)
{
	standAt(k, c, crateX, 2); press(k, 0, A());
	standAt(k, c, boardX, boardY); press(k, 0, A());
	hold(k, 0, X(), Kitchen::CHOP_TIME + 0.1f);
	press(k, 0, A());
}


// A player who walks for real (the D-pad held, a step at a time) and follows the tutorial
struct Bot {
	Kitchen& k; Tutorial& t; int chef = 0; int frames = 0;
	void tick(Buttons h = Buttons(), Buttons p = Buttons())
	{
		std::vector<Buttons> held(k.chefs.size()), pr(k.chefs.size());
		held[chef] = h; pr[chef] = p;
		k.step(1 / 60.f, held, pr);
		t.check(k, chef);
		frames++;
	}
	// to the floor beside square (tx, ty), facing it; false if it can't
	bool walkTo(int tx, int ty)
	{
		Chef& c = k.chefs[chef];
		int sx = (int)c.x, sy = (int)c.y;
		int prev[MH][MW]; for (auto& r : prev) for (auto& v : r) v = -2;
		std::deque<int> q{ sy * MW + sx }; prev[sy][sx] = -1;
		int goal = -1;
		static const int d[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
		while (!q.empty() && goal < 0) {
			int at = q.front(); q.pop_front();
			int x = at % MW, y = at / MW;
			for (auto& v : d) if (x + v[0] == tx && y + v[1] == ty) goal = at;
			for (auto& v : d) {
				int nx = x + v[0], ny = y + v[1];
				if (!k.inside(nx, ny) || k.cells[ny][nx].solid() || prev[ny][nx] != -2) continue;
				prev[ny][nx] = at; q.push_back(ny * MW + nx);
			}
		}
		if (goal < 0) return false;
		std::vector<int> path;
		for (int at = goal; at >= 0; at = prev[at / MW][at % MW]) path.insert(path.begin(), at);
		for (int at : path) {
			float gx = at % MW + 0.5f, gy = at / MW + 0.5f;
			for (int i = 0; i < 240; ++i) {
				float dx = gx - c.x, dy = gy - c.y;
				if (std::fabs(dx) < 0.1f && std::fabs(dy) < 0.1f) break;
				Buttons h; h.right = dx > 0.07f; h.left = dx < -0.07f; h.down = dy > 0.07f; h.up = dy < -0.07f;
				tick(h);
			}
		}
		// face it
		int gx = goal % MW, gy = goal / MW;
		Buttons h; h.right = tx > gx; h.left = tx < gx; h.down = ty > gy; h.up = ty < gy;
		for (int i = 0; i < 4; ++i) tick(h);
		int ax, ay;
		return k.target(c, ax, ay) && ax == tx && ay == ty;
	}
	bool use(int tx, int ty) { if (!walkTo(tx, ty)) return false; Buttons p; p.a = true; tick(Buttons(), p); return true; }
	bool chop(int tx, int ty) { if (!walkTo(tx, ty)) return false; Buttons h; h.x = true; for (float s = 0; s < Kitchen::CHOP_TIME + 0.2f; s += 1 / 60.f) tick(h); return true; }
	void wait(float secs) { for (float s = 0; s < secs; s += 1 / 60.f) tick(); }
	// fetch from a crate, chop at the board (7,5), take it up
	bool chopped(int crateX) { return use(crateX, 2) && use(7, 5) && chop(7, 5) && use(7, 5); }
};

int main()
{
	// --- the map
	{
		Kitchen k;
		CHECK(k.chefs.size() == 2, "two chefs");
		CHECK(!k.blocked(k.chefs[0].x, k.chefs[0].y) && !k.blocked(k.chefs[1].x, k.chefs[1].y), "chefs start on the floor");
		CHECK(k.orders.size() == 2, "two orders to start");
		CHECK(k.crateFood(1, 2) == TOMATO && k.crateFood(2, 2) == TOMATO && k.crateFood(10, 2) == BREAD, "crates are two squares");
		CHECK(k.cells[10][1].vessel.kind == Vessel::POT && k.cells[10][4].vessel.kind == Vessel::PAN, "pots and pans");
	}
	// --- a salad: tomato and cabbage chopped, on a plate, served
	{
		Kitchen k; k.orders.clear(); want(k, SALAD);
		Chef& c = k.chefs[0];
		standAt(k, c, 1, 2); press(k, 0, A());
		CHECK(c.hand.kind == Thing::FOOD && c.hand.food == (Bit{ TOMATO, RAW }), "a tomato from its crate");
		standAt(k, c, 7, 5); press(k, 0, A());
		CHECK(c.hand.empty() && k.cells[5][7].thing.food.f == TOMATO, "put on the board");
		hold(k, 0, X(), 0.5f);
		CHECK(k.cells[5][7].thing.chop > 0.2f && k.cells[5][7].thing.food.s == RAW, "chopping is under way");
		hold(k, 0, X(), Kitchen::CHOP_TIME);
		CHECK(k.cells[5][7].thing.food == (Bit{ TOMATO, CUT }), "chopped");
		press(k, 0, A());
		CHECK(c.hand.food == (Bit{ TOMATO, CUT }), "chopped tomato in hand");
		// a plate on the counter, the tomato on it
		standAt(k, c, 12, 2); press(k, 0, A());
		CHECK(c.hand.kind == Thing::FOOD, "can't take a plate with the hands full");
		standAt(k, c, 11, 2); press(k, 0, A());
		CHECK(c.hand.empty() && k.cells[2][11].thing.food.f == TOMATO, "tomato on the counter");
		standAt(k, c, 12, 2); press(k, 0, A());
		CHECK(c.hand.kind == Thing::PLATE, "a plate");
		standAt(k, c, 11, 2); press(k, 0, A());
		CHECK(c.hand.on.size() == 1 && k.cells[2][11].thing.empty(), "plate picks up the tomato from the counter");
		standAt(k, c, 14, 2); press(k, 0, A());   // (14,2) is a plain counter: the plate put down
		CHECK(k.cells[2][14].thing.kind == Thing::PLATE, "plate on the counter");
		fetchAndChop(k, c, 3, 8, 5);
		CHECK(c.hand.food == (Bit{ CABBAGE, CUT }), "chopped cabbage");
		standAt(k, c, 14, 2); press(k, 0, A());
		CHECK(c.hand.empty() && plateIs(k.cells[2][14].thing.on) == SALAD, "a salad on the plate");
		press(k, 0, A());
		standAt(k, c, 15, 3); press(k, 0, A());
		CHECK(hasEvent(k, Event::SERVED) && k.served == 1 && k.tips >= 20 && c.hand.empty(), "salad served, tips");
	}
	// --- tomato soup: three chopped tomatoes in a pot, cooked, into a bowl
	{
		Kitchen k; k.orders.clear(); want(k, TOMATO_SOUP);
		Chef& c = k.chefs[0];
		for (int i = 0; i < 3; ++i) {
			fetchAndChop(k, c, 1, 7, 5);
			standAt(k, c, 8, 10); press(k, 0, A());
		}
		Vessel& v = k.cells[10][8].vessel;
		CHECK(v.n == 3 && v.full(), "three tomatoes in the pot");
		fetchAndChop(k, c, 1, 7, 5);
		standAt(k, c, 8, 10); press(k, 0, A());
		CHECK(v.n == 3 && c.hand.food.f == TOMATO, "a fourth doesn't go in");
		standAt(k, c, 12, 10); press(k, 0, A());
		CHECK(c.hand.empty() && hasEvent(k, Event::BIN), "bin");
		standAt(k, c, 12, 2); press(k, 0, A());
		standAt(k, c, 8, 10); press(k, 0, A());
		CHECK(c.hand.on.empty(), "soup not ready yet");
		wait(k, Vessel::POT_DONE);
		CHECK(v.done(), "soup cooked");
		press(k, 0, A());
		CHECK(plateIs(c.hand.on) == TOMATO_SOUP && v.n == 0, "soup in the bowl, pot empty");
		standAt(k, c, 15, 4); press(k, 0, A());
		CHECK(k.served == 1, "soup served");
	}
	// --- cheeseburger: meat chopped and fried, cheese chopped, bread
	{
		Kitchen k; k.orders.clear(); want(k, BURGER);
		Chef& c = k.chefs[0];
		fetchAndChop(k, c, 5, 7, 5);
		CHECK(c.hand.food == (Bit{ MEAT, CUT }), "chopped meat");
		standAt(k, c, 4, 10); press(k, 0, A());
		CHECK(k.cells[10][4].vessel.n == 1 && c.hand.empty(), "in the pan");
		standAt(k, c, 12, 2); press(k, 0, A());
		standAt(k, c, 14, 2); press(k, 0, A());     // plate down
		standAt(k, c, 9, 2); press(k, 0, A());      // bread
		standAt(k, c, 14, 2); press(k, 0, A());
		CHECK(k.cells[2][14].thing.on.size() == 1, "bread on the plate");
		fetchAndChop(k, c, 7, 8, 5);
		standAt(k, c, 14, 2); press(k, 0, A());
		CHECK(k.cells[2][14].thing.on.size() == 2, "cheese on the plate");
		fetchAndChop(k, c, 1, 7, 5);
		standAt(k, c, 14, 2); press(k, 0, A());
		CHECK(c.hand.food.f == TOMATO && k.cells[2][14].thing.on.size() == 2, "a tomato doesn't go on a burger");
		standAt(k, c, 12, 10); press(k, 0, A());
		wait(k, Vessel::PAN_DONE);
		standAt(k, c, 14, 2); press(k, 0, A());     // plate up
		standAt(k, c, 4, 10); press(k, 0, A());
		CHECK(plateIs(c.hand.on) == BURGER, "a cheeseburger");
		standAt(k, c, 15, 5); press(k, 0, A());
		CHECK(k.served == 1, "burger served");
	}
	// --- burning, a wrong dish, a missed order, the clock
	{
		Kitchen k; k.orders.clear(); want(k, SALAD);
		Chef& c = k.chefs[0];
		fetchAndChop(k, c, 5, 7, 5);
		standAt(k, c, 5, 10); press(k, 0, A());
		Vessel& v = k.cells[10][5].vessel;
		wait(k, Vessel::PAN_DONE + Vessel::BURNS_AFTER * 0.5f);
		CHECK(v.warning() && !v.burnt(), "warning before it burns");
		wait(k, Vessel::BURNS_AFTER);
		CHECK(v.burnt(), "burnt");
		press(k, 0, A());
		CHECK(c.hand.food == (Bit{ MEAT, BURNT }) && v.n == 0, "burnt patty taken off");
		standAt(k, c, 12, 2); press(k, 0, A());
		CHECK(c.hand.food.s == BURNT, "can't take a plate holding it");
		standAt(k, c, 12, 10); press(k, 0, A());
		standAt(k, c, 12, 2); press(k, 0, A());
		standAt(k, c, 15, 3); press(k, 0, A());
		CHECK(hasEvent(k, Event::WRONG) && c.hand.kind == Thing::PLATE, "an empty plate isn't served");
		Kitchen m; m.orders.clear();
		Order o; o.r = SALAD; o.left = o.total = 1; m.orders.push_back(o); m.tips = 30; m.nextOrder = 99;
		Order o2; o2.r = BURGER; o2.left = o2.total = 99; m.orders.push_back(o2);
		wait(m, 1.1f);
		CHECK(m.missed == 1 && m.tips == 20, "a missed order costs 10 tips");
		wait(m, 200);
		CHECK(m.over && m.time == 0, "the round ends");
	}
	// --- walking: counters stop a chef, chefs push apart
	{
		Kitchen k; Chef& c = k.chefs[0];
		c.x = 1.5f; c.y = 3.5f;
		Buttons up; up.up = true;
		hold(k, 0, up, 2);
		CHECK(c.y > 3.3f - 0.01f && c.y < 3.4f, "stops at the crates");
		k.chefs[1].x = c.x + 0.1f; k.chefs[1].y = c.y;
		wait(k, 0.1f);
		CHECK(std::hypot(k.chefs[1].x - c.x, k.chefs[1].y - c.y) > 0.5f, "chefs don't overlap");
	}

	// --- the tutorial, played through by walking
	{
		Kitchen k(2, 5, TUTORIAL);
		Tutorial t; t.begin(k);
		Bot b{ k, t };
		CHECK(k.orders.empty(), "the tutorial starts with no customer");
		CHECK(b.walkTo(12, 10) && t.step == 1, "walked: lesson 2");
		CHECK(b.use(1, 2) && t.step == 2, "a tomato: lesson 3");
		CHECK(b.use(7, 5) && t.step == 3, "on the board: lesson 4");
		CHECK(b.chop(7, 5) && t.step == 4, "chopped: lesson 5");
		CHECK(b.use(12, 2) && t.step == 5, "a plate: lesson 6");
		CHECK(b.use(7, 5) && t.step == 6, "tomato on the plate: lesson 7");
		CHECK(b.use(14, 2) && t.step == 7, "plate down: lesson 8");
		CHECK(b.chopped(3) && t.step == 8, "lettuce chopped: lesson 9");
		CHECK(b.use(14, 2) && t.step == 9 && k.orders.size() == 1 && k.orders[0].r == SALAD, "a salad, and a customer wants one");
		CHECK(b.use(14, 2) && b.use(15, 3) && t.step == 10 && k.served == 1, "served: soup next");
		CHECK(k.orders.size() == 1 && k.orders[0].r == TOMATO_SOUP, "a customer for soup");
		for (int i = 0; i < 3; ++i) CHECK(b.chopped(1) && b.use(8, 10), "a tomato in the pot");
		CHECK(t.step == 11, "the pot's full: lesson 12");
		CHECK(b.use(12, 2), "a plate for the soup");
		for (int i = 0; i < 20 && t.step == 11; ++i) { b.wait(1); b.use(8, 10); }
		CHECK(t.step == 12, "scooped: lesson 13");
		CHECK(b.use(15, 4) && t.step == 13 && k.served == 2, "soup served");
		CHECK(b.chopped(5) && b.use(4, 10) && t.step == 14, "meat in the pan: lesson 15");
		CHECK(b.use(12, 2) && b.use(14, 2) && b.use(9, 2) && b.use(14, 2), "bread on a plate");
		CHECK(b.chopped(7) && b.use(14, 2), "cheese on it");
		b.wait(Vessel::PAN_DONE);
		CHECK(b.use(14, 2) && b.use(4, 10) && t.step == 15, "the burger: lesson 16");
		CHECK(b.use(15, 5) && t.step == 16 && k.served == 3, "burger served");
		Buttons dash; dash.b = true; b.tick(Buttons(), dash);
		CHECK(t.step == 17, "dashed");
		t.switched = true; b.chef = 1; b.tick();
		CHECK(t.step == 18, "switched chef");
		t.aPressed = true; b.tick();
		CHECK(t.finished, "the tutorial is done");
		CHECK(k.missed == 0, "nobody left unhappy");
		bool burnt = false;
		for (auto& row : k.cells) for (auto& c : row) burnt = burnt || c.vessel.burnt();
		CHECK(!burnt, "nothing burns in the tutorial");
		printf("  (the tutorial took %.0f s of play)\n", b.frames / 60.0);
	}
	// --- CALM: no clock, customers who leave unhappy take hearts
	{
		Kitchen k(2, 9, CALM);
		CHECK(k.orders.size() == 2 && k.hearts == 3, "calm: two customers, three hearts");
		wait(k, 50);
		CHECK(k.time == 180 && !k.over, "no clock");
		wait(k, 40);
		CHECK(k.missed >= 2 && k.hearts == 3 - k.missed, "each customer left waiting takes a heart");
		wait(k, 300);
		CHECK(k.over && k.hearts == 0, "no hearts left: over");
		Kitchen m(2, 9, CALM); m.hearts = 2; m.orders.clear(); want(m, SALAD); m.served = Kitchen::HEART_BACK - 1;
		Chef& c = m.chefs[0];
		c.hand = Thing::plate(); c.hand.on = { { TOMATO, CUT }, { CABBAGE, CUT } };
		standAt(m, c, 15, 3); press(m, 0, A());
		CHECK(m.hearts == 3 && hasEvent(m, Event::HEART), "a heart back every eighth served");
		CHECK(Kitchen(2, 1, CALM).orderGap() == 16 && Kitchen(2, 1, CALM).patience() == 1, "slow at first");
		m.played = 600;
		CHECK(m.orderGap() < 3 + 9 && m.patience() < 1, "busier later");
	}
	// --- a CALM game saved and taken up again
	{
		Kitchen k(2, 4, CALM);
		wait(k, 3);
		k.chefs[0].hand = Thing::plate(); k.chefs[0].hand.on = { { BREAD, RAW }, { MEAT, COOKED } };
		k.cells[5][7].thing = Thing::ofFood(CHEESE); k.cells[5][7].thing.chop = 0.5f;
		k.cells[10][8].vessel.n = 3; k.cells[10][8].vessel.cook = 4;
		k.hearts = 2; k.tips = 77; k.served = 3;
		std::string s = k.save();
		Kitchen m(2, 1, RUSH);
		CHECK(m.load(s), "loaded");
		CHECK(m.mode == CALM && m.hearts == 2 && m.tips == 77 && m.served == 3 && m.orders.size() == k.orders.size(), "the numbers back");
		CHECK(m.chefs.size() == 2 && plateIs(m.chefs[0].hand.on) == -1 && m.chefs[0].hand.on.size() == 2, "the chef's plate back");
		CHECK(m.cells[5][7].thing.food.f == CHEESE && std::fabs(m.cells[5][7].thing.chop - 0.5f) < 0.01f, "the half-chopped cheese back");
		CHECK(m.cells[10][8].vessel.n == 3 && m.cells[10][8].vessel.kind == Vessel::POT && std::fabs(m.cells[10][8].vessel.cook - 4) < 0.01f, "the pot back");
		CHECK(m.save() == s, "saved again, the same");
		Kitchen bad(2, 1, RUSH);
		CHECK(!bad.load("bistro-save 1\nmode 0\n") && bad.mode == RUSH, "half a save isn't loaded");
		CHECK(!bad.load("hello"), "nor something else");
	}
	printf("bistro-test: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
