#pragma once
// The systems games' artworks are fetched for (banana-scrape): a system's id -> its collection in libretro's thumbnails
// (thumbnails.libretro.com/<collection>/). Settings > Look & sound > Games artworks > Systems lists these; the fetch
// goes through them, skipping the ones set to Skip there (settings.conf's art.<id> = off). (Game & Watch has none: its covers are its own, from MAME's artwork.)
#include <map>
#include <string>

namespace banana
{
	inline const std::map<std::string, std::string>& artCollections()
	{
		static const std::map<std::string, std::string> m = {
			{ "nds", "Nintendo - Nintendo DS" }, { "gba", "Nintendo - Game Boy Advance" }, { "gbc", "Nintendo - Game Boy Color" },
			{ "gb", "Nintendo - Game Boy" }, { "nes", "Nintendo - Nintendo Entertainment System" },
			{ "snes", "Nintendo - Super Nintendo Entertainment System" }, { "megadrive", "Sega - Mega Drive - Genesis" },
			{ "fds", "Nintendo - Family Computer Disk System" }, { "mastersystem", "Sega - Master System - Mark III" },
			{ "gamegear", "Sega - Game Gear" }, { "sg-1000", "Sega - SG-1000" }, { "megacd", "Sega - Mega-CD - Sega CD" }, { "sega32x", "Sega - 32X" },
			{ "ngp", "SNK - Neo Geo Pocket" }, { "ngpc", "SNK - Neo Geo Pocket Color" }, { "wonderswan", "Bandai - WonderSwan" },
			{ "wonderswancolor", "Bandai - WonderSwan Color" }, { "atarilynx", "Atari - Lynx" }, { "virtualboy", "Nintendo - Virtual Boy" },
			{ "pokemini", "Nintendo - Pokemon Mini" }, { "c64", "Commodore - 64" }, { "zxspectrum", "Sinclair - ZX Spectrum" },
			{ "pcengine", "NEC - PC Engine - TurboGrafx 16" }, { "pcenginecd", "NEC - PC Engine CD - TurboGrafx-CD" }, { "atari2600", "Atari - 2600" },
			{ "atari7800", "Atari - 7800" }, { "colecovision", "Coleco - ColecoVision" }, { "intellivision", "Mattel - Intellivision" },
			{ "vectrex", "GCE - Vectrex" }, { "odyssey2", "Magnavox - Odyssey2" },
			{ "psx", "Sony - PlayStation" }, { "n64", "Nintendo - Nintendo 64" }, { "psp", "Sony - PlayStation Portable" },
			{ "arcade", "FBNeo - Arcade Games" }, { "neogeo", "FBNeo - Arcade Games" }, { "cps1", "FBNeo - Arcade Games" },
			{ "cps2", "FBNeo - Arcade Games" }, { "cps3", "FBNeo - Arcade Games" } };
		return m;
	}
}
