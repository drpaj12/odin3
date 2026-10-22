# Local build of the Odin III paper. Overleaf (pdfLaTeX) is the primary path; see README.md.
#
#   make              odin3.pdf with Tectonic (default) or, with LATEX=latexmk, with TeX Live
#   make figures      every TikZ figure standalone -> build/figures.pdf, then IMAGE/svg/*.svg
#   make fig-ast      IMAGE/fig_ast.svg -> IMAGE/fig_ast.pdf (the paper includes the PDF)
#   make zip          the Overleaf upload zip (ZIP=path to put it elsewhere)
#   make graphviz     re-render the old Graphviz figures in IMAGE/graphviz/ (needs dot)
#   make clean        remove build/
#
# Tectonic is XeTeX-based and fetches TeX packages on demand; the wrapper below makes it use the
# same Times/Courier Type 1 fonts as pdfLaTeX, so page breaks and figure fits match Overleaf.

TECTONIC ?= $(HOME)/odin3-ws/work/tools/tectonic/tectonic
VENV_PY  ?= $(HOME)/odin3-ws/work/tools/venv/bin/python
PYTHON3  ?= python3
LATEX    ?= tectonic
DOT      ?= dot
ZIP      ?= odin3-paper.zip

FIGTEX := IMAGE/odin3-tikz-styles.tex $(wildcard IMAGE/fig_*.tex)
SOURCES := odin3.tex joined.bib $(FIGTEX) IMAGE/fig_ast.pdf

.PHONY: all figures fig-ast zip graphviz clean

all: build/odin3.pdf

build/odin3.pdf: $(SOURCES)
	mkdir -p build
ifeq ($(LATEX),tectonic)
	printf '%s\n' '\let\ODINdocumentclass\documentclass' \
	    '\def\documentclass[#1]#2{\ODINdocumentclass[#1]{#2}\usepackage[OT1]{fontenc}}' \
	    '\input{odin3.tex}' > build/odin3-local.tex
	$(TECTONIC) -X compile --keep-logs --outdir build -Z search-path=$(CURDIR) build/odin3-local.tex
	mv build/odin3-local.pdf build/odin3.pdf
	mv build/odin3-local.log build/odin3.log
else
	$(LATEX) -pdf -bibtex -interaction=nonstopmode -halt-on-error -outdir=build odin3.tex
endif
	@grep -a 'ODINFIT' build/odin3.log || true

figures: $(FIGTEX) IMAGE/figures.tex
	mkdir -p build IMAGE/svg
	cd IMAGE && $(TECTONIC) -X compile --keep-logs --outdir ../build figures.tex
	$(VENV_PY) -I figtool.py svg build/figures.pdf

fig-ast: IMAGE/fig_ast.svg
	$(VENV_PY) -I figtool.py ast

zip: $(SOURCES)
	$(PYTHON3) figtool.py zip $(ZIP)

graphviz:
	for f in IMAGE/graphviz/*.dot; do $(DOT) -Tpdf $$f -o $${f%.dot}.pdf || exit 1; done

clean:
	rm -rf build
