# Naughty Bear Recompiled - PS5 native homebrew (title PPSA99862). See README.md.
#
#   make                          build dist/PPSA99862
#   make deploy PS5_HOST=<ip>     build, then copy it to /data/homebrew/PPSA99862 over FTP
#   make launch PS5_HOST=<ip>     start it (sends a launcher payload to port 9021)
#   make close PS5_HOST=<ip>      close it
#   make undeploy PS5_HOST=<ip>   remove it from the console
SHELL := /bin/bash
.DEFAULT_GOAL := app

TITLE_ID ?= PPSA99862
PS5_HOST ?=
FTP_PORT ?= 2121
PS5_ELF_PORT ?= 9021
SDK := $(abspath .deps/native/ps5-payload-sdk)

.PHONY: app deploy undeploy launch close clean

app:
	@TITLE_ID=$(TITLE_ID) bash tools/build.sh

deploy:
	@PS5_HOST=$(PS5_HOST) FTP_PORT=$(FTP_PORT) bash tools/deploy.sh

undeploy:
	@PS5_HOST=$(PS5_HOST) FTP_PORT=$(FTP_PORT) bash tools/deploy.sh undeploy

launch:
	@PS5_PAYLOAD_SDK=$(SDK) bash tools/send-controller.sh launch $(TITLE_ID) "$(PS5_HOST)" $(PS5_ELF_PORT)

close:
	@PS5_PAYLOAD_SDK=$(SDK) bash tools/send-controller.sh close $(TITLE_ID) "$(PS5_HOST)" $(PS5_ELF_PORT)

clean:
	@rm -rf build dist
