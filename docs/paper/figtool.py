#!/usr/bin/env python3
"""Figure and packaging helper for the Odin III paper (called by the Makefile).

  figtool.py zip OUT.zip         write the Overleaf upload zip (stdlib only)
  figtool.py svg FIGURES.pdf     split the standalone figure PDF into IMAGE/svg/<name>.svg
                                 (needs PyMuPDF: pip install pymupdf)
  figtool.py ast                 convert IMAGE/fig_ast.svg to IMAGE/fig_ast.pdf
                                 (needs CairoSVG: pip install cairosvg, and system libcairo)

Run it from docs/paper. The page order of IMAGE/figures.tex is FIGURES below.
"""

import glob
import sys
import zipfile

# Page order of IMAGE/figures.tex: one TikZ figure per page.
FIGURES = ["pipeline", "project", "elab_seam", "proc", "util", "ir_model", "prov", "srcman",
           "techlib", "abi", "re", "yggdrasil"]


def overleaf_files() -> list[str]:
    """What the paper needs on Overleaf: the source, the bibliography, the shared TikZ styles,
    every TikZ figure, and the PDF converted from the one SVG figure."""
    figs = sorted(f for f in glob.glob("IMAGE/fig_*.tex"))
    return ["odin3.tex", "odin3.bib", "IMAGE/odin3-tikz-styles.tex", *figs, "IMAGE/fig_ast.pdf"]


def make_zip(out: str) -> None:
    files = overleaf_files()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(f)
    print(f"{out}: {len(files)} files")
    for f in files:
        print(f"  {f}")


def split_svg(pdf: str) -> None:
    import pymupdf  # noqa: PLC0415 (optional dependency)

    doc = pymupdf.open(pdf)
    if doc.page_count != len(FIGURES):
        sys.exit(f"{pdf}: {doc.page_count} pages, expected {len(FIGURES)} (see FIGURES)")
    for page, name in zip(doc, FIGURES):
        out = f"IMAGE/svg/fig_{name}.svg"
        with open(out, "w", encoding="utf-8") as f:
            f.write(page.get_svg_image(text_as_path=True))
        print(out)


def ast_pdf() -> None:
    import cairosvg  # noqa: PLC0415 (optional dependency)

    cairosvg.svg2pdf(url="IMAGE/fig_ast.svg", write_to="IMAGE/fig_ast.pdf")
    print("IMAGE/fig_ast.pdf")


def main() -> None:
    if len(sys.argv) >= 2 and sys.argv[1] == "zip" and len(sys.argv) == 3:
        make_zip(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == "svg":
        split_svg(sys.argv[2])
    elif len(sys.argv) == 2 and sys.argv[1] == "ast":
        ast_pdf()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
