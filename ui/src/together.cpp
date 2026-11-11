// banana-together: Play together, the Apps tile's (settings_together.cpp): the handhelds on the same Wi-Fi see each
// other, one invites another, and the two are linked until one leaves (the games they then start are played as two
// players: RetroArch's netplay, a later step). Only the handhelds' own small messages, on the Wi-Fi alone (UDP):
//   HELLO   every 2 s, to the network's broadcast address: who it is (an id, the name, BananaOS's version, what it's
//           doing: in the menu or a game's title), and whom it's linked with
//   INVITE, ACCEPT, DECLINE, LEAVE   to one handheld: an invitation, its answer, and the end of a link
//   START   to the linked one: a game started here (its system, its file, its size, the core, its title): join it?
//   ANSWER  back: "join" (it's starting the game, connected to this one's) or "stay" (it stays out)
// Nothing is answered unasked: an invitation is only shown, and only the menu, once the player said so, accepts it.
//   banana-together serve
// The menu and this talk through small files in /run/banana (the menu watches the folder):
//   together-me      the menu's: the name, the version
//   together-state   the menu's too, while a game runs: "game" and its title (none: in the menu)
//   together-peers   the handhelds seen in the last 8 s: id, address, name, version, state, game, linked with
//   together-invite  an invitation waiting for an answer: id, name (gone after 60 s, or once answered)
//   together-link    "linked" or "waiting" (for an answer to ours): id, address, name, version
//   together-news    the last thing that happened, for a banner: a number, then accepted/declined/left/gone/
//                    joining/stayout, the name
//   together-start   the linked one started a game: a number, its name, its address, then START's fields
//   together.pid     this one's process, while it runs
// and the menu tells it what to do by a datagram on /run/banana/together.sock: invite <id>, accept, decline, cancel,
// leave, start <system> TAB <file> TAB <size> TAB <core> TAB <title>, answer join|stay. Environment (the test's): TOGETHER_RUN (the folder), TOGETHER_PORT (45789), TOGETHER_ANNOUNCE ("host:port"
// a comma between them: where the HELLOs go instead of the broadcast addresses), TOGETHER_ID (the id).
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <map>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace
{
	std::string run = "/run/banana";
	int port = 45789;
	volatile sig_atomic_t stop = 0;

	double now()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::string readFile(const std::string& p)
	{
		std::ifstream f(p, std::ios::binary);
		std::stringstream s;
		s << f.rdbuf();
		return s.str();
	}

	// written whole (a temporary file renamed over it): the menu never reads half of it; "" removes it
	void writeFile(const std::string& name, const std::string& s)
	{
		std::string p = run + "/" + name;
		if (s.empty()) { unlink(p.c_str()); return; }
		std::string t = p + ".new";
		FILE* f = fopen(t.c_str(), "w");
		if (!f) return;
		fwrite(s.data(), 1, s.size(), f);
		fclose(f);
		rename(t.c_str(), p.c_str());
	}

	std::vector<std::string> split(const std::string& s, char c)
	{
		std::vector<std::string> r;
		std::stringstream in(s);
		std::string x;
		while (std::getline(in, x, c)) r.push_back(x);
		return r;
	}

	// a field of a message: no tabs or line ends in it, and not too long
	std::string clean(std::string s, size_t max = 60)
	{
		for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
		return s.size() > max ? s.substr(0, max) : s;
	}

	struct Me { std::string id, name, version, state = "menu", game; };
	struct Peer { std::string id, addr, name, version, state, game, linkedWith; double seen = 0; };

	// where the HELLOs go: the broadcast address of each network the handheld is on (Wi-Fi), or the test's list
	std::vector<sockaddr_in> announceTo()
	{
		std::vector<sockaddr_in> r;
		auto add = [&](in_addr a, int p)
		{
			sockaddr_in s{};
			s.sin_family = AF_INET; s.sin_port = htons(p); s.sin_addr = a;
			r.push_back(s);
		};
		const char* list = getenv("TOGETHER_ANNOUNCE");
		if (list && *list)
		{
			for (auto& hp : split(list, ','))
			{
				size_t c = hp.rfind(':');
				in_addr a{};
				if (c != std::string::npos && inet_pton(AF_INET, hp.substr(0, c).c_str(), &a) == 1) add(a, atoi(hp.c_str() + c + 1));
			}
			return r;
		}
		ifaddrs* ifs = nullptr;
		if (getifaddrs(&ifs) != 0) return r;
		for (ifaddrs* i = ifs; i; i = i->ifa_next)
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) && !(i->ifa_flags & IFF_LOOPBACK) &&
				(i->ifa_flags & IFF_UP))
			{
				// (an address given without its broadcast one has 0.0.0.0 there, which only reaches the handheld itself:
				// then it's the address with the netmask's host bits all set)
				in_addr b{};
				if (i->ifa_broadaddr) b = ((sockaddr_in*)i->ifa_broadaddr)->sin_addr;
				if (b.s_addr == 0 && i->ifa_netmask)
					b.s_addr = ((sockaddr_in*)i->ifa_addr)->sin_addr.s_addr | ~((sockaddr_in*)i->ifa_netmask)->sin_addr.s_addr;
				if (b.s_addr != 0) add(b, port);
			}
		freeifaddrs(ifs);
		return r;
	}

	// the id: kept on the card, so a handheld is the same one to the others from one start to the next
	std::string myId()
	{
		if (const char* e = getenv("TOGETHER_ID")) return e;
		std::string p = "/data/banana/together-id", id = readFile(p);
		id = id.substr(0, id.find('\n'));
		if (id.size() == 16) return id;
		std::random_device rd;
		char b[17];
		snprintf(b, sizeof b, "%08x%08x", rd(), rd());
		id = b;
		if (FILE* f = fopen(p.c_str(), "w")) { fprintf(f, "%s\n", b); fclose(f); }
		return id;
	}

	void onSignal(int) { stop = 1; }
}

int main(int argc, char** argv)
{
	if (argc < 2 || strcmp(argv[1], "serve") != 0)
	{
		fprintf(stderr, "banana-together serve\n");
		return 2;
	}
	if (const char* e = getenv("TOGETHER_RUN")) run = e;
	if (const char* e = getenv("TOGETHER_PORT")) port = atoi(e);
	signal(SIGTERM, onSignal);
	signal(SIGINT, onSignal);
	signal(SIGPIPE, SIG_IGN);

	int udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int one = 1;
	setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	sockaddr_in me{};
	me.sin_family = AF_INET; me.sin_port = htons(port); me.sin_addr.s_addr = htonl(INADDR_ANY);
	if (udp < 0 || bind(udp, (sockaddr*)&me, sizeof me) != 0) { perror("banana-together: udp"); return 1; }

	std::string sockPath = run + "/together.sock";
	unlink(sockPath.c_str());
	int ctl = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	sockaddr_un su{};
	su.sun_family = AF_UNIX;
	snprintf(su.sun_path, sizeof su.sun_path, "%s", sockPath.c_str());
	if (ctl < 0 || bind(ctl, (sockaddr*)&su, sizeof su) != 0) { perror("banana-together: control socket"); return 1; }
	chmod(sockPath.c_str(), 0600);
	writeFile("together.pid", std::to_string(getpid()) + "\n");

	Me self;
	self.id = myId();
	std::map<std::string, Peer> peers;
	struct Link { std::string id, addr, name, version; bool linked = false; double since = 0, linkedAt = 0; } link;	// since: last heard
	struct Invite { std::string id, addr, name; double since = 0; } invite;
	int newsNo = 0, startNo = 0;
	auto news = [&](const std::string& what, const std::string& who) { writeFile("together-news", std::to_string(++newsNo) + "\t" + what + "\t" + who + "\n"); };
	auto sendTo = [&](const std::string& addr, const std::string& msg)
	{
		sockaddr_in to{};
		to.sin_family = AF_INET; to.sin_port = htons(port);
		size_t c = addr.rfind(':');	// ("host:port": the test's handhelds, each on a port of its own)
		if (c != std::string::npos) { to.sin_port = htons(atoi(addr.c_str() + c + 1)); inet_pton(AF_INET, addr.substr(0, c).c_str(), &to.sin_addr); }
		else inet_pton(AF_INET, addr.c_str(), &to.sin_addr);
		sendto(udp, msg.data(), msg.size(), 0, (sockaddr*)&to, sizeof to);
	};
	auto line = [&](const std::string& kind) { return "BT1\t" + kind + "\t" + self.id + "\t" + clean(self.name) + "\t" + clean(self.version, 30); };
	auto writeLink = [&]()
	{
		writeFile("together-link", link.id.empty() ? "" : std::string(link.linked ? "linked" : "waiting") + "\t" + link.id + "\t" + link.addr + "\t" +
			link.name + "\t" + link.version + "\n");
	};
	auto writeInvite = [&]() { writeFile("together-invite", invite.id.empty() ? "" : invite.id + "\t" + invite.name + "\n"); };
	auto readMe = [&]()
	{
		auto f = split(readFile(run + "/together-me"), '\n');
		auto c = f.empty() ? std::vector<std::string>() : split(f[0], '\t');
		self.name = c.size() > 0 && !c[0].empty() ? c[0] : "RG DS";
		self.version = c.size() > 1 ? c[1] : "";
		auto st = split(readFile(run + "/together-state"), '\n');
		auto sc = st.empty() ? std::vector<std::string>() : split(st[0], '\t');
		self.state = sc.size() > 0 && !sc[0].empty() ? sc[0] : "menu";
		self.game = sc.size() > 1 ? sc[1] : "";
	};
	writeFile("together-peers", "");
	writeFile("together-invite", "");
	writeFile("together-link", "");

	double nextHello = 0, lastPeers = -1;
	std::string peersWritten;
	while (!stop)
	{
		double t = now();
		if (t >= nextHello)
		{
			readMe();
			std::string hello = line("HELLO") + "\t" + self.state + "\t" + clean(self.game, 80) + "\t" + (link.linked ? link.id : "") + "\n";
			for (auto& a : announceTo()) sendto(udp, hello.data(), hello.size(), 0, (sockaddr*)&a, sizeof a);
			nextHello = t + 2;
		}
		// the ones not heard from in 8 s are gone; the linked one too (and the link with it), after 15 s
		for (auto it = peers.begin(); it != peers.end();)
		{
			if (t - it->second.seen > 8) it = peers.erase(it); else ++it;
		}
		if (!link.id.empty() && link.linked && !peers.count(link.id) && t - link.since > 15)
		{
			news("gone", link.name);
			link = Link();
			writeLink();
		}
		if (!link.id.empty() && !link.linked && t - link.since > 60) { news("noanswer", link.name); link = Link(); writeLink(); }
		if (!invite.id.empty() && t - invite.since > 60) { invite = Invite(); writeInvite(); }
		std::string ps;
		for (auto& [id, p] : peers)
			ps += p.id + "\t" + p.addr + "\t" + p.name + "\t" + p.version + "\t" + p.state + "\t" + p.game + "\t" + p.linkedWith + "\n";
		if (ps != peersWritten || t - lastPeers > 5) { writeFile("together-peers", ps.empty() ? "\n" : ps); peersWritten = ps; lastPeers = t; }

		pollfd pf[2] = { { udp, POLLIN, 0 }, { ctl, POLLIN, 0 } };
		int to = (int)std::max(50.0, (nextHello - now()) * 1000);
		if (poll(pf, 2, std::min(to, 1000)) <= 0) continue;
		char buf[1024];
		if (pf[0].revents & POLLIN)
		{
			sockaddr_in from{};
			socklen_t fl = sizeof from;
			ssize_t n = recvfrom(udp, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
			if (n > 0)
			{
				buf[n] = 0;
				auto c = split(std::string(buf, n).substr(0, std::string(buf, n).find('\n')), '\t');
				char ip[INET_ADDRSTRLEN] = "";
				inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
				std::string addr = ip;
				if (getenv("TOGETHER_ANNOUNCE")) addr += ":" + std::to_string(ntohs(from.sin_port));	// (the test's: a port each)
				if (c.size() >= 5 && c[0] == "BT1" && c[2] != self.id)
				{
					const std::string& kind = c[1];
					const std::string &id = c[2], &name = c[3], &version = c[4];
					if (kind == "HELLO")
					{
						Peer& p = peers[id];
						p.id = id; p.addr = addr; p.name = name; p.version = version; p.seen = t;
						p.state = c.size() > 5 ? c[5] : "menu";
						p.game = c.size() > 6 ? c[6] : "";
						p.linkedWith = c.size() > 7 ? c[7] : "";
						// the other one left without saying (restarted): its HELLOs don't name this one any more (after a
						// moment: the first ones after an ACCEPT may have been on their way)
						if (link.linked && link.id == id)
						{
							link.since = t;
							if (p.linkedWith != self.id && t - link.linkedAt > 6) { news("left", link.name); link = Link(); writeLink(); }
						}
					}
					else if (kind == "INVITE")
					{
						// one at a time: a second asking while linked, or with one already waiting, is told no
						if (link.linked || (!invite.id.empty() && invite.id != id)) sendTo(addr, line("DECLINE") + "\n");
						else if (!link.id.empty() && link.id == id)
						{
							// both invited each other: that's a yes from both
							link.linked = true; link.since = link.linkedAt = t; link.addr = addr;
							sendTo(addr, line("ACCEPT") + "\n");
							writeLink();
							news("accepted", name);
						}
						else { invite = Invite{ id, addr, name, t }; writeInvite(); }
					}
					else if (kind == "ACCEPT" && link.id == id && !link.linked)
					{
						link.linked = true; link.since = link.linkedAt = t; link.addr = addr; link.version = version;
						writeLink();
						news("accepted", name);
					}
					else if (kind == "DECLINE" && link.id == id && !link.linked)
					{
						link = Link();
						writeLink();
						news("declined", name);
					}
					else if (kind == "START" && link.linked && link.id == id && c.size() >= 10)
					{
						// the linked one started a game: the menu asks whether to join it (its address without the
						// test's port: the game's netplay has a port of its own)
						std::string ip = addr.substr(0, addr.find(':'));
						writeFile("together-start", std::to_string(++startNo) + "\t" + name + "\t" + ip + "\t" + c[5] + "\t" + c[6] + "\t" +
							c[7] + "\t" + c[8] + "\t" + c[9] + "\n");
					}
					else if (kind == "ANSWER" && link.linked && link.id == id && c.size() >= 6)
						news(c[5] == "join" ? "joining" : "stayout", name);
					else if (kind == "LEAVE")
					{
						if (link.id == id) { link = Link(); writeLink(); news("left", name); }
						if (invite.id == id) { invite = Invite(); writeInvite(); }
					}
				}
			}
		}
		if (pf[1].revents & POLLIN)
		{
			ssize_t n = recv(ctl, buf, sizeof buf - 1, 0);
			if (n > 0)
			{
				buf[n] = 0;
				std::string cmd(buf), arg;
				size_t sp = cmd.find(' ');
				if (sp != std::string::npos) { arg = cmd.substr(sp + 1); cmd.resize(sp); }
				while (!arg.empty() && (arg.back() == '\n' || arg.back() == ' ')) arg.pop_back();
				while (!cmd.empty() && cmd.back() == '\n') cmd.pop_back();
				readMe();
				if (cmd == "invite" && peers.count(arg) && link.id.empty())
				{
					const Peer& p = peers[arg];
					link = Link{ p.id, p.addr, p.name, p.version, false, t, 0 };
					writeLink();
					if (invite.id == p.id)
					{
						// it had asked us first: answering it is a yes
						link.linked = true; link.linkedAt = t;
						sendTo(p.addr, line("ACCEPT") + "\n");
						invite = Invite(); writeInvite();
						writeLink();
						news("accepted", p.name);
					}
					else sendTo(p.addr, line("INVITE") + "\n");
				}
				else if (cmd == "accept" && !invite.id.empty() && !link.linked)
				{
					auto p = peers.find(invite.id);
					link = Link{ invite.id, invite.addr, invite.name, p != peers.end() ? p->second.version : "", true, t, t };
					sendTo(invite.addr, line("ACCEPT") + "\n");
					invite = Invite();
					writeInvite();
					writeLink();
				}
				else if (cmd == "decline" && !invite.id.empty())
				{
					sendTo(invite.addr, line("DECLINE") + "\n");
					invite = Invite();
					writeInvite();
				}
				else if (cmd == "start" && link.linked)
				{
					// (the fields as the menu wrote them, tabs between: system, file, size, core, title)
					auto f = split(arg, '\t');
					if (f.size() >= 5)
						sendTo(link.addr, line("START") + "\t" + clean(f[0]) + "\t" + clean(f[1], 300) + "\t" + clean(f[2]) + "\t" + clean(f[3]) +
							"\t" + clean(f[4], 100) + "\n");
				}
				else if (cmd == "answer" && link.linked)
					sendTo(link.addr, line("ANSWER") + "\t" + (arg == "join" ? "join" : "stay") + "\n");
				else if ((cmd == "leave" || cmd == "cancel") && !link.id.empty())
				{
					sendTo(link.addr, line("LEAVE") + "\n");
					link = Link();
					writeLink();
				}
			}
		}
	}
	// gone: the linked one told, and the files taken away (the menu shows nothing of it then)
	readMe();
	if (!link.id.empty()) sendTo(link.addr, line("LEAVE") + "\n");
	if (!invite.id.empty()) sendTo(invite.addr, line("DECLINE") + "\n");
	for (const char* f : { "together-peers", "together-invite", "together-link" }) writeFile(f, "");
	unlink(sockPath.c_str());
	writeFile("together.pid", "");
	return 0;
}
