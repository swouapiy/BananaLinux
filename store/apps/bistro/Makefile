# Banana Bistro. The device build is buildroot/package/banana-ui's (with -lasound); here: the rules' test and the
# screens drawn to PNGs (no device code: BISTRO_NO_DEVICE).
CXX ?= c++
OUT ?= out

test: $(OUT)/bistro-test $(OUT)/bistro-host
	$(OUT)/bistro-test
	$(OUT)/bistro-host --smoke $(OUT)/screen

$(OUT)/bistro-test: bistro-test.cpp bistro-game.h
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O1 -Wall -Wextra -o $@ bistro-test.cpp

$(OUT)/bistro-host: bistro.cpp bistro-game.h bistro-assets.h bistro-sounds.h
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -DBISTRO_NO_DEVICE -o $@ bistro.cpp

clean:
	rm -rf $(OUT)

.PHONY: test clean
