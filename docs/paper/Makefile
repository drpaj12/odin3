# Local build of the Odin III paper. Overleaf (pdfLaTeX) is the primary path; see README.md.
#
#   make              build/odin3.pdf with the system TeX Live (latexmk -pdf)
#   make figures      every TikZ figure standalone -> build/figures.pdf, then IMAGE/svg/*.svg
#   make fig-ast      IMAGE/fig_ast.svg -> IMAGE/fig_ast.pdf (the paper includes the PDF)
#   make zip          the Overleaf upload zip (ZIP=path to put it elsewhere)
#   make graphviz     re-render the old Graphviz figures in IMAGE/graphviz/ (needs dot)
#   make clean        remove build/
#
# Needs pdflatex, bibtex and latexmk from a TeX Live install. IEEEtran.cls and IEEEtran.bst (V1.8b,
# LPPL 1.3, see IEEEtran-NOTICE.md) sit beside odin3.tex.

VENV_PY  ?= $(HOME)/odin3-ws/work/tools/venv/bin/python
PYTHON3  ?= python3
LATEX    ?= latexmk
DOT      ?= dot
ZIP      ?= odin3-paper.zip

FIGTEX := IMAGE/odin3-tikz-styles.tex $(wildcard IMAGE/fig_*.tex)
SOURCES := odin3.tex register.tex joined.bib $(FIGTEX) IMAGE/fig_ast.pdf

.PHONY: all figures fig-ast zip graphviz clean

all: build/odin3.pdf

build/odin3.pdf: $(SOURCES)
	mkdir -p build
	$(LATEX) -pdf -bibtex -interaction=nonstopmode -halt-on-error -outdir=build odin3.tex
	@grep -a 'ODINFIT' build/odin3.log || true

figures: $(FIGTEX) IMAGE/figures.tex
	mkdir -p build IMAGE/svg
	cd IMAGE && $(LATEX) -pdf -interaction=nonstopmode -halt-on-error -outdir=../build figures.tex
	$(VENV_PY) -I figtool.py svg build/figures.pdf

fig-ast: IMAGE/fig_ast.svg
	$(VENV_PY) -I figtool.py ast

zip: $(SOURCES)
	$(PYTHON3) figtool.py zip $(ZIP)

graphviz:
	for f in IMAGE/graphviz/*.dot; do $(DOT) -Tpdf $$f -o $${f%.dot}.pdf || exit 1; done

clean:
	rm -rf build
