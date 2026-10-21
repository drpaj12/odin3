# Odin III paper (IEEE conference draft)

`odin3.tex` is the paper in the IEEEtran conference class, `odin3.bib` its references, and
`IMAGE/` its figures. Gaps that need measurements or Peter's input are marked `\todo{…}` in the
text and print as **[TODO: …]**.

The paper has two parts. Part I (Sections I to VI, then the references) is the 8-page FCCM paper:
the "final frontier" story of Odin, Odin II and Odin III, background, architecture, results,
discussion and conclusion. Part II (Sections VII to XVI, after the references) describes the system
in detail for its users after v1.0.

## Figures

Fifteen figures are TikZ, drawn at print size in the paper's fonts; one, the byte-level AST record,
is a hand-drawn SVG.

- `IMAGE/odin3-tikz-styles.tex` holds the shared palette and styles. The palette is ColorBrewer
  Blues and Greys, which is colourblind-safe. It also holds the build-status key and two
  helpers: `\odinrecord`, a struct drawn as a matrix, and `\odinfit`, a safety net that scales a
  figure down only if it is wider than the column and writes `ODINFIT` to the log when it does.
- `IMAGE/fig_<name>.tex` holds one `tikzpicture` per figure. The paper pulls each one in with
  `\odinfit{\columnwidth}{\input{IMAGE/fig_<name>}}`, or `\textwidth` inside `figure*`. The
  preamble loads `tikz` with the `positioning`, `fit`, `arrows.meta`, `shapes.geometric`,
  `backgrounds`, `calc` and `matrix` libraries, then inputs the styles file.
- `IMAGE/fig_ast.svg` is the source of the AST figure. The paper includes `IMAGE/fig_ast.pdf`,
  converted from the SVG with CairoSVG (`make fig-ast`), because the `svg` package needs
  shell-escape and Inkscape. Commit both files.
- `IMAGE/svg/fig_<name>.svg` are SVG exports of the TikZ figures for READMEs and the web
  (`make figures`). They are generated files, and the paper does not use them.
- `IMAGE/figures.tex` builds every TikZ figure standalone, one per page, for previews and the
  SVG export. It is not part of the paper.
- `IMAGE/graphviz/` keeps the original Graphviz sources and PDFs as a fallback. To go back to
  one, replace its `\odinfit{…}{\input{…}}` line with
  `\includegraphics[width=\columnwidth]{graphviz/fig_<name>}`.

Build status is coded by fill and by border, so it survives greyscale printing:

| State | Look | Meaning |
|---|---|---|
| built | mid-blue fill, solid border | merged on `main` with its exit test |
| in progress | pale-blue fill, solid border | being implemented against an approved plan |
| specified | white fill, dashed blue border | approved specification and plan |
| planned | white fill, dotted grey border | a later phase (Phase 3's plan is approved) |

The status shown is as of the v1.0.0 release, 2026-10-21 (v1.0 phases: ADR D11):

- **Built:** Phases 0 to 7, all closed (see `docs/RELEASE-NOTES-1.0.md`). FF feature matching (4.6)
  and the CIRCT, EDIF and VQM writers were dropped from v1.0.
- **Planned (white, dotted):** the `slang` front end and the executable hooks.
- **After v1.0:** SystemVerilog (slang), VHDL (GHDL), Wyrd-I and Loki, in their own repositories.

When the status changes, update the `built`/`inprog`/`spec`/`planned` keys in the figures, the
features table and the phases table together.

| File | Shows | Kind | Width |
|---|---|---|---|
| `fig_pipeline` | the system pipeline, with build status | TikZ | `\textwidth` |
| `fig_project` | four project formats filling one record | TikZ | `\columnwidth` |
| `fig_elab_seam` | two-stage elaboration and the elaborated-AST seam | TikZ | `\textwidth` |
| `fig_proc` | the `proc` pass: Yosys 0.55 `proc_*` sub-passes as phases (2E spec §2) | TikZ | `\columnwidth` |
| `fig_util` | the `src/util` containers and what each uses | TikZ | `\columnwidth` |
| `fig_ir_model` | the IR object model, record sizes and ID references | TikZ | `\textwidth` |
| `fig_prov` | the provenance lineage DAG (illustrative) | TikZ | `\columnwidth` |
| `fig_srcman` | the source manager's location space as an arc diagram (2A spec §3.1 example) | TikZ | `\columnwidth` |
| `fig_ast` | the 24-byte AST record, node, child and payload tables | SVG → PDF | `\columnwidth` |
| `fig_techlib` | the `.o3lib` tech library and its users | TikZ | `\columnwidth` |
| `fig_abi` | the C ABI and plugin model | TikZ | `\columnwidth` |
| `fig_re` | the reverse-engineering and analysis flow | TikZ | `\columnwidth` |
| `fig_yggdrasil` | Yggdrasil: Odin III as the trunk, with Wyrd-I, Loki, Leman Russ, Bifrost and Midgard | TikZ | `\textwidth` |
| `fig_flow` | the flow of a design, with what each stage does (drawn by `fig_flow_base`) | TikZ | `\textwidth` |
| `fig_flow_verify` | the same flow, annotated with the verification techniques | TikZ | `\textwidth` |
| `fig_flow_prov` | the same flow, annotated with the provenance records | TikZ | `\textwidth` |

## Build on Overleaf (primary)

1. Make the upload zip from this directory with `make zip`, or `make zip ZIP=/some/path.zip`.
   It uses Python's `zipfile`, so `zip` is not needed. The zip holds:
   - `odin3.tex` and `odin3.bib`;
   - `IMAGE/odin3-tikz-styles.tex`;
   - every `IMAGE/fig_*.tex`;
   - `IMAGE/fig_ast.pdf`.

   It leaves out `figures.tex`, `svg/` and `graphviz/`.
2. In Overleaf, choose *New Project → Upload Project* and pick the zip.
3. Under *Menu → Compiler*, choose **pdfLaTeX**. The main document is `odin3.tex`.

No shell-escape is needed. The paper uses only packages that Overleaf ships: IEEEtran, cite,
amsmath, graphicx, booktabs, array, url, listings and tikz. It uses relative paths only, and Overleaf
runs BibTeX itself. pdfLaTeX warns that `fig_ast.pdf` is PDF 1.7. That warning is harmless.

## Build locally (optional)

No TeX Live is installed here. The local path is [Tectonic](https://tectonic-typesetting.github.io)
0.17.0: the static `x86_64-unknown-linux-musl` release, with its SHA-256 checked against the
GitHub release. It lives at `~/odin3-ws/work/tools/tectonic/tectonic` and fetches TeX packages on
demand. Tectonic is XeTeX-based, so the Makefile compiles through a wrapper that loads
`[OT1]{fontenc}`. That makes Tectonic use the same Times/Courier Type 1 fonts as pdfLaTeX, so
line breaks, page count and figure fits match Overleaf.

The figure helpers run from a venv at `~/odin3-ws/work/tools/venv` with `pymupdf` and
`cairosvg` installed. CairoSVG also needs the system `libcairo2`.

```bash
make                  # build/odin3.pdf (Tectonic); make LATEX=latexmk for a TeX Live install
make figures          # standalone figures -> build/figures.pdf -> IMAGE/svg/*.svg
make fig-ast          # IMAGE/fig_ast.svg -> IMAGE/fig_ast.pdf
make zip              # odin3-paper.zip for Overleaf
make graphviz         # re-render IMAGE/graphviz/*.pdf (needs graphviz)
make clean            # remove build/
```

`figtool.py` holds the Python behind `figures`, `fig-ast` and `zip`.

## Sources

Every claim is taken from the repository:

- `docs/DESIGN.md`, `docs/IR.md`, `docs/ADR/` and `docs/PHASE0.md`–`PHASE3.md` (PHASE2 #16 and
  PHASE3 #28, Peter's rulings of 2026-10-12, come from PR #75);
- `git log` (merge commits and their review rounds);
- `docs/specs/` and `docs/log_cost_of_tokens_and_time.md`;
- the headers under `include/odin3/` and `src/`;
- for the comparison, the read-only Yosys and Odin II trees under `~/odin3-ws/external`.

Record sizes are `sizeof` on x86-64 with gcc. The bibliographic details were written from memory
and need checking before submission.
