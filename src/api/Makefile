 PYTHON = python3
PIP = pip3
TARGET = api_wrapper.py
INSTALL_DIR = /usr/local/bin

.PHONY: all install run clean check deps

all: check

check:
	@$(PYTHON) -c "import fastapi" 2>/dev/null || (echo "Missing fastapi. Run: make deps" && exit 1)
	@$(PYTHON) -c "import uvicorn" 2>/dev/null || (echo "Missing uvicorn. Run: make deps" && exit 1)
	@$(PYTHON) -c "import requests" 2>/dev/null || (echo "Missing requests. Run: make deps" && exit 1)

deps:
	$(PIP) install --user fastapi uvicorn requests urllib3

install: check
	install -m 755 $(TARGET) $(INSTALL_DIR)/$(TARGET)

run:
	$(PYTHON) $(TARGET)

clean:
	rm -rf __pycache__ *.pyc

uninstall:
	rm -f $(INSTALL_DIR)/$(TARGET)

.PHONY: all install run clean check deps uninstall
