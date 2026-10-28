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
src = src.replace("\\input{glossary}\n", "".join(ln for ln in gloss.splitlines(True) if not ln.startswith("%")))
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
sub("All the code is available at \\url{https://github.com/drpaj12/YggdrasilIII}.", "All the code is public (URL withheld for review).")
sub("as a package \\url{https://github.com/drpaj12/YggdrasilIII}.", "as a package (URL withheld for review).")
sub("watched daily by the author of Odin and Odin~II", "watched daily by an experienced FPGA CAD developer")
sub("which is why I see it as the final frontier of the Odin line (at least for me).", "which is why I see it as a final frontier.")
# future experiments: no names that point to the authors' earlier work
sub("our earlier verilogTown \\cite{verilogTown}", "an earlier Verilog teaching game~\\cite{verilogTown}")
sub("a port of my verilogTown Verilog teaching game", "a port of an earlier Verilog teaching game")
sub("continues the netlist-to-RTL work of Faruque et al.~", "continues earlier netlist-to-RTL work~")
sub("the placement experiment of Gharibian et al.~", "an earlier placement experiment~")
sub("Gharibian et al.\\ addressed it in placement", "Earlier work addressed it in placement")
sub("Midgard~III (verilogTown~\\cite{verilogtown2017})", "Midgard~III (a Verilog teaching game~\\cite{verilogtown2017})")
sub("I wrote Odin in 2005 and Odin~II in 2010.", "Odin was written in 2005 and Odin~II in 2010.")
sub("the original creation of Odin and Odin II.  We verified our original work with humans, which was costly.", "the original creation of Odin and Odin~II, which humans verified at high cost.")
sub("now called Midgard, to fit the Norse theme.", "now called Midgard~III.")
sub("Yggdrasil~III, the world tree (Fig.", "Yggdrasil~III (Fig.")
sub(" (Mimisbrunnr, Urdarbrunnr and Hvergelmir)", "")
sub("Other programs (Loki, Wyrd, Norns, Mimir) can drive", "Other programs of the suite can drive")
sub("Verilog Synthesis for FPGAs - again\\\\", "Verilog Synthesis for FPGAs\\\\")
sub("Our latest paper applies", "Later work applies")
for bad in ("verilogTown", "Gate-Breaker", "Gharibian", "Faruque", "Seeker"):
    for m in re.finditer(bad, src):
        if "\\cite" not in src[m.start() - 2:m.start()] and not src[m.start() - 6:m.start()].endswith("{"):
            sys.exit("name left: " + src[m.start() - 60:m.start() + 40])
for k, a in amap.items():
    src = re.sub(r"(?<=[{,\s])%s(?=[,}\s])" % re.escape(k), a, src)
# Wells of the tree figure: the Norse well names, glosses and the self-cited baseline are withheld.
WELLS = [("Mimisbrunnr", "Well 1"), ("Urdarbrunnr", "Well 2"), ("Hvergelmir", "Well 3"),
         ("{Mimir's well of wisdom: prior work, benchmarks}", "{prior work, benchmarks}"),
         ("{the Norns' well: FPGA CAD and its views}", "{FPGA CAD and its views}"),
         ("{the spring of all rivers: synthesis sources}", "{synthesis sources}"),
         ("Gate-Breaker", "netlist-to-RTL baseline")]
def blind_tree(t):
    t = "\n".join(ln for ln in t.splitlines() if not ln.lstrip().startswith("%")) + "\n"
    for a, b in WELLS:
        if a not in t:
            sys.exit("tree: missing " + a)
        t = t.replace(a, b)
    return t
# placeholder name and blind figure copies
src = rename(src)
os.makedirs("IMAGE/blind", exist_ok=True)
figs = sorted(set(re.findall(r"\\input\{IMAGE/(fig_[^}]*)\}", src)))
figs.append("fig_flow_base")
for f in figs:
    t = rename(open("IMAGE/%s.tex" % f).read())
    t = t.replace("\\odinsub{(verilogTown)}", "\\odinsub{(Verilog teaching game)}")
    if f == "fig_yggdrasil":
        t = blind_tree(t)
    t = re.sub(r"\\input\{IMAGE/fig_", r"\\input{IMAGE/blind/fig_", t)
    open("IMAGE/blind/%s.tex" % f, "w").write(t)
src = src.replace("\\input{IMAGE/fig_", "\\input{IMAGE/blind/fig_")
# Odin and Odin II stay cited, so the tree is scaled to 66% to keep 8 pages.
sub("\\odinfit{\\textwidth}{\\input{IMAGE/blind/fig_yggdrasil}}",
    "\\scalebox{0.66}{\\odinfit{\\textwidth}{\\input{IMAGE/blind/fig_yggdrasil}}}")
body = "\n".join(ln for ln in src.splitlines() if not ln.lstrip().startswith("%"))
for f in figs:
    body += "\n".join(ln for ln in open("IMAGE/blind/%s.tex" % f).read().splitlines() if not ln.lstrip().startswith("%"))
for bad in ("drpaj12", "Jamieson", "Miami", "Odin~III", "Odin III", "verilogTown") + THEME:
    if bad in body:
        i = body.index(bad)
        sys.exit("leak: " + body[i - 60:i + 40])
hdr = ("% Double-blind 8-page version for FCCM (Part I only), made from odin3.tex by a script: authors,\n"
       "% self-citations (blind.bib), repository URLs, the tool and suite names (placeholders " + NAME + ", AnonSuite, ...) withheld;\n"
       "% figures from IMAGE/blind/. On acceptance use odin3.tex.\n"
       "% Companion file: odin3.tex is the same paper with authors and names restored; this blind\n"
       "% copy is the version submitted to the conference, and the non-blind copy is used on acceptance.\n")
open("odin3_blind.tex", "w").write(hdr + src)
print("ok", figs)
