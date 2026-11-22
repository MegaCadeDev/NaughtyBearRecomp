# Naughty Bear Recompiled - Switch 2 homebrew (an NRO for kaiHEN's hbmenu). See README.md.
#
#   make           build dist/switch/NaughtyBearRecomp (copy dist/switch to the SD card's root);
#                  NB_VERSION=x.y.z sets the version hbmenu shows
#   make clean     remove the build (keeps the dependencies in .deps)
SHELL := /bin/bash
.DEFAULT_GOAL := app

.PHONY: app clean

NB_VERSION ?= 1.0.0

app:
	@NB_VERSION=$(NB_VERSION) bash tools/build.sh

clean:
	rm -rf build dist
