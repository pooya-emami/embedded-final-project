CC = gcc
SUBDIRS = src/server src/relay
SERVICE_FILES = systemd/relay.service systemd/server.service
CONFIG_FILE = server.conf
INSTALL_DIR = /usr/local/bin

.PHONY: all clean install uninstall $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

install: all
	sudo mkdir -p $(INSTALL_DIR)
	# sudo cp src/server/$(CONFIG_FILE) $(INSTALL_DIR)/server.conf
	sudo install -m 755 src/server/server $(INSTALL_DIR)/server
	sudo install -m 755 src/relay/mjpeg_relay $(INSTALL_DIR)/mjpeg_relay
	
	sudo mkdir -p /etc/systemd/system
	for f in $(SERVICE_FILES); do \
		if [ -f "$$f" ]; then \
			sudo install -m 644 $$f /etc/systemd/system/; \
		fi; \
	done
	
	sudo systemctl daemon-reload
	@echo "Installation complete"
	@echo "Start: sudo systemctl start relay.service server.service"

uninstall:
	sudo rm -f $(INSTALL_DIR)/server $(INSTALL_DIR)/mjpeg_relay
	sudo rm -f $(INSTALL_DIR)/server.conf
	sudo rm -f /etc/systemd/system/relay.service /etc/systemd/system/server.service
	sudo rm -f /etc/systemd/system/api-wrapper.service /etc/systemd/system/camera.service
	sudo systemctl daemon-reload
	@echo "Uninstalled"

.PHONY: all clean install uninstall $(SUBDIRS)