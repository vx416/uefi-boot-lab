OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd

.PHONY: build collect-linux e2e-linux ovmf run

build:
	./scripts/build-firmware-view.sh

collect-linux:
	./scripts/collect-linux-view.sh

e2e-linux:
	./scripts/run-e2e-linux.sh $(OVMF_CODE)

ovmf:
	./scripts/build-ovmf-with-producer.sh

run:
	@echo "Usage: ./scripts/run-ovmf.sh /path/to/OVMF_CODE.fd /path/to/fat-dir"
