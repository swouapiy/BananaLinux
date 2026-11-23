// Bistro: the tutorial's lessons. Each one says what to do, which squares to light up, and when it's done (looked at
// in the kitchen after each step). No drawing here: bistro.cpp shows them, bistro-test.cpp plays them through.
#pragma once
#include "bistro-game.h"

namespace bistro {

struct Lesson {
	const char* title;
	const char* text;        // one or two sentences
	const char* where;       // the map's letters of the squares to light up ("" none)
	const char* keys;        // the buttons it's about, as the bottom screen shows them ("A", "X", "D-PAD"...)
	int order = -1;          // a customer comes in with this order as the lesson starts
	bool pressA = false;     // read it, then A (nothing to do in the kitchen)
};

static const Lesson LESSONS[] = {
	{ "WALK", "Walk around the kitchen with the D-pad.", "", "D-PAD" },
	{ "TAKE A TOMATO", "Walk up to the tomato crate, top left, and press A to take a tomato.", "Tt", "A" },
	{ "TO THE BOARD", "Face a chopping board and press A to put the tomato on it.", "K", "A" },
	{ "CHOP IT", "Stand at the board and hold X until the bar is full.", "K", "X" },
	{ "TAKE A PLATE", "Your hands must be empty. Take a plate from the pile, top right, with A.", "P", "A" },
	{ "TOMATO ON THE PLATE", "Holding the plate, face the chopped tomato and press A: it goes on the plate.", "K", "A" },
	{ "PUT THE PLATE DOWN", "Face an empty counter and press A to put the plate down.", "", "A" },
	{ "LETTUCE", "Take some lettuce from its crate, put it on a board, and hold X to chop it.", "LlK", "A X" },
	{ "MAKE A SALAD", "Take the chopped lettuce (A) and put it on the plate (A). Tomato and lettuce: a salad!", "", "A" },
	{ "SERVE IT", "A customer wants a salad! Take the plate to the yellow counter, on the right, and press A.", "W", "A", SALAD },
	{ "SOUP IN THE POT", "Tomato soup: chop three tomatoes and put them in a pot, one at a time.", "TtKS", "A X", TOMATO_SOUP },
	{ "SCOOP THE SOUP", "When the pot's bar is full, face it holding an empty plate and press A.", "SP", "A" },
	{ "SERVE THE SOUP", "Take the soup to the yellow counter and press A.", "W", "A" },
	{ "FRY THE MEAT", "A cheeseburger! Take meat, chop it on a board, and put it in a pan.", "MmKF", "A X", BURGER },
	{ "BUILD THE BURGER", "On one plate: bread, chopped cheese, and the fried meat (face the pan with the plate).", "BbHhKFP", "A X" },
	{ "SERVE THE BURGER", "Take the cheeseburger to the yellow counter and press A.", "W", "A" },
	{ "DASH", "Press B for a quick dash across the kitchen.", "", "B" },
	{ "TWO CHEFS", "Press Y to switch to the other chef. Use both: one chops while the other cooks!", "", "Y" },
	{ "BURNING AND THE BIN", "Left too long, pots and pans burn (a red ! warns you). Burnt food goes in the bin, bottom right. Press A.", "X", "A", -1, true },
};
constexpr int NLESSONS = (int)(sizeof(LESSONS) / sizeof(LESSONS[0]));

class Tutorial {
public:
	int step = 0;
	bool finished = false;
	float walked = 0;
	int servedAt = 0;
	bool dashed = false, switched = false, aPressed = false;
	float lastX = -1, lastY = -1;

	// a lesson's start: its customer, and what it measures from
	void begin(Kitchen& k)
	{
		const Lesson& l = LESSONS[step];
		if (l.order >= 0) k.addOrder(l.order);
		servedAt = k.served; walked = 0; dashed = switched = aPressed = false; lastX = lastY = -1;
	}
	// looked at after each step of play (the kitchen's events still there); true when the lesson moved on
	bool check(Kitchen& k, int active)
	{
		if (finished) return false;
		const Chef& c = k.chefs[active];
		if (lastX >= 0) walked += std::hypot(c.x - lastX, c.y - lastY);
		lastX = c.x; lastY = c.y;
		for (auto& e : k.events) if (e.kind == Event::DASH) dashed = true;
		if (!done(k, c)) return false;
		if (++step >= NLESSONS) { finished = true; return true; }
		begin(k);
		return true;
	}
	void skip(Kitchen& k) { if (finished) return; if (++step >= NLESSONS) finished = true; else begin(k); }

	static bool isFood(const Thing& t, Food f, Stage s) { return t.kind == Thing::FOOD && t.food == Bit{ f, s }; }
	static bool plateHas(const Thing& t, Food f, Stage s)
	{
		if (t.kind != Thing::PLATE) return false;
		for (auto& b : t.on) if (b == Bit{ f, s }) return true;
		return false;
	}
	static bool plateOf(const Thing& t, int r) { return t.kind == Thing::PLATE && plateIs(t.on) == r; }

	bool done(const Kitchen& k, const Chef& c) const
	{
		auto onBoard = [&](Food f, Stage s) {
			for (int y = 0; y < MH; ++y)
				for (int x = 0; x < MW; ++x) if (k.cells[y][x].kind == 'K' && isFood(k.cells[y][x].thing, f, s)) return true;
			return false;
		};
		auto potsFull = [&](Vessel::Kind kind) {
			for (int y = 0; y < MH; ++y)
				for (int x = 0; x < MW; ++x) { auto& v = k.cells[y][x].vessel; if ((k.cells[y][x].kind == 'S' || k.cells[y][x].kind == 'F') && v.kind == kind && v.full()) return true; }
			return false;
		};
		bool served = k.served > servedAt;
		switch (step) {
		case 0: return walked > 3;
		case 1: return k.anywhere([](const Thing& t) { return t.kind == Thing::FOOD && t.food.f == TOMATO; });
		case 2: return onBoard(TOMATO, RAW) || k.anywhere([](const Thing& t) { return isFood(t, TOMATO, CUT); });
		case 3: return k.anywhere([](const Thing& t) { return isFood(t, TOMATO, CUT) || plateHas(t, TOMATO, CUT); });
		case 4: return k.anywhere([](const Thing& t) { return t.kind == Thing::PLATE; });
		case 5: return k.anywhere([](const Thing& t) { return plateHas(t, TOMATO, CUT); });
		case 6: {
			// a plate on a counter (or already more on it)
			for (int y = 0; y < MH; ++y)
				for (int x = 0; x < MW; ++x) if (k.cells[y][x].thing.kind == Thing::PLATE) return true;
			return k.anywhere([](const Thing& t) { return plateOf(t, SALAD); });
		}
		case 7: return k.anywhere([](const Thing& t) { return isFood(t, CABBAGE, CUT) || plateHas(t, CABBAGE, CUT); });
		case 8: return k.anywhere([](const Thing& t) { return plateOf(t, SALAD); }) || served;
		case 9: return served;
		case 10: return potsFull(Vessel::POT) || k.anywhere([](const Thing& t) { return plateOf(t, TOMATO_SOUP); }) || served;
		case 11: return k.anywhere([](const Thing& t) { return plateOf(t, TOMATO_SOUP); }) || served;
		case 12: return served;
		case 13: return potsFull(Vessel::PAN) || k.anywhere([](const Thing& t) { return isFood(t, MEAT, COOKED) || plateHas(t, MEAT, COOKED); }) || served;
		case 14: return k.anywhere([](const Thing& t) { return plateOf(t, BURGER); }) || served;
		case 15: return served;
		case 16: return dashed;
		case 17: return switched;
		case 18: return aPressed;
		default: return true;
		}
		(void)c;
	}
};

} // namespace bistro
