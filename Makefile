CC = gcc
CXX = g++
SUBDIRS = src/server src/relay src/detection
SERVICE_FILES = systemd/relay.service systemd/server.service systemd/detection.service
INSTALL_DIR = /usr/local/bin

.PHONY: all clean install uninstall $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done
	rm -f server relay detection_server

install: all
	sudo mkdir -p $(INSTALL_DIR)
	sudo install -m 755 src/server/server $(INSTALL_DIR)/security_server
	sudo install -m 755 src/relay/mjpeg_relay $(INSTALL_DIR)/security_relay
	sudo install -m 755 src/detection/detection_server $(INSTALL_DIR)/detection_server

	sudo mkdir -p /etc/systemd/system
	for f in $(SERVICE_FILES); do \
		if [ -f "$$f" ]; then \
			sudo install -m 644 $$f /etc/systemd/system/; \
		fi; \
	done

	sudo systemctl daemon-reload
	sudo systemctl enable relay.service server.service detection.service 2>/dev/null || true
	@echo "Installation complete"

uninstall:
	sudo systemctl disable relay.service server.service detection.service 2>/dev/null || true
	sudo rm -f $(INSTALL_DIR)/security_server $(INSTALL_DIR)/security_relay $(INSTALL_DIR)/detection_server
	sudo rm -f /etc/systemd/system/relay.service /etc/systemd/system/server.service /etc/systemd/system/detection.service
	sudo systemctl daemon-reload
	@echo "Uninstalled"

.PHONY: all clean install uninstall