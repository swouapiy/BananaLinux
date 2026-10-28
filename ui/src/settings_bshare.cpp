// banana-ui: Bshare, the shelf's tile. Files between this handheld and phones or computers with LocalSend on the same
// Wi-Fi (as the settings' pages: a list on the bottom panel, what is chosen explained on the top one).
//   home      a device asking to send (Accept, Location, Decline), then Receive, Send files, Name, Received files,
//             About Bshare
//   location  a folder on the cards for what is being received (instead of the usual places)
//   browse    the cards: A opens a folder, Y marks a file or a folder, START sends what is marked
//   devices   the phones and computers seen: A sends to one
//   status    the send: waiting for the other side, the files going, done; Stop
// banana-bshare does the network (bshare.cpp): its "serve" runs while Receive is on or this page is open, its "send"
// for each send. They talk through /run/banana's bshare-* files; this reads and draws them.
#include "settings.h"
#include "settings_defaults.h"
#include "settings_draw.h"
#include "nas.h"
#include "fetch/names.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using rnds::Color;

namespace banana
{
	namespace
	{
		double steadyMs()
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		std::string bsRun()
		{
			const char* e = getenv("BSHARE_RUN");	// (the headless test's)
			return e && *e ? e : "/run/banana";
		}

		std::string helper()
		{
			const char* e = getenv("BANANA_BSHARE");	// (a stand-in, for the headless test)
			return e && *e ? e : "/usr/bin/banana-bshare";
		}

		std::string readAll(const std::string& p)
		{
			std::ifstream f(p);
			std::stringstream s;
			s << f.rdbuf();
			return s.str();
		}

		std::vector<std::string> fields(const std::string& line)
		{
			std::vector<std::string> c;
			std::stringstream s(line);
			std::string x;
			while (std::getline(s, x, '\t')) c.push_back(x);
			return c;
		}

		pid_t servePid()
		{
			int p = atoi(readAll(bsRun() + "/bshare.pid").c_str());
			return p > 0 && kill(p, 0) == 0 ? p : 0;
		}

		// a program started on its own (setsid, a grandchild of nobody here: no zombie), its output dropped (it logs)
		void detached(const std::vector<std::string>& argv)
		{
			pid_t p = fork();
			if (p == 0)
			{
				setsid();
				if (fork() == 0)
				{
					int nul = ::open("/dev/null", O_RDWR);
					if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
					std::vector<char*> a;
					for (auto& s : argv) a.push_back((char*)s.c_str());
					a.push_back(nullptr);
					execv(a[0], a.data());
					_exit(127);
				}
				_exit(0);
			}
			if (p > 0) while (waitpid(p, nullptr, 0) < 0 && errno == EINTR) {}
		}

		std::string files(long long n) { return std::to_string(n) + (n == 1 ? " file" : " files"); }

		long long countFiles(const std::string& dir, int depth = 0)
		{
			long long n = 0;
			DIR* d = opendir(dir.c_str());
			if (!d) return 0;
			while (dirent* e = readdir(d))
			{
				if (e->d_name[0] == '.') continue;
				std::string p = dir + "/" + e->d_name;
				struct stat st;
				if (stat(p.c_str(), &st) != 0) continue;
				if (S_ISDIR(st.st_mode)) { if (depth < 8) n += countFiles(p, depth + 1); }
				else n++;
			}
			closedir(d);
			return n;
		}
	}

	// Receive on (Ask or Quick save): the helper serving from the start, as the menu comes up; off: stopped
	void Settings::bshareServe(bool want)
	{
		pid_t p = servePid();
		if (want && !p) detached({ helper(), "serve" });
		if (!want && p) kill(p, SIGTERM);
	}

	bool Settings::bshareReceiving() const
	{
		return getConfValue(data + "/settings.conf", "bshare_receive", "off") != "off";
	}

	struct Settings::Bshare
	{
		enum Sub { HOME, LOCATION, BROWSE, DEVICES, STATUS, SUBS };
		enum Kind { K_ACCEPT, K_LOCATION, K_DECLINE, K_RECEIVE, K_SEND, K_SENDING, K_NAME, K_RECEIVED, K_ABOUT, K_HERE, K_ITEM, K_PEER, K_STOP };
		struct Row { std::string label, value, icon; Kind kind; int index = -1; bool choice = false, mark = false, warn = false; };
		struct Item { std::string name, path; bool dir = false; long long size = 0; };
		struct Peer { std::string fp, alias, model, type; };

		Settings* ui = nullptr;
		Sub sub = HOME;
		int sel[SUBS] = {}, top[SUBS] = {};
		std::string message;
		// the request of a device asking to send (bshare-request), and where its files go (empty: the usual places)
		std::string reqId, reqFrom, reqModel, reqFirst, location;
		long long reqFiles = 0, reqBytes = 0;
		double reqSince = 0;
		std::string receiveLine;	// bshare-receive, as last read
		// browse / location: the folder shown ("" the cards), its entries, what is marked
		std::string dir;
		std::vector<Item> items;
		std::set<std::string> marked;
		std::vector<nas::Card> cards;
		// devices, and the send
		std::vector<Peer> peers;
		std::vector<std::string> sendWhat;
		std::string sendTo;	// the device chosen (shown until the helper's first word)
		pid_t sender = 0;
		std::string sendLine;
		long long received = 0;
		double lookedAt = 0;

		std::string conf() const { return ui->data + "/settings.conf"; }
		std::string receive() const { return getConfValue(conf(), "bshare_receive", "off"); }
		std::string alias() const
		{
			std::string a = getConfValue(conf(), "bshare_name", "");
			if (!a.empty()) return a;
			std::string b = readAll("/etc/banana/board");
			return b.compare(0, 4, "rgds") == 0 && b.compare(0, 9, "rgds-plus") != 0 ? "RG DS" : "RG DS Plus";
		}
		static const char* receiveName(const std::string& v) { return v == "ask" ? "Ask" : v == "auto" ? "Quick save" : "Off"; }

		void open()
		{
			sub = HOME;
			message.clear();
			cards = nas::cards(ui->romRoot);
			received = countFiles(ui->data + "/received");
			Settings::bshareServe(true);	// (seen while the page is open, to send; stopped on close if Receive is off)
			poll(true);
		}

		void close()
		{
			Settings::bshareServe(receive() != "off");
		}

		// the helper's files read again: a request, the receive's and the send's state, the devices
		bool poll(bool force = false)
		{
			double t = steadyMs();
			if (!force && t - lookedAt < 400) return false;
			lookedAt = t;
			bool changed = false;
			std::string req = readAll(bsRun() + "/bshare-request"), id, from, model, first;
			long long nf = 0, nb = 0;
			std::stringstream in(req);
			std::string l;
			while (std::getline(in, l))
			{
				auto c = fields(l);
				if (c.size() < 2) continue;
				if (c[0] == "id") id = c[1]; else if (c[0] == "from") from = c[1]; else if (c[0] == "model") model = c[1];
				else if (c[0] == "files") nf = atoll(c[1].c_str()); else if (c[0] == "bytes") nb = atoll(c[1].c_str());
				else if (c[0] == "first") first = c[1];
			}
			if (id != reqId)
			{
				reqId = id; reqFrom = from; reqModel = model; reqFirst = first; reqFiles = nf; reqBytes = nb;
				reqSince = t;
				location.clear();
				if (!id.empty() && sub != LOCATION) { sub = HOME; sel[HOME] = top[HOME] = 0; }
				if (id.empty() && sub == LOCATION) sub = HOME;	// (answered elsewhere, or a minute gone)
				changed = true;
			}
			std::string rl = readAll(bsRun() + "/bshare-receive");
			if (rl != receiveLine)
			{
				if (receiveLine.size() && rl.compare(0, 4, "done") == 0) received = countFiles(ui->data + "/received");
				receiveLine = rl;
				changed = true;
			}
			std::string sl = readAll(bsRun() + "/bshare-send");
			if (sl != sendLine) { sendLine = sl; changed = true; }
			if (sender > 0 && waitpid(sender, nullptr, WNOHANG) == sender) { sender = 0; changed = true; }
			std::vector<Peer> ps;
			std::stringstream pin(readAll(bsRun() + "/bshare-peers"));
			while (std::getline(pin, l))
			{
				auto c = fields(l);
				if (c.size() >= 7) ps.push_back({ c[0], c[4], c[5], c[6] });
			}
			if (ps.size() != peers.size() || !std::equal(ps.begin(), ps.end(), peers.begin(), [](const Peer& a, const Peer& b) { return a.fp == b.fp && a.alias == b.alias; }))
			{
				peers = ps;
				changed = true;
			}
			return changed;
		}

		void answer(bool yes)
		{
			std::string a = (yes ? "accept " : "decline ") + reqId + (yes && !location.empty() ? " " + location : "") + "\n";
			std::string p = bsRun() + "/bshare-answer", t = p + ".new";
			if (FILE* f = fopen(t.c_str(), "w")) { fputs(a.c_str(), f); fclose(f); rename(t.c_str(), p.c_str()); }
			message = yes ? "Receiving from " + reqFrom + "..." : "Declined.";
			reqId.clear();
			sub = HOME;
			sel[HOME] = top[HOME] = 0;
		}

		// ---- the folders -------------------------------------------------------------------------------------------
		void list(std::string d)	// (by value: d is often one of items' own paths, cleared below)
		{
			dir = d;
			items.clear();
			if (d.empty())
			{
				for (auto& c : cards) items.push_back({ c.label, c.root, true, 0 });
				return;
			}
			DIR* h = opendir(d.c_str());
			if (!h) { message = "That folder can't be read."; return; }
			while (dirent* e = readdir(h))
			{
				if (e->d_name[0] == '.') continue;
				Item it;
				it.name = e->d_name;
				it.path = d + "/" + it.name;
				struct stat st;
				if (stat(it.path.c_str(), &st) != 0) continue;
				it.dir = S_ISDIR(st.st_mode);
				it.size = st.st_size;
				if (!it.dir && sub == LOCATION) continue;	// (a location is a folder)
				items.push_back(it);
			}
			closedir(h);
			std::sort(items.begin(), items.end(), [](const Item& a, const Item& b)
			{
				if (a.dir != b.dir) return a.dir;
				return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
			});
			sel[sub] = top[sub] = 0;
		}

		std::string shown(const std::string& path) const
		{
			for (auto& c : cards)
				if (path.compare(0, c.root.size(), c.root) == 0) return c.label + (path.size() > c.root.size() ? path.substr(c.root.size()) : "");
			return path;
		}

		void up()
		{
			for (auto& c : cards) if (dir == c.root) { list(""); return; }
			list(fetch::parentPath(dir));
		}

		// ---- the rows ----------------------------------------------------------------------------------------------
		std::vector<Row> rows() const
		{
			std::vector<Row> r;
			auto add = [&](const std::string& label, const std::string& value, Kind k) -> Row& { r.push_back({ label, value, "", k }); return r.back(); };
			switch (sub)
			{
			case HOME:
				if (!reqId.empty())
				{
					add("Accept", "", K_ACCEPT).icon = "bshare-receive";
					add("Location", location.empty() ? "Usual places" : shown(location), K_LOCATION);
					add("Decline", "", K_DECLINE);
				}
				{ Row& x = add("Receive", receiveName(receive()), K_RECEIVE); x.choice = true; x.icon = "bshare-receive"; }
				add("Send files", "", K_SEND).icon = "bshare-send";
				if (sender > 0 || sendLine.compare(0, 7, "sending") == 0 || sendLine.compare(0, 7, "waiting") == 0)
				{
					auto c = fields(sendLine);
					add("Sending", c.size() >= 4 ? c[2] + " of " + c[3] : "...", K_SENDING);
				}
				add("Name", alias(), K_NAME);
				add("Received files", files(received), K_RECEIVED);
				add("About Bshare", "", K_ABOUT);
				break;
			case LOCATION:
				if (!dir.empty()) add("This folder", "", K_HERE).icon = "bshare-receive";
				for (size_t i = 0; i < items.size(); i++) add(items[i].name, "Folder", K_ITEM).index = (int)i;
				break;
			case BROWSE:
				for (size_t i = 0; i < items.size(); i++)
				{
					Row& x = add(items[i].name, items[i].dir ? "Folder" : fetch::bytes(items[i].size), K_ITEM);
					x.index = (int)i;
					x.mark = marked.count(items[i].path) > 0;
				}
				break;
			case DEVICES:
				for (size_t i = 0; i < peers.size(); i++)
				{
					const Peer& p = peers[i];
					std::string kind = p.type == "desktop" || p.type == "laptop" ? "Computer" : p.type == "mobile" && p.model.compare(0, 5, "RG DS") == 0 ? "Handheld" :
						p.type == "mobile" ? "Phone" : p.type == "web" ? "Browser" : "Device";
					Row& x = add(p.alias, kind, K_PEER);
					x.index = (int)i;
					x.icon = kind == "Computer" ? "bshare-computer" : kind == "Handheld" ? "bshare-handheld" : "bshare-phone";
				}
				break;
			case STATUS:
				if (sender > 0) add("Stop", "", K_STOP);
				break;
			default:
				break;
			}
			return r;
		}

		Result go(Sub s)
		{
			sub = s;
			message.clear();
			return MOVED;
		}

		Result activate(const Row& row)
		{
			switch (row.kind)
			{
			case K_ACCEPT: answer(true); return CHANGED;
			case K_DECLINE: answer(false); return CHANGED;
			case K_LOCATION: sub = LOCATION; list(""); message.clear(); return MOVED;
			case K_RECEIVE: return change(1);
			case K_SEND: sub = BROWSE; marked.clear(); list(""); message.clear(); return MOVED;
			case K_SENDING: return go(STATUS);
			case K_NAME:
				ui->startEntry(ENTRY_BSHARE, "Bshare's name", "How phones and computers see this handheld in LocalSend.", BSHARE, alias());
				return MOVED;
			case K_RECEIVED: case K_ABOUT: return NONE;
			case K_HERE: location = dir; return go(HOME);
			case K_ITEM:
			{
				const Item& it = items[row.index];
				if (it.dir) { list(it.path); return MOVED; }
				return NONE;
			}
			case K_PEER:
			{
				std::vector<std::string> a = { helper(), "send", peers[row.index].fp };
				sendTo = peers[row.index].alias;
				for (auto& w : sendWhat) a.push_back(w);
				unlink((bsRun() + "/bshare-send").c_str());
				sendLine.clear();
				pid_t p = fork();
				if (p == 0)
				{
					int nul = ::open("/dev/null", O_RDWR);
					if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
					std::vector<char*> v;
					for (auto& s : a) v.push_back((char*)s.c_str());
					v.push_back(nullptr);
					execv(v[0], v.data());
					_exit(127);
				}
				sender = p > 0 ? p : 0;
				marked.clear();
				return go(STATUS);
			}
			case K_STOP:
				if (sender > 0) kill(sender, SIGTERM);
				return CHANGED;
			}
			return NONE;
		}

		Result change(int dir)
		{
			static const char* const V[] = { "off", "ask", "auto" };
			std::string v = receive();
			int i = v == "ask" ? 1 : v == "auto" ? 2 : 0;
			i = (i + dir + 3) % 3;
			setConfValue(conf(), "bshare_receive", V[i]);
			return CHANGED;	// (the helper reads it at each request; it runs anyway while this page is open)
		}

		// START in the browser: what is marked (or the file picked) goes to a device
		Result sendMarked(const Row* cur)
		{
			sendWhat.assign(marked.begin(), marked.end());
			if (sendWhat.empty() && cur && cur->kind == K_ITEM && !items[cur->index].dir) sendWhat.push_back(items[cur->index].path);
			if (sendWhat.empty()) { message = "Mark files or folders with Y first, then START sends them."; return MOVED; }
			Settings::bshareServe(true);
			sel[DEVICES] = top[DEVICES] = 0;
			return go(DEVICES);
		}

		Result back()
		{
			switch (sub)
			{
			case HOME: ui->close(); return CLOSED;
			case LOCATION: case BROWSE:
				if (dir.empty()) return go(HOME);
				up();
				return MOVED;
			case DEVICES: sub = BROWSE; return MOVED;
			case STATUS: return go(HOME);
			default: return go(HOME);
			}
		}

		Result press(Key k)
		{
			std::vector<Row> r = rows();
			int n = (int)r.size();
			int& s = sel[sub];
			int& t = top[sub];
			if (s >= n) s = std::max(0, n - 1);
			const Row* cur = n ? &r[s] : nullptr;
			auto moveTo = [&](int to)
			{
				if (!n) return NONE;
				s = (to % n + n) % n;
				if (s < t) t = s;
				if (s >= t + ROWS) t = s - ROWS + 1;
				return MOVED;
			};
			if (k != Key::NONE && sub != STATUS) message.clear();
			switch (k)
			{
			case Key::UP: return moveTo(s - 1);
			case Key::DOWN: return moveTo(s + 1);
			case Key::L: if (!n) return NONE; s = std::max(0, s - ROWS); t = std::max(0, t - ROWS); if (s < t) t = s; return MOVED;
			case Key::R: if (!n) return NONE; s = std::min(n - 1, s + ROWS); if (s >= t + ROWS) t = s - ROWS + 1; return MOVED;
			case Key::LEFT: case Key::RIGHT:
				if (cur && cur->choice) return change(k == Key::LEFT ? -1 : 1);
				if (k == Key::LEFT && sub != HOME) return back();
				return NONE;
			case Key::B: return back();
			case Key::A: return cur ? activate(*cur) : NONE;
			case Key::Y:
				if (sub == BROWSE && cur && cur->kind == K_ITEM && !dir.empty())
				{
					const std::string& p = items[cur->index].path;
					if (!marked.erase(p)) marked.insert(p);
					return moveTo(s + 1);
				}
				return NONE;
			case Key::START:
				if (sub == BROWSE) return sendMarked(cur);	// (START, not X: away from Y's marking, the user's choice)
				return NONE;
			default:
				return NONE;
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
			int& s = sel[sub];
			int& t = top[sub];
			for (int i = 0; i < ROWS && t + i < (int)r.size(); i++)
			{
				float ry = 82 + i * 64;
				if (dy < ry || dy > ry + 56) continue;
				int at = t + i;
				if (sub == BROWSE && dx < 70 && !dir.empty()) { s = at; return press(Key::Y); }	// (the tick's place)
				if (at != s) { s = at; return MOVED; }
				if (r[at].choice) return press(dx < 320 ? Key::LEFT : Key::RIGHT);
				return press(Key::A);
			}
			if (std::fabs(cy - dy) > 40 && !r.empty())
			{
				int n = (int)r.size();
				t = std::max(0, std::min(std::max(0, n - ROWS), t + (cy < dy ? ROWS : -ROWS)));
				s = std::max(t, std::min(s, t + ROWS - 1));
				return MOVED;
			}
			return NONE;
		}

		// ---- the top panel -----------------------------------------------------------------------------------------
		struct Top { std::string label = "Bshare", title, accent, help, hint; double a = -1, b = -1; std::string aLabel, bLabel; };

		Top topPanel(const std::vector<Row>& r) const
		{
			Top t;
			const Row* cur = sel[sub] < (int)r.size() ? &r[sel[sub]] : nullptr;
			std::string me = alias();
			auto rc = fields(receiveLine);
			switch (sub)
			{
			case HOME:
				if (!cur) break;
				t.title = cur->label;
				t.accent = cur->value;
				switch (cur->kind)
				{
				case K_ACCEPT: case K_DECLINE: case K_LOCATION:
				{
					int left = std::max(0, 60 - (int)((steadyMs() - reqSince) / 1000));
					t.title = reqFrom + " wants to send";
					t.accent = files(reqFiles) + ", " + fetch::bytes(reqBytes) + "  ·  " + std::to_string(left) + " s";
					t.help = "First: " + reqFirst + ".";
					t.hint = cur->kind == K_DECLINE ? "Nothing is saved, and the other side is told." :
						!location.empty() ? "Everything goes into " + shown(location) + "." :
						cur->kind == K_LOCATION ? "A picks a folder on the cards for all of them." :
						"Games go into their system's folder, everything else into banana/received.";
					break;
				}
				case K_RECEIVE:
				{
					std::string v = receive();
					t.help = "Phones and computers with LocalSend can send files to this handheld.";
					t.hint = v == "ask" ? "Seen as " + me + " on your Wi-Fi. You accept each transfer here." :
						v == "auto" ? "Seen as " + me + ". Files are saved without asking: only on a network you trust." :
						"Not seen by other devices.";
					if (rc.size() >= 6 && rc[0] == "receiving")
						t.hint = "Receiving from " + rc[1] + ": " + rc[2] + " of " + rc[3] + " files, " + fetch::bytes(atoll(rc[4].c_str())) + " of " + fetch::bytes(atoll(rc[5].c_str())) + ".";
					break;
				}
				case K_SEND: t.help = "Pick files or folders on the cards, then send them to a phone or computer with LocalSend open."; break;
				case K_SENDING: t.help = "A shows the send."; break;
				case K_NAME: t.help = "How phones and computers see this handheld in LocalSend. A changes it."; break;
				case K_RECEIVED:
					t.help = "Games go straight into their system's folder. Everything else goes into banana/received on the card.";
					if (rc.size() >= 4 && rc[0] == "done") t.hint = "Last: " + files(atoll(rc[2].c_str())) + " from " + rc[1] + ".";
					break;
				case K_ABOUT:
					t.title = "About Bshare";
					t.help = "Bshare works with LocalSend, the free and open-source app by Tien Do Nam and its contributors (localsend.org). "
						"It is Banana's own small version of LocalSend's open protocol: get LocalSend on your phone or computer, on the same Wi-Fi.";
					break;
				default: break;
				}
				break;
			case LOCATION:
				t.label = "Location";
				t.title = dir.empty() ? "The cards" : shown(dir);
				t.help = "A opens a folder. This folder: everything being received goes here.";
				break;
			case BROWSE:
				t.label = "Send files";
				t.title = cur ? cur->label : dir.empty() ? "The cards" : shown(dir);
				t.accent = cur ? cur->value : "";
				t.help = "A opens a folder. Y marks a file or a folder. START sends what is marked.";
				if (!marked.empty()) t.hint = std::to_string(marked.size()) + " marked.";
				break;
			case DEVICES:
				t.label = "Send files";
				t.title = cur ? cur->label : "Looking for devices";
				t.accent = cur ? cur->value : "";
				t.help = "Phones and computers with LocalSend open, on the same Wi-Fi. They ask there before taking the files.";
				break;
			case STATUS:
			{
				auto c = fields(sendLine);
				t.label = "Send files";
				std::string who = c.size() >= 2 ? c[1] : sendTo;
				std::string what = c.empty() ? (sender > 0 ? "waiting" : "") : c[0];
				t.title = what == "waiting" ? "Waiting for " + who : what == "sending" ? "Sending to " + who : what == "done" ? "Sent to " + who :
					what == "declined" ? who + " said no" : what == "busy" ? who + " is busy" : what == "stopped" ? "Stopped" : what == "partly" ? "Partly sent" : what.empty() ? "" : "It didn't work";
				if (c.size() >= 6) t.accent = c[2] + " of " + c[3] + " files";
				t.help = what == "waiting" ? "The other side asks its user first." : what == "sending" ? "You can close this page: it carries on." :
					what == "done" ? "All the files arrived." : what == "declined" ? "Nothing was sent." : what == "error" ? "The device couldn't be reached. Is LocalSend open, on the same Wi-Fi?" : "";
				if (c.size() >= 6 && atoll(c[5].c_str()) > 0)
				{
					t.a = (double)atoll(c[4].c_str()) / (double)atoll(c[5].c_str());
					t.aLabel = fetch::bytes(atoll(c[4].c_str())) + " of " + fetch::bytes(atoll(c[5].c_str()));
				}
				break;
			}
			default: break;
			}
			if (!message.empty()) t.help = message;
			return t;
		}

		std::string header() const
		{
			switch (sub)
			{
			case HOME: return reqId.empty() ? "Bshare" : "A transfer";
			case LOCATION: return dir.empty() ? "Location" : shown(dir);
			case BROWSE: return dir.empty() ? "Send files" : shown(dir);
			case DEVICES: return "Send " + files((long long)sendWhat.size()) + " to";
			case STATUS: return "Sending";
			default: return "";
			}
		}

		std::string emptyText() const
		{
			if (sub == DEVICES) return "Looking... Open LocalSend on the other device.";
			if (sub == BROWSE || sub == LOCATION) return "An empty folder.";
			return "";
		}

		std::vector<Settings::Chip> chips(const std::vector<Row>& r) const
		{
			const Row* cur = sel[sub] < (int)r.size() ? &r[sel[sub]] : nullptr;
			std::vector<Settings::Chip> cs = { { Key::B, "B", sub == HOME ? "Close" : "Back" } };
			std::string a;
			switch (sub)
			{
			case HOME:
				a = !cur ? "" : cur->kind == K_ACCEPT ? "Accept" : cur->kind == K_DECLINE ? "Decline" : cur->kind == K_RECEIVE ? "Change" :
					cur->kind == K_NAME ? "Edit" : cur->kind == K_RECEIVED || cur->kind == K_ABOUT ? "" : "Open";
				break;
			case LOCATION: a = !cur ? "" : cur->kind == K_HERE ? "Choose" : "Open"; break;
			case BROWSE:
				if (!dir.empty()) cs.push_back({ Key::Y, "Y", "Mark" });
				cs.push_back({ Key::START, "START", "Send" });
				a = cur && items[cur->index].dir ? "Open" : "";
				break;
			case DEVICES: a = cur ? "Send" : ""; break;
			case STATUS: a = cur ? "Stop" : ""; break;
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
			be->beginScreen(0);
			ui->box("screen", false, 0, 0, 640, 480);
			ui->box("hero", false, 18, 20, 604, 436);
			ui->icon("bshare", 50, 56, 3);
			ui->text(ui->fit(t.label, true, 17, 420), true, 17, 170, 62, p.muted, true, 1.4f);
			ui->text(ui->fit(t.title.empty() ? " " : t.title, true, 30, 420), true, 30, 170, 88, p.ink);
			if (!t.accent.empty()) ui->text(ui->fit(t.accent, true, 22, 420), true, 22, 170, 130, p.accent);
			ui->wrapped(t.help, 20, 50, 196, 540, t.hint.empty() && t.a < 0 ? 10 : 3, message.empty() ? p.muted : p.ink);
			if (!t.hint.empty()) ui->wrapped(t.hint, 20, 50, Settings::HINT_TOP, 540, 5, p.ink);
			if (t.a >= 0)
			{
				ui->meter(50, 316, 540, t.a);
				ui->text(ui->fit(t.aLabel, false, 16, 540), false, 16, 50, 338, p.muted);
			}
			be->endScreen();

			be->beginScreen(1);
			ui->box("screen", false, 0, 0, 640, 480);
			ui->box("bubble", false, 18, 14, 604, 54);
			std::string count = r.empty() ? "" : std::to_string(sel[sub] + 1) + " / " + std::to_string(r.size());
			if (sub == BROWSE && !marked.empty()) count = std::to_string(marked.size()) + " marked";
			float cw = ui->text(count, true, 17, 0, 0, p.muted, false, 0, true);
			ui->text(ui->fit(header(), true, 22, 540 - cw), true, 22, 38, 28, p.ink);
			ui->text(count, true, 17, 602 - cw, 31, p.muted);
			int& s = sel[sub];
			int& tp = top[sub];
			if (s >= (int)r.size()) s = std::max(0, (int)r.size() - 1);
			if (s < tp) tp = s;
			if (s >= tp + ROWS) tp = s - ROWS + 1;
			for (int i = 0; i < ROWS && tp + i < (int)r.size(); i++)
			{
				const Row& row = r[tp + i];
				bool on = tp + i == s;
				float y = 82 + i * 64;
				ui->box(on ? "pick" : "slot", on, 18, y, 604, 56);
				Color c = on ? p.onPick : p.muted;
				Color vc = row.warn ? (on ? p.onPickAccent : p.accent) : c;
				float lx = 38;
				if (row.mark) { ui->tick(38, y + 21, on ? p.onPick : p.accent); lx = 62; }
				else if (!row.icon.empty()) { ui->icon(row.icon, 34, y + 12, 2); lx = 76; }
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
			if (r.empty()) ui->text(emptyText(), true, 20, 38, 98, p.muted);
			ui->drawChips(chips(r));
			be->endScreen();
		}

		bool update(double)
		{
			return poll();
		}

		int wake(double) const
		{
			// the files are looked at twice a second while the page is open: a request's countdown, the send's progress
			return 500;
		}

		Result entered(const std::string& text)
		{
			std::string v = text;
			while (!v.empty() && v.back() == ' ') v.pop_back();
			setConfValue(conf(), "bshare_name", v);
			// (the helper says its name as it starts: started again with the new one)
			pid_t p = servePid();
			if (p) { kill(p, SIGTERM); for (int i = 0; i < 20 && kill(p, 0) == 0; i++) usleep(50000); }
			Settings::bshareServe(true);
			return CHANGED;
		}
	};

	void Settings::openBshare()
	{
		if (!bs)
		{
			bs = std::make_shared<Bshare>();
			bs->ui = this;
		}
		mOpen = true;
		page = BSHARE;
		bs->open();
	}

	void Settings::closeBshare() { if (bs) bs->close(); }
	Settings::Result Settings::pressBshare(Key k) { return bs ? bs->press(k) : NONE; }
	Settings::Result Settings::touchBshare(float cx, float cy) { return bs ? bs->touch(cx, cy) : NONE; }
	void Settings::renderBshare() { if (bs) bs->render(); }
	bool Settings::updateBshare(double t) { return bs && bs->update(t); }
	int Settings::bshareWake(double t) const { return bs && page == BSHARE ? bs->wake(t) : -1; }

	Settings::Result Settings::finishBshareEntry()
	{
		std::string text = pw;
		pw.clear();
		page = BSHARE;
		message.clear();
		return bs ? bs->entered(text) : MOVED;
	}
}
