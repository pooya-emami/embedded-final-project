CC = gcc
SUBDIRS = src/server src/relay

.PHONY: all clean install uninstall $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

# Copies binaries + unit files into place and reloads systemd.
# Run as: sudo make install
install: all
	install -m 755 src/server/server /usr/local/bin/server
	install -m 755 src/relay/mjpeg_relay /usr/local/bin/mjpeg_relay
	install -m 644 systemd/*.service /etc/systemd/system/
	systemctl daemon-reload
	@echo "Installed. Enable with: sudo systemctl enable --now webserver.service camera.service relay.service"

uninstall:
	rm -f /usr/local/bin/server /usr/local/bin/mjpeg_relay
	rm -f /etc/systemd/system/webserver.service /etc/systemd/system/camera.service /etc/systemd/system/relay.service
	systemctl daemon-reload
