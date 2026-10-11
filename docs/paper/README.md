# Odin III paper (IEEE conference draft)

`odin3.tex` is the paper in the IEEEtran conference class, `odin3.bib` its references, and
`IMAGE/` its figures: each figure is a Graphviz source `IMAGE/<name>.dot` with the rendered
`IMAGE/<name>.pdf` (included by the paper) and `IMAGE/<name>.png` (for quick previews), all
committed. Gaps that need measurements or Peter's input are marked `\todo{…}` in the text and
print as **[TODO: …]**.

## Build on Overleaf (primary)

1. Make the upload zip from this directory:

   ```bash
   cd docs/paper && zip -r odin3-paper.zip odin3.tex odin3.bib IMAGE
   ```

   (`make zip` does the same.)
2. In Overleaf: *New Project → Upload Project*, choose `odin3-paper.zip`.
3. *Menu → Compiler*: **pdfLaTeX**. Main document: `odin3.tex`.

No shell-escape is needed. The paper uses only packages Overleaf ships (IEEEtran, cite, amsmath,
graphicx, booktabs, url, listings), relative paths only, and `\graphicspath{{IMAGE/}}`;
Overleaf runs BibTeX itself. Overleaf cannot run `dot`, so the rendered PDFs in `IMAGE/` are
what it uses: re-render and re-upload them after changing a `.dot` file.

## Build locally (optional)

```bash
sudo apt install texlive-latex-extra texlive-publishers latexmk   # once (IEEEtran is in texlive-publishers)
make            # latexmk: pdfLaTeX + BibTeX -> odin3.pdf
make figures    # re-render every IMAGE/*.dot to .pdf and .png (needs graphviz)
make clean      # remove LaTeX build products
```

## Figures

| File | Shows |
|---|---|
| `fig_pipeline` | the system pipeline, built / specified / planned |
| `fig_project` | project-file formats filling the project record |
| `fig_elab_seam` | two-stage elaboration and the elaborated-AST seam |
| `fig_util` | the `src/util` containers |
| `fig_ir_model` | the IR object model and record sizes |
| `fig_prov` | the provenance lineage DAG (illustrative) |
| `fig_srcman` | the source manager's location space (2A spec §3.1 example) |
| `fig_ast` | the 24-byte AST record, child table, payloads, symbol table |
| `fig_techlib` | the `.o3lib` tech library and its users |
| `fig_abi` | the C ABI and plugin model |

## Sources

Every claim is taken from the repository: `docs/DESIGN.md`, `docs/IR.md`, `docs/ADR/`,
`docs/PHASE0.md`–`PHASE2.md`, `docs/specs/`, `docs/log_cost_of_tokens_and_time.md`, the headers
under `include/odin3/` and `src/`, and (for the comparison) the read-only Yosys and Odin II trees
under `~/odin3-ws/external`. Record sizes are `sizeof` on x86-64 with gcc. Bibliographic details
were written from memory and need checking before submission.
