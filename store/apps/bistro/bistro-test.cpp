// Banana Bistro: the kitchen's rules, played. Each recipe made and served, burning, the bin, a wrong dish, a missed
// order, chefs and counters. g++ -std=c++17 -O1 -o bistro-test bistro-test.cpp && ./bistro-test
#include "bistro-game.h"
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
	printf("bistro-test: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
