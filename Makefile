CC = gcc
SUBDIRS = src/server src/relay
SERVICE_FILES = systemd/relay.service systemd/server.service

.PHONY: all clean install uninstall $(SUBDIRS)

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
	@echo "Installation complete"
	@echo "Start: sudo systemctl start relay.service server.service api-wrapper.service"
	@echo "Swagger: http://192.168.137.100:8000/docs"

uninstall:
	rm -f /usr/local/bin/server /usr/local/bin/mjpeg_relay
	rm -f /etc/systemd/system/relay.service /etc/systemd/system/server.service
	rm -f /etc/systemd/system/api-wrapper.service /etc/systemd/system/camera.service
	systemctl daemon-reload
	@echo "Uninstalled"

.PHONY: all clean install uninstall $(SUBDIRS)