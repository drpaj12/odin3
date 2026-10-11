# Optional local build of the Odin III paper. Overleaf (pdfLaTeX) is the primary path; see
# README.md. `make` builds odin3.pdf with latexmk (pdfLaTeX + BibTeX); `make figures`
# re-renders every IMAGE/*.dot to .pdf (used by the paper) and .png (for previews).

DOT     ?= dot
LATEXMK ?= latexmk

DOTS := $(wildcard IMAGE/*.dot)
PDFS := $(DOTS:.dot=.pdf)
PNGS := $(DOTS:.dot=.png)

.PHONY: all figures clean zip

all: odin3.pdf

odin3.pdf: odin3.tex odin3.bib $(PDFS)
	$(LATEXMK) -pdf -bibtex -interaction=nonstopmode -halt-on-error odin3.tex

figures:
	for f in $(DOTS); do \
	    $(DOT) -Tpdf $$f -o $${f%.dot}.pdf && \
	    $(DOT) -Tpng -Gdpi=150 $$f -o $${f%.dot}.png || exit 1; \
	done

IMAGE/%.pdf: IMAGE/%.dot
	$(DOT) -Tpdf $< -o $@

IMAGE/%.png: IMAGE/%.dot
	$(DOT) -Tpng -Gdpi=150 $< -o $@

zip:
	zip -r -FS odin3-paper.zip odin3.tex odin3.bib IMAGE

clean:
	$(LATEXMK) -C -bibtex odin3.tex
