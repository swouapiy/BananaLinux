#pragma once
// banana-ui: the theme's menu sounds (rnds/sounds/*.wav) through ALSA. The device is opened on the first sound, kept
// playing (silence between sounds, so the RG DS Plus's amplifiers stay started) and closed after 20 quiet seconds, and
// when a game starts (release()), so nothing holds the audio device while a game wants it: the game starts at once,
// the launch's chime playing out under its start (the device then closed in the sound thread; a game that opens it
// first waits in the kernel for it, logs/sound-check.log says how often an emulator found it held). A launch's last cue
// (play(name, true)) closes it as soon as that sound has played out: on the RG DS Plus the close takes ~0.6 s (the
// aw88166 amplifiers' stop), which then runs during the launch animation's last frames instead of after them.
// A launch's cue plays over the sound before it (the "ready" chime A starts the launch with) instead of cutting it off;
// any other new sound cuts the one playing.
// The theme's sounds are loaded again when the theme changes (reload); the intro and the outro always play the menu's
// own sounds (playBase), whichever theme is chosen.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace banana
{
	class Sound
	{
	public:
		explicit Sound(const std::string& dir);
		~Sound();
		void play(const std::string& name, bool closeAfter = false);	// closeAfter: the device closed once it has played
		void playBase(const std::string& name) { play("base:" + name); }	// the menu's own sound, not the theme's
		void reload(const std::string& dir);	// the theme's sounds again (rnds::themeFiles: the theme changed)
		// close the device now (a game is starting); wait: until it is closed, else the close goes on in the sound thread
		// (a game opening the device meanwhile waits in the kernel for it, while everything before its open goes on)
		void release(bool wait = true);
		bool enabled = true;
		std::function<void(const std::string&)> onLog;	// (the sound thread's: a release's close, how long after it)
		std::atomic<float> gain{ 1.0f };	// Settings > Look & sound > Sound > Menu sounds: 1 Normal, less Quiet

	private:
		struct Clip { std::vector<int16_t> pcm; int rate = 44100, channels = 2; };
		std::map<std::string, std::shared_ptr<const Clip>> clips;	// (under m: reload swaps them while one may play)
		std::thread th;
		std::mutex m;
		std::condition_variable cv;
		std::string queued;
		bool stop = false, closeNow = false, closeAfterQueued = false;
		bool releaseTimed = false;	// release() asked at releasedAt: its close logged (under m)
		std::chrono::steady_clock::time_point releasedAt;
		void logRelease();
		void run();
	};
}
