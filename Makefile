CC = gcc
SUBDIRS = src/server src/relay
SERVICE_FILES = systemd/relay.service systemd/server.service systemd/camera.service

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

install: all
	install -m 755 src/server/server /usr/local/bin/server
	install -m 755 src/relay/mjpeg_relay /usr/local/bin/mjpeg_relay
	
	mkdir -p /etc/systemd/system
	for f in $(SERVICE_FILES); do \
		if [ -f "$$f" ]; then \
			install -m 644 $$f /etc/systemd/system/; \
		fi; \
	done
	
	systemctl daemon-reload
	@echo ""
	@echo "=== Installation Complete ==="
	@echo "To start services:"
	@echo "  sudo systemctl start relay.service"
	@echo "  sudo systemctl start server.service"
	@echo ""
	@echo "To enable auto-start on boot:"
	@echo "  sudo systemctl enable relay.service"
	@echo "  sudo systemctl enable server.service"
	@echo ""
	@echo "To check status:"
	@echo "  sudo systemctl status relay.service"
	@echo "  sudo systemctl status server.service"

uninstall:
	rm -f /usr/local/bin/server
	rm -f /usr/local/bin/mjpeg_relay
	
	rm -f /etc/systemd/system/relay.service
	rm -f /etc/systemd/system/server.service
	rm -f /etc/systemd/system/camera.service
	
	systemctl daemon-reload
	@echo "Uninstalled"

.PHONY: all clean install uninstall $(SUBDIRS)