#!/usr/bin/env python3
"""Make odin3_blind.tex (FCCM double-blind, Part I only) from odin3.tex. Run in docs/paper."""
import os, re, sys
NAME, CODE = "AnonSynth", "anonsynth"
src = open("odin3.tex").read()
# Odin and Odin II stay cited: they are named in the text, so blinding them would point to the author.
keys = ["verilogTown","verilogtown2017","faruque2025gatebreaker",
        "gharibian2010finding","gharibian2013analyzing","gharibian2014identifying","jamieson2022datamining",
        "sarihi2024seekers"]
amap = {k: "anon%02d" % (i + 1) for i, k in enumerate(keys)}
def sub(old, new, s=None):
    global src
    n = src.count(old)
    if n != 1:
        sys.exit("count %d: %s" % (n, old[:70]))
    src = src.replace(old, new)
def cut(start, end, new="", keep_end=True):
    """Replace the text from start up to end (end stays) with new; each marker occurs once."""
    global src
    if src.count(start) != 1:
        sys.exit("cut start count %d: %s" % (src.count(start), start[:70]))
    i = src.index(start)
    j = src.index(end, i + len(start))
    src = src[:i] + new + (src[j:] if keep_end else src[j + len(end):])
# Suite and tool names (Norse theme) to placeholders, longest first.
NAMES = [("Leman~Russ~III", "AnonHT"), ("Leman Russ III", "AnonHT"), ("The Norns~III", "AnonPlace"),
         ("The Norns III", "AnonPlace"), ("Norns~III", "AnonPlace"), ("Norns III", "AnonPlace"),
         ("Yggdrasil~III", "AnonSuite"), ("Yggdrasil III", "AnonSuite"), ("Wyrd~III", "AnonGUI"),
         ("Wyrd III", "AnonGUI"), ("Loki~III", "AnonRE"), ("Loki III", "AnonRE"),
         ("Verdandi~III", "AnonTrace~III"), ("Verdandi III", "AnonTrace III"),
         ("Midgard~III", "AnonGame"), ("Midgard III", "AnonGame"), ("Bifrost~III", "AnonSim"),
         ("Bifrost III", "AnonSim"), ("M\\'{\\i}mir~III", "AnonData"), ("M\\'{i}mir~III", "AnonData"),
         ("M\\'{\\i}mir III", "AnonData"), ("M\\'{i}mir III", "AnonData"), ("M\u00edmir~III", "AnonData"),
         ("M\u00edmir III", "AnonData"), ("Mimir~III", "AnonData"), ("Mimir III", "AnonData")]
THEME = ("Yggdrasil", "Wyrd", "Loki", "Midgard", "Bifrost", "Leman", "Norns", "Norse", "world tree",
         "Verdandi", "Mimir", "M\\'{\\i}mir", "M\u00edmir", "Mimisbrunnr", "Urdarbrunnr", "Hvergelmir", "Gate-Breaker")
def rename(t):
    t = t.replace("Odin~III", NAME).replace("Odin III", NAME)
    for a, b in NAMES:
        t = t.replace(a, b)
    return re.sub(r"(?<!IMAGE/)odin3(?!-tikz-styles)", CODE, t)
# Part I only: cut Part II and the second reference list
head, sep, _ = src.partition("\\bibliography{joined}\n")
assert sep
src = head + "\\bibliography{blind,joined}\n\n\\end{document}\n"
src = re.sub(r"\\usepackage\[resetlabels\]\{multibib\}\n\\newcites\{all\}\{[^}]*\}\n", "", src)
src = re.sub(r"% Part I keys[^\n]*\n\\nociteall\{[^}]*\}\n", "", src)
# The glossary is inlined (the blind copy has no second file) and loses the self-citation of the thesis.
gloss = open("glossary.tex").read()
gloss = gloss.replace("~\\cite{jamiesonthesis}", "").replace("\\cite{jamiesonthesis,luuthesis}", "\\cite{luuthesis}")
gloss = gloss.replace("(a cluster in)", "(a cluster)")
# terms defined inline in the Introduction (tag ``% ref:``) are not repeated in the blind copy
src = src.replace("\\input{glossary}\n", "".join(ln for ln in gloss.splitlines(True)
                                                if not ln.startswith("%") and "% ref:" not in ln))
if "jamiesonthesis" in src.replace("\\cite{jamiesonthesis}", "", 0) and False:
    sys.exit("thesis")
src = src.replace("~\\cite{jamiesonthesis}", "").replace("\\cite{jamiesonthesis,luuthesis}", "\\cite{luuthesis}")
for pat, rep in [(r" \(Part~II, Sec\.~\\ref\{[^}]*\}\)", ""), (r"; Part~II, Sec\.~\\ref\{[^}]*\}\)", ")"),
                 (r" \(Part~II, Table~\\ref\{tab:compare\}, is the full comparison\)", ""),
                 (r" Part~II, Sec\.~\\ref\{sec:verify\} explains each method\.", ""),
                 (r"; Part~II, Sec\.~\\ref\{sec:background-detail\}\)", ")"),
                 (r" Part~II gives the measured table\.", ""),
                 (r"; Part~II explains each method\.", "."),
                 (r" \(Appendix~A\)", " (the research log)"),
                 (r"A failed one is a line in the research log \(the research log\) and the Discussion\.",
                  "A failed one is reported in the Discussion."),
                 (r"\(Sec\.~\\ref\{sec:abi\}\)", "")]:
    src, n = re.subn(pat, rep, src)
    print("blind pattern", n, pat[:50])
if "Part~II" in src:
    sys.exit("Part~II left: " + src[src.index("Part~II") - 80:src.index("Part~II") + 60])
# ---- FCCM page budget: 8 pages plus references. The open version keeps everything. --------------
# The suite paragraph shrinks to three sentences and the tree figure goes; the suite is Part II / open copy.
cut("I call this new suite Yggdrasil~III, the world tree", "\n\\begin{figure*}[tp]\n\\centering\n\\odinfit{\\textwidth}{\\input{IMAGE/fig_yggdrasil}}",
    "I call this new suite Yggdrasil~III. Odin~III is its trunk: every path through the suite passes its one provenance IR. "
    "The other tools of the suite (an experiment runner and GUI, a simulation bridge, a game, a reverse-engineering tool, "
    "a placement experiment, the circuit knowledge base and a record-and-replay viewer) branch from it and use it as a package. "
    "All the code is public (URL withheld for review).\n")
cut("\n\\begin{figure*}[tp]\n\\centering\n\\odinfit{\\textwidth}{\\input{IMAGE/fig_yggdrasil}}", "\\label{fig:yggdrasil}\n\\end{figure*}\n", "\n", keep_end=False)
sub("the Mimir~III dataset (Fig.~\\ref{fig:yggdrasil}):", "the Mimir~III dataset:")
# Table I is in Part II of the open version.
cut("\\begin{table*}[t]\n\\caption{Odin II, Yosys (with Parmys) and Odin III compared}", "Table~\\ref{tab:compare-short} summarizes the differences. Yosys is", "")
sub("Table~\\ref{tab:compare-short} summarizes the differences. Yosys is", "Odin~III differs from both predecessors in its core language, its netlist records, its provenance on every node and its linked ABC. Yosys is")
# III-A technical text: one paragraph instead of six.
cut("\\emph{Technical.} Odin~III reads a design into one IR and changes it one pass at a time.", "\\subsection{Checking against oracles}",
    "\\emph{Technical.} Odin~III reads a design into one IR and changes it one pass at a time (Fig.~\\ref{fig:flow}). "
    "Elaboration runs in two stages and the word-level passes follow Yosys~0.55; \\code{partial\\_map} binds multipliers, adders and RAMs to the blocks "
    "the architecture lists, with Parmys's rule as the default; \\code{lower} turns word cells into bit cells; a linked ABC maps the logic to LUTs; "
    "and \\code{write\\_blif --vtr} writes the BLIF that VPR reads. Where Odin~III and Parmys could differ, \\code{--vtr-compat} makes Odin~III do what "
    "Parmys does and print a WARNING; where Parmys is wrong to the RTL, Odin~III stays correct and declares the difference (Sec.~\\ref{sec:parmys-bugs}).\n\n")
# fig_flow_verify out (it is Fig. 2 of the open version).
cut("\\begin{figure*}[t]\n\\centering\n\\odinfit{\\textwidth}{\\input{IMAGE/fig_flow_verify}}", "\\label{fig:flow-verify}\n\\end{figure*}\n", "", keep_end=False)
cut("\\emph{Technical.} Figure~\\ref{fig:flow-verify} marks which technique checks which stage:", "\n\n\\subsection{One provenance-tracked IR}",
    "\\emph{Technical.} The techniques are refinement proofs by SAT (\\code{lower-equiv}, \\code{opt-equiv}), random module generators checked against Yosys \\code{techmap}, "
    "Verilator random vectors (\\code{fcheck}), golden BLIFs, and the exit-code convention of Sec.~\\ref{sec:abi-summary}.")
# cost: v1.0 table only (the open version also has the after-v1.0 table and the DeepSeek window).
cut("After v1.0 the suite work continued on the same process.", "\n% ------------------------------------------------------------------------------------------\n\\section{Experiments}",
    "")
# the companion Norns work is a sibling paper: no citation in the blind copy.
sub(" The companion work on The Norns~III placement experiment~\\cite{nornsIII}, which uses the attribution, reports that it agreed with VPR's critical-path delay within 0.5\\% in 999 of 999 runs (a different property, not part of this verdict).", "")
# identity cues: the coined idiom and the self-description
sub("After the ``MAIking software'' moment of December 2025, when AI coding agents began", "After the moment of December 2025 when AI coding agents began")
sub("I state the caveats of this MAIking:", "I state the caveats of this build:")
sub("Then came what I call the ``MAIking software'' moment of December 2025: AI coding agents", "Then came the moment of December 2025: AI coding agents")
sub("For an experienced engineer like myself who has the burden of ``job'' this superpower has allowed me to MAIke again.", "For an experienced engineer with a full-time job, this superpower allows making software again.")
# Background II-B to II-E trimmed (the open version keeps them in full).
cut("\\emph{The Odin~II flow.}", "\\subsection{VTR and partial mapping}",
    "\\emph{The Odin~II flow.} Odin~II~\\cite{jamieson2011odin2} parses Verilog into an abstract syntax tree (AST) and turns it into one flattened netlist of nodes, "
    "pins, and nets; it reads the VPR architecture file to decide which multipliers, adders, and memories become dedicated blocks, and writes the rest as BLIF for ABC~\\cite{brayton2010abc}.\n\n"
    "\\emph{The Yosys approach.} Yosys~\\cite{wolf2013yosys} parses Verilog into an AST, and \\code{genrtlil} turns each module into RTLIL, a word-level netlist with the hierarchy kept; "
    "every later step (\\code{proc}, \\code{opt}, \\code{flatten}, \\code{techmap}, \\code{abc}) is a pass over RTLIL that never sees the AST.\n\n")
cut("Both models attach to VTR~", "\\subsection{The provenance problem}",
    "Both models attach to VTR~\\cite{luu2009vpr5,rose2012vtr,luu2014vtr7,murray2020vtr8}: a front end, ABC, then VPR's packing, placement, and routing on an XML-described architecture. "
    "Luu's architecture description language~\\cite{luu2011packing} models each dedicated block's primitive, so a front end that reads the same file can map part of a design "
    "onto those blocks and leave the rest soft for ABC; VTR~7.0~\\cite{luu2014vtr7} and the study of hard adders~\\cite{luu2014hardadders} carried this to adders. "
    "Parmys~\\cite{parmys} brings the same partial mapping onto Yosys's word-level netlists and is now VTR's default front end.\n\n")
cut("Synthesis modifies some of the designer's functional intention.", "\\subsection{Oracles, ASTs and elaboration}",
    "Synthesis modifies the designer's structure: flattening removes the hierarchy, optimization merges and splits operators, and LUT mapping covers the logic with cells that no source line names. "
    "I call the task of linking each object of the final netlist back to the HDL that produced it the \\emph{provenance problem}. "
    "Earlier work addressed it in placement by recovering system-level information from the flat netlist~\\cite{gharibian2010finding,gharibian2013analyzing,gharibian2014identifying}, "
    "later work applies data-mining clustering to such algorithms~\\cite{jamieson2022datamining}, and Goeders and Wilton trace signals of a high-level synthesis-built circuit back to their source for in-system debug~\\cite{goeders2014effective,goeders2015dynamic,goeders2017signaltracing}. "
    "Each had to recover a link the synthesis tool didn't keep; Odin~III keeps it (Sec.~\\ref{sec:arch}).\n\n")
cut("\\code{tools/run-oracle.sh} runs an oracle", "\n% ------------------------------------------------------------------------------------------\n\\section{The Innovations}",
    "An oracle is run on a design and an architecture and its netlist, logs, and commit are stored as a golden; Odin~III's output is compared with the golden for identity after canonical renaming and for functional equivalence. "
    "An oracle is trusted because it is the reference, not because it is always right (Sec.~\\ref{sec:parmys-bugs}). "
    "An oracle makes the process much simpler than the original creation of Odin and Odin~II, which humans verified at high cost.\n")
# further trims to reach 8 pages: the community question, the implications bullets, the AI-tools paragraph of the ABI.
cut("The community may object to such a process.", "I call this new suite Yggdrasil~III.", "")
cut("What implications does this work push to our field:", "\n% ------------------------------------------------------------------------------------------\n\\section{Future Work}",
    "What this means for the field: the ``superpower'' of building complex software with agents is liberating for an experienced engineer, and for an inexperienced one it is an educational question, "
    "whether it builds or limits the engineer's semantic knowledge of synthesis. I believe the path forward is that knowledge, developed through first-principles exercises (humans building and testing the system) "
    "and the right moments for second-principle design, which is System Thinking~\\cite{cabrera2015systems}. What experiments I can now run is limited more by my imagination and time than by having to build the tool.\n")
cut("The same boundary is how AI tools attach.", "\n\\subsection{Odin III against Odin II and Yosys}",
    "The same boundary is how AI tools attach. Agents built Odin~III by driving its command-line scripts and reading its tools' exit codes (0 holds, 1 differs, 2 tool error, 3 hard failure, 4 unproven, 77 skipped), "
    "and through the Python binding an agent can walk the IR and its provenance and add cell types, recording its decisions as DERIVED provenance records. "
    "No hypothesis tests the ABI itself here; the suite papers use it.\n")
cut("Its limits are the ones stated in the Introduction.", "\n\\begin{table}[t]\n\\caption{Agent cost of Odin~III v1.0", "")
sub(" Active time passed 20\\,h on six of the 14 days because agents worked through the night.", "")
sub(" Hash names still depend on the directory of the design. The ABI keeps attributes as strings, node IDs are module-local, and there are no bulk readers.", "")
sub(": Phase~3 closed on the existing M2 and M3 data, because the front end to elaboration is the priority (PHASE3 decision 58).", ".")
cut("\\emph{What was not measured.}", "What this means for the field", "")
# authors and URLs
sub("""\\author{\\IEEEauthorblockN{Peter Jamieson}
\\IEEEauthorblockA{Electrical and Computer Engineering, Miami University\\\\
Oxford, OH, USA\\\\
jamiespa@miamioh.edu}}""",
"""\\author{\\IEEEauthorblockN{Anonymous Author(s)}
\\IEEEauthorblockA{Affiliation withheld for double-blind review}}""")
sub("""started from nothing. I used Icarus Verilog~\\cite{icarus} as the front end, but Icarus treats Verilog as a simulation language, and building synthesis on a simulator's front end misled me and cost a young engineer a year of work to learn the basics of Verilog synthesis. I wrote Odin~II~\\cite{jamieson2011odin2} in my extra time during my post-doc at Imperial College, while my future wife was out of the country, and there I learned to write a proper compiler front end:""",
"""started from nothing. It used Icarus Verilog~\\cite{icarus} as the front end, but Icarus treats Verilog as a simulation language, and building synthesis on a simulator's front end cost a year of work to learn the basics of Verilog synthesis. Odin~II~\\cite{jamieson2011odin2} followed and wrote a proper compiler front end:""")
sub("Odin~III is public at \\url{https://github.com/drpaj12/odin3}.", "Odin~III is public (URL withheld for review).")
sub("as a package \\url{https://github.com/drpaj12/YggdrasilIII}.", "as a package (URL withheld for review).")
sub("watched daily by the author of Odin and Odin~II", "watched daily by an experienced FPGA CAD developer")
# future experiments: no names that point to the authors' earlier work
sub("our earlier verilogTown \\cite{verilogTown}", "an earlier Verilog teaching game~\\cite{verilogTown}")
sub("I wrote Odin in 2005 and Odin~II in 2010.", "Odin was written in 2005 and Odin~II in 2010.")
sub("now called Midgard, to fit the Norse theme.", "now called Midgard~III.")
sub("Other programs (Loki, Wyrd, Norns, Mimir) can drive", "Other programs of the suite can drive")
sub("Verilog Synthesis for FPGAs - again\\\\", "Verilog Synthesis for FPGAs\\\\")
for bad in ("verilogTown", "Gate-Breaker", "Gharibian", "Faruque", "Seeker"):
    for m in re.finditer(bad, src):
        if "\\cite" not in src[m.start() - 2:m.start()] and not src[m.start() - 6:m.start()].endswith("{"):
            sys.exit("name left: " + src[m.start() - 60:m.start() + 40])
for k, a in amap.items():
    src = re.sub(r"(?<=[{,\s])%s(?=[,}\s])" % re.escape(k), a, src)
# placeholder name and blind figure copies
src = rename(src)
os.makedirs("IMAGE/blind", exist_ok=True)
figs = sorted(set(re.findall(r"\\input\{IMAGE/(fig_[^}]*)\}", src)))
figs.append("fig_flow_base")
for f in figs:
    t = rename(open("IMAGE/%s.tex" % f).read())
    t = t.replace("\\odinsub{(verilogTown)}", "\\odinsub{(Verilog teaching game)}")
    t = re.sub(r"\\input\{IMAGE/fig_", r"\\input{IMAGE/blind/fig_", t)
    open("IMAGE/blind/%s.tex" % f, "w").write(t)
src = src.replace("\\input{IMAGE/fig_", "\\input{IMAGE/blind/fig_")
body = "\n".join(ln for ln in src.splitlines() if not ln.lstrip().startswith("%"))
for f in figs:
    body += "\n".join(ln for ln in open("IMAGE/blind/%s.tex" % f).read().splitlines() if not ln.lstrip().startswith("%"))
for bad in ("drpaj12", "Jamieson", "Miami", "Odin~III", "Odin III", "verilogTown", "MAIk", "myself", "nornsIII",
            "r5-results", "Imperial", "wife") + THEME:
    if bad in body:
        i = body.index(bad)
        sys.exit("leak: " + body[i - 60:i + 40])
hdr = ("% Double-blind 8-page version for FCCM (Part I only), made from odin3.tex by a script: authors,\n"
       "% self-citations (blind.bib), repository URLs, the tool and suite names (placeholders " + NAME + ", AnonSuite, ...) withheld;\n"
       "% figures from IMAGE/blind/. On acceptance use odin3.tex.\n"
       "% Companion file: odin3.tex is the same paper with authors and names restored; this blind\n"
       "% copy is the version submitted to the conference, and the non-blind copy is used on acceptance.\n")
# comment lines of the open version name files and people: none goes into the blind copy
src = "".join(ln for ln in src.splitlines(True) if not ln.lstrip().startswith("%"))
open("odin3_blind.tex", "w").write(hdr + src)
print("ok", figs)
