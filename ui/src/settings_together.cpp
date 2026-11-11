// banana-ui: Play together, the Apps tile's. The handhelds on the same Wi-Fi, seen by their names (the DS name), one
// invited, and the two linked until one leaves; the games then started are played as two players (a later step:
// RetroArch's netplay, the other handheld asked each time). As the settings' pages: a list on the bottom panel, what
// is chosen explained on the top one. One list:
//   an invitation waiting: Accept, Decline          linked: Playing with <name>, Leave
//   waiting for an answer: Cancel                     then the handhelds seen (A invites one), then Seen by others
// banana-together does the network (together.cpp): it runs while this page is open, while Seen by others is Always,
// and while a link is on; they talk through /run/banana's together-* files and its together.sock.
#include "settings.h"
#include "settings_defaults.h"
#include "settings_draw.h"
#include "parental.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using rnds::Color;

namespace banana
{
	namespace
	{
		std::string tgRun()
		{
			const char* e = getenv("TOGETHER_RUN");	// (the test's stand-in)
			return e && *e ? e : "/run/banana";
		}
		std::string tgHelper()
		{
			const char* e = getenv("BANANA_TOGETHER");
			return e && *e ? e : "/usr/bin/banana-together";
		}
		std::string tgRead(const std::string& name)
		{
			std::ifstream f(tgRun() + "/" + name);
			std::stringstream s;
			s << f.rdbuf();
			return s.str();
		}
		std::vector<std::string> tgFields(const std::string& line)
		{
			std::vector<std::string> c;
			std::stringstream in(line);
			std::string x;
			while (std::getline(in, x, '\t')) c.push_back(x);
			return c;
		}
		// the helper running: the process its together.pid names, if it's still banana-together
		pid_t tgPid()
		{
			pid_t p = atoi(tgRead("together.pid").c_str());
			if (p <= 0 || kill(p, 0) != 0) return 0;
			std::ifstream f("/proc/" + std::to_string(p) + "/cmdline");
			std::string a0;
			std::getline(f, a0, '\0');
			return a0.find("banana-together") != std::string::npos ? p : 0;
		}
		// a word to the helper, on its socket
		void tgSay(const std::string& cmd)
		{
			int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
			if (s < 0) return;
			sockaddr_un a{};
			a.sun_family = AF_UNIX;
			snprintf(a.sun_path, sizeof a.sun_path, "%s/together.sock", tgRun().c_str());
			sendto(s, cmd.data(), cmd.size(), 0, (sockaddr*)&a, sizeof a);
			::close(s);
		}
	}

	// the helper on or off; it's started on its own (setsid, its output in together.log), as banana-bshare is
	void Settings::togetherServe(bool want)
	{
		pid_t p = tgPid();
		if (want && !p)
		{
			pid_t c = fork();
			if (c == 0)
			{
				setsid();
				int null = ::open("/dev/null", O_RDWR);
				if (null >= 0) dup2(null, 0);
				// (its output: what became of each link, and why, for the logs)
				int log = ::open((tgRun() + "/together.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (log < 0) log = null;
				if (log >= 0) { dup2(log, 1); dup2(log, 2); }
				std::string h = tgHelper();
				execl(h.c_str(), h.c_str(), "serve", (char*)nullptr);
				_exit(127);
			}
		}
		if (!want && p) kill(p, SIGTERM);
	}

	void Settings::togetherSay(const std::string& cmd) { tgSay(cmd); }
	std::string Settings::togetherFile(const std::string& name) { return tgRead(name); }

	// Seen by others: Always, the helper from the start (the menu then shows an invitation as a banner)
	bool Settings::togetherAlways() const
	{
		return getConfValue(data + "/settings.conf", "together_visible", "open") == "always";
	}

	struct Settings::Together
	{
		enum Kind { K_ACCEPT, K_DECLINE, K_LINKED, K_LEAVE, K_WAITING, K_CANCEL, K_PEER, K_VISIBLE, K_JOIN, K_STAYOUT, K_DROPIN };
		struct Row { std::string label, value, icon; Kind kind; int index = -1; bool choice = false, warn = false; };
		struct Peer { std::string id, name, version, state, game, linkedWith; };

		Settings* ui = nullptr;
		int sel = 0, top = 0;
		std::string message;
		std::vector<Peer> peers;
		std::string inviteId, inviteName;		// together-invite
		std::string linkState, linkId, linkName;	// together-link: "linked" or "waiting"
		std::string linkDoing, linkGame;		// the linked one's HELLO: "hosting" (a game to join) and its title
		std::string seen;						// the files as last read (a change redraws)
		// the friend started a game (together-start): from whom, its address, the game's system, file, size, core, title;
		// the game found here for it ("": none), and why not
		struct Ask { std::string from, addr, system, file, core, title, path; long long size = 0; } ask;
		bool asking = false;
		double lookedAt = 0;

		std::string conf() const { return ui->data + "/settings.conf"; }
		std::string myName() const { return ui->playerName(); }
		std::string myVersion() const { return ui->version.empty() ? "dev" : ui->version; }

		// what this handheld tells the others: its name, version, and that it's in the menu
		void writeMe() const
		{
			std::string p = tgRun() + "/together-me", t = p + ".new";
			std::ofstream(t) << myName() << '\t' << myVersion() << "\tmenu\t\n";
			rename(t.c_str(), p.c_str());
		}

		void open()
		{
			message.clear();
			sel = top = 0;
			if (!ui->wifiOn()) message = "Wi-Fi is off: switch it on in Network first.";
			writeMe();
			Settings::togetherServe(true);	// (seen while the page is open)
			poll(true);
		}

		void close()
		{
			// the helper stays for a link, or when it's to be seen always
			poll(true);
			Settings::togetherServe(ui->togetherAlways() || !linkState.empty());
		}

		// the helper's files read again (twice a second while the page is open); true when something changed
		bool poll(bool force = false)
		{
			double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
			if (!force && t - lookedAt < 400) return false;
			lookedAt = t;
			std::string ps = tgRead("together-peers"), iv = tgRead("together-invite"), lk = tgRead("together-link");
			std::string all = ps + "|" + iv + "|" + lk;
			if (all == seen) return false;
			seen = all;
			auto ic = tgFields(iv.substr(0, iv.find('\n')));
			inviteId = ic.size() >= 2 ? ic[0] : ""; inviteName = ic.size() >= 2 ? ic[1] : "";
			auto lc = tgFields(lk.substr(0, lk.find('\n')));
			linkState = lc.size() >= 4 ? lc[0] : ""; linkId = lc.size() >= 4 ? lc[1] : ""; linkName = lc.size() >= 4 ? lc[3] : "";
			peers.clear();
			linkDoing.clear(); linkGame.clear();
			std::stringstream in(ps);
			std::string l;
			while (std::getline(in, l))
			{
				auto c = tgFields(l);
				if (c.size() < 6) continue;
				Peer p{ c[0], c[2], c[3], c[4], c[5], c.size() > 6 ? c[6] : "" };
				if (p.id != linkId) peers.push_back(p);	// (the linked one has its own row)
				else { linkDoing = p.state; linkGame = p.game; }
			}
			std::sort(peers.begin(), peers.end(), [](const Peer& a, const Peer& b) { return a.name < b.name; });
			return true;
		}

		// the friend's game (together-start's fields after its number): who, where, which, and the game here for it
		void fillAsk(const std::vector<std::string>& s)
		{
			auto f = [&](size_t i) { return i < s.size() ? s[i] : std::string(); };
			ask.from = f(0); ask.addr = f(1); ask.system = f(2); ask.file = f(3); ask.size = atoll(f(4).c_str()); ask.core = f(5); ask.title = f(6);
			ask.path = ui->findGame ? ui->findGame(ask.system, ask.file, ask.size) : "";
		}

		static std::string doing(const Peer& p)
		{
			if (!p.linkedWith.empty()) return "Playing with someone";
			if (p.state == "game" || p.state == "hosting") return p.game.empty() ? "In a game" : p.game;
			return "In the menu";
		}

		// ---- the rows ----------------------------------------------------------------------------------------------
		std::vector<Row> rows() const
		{
			std::vector<Row> r;
			auto add = [&](const std::string& label, const std::string& value, Kind k) -> Row& { r.push_back({ label, value, "", k }); return r.back(); };
			if (asking)
			{
				// the friend's game: join it, or stay out (always asked); not here: only OK
				if (!ask.path.empty()) { add("Join " + ask.title, "Player 2", K_JOIN).icon = "bshare-accept"; add("Stay out", "", K_STAYOUT).icon = "bshare-decline"; }
				else add("OK", "", K_STAYOUT);
				return r;
			}
			if (!inviteId.empty() && linkState != "linked")
			{
				add("Accept " + inviteName, "", K_ACCEPT).icon = "bshare-accept";
				add("Decline", "", K_DECLINE).icon = "bshare-decline";
			}
			if (linkState == "linked")
			{
				add("Playing with " + linkName, linkDoing == "hosting" && !linkGame.empty() ? linkGame : "Linked", K_LINKED).icon = "bshare-handheld";
				// the friend's game, running: joined late, as when asked
				if (linkDoing == "hosting" && !linkGame.empty()) add("Join " + linkGame, "Player 2", K_DROPIN).icon = "bshare-accept";
				add("Leave", "", K_LEAVE).icon = "bshare-decline";
			}
			else if (linkState == "waiting")
			{
				add("Waiting for " + linkName, "Asked", K_WAITING).icon = "bshare-handheld";
				add("Cancel", "", K_CANCEL).icon = "bshare-decline";
			}
			for (size_t i = 0; i < peers.size(); i++)
			{
				bool other = peers[i].version != myVersion();
				Row& x = add(peers[i].name, other ? "Other version" : doing(peers[i]), K_PEER);
				x.index = (int)i;
				x.icon = "bshare-handheld";
				x.warn = other;
			}
			add("Seen by others", ui->togetherAlways() ? "Always" : "While open", K_VISIBLE).choice = true;
			return r;
		}

		Result activate(const Row& row)
		{
			switch (row.kind)
			{
			case K_ACCEPT: tgSay("accept"); message.clear(); return MOVED;
			case K_DECLINE: tgSay("decline"); message.clear(); return MOVED;
			case K_LEAVE: tgSay("leave"); message.clear(); return MOVED;
			case K_CANCEL: tgSay("cancel"); message.clear(); return MOVED;
			case K_PEER:
			{
				const Peer& p = peers[row.index];
				if (p.version != myVersion()) { message = p.name + " has BananaOS " + p.version + ", this one " + myVersion() + ": update both to play together."; return MOVED; }
				if (!linkState.empty()) { message = "Leave " + linkName + " first."; return MOVED; }
				if (!p.linkedWith.empty()) { message = p.name + " is playing with someone already."; return MOVED; }
				tgSay("invite " + p.id);
				message.clear();
				return MOVED;
			}
			case K_VISIBLE: return change(1);
			case K_DROPIN:
			{
				// the game's details: its start, as the friend sent it (together-start), if it's this game's
				std::string st = tgRead("together-start");
				auto f = tgFields(st.substr(0, st.find('\n')));
				if (f.size() < 8 || f[7] != linkGame) { message = linkName + "'s game can't be joined from here. Ask them to start it again."; return MOVED; }
				fillAsk(std::vector<std::string>(f.begin() + 1, f.end()));
				if (ask.path.empty())
				{
					message = ask.title + " isn't on this handheld" + (parental::on(ui->data) ? ", or isn't one of the allowed games." : " (" + ask.file + ").");
					return MOVED;
				}
				[[fallthrough]];
			}
			case K_JOIN:
				tgSay("answer join");
				asking = false;
				ui->joinPath = ask.path; ui->joinAddr = ask.addr; ui->joinCore = ask.core;
				ui->close();
				return JOIN;
			case K_STAYOUT:
				tgSay("answer stay");
				asking = false;
				sel = top = 0;
				return MOVED;
			default: return NONE;
			}
		}

		Result change(int)
		{
			setConfValue(conf(), "together_visible", ui->togetherAlways() ? "open" : "always");
			return CHANGED;	// (the page is open: the helper runs; closing it keeps or stops it, by this)
		}

		Result press(Key k)
		{
			std::vector<Row> r = rows();
			int n = (int)r.size();
			if (sel >= n) sel = std::max(0, n - 1);
			const Row* cur = n ? &r[sel] : nullptr;
			auto moveTo = [&](int to)
			{
				if (!n) return NONE;
				sel = (to % n + n) % n;
				if (sel < top) top = sel;
				if (sel >= top + ROWS) top = sel - ROWS + 1;
				return MOVED;
			};
			if (k != Key::NONE && k != Key::A) message.clear();
			switch (k)
			{
			case Key::UP: return moveTo(sel - 1);
			case Key::DOWN: return moveTo(sel + 1);
			case Key::L: if (!n) return NONE; sel = std::max(0, sel - ROWS); top = std::max(0, top - ROWS); if (sel < top) top = sel; return MOVED;
			case Key::R: if (!n) return NONE; sel = std::min(n - 1, sel + ROWS); if (sel >= top + ROWS) top = sel - ROWS + 1; return MOVED;
			case Key::LEFT: case Key::RIGHT:
				if (cur && cur->choice) return change(k == Key::LEFT ? -1 : 1);
				return NONE;
			case Key::B:
				if (asking) { for (const Row& x : r) if (x.kind == K_STAYOUT) return activate(x); }	// (B: stays out)
				ui->close();
				return CLOSED;
			case Key::A: return cur ? activate(*cur) : NONE;
			default: return NONE;
			}
		}

		Result touch(float cx, float cy)
		{
			float dx = ui->downX, dy = ui->downY;
			std::vector<Row> r = rows();
			if (dy >= 418 && dy <= 466)
			{
				Key k = ui->chipAt(chips(r), dx);
				return k == Key::NONE ? NONE : press(k);
			}
			for (int i = 0; i < ROWS && top + i < (int)r.size(); i++)
			{
				float ry = 82 + i * 64;
				if (dy < ry || dy > ry + 56) continue;
				int at = top + i;
				if (at != sel) { sel = at; return MOVED; }
				if (r[at].choice) return press(dx < 320 ? Key::LEFT : Key::RIGHT);
				return press(Key::A);
			}
			if (std::fabs(cy - dy) > 40 && !r.empty())
			{
				int n = (int)r.size();
				top = std::max(0, std::min(std::max(0, n - ROWS), top + (cy < dy ? ROWS : -ROWS)));
				sel = std::max(top, std::min(sel, top + ROWS - 1));
				return MOVED;
			}
			return NONE;
		}

		// ---- the top panel -----------------------------------------------------------------------------------------
		struct Top { std::string title, accent, help, hint; };

		Top topPanel(const std::vector<Row>& r) const
		{
			Top t;
			const Row* cur = sel < (int)r.size() ? &r[sel] : nullptr;
			if (cur) { t.title = cur->label; t.accent = cur->value; }
			if (!cur) return t;
			switch (cur->kind)
			{
			case K_ACCEPT: t.help = inviteName + " wants to play together. Accept, and the games either of you starts can be played as two players."; break;
			case K_DECLINE: t.help = "Says no to " + inviteName + "."; break;
			case K_LINKED: t.help = "You and " + linkName + " are linked. When one of you starts a game, the other is asked to join as player 2."; break;
			case K_DROPIN: t.help = linkName + " is playing " + linkGame + ". Join as player 2: it starts here, connected to " + linkName + "'s game."; break;
			case K_LEAVE: t.help = "Ends the link with " + linkName + ". Each plays alone again."; break;
			case K_WAITING: t.help = linkName + " is asked. The link starts when they accept."; break;
			case K_CANCEL: t.help = "Takes the invitation back."; break;
			case K_PEER:
			{
				const Peer& p = peers[cur->index];
				t.help = p.version != myVersion()
					? p.name + " has BananaOS " + p.version + " and this handheld " + myVersion() + ". Both need the same version to play together."
					: "A invites " + p.name + " to play together.";
				break;
			}
			case K_JOIN: t.help = ask.from + " started " + ask.title + ". Join as player 2: it starts here, connected to " + ask.from + "'s game."; break;
			case K_STAYOUT:
				if (ask.path.empty()) t.title = ask.title + " isn't on this handheld";
				t.help = !ask.path.empty() ? ask.from + " plays " + ask.title + " alone. You stay in the menu."
					: ask.from + " started " + ask.title + ", but it isn't on this handheld" +
						(parental::on(ui->data) ? ", or isn't one of the allowed games." : " (" + ask.file + ").") + " " + ask.from + " plays alone.";
				break;
			case K_VISIBLE:
				t.help = "When the other handhelds on this Wi-Fi see you.";
				t.hint = ui->togetherAlways() ? "Always: an invitation comes as a notification, even with this page closed."
					: "Only while Play together is open.";
				break;
			}
			if (peers.empty() && linkState.empty() && cur->kind == K_VISIBLE)
				t.hint = "Looking for handhelds on this Wi-Fi. Open Play together on the other one too.";
			if (!message.empty()) t.help = message;
			return t;
		}

		std::vector<Settings::Chip> chips(const std::vector<Row>& r) const
		{
			const Row* cur = sel < (int)r.size() ? &r[sel] : nullptr;
			std::vector<Settings::Chip> cs = { { Key::B, "B", asking ? (ask.path.empty() ? "OK" : "Stay out") : "Close" } };
			std::string a;
			if (cur)
				switch (cur->kind)
				{
				case K_ACCEPT: a = "Accept"; break;
				case K_DECLINE: a = "Decline"; break;
				case K_LEAVE: a = "Leave"; break;
				case K_CANCEL: a = "Cancel"; break;
				case K_PEER: a = "Invite"; break;
				case K_VISIBLE: a = "Change"; break;
				case K_JOIN: case K_DROPIN: a = "Join"; break;
				case K_STAYOUT: a = ask.path.empty() ? "OK" : "Stay out"; break;
				default: break;
				}
			if (!a.empty()) cs.push_back({ Key::A, "A", a, true });
			return cs;
		}

		// ---- drawing -----------------------------------------------------------------------------------------------
		void render()
		{
			SPal p = pal(ui->light);
			std::vector<Row> r = rows();
			Top t = topPanel(r);
			rnds::Backend* be = ui->be;
			ui->beginScreen(0);
			ui->box("screen", false, 0, 0, 640, 480);
			ui->box("hero", false, 18, 20, 604, 436);
			ui->icon("together", 50, 56, 3);
			ui->text("Play together", true, 17, 170, 62, p.muted, true, 1.4f);
			ui->text(ui->fit(t.title.empty() ? " " : t.title, true, 30, 420), true, 30, 170, 88, p.ink);
			if (!t.accent.empty()) ui->text(ui->fit(t.accent, true, 22, 420), true, 22, 170, 130, p.accent);
			ui->wrapped(t.help, 20, 50, 196, 540, t.hint.empty() ? 10 : 3, message.empty() ? p.muted : p.ink);
			if (!t.hint.empty()) ui->wrapped(t.hint, 20, 50, Settings::HINT_TOP, 540, 5, p.ink);
			be->endScreen();

			ui->beginScreen(1);
			ui->box("screen", false, 0, 0, 640, 480);
			ui->box("bubble", false, 18, 14, 604, 54);
			std::string count = std::to_string(sel + 1) + " / " + std::to_string(r.size());
			float cw = ui->text(count, true, 17, 0, 0, p.muted, false, 0, true);
			ui->text(ui->fit("Seen as " + myName(), true, 22, 540 - cw), true, 22, 38, 28, p.ink);
			ui->text(count, true, 17, 602 - cw, 31, p.muted);
			if (sel >= (int)r.size()) sel = std::max(0, (int)r.size() - 1);
			if (sel < top) top = sel;
			if (sel >= top + ROWS) top = sel - ROWS + 1;
			for (int i = 0; i < ROWS && top + i < (int)r.size(); i++)
			{
				const Row& row = r[top + i];
				bool on = top + i == sel;
				float y = 82 + i * 64;
				ui->box(on ? "pick" : "slot", on, 18, y, 604, 56);
				Color c = on ? p.onPick : p.muted;
				Color vc = row.warn ? (on ? p.onPickAccent : p.accent) : c;
				float lx = 38;
				if (!row.icon.empty()) { ui->icon(row.icon, 34, y + 12, 2); lx = 76; }
				float vw = row.value.empty() ? 0 : ui->text(row.value, true, 19, 0, 0, vc, false, 0, true);
				float room = (row.choice ? 556 - vw - 16 : 586 - vw) - lx - 16;
				ui->text(ui->fit(row.label, true, 20, room), true, 20, lx, y + 16, c);
				if (row.value.empty()) continue;
				std::string v = ui->fit(row.value, true, 19, row.choice ? 300 : 330);
				vw = ui->text(v, true, 19, 0, 0, vc, false, 0, true);
				if (row.choice)
				{
					ui->arrow(true, 576, y + 21, vc);
					ui->text(v, true, 19, 566 - vw, y + 17, vc);
					ui->arrow(false, 556 - vw - 2 - std::round(13 / 2.0f + 1), y + 21, vc);
				}
				else ui->text(v, true, 19, 586 - vw, y + 17, vc);
			}
			ui->drawChips(chips(r));
			be->endScreen();
		}

		bool update(double) { return poll(); }
		int wake(double) const { return 500; }	// (the handhelds come and go: looked at twice a second)
	};

	void Settings::openTogether()
	{
		if (!tg)
		{
			tg = std::make_shared<Together>();
			tg->ui = this;
		}
		mOpen = true;
		page = TOGETHER;
		tg->open();
	}

	void Settings::togetherAsk(const std::vector<std::string>& s)
	{
		openTogether();
		tg->fillAsk(s);
		tg->asking = true;
		tg->sel = tg->top = 0;
		tg->message.clear();
	}

	void Settings::closeTogether() { if (tg) tg->close(); }	// (from Settings' close: the page is LIST by then)
	Settings::Result Settings::pressTogether(Key k) { return tg ? tg->press(k) : NONE; }
	Settings::Result Settings::touchTogether(float cx, float cy) { return tg ? tg->touch(cx, cy) : NONE; }
	void Settings::renderTogether() { if (tg) tg->render(); }
	bool Settings::updateTogether(double t) { return tg && tg->update(t); }
	int Settings::togetherWake(double t) const { return tg && page == TOGETHER ? tg->wake(t) : -1; }
}
