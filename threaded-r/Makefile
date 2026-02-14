SHELL := /bin/bash

.PHONY: help init-layout bootstrap docs-check doc-garden smoke partial full release bench-quick bench-full abi

help:
	@echo "Targets:"
	@echo "  make init-layout   # create standard repo layout dirs"
	@echo "  make bootstrap     # clone upstream and configure/build ref + mtl builds"
	@echo "  make docs-check    # mechanical checks for agent-facing docs/plans"
	@echo "  make doc-garden    # report stale docs/plans and cleanup markers"
	@echo "  make smoke         # fast correctness + isolation checks"
	@echo "  make partial       # smoke + partial checks"
	@echo "  make full          # partial + full checks + full benchmark"
	@echo "  make release       # strict + drop-in release validation"
	@echo "  make bench-quick   # quick benchmark artifact refresh"
	@echo "  make bench-full    # fuller benchmark artifact refresh"
	@echo "  make abi           # ABI/package load smoke"

init-layout:
	@bash harness/scripts/layout-init.sh

bootstrap:
	@bash harness/scripts/bootstrap.sh

docs-check:
	@bash harness/scripts/check-docs.sh

doc-garden:
	@bash harness/scripts/doc-garden.sh

smoke:
	@bash harness/scripts/check-smoke.sh

partial:
	@bash harness/scripts/check-partial.sh

full:
	@bash harness/scripts/check-full.sh

release:
	@bash harness/scripts/validate-release.sh

bench-quick:
	@bash harness/scripts/bench-quick.sh

bench-full:
	@bash harness/scripts/bench-full.sh

abi:
	@bash harness/abi/run-abi-smoke.sh
