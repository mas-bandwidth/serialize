# ROADMAP.md is generated, end to end, from docs/roadmap.sexp by tools/roadmap/render.lisp
# (SBCL), vendored from mas-bandwidth/serialize.moo (from moo, itself from mas-bandwidth/schema). Edit the data, then
# `make -f tools/roadmap/roadmap.mk roadmap` (this repo builds with CMake and ignores a root Makefile);
# never edit ROADMAP.md by hand. `make -f tools/roadmap/roadmap.mk roadmap-check` is the drift gate:
# it exits 1 when the committed page is not what the data renders.
SBCL ?= sbcl

.PHONY: roadmap roadmap-check
roadmap:
	@command -v $(SBCL) >/dev/null 2>&1 || { echo "make roadmap REFUSES: sbcl is not installed (tools/roadmap/render.lisp needs it)"; exit 2; }
	$(SBCL) --script tools/roadmap/render.lisp docs/roadmap.sexp ROADMAP.md

roadmap-check:
	@command -v $(SBCL) >/dev/null 2>&1 || { echo "make roadmap-check REFUSES: sbcl is not installed (tools/roadmap/render.lisp needs it)"; exit 2; }
	$(SBCL) --script tools/roadmap/render.lisp --check docs/roadmap.sexp ROADMAP.md
