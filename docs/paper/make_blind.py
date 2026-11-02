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
src = src.replace("~\\cite{jamiesonthesis}", "").replace("\\cite{jamiesonthesis,luuthesis}", "\\cite{luuthesis}")
for pat, rep in [(r" \(the lineage is Fig\.~\\ref\{fig:prov\} of Part~II\)", ""),
                 (r" \((?:the plugin model is )?Fig\.~\\ref\{fig:abi\} of Part~II\)", ""),
                 (r" \((?:Table~\\ref\{[^}]*\} in )?Appendix~\\ref\{sec:moved2?\}[^)]*\)", ""),
                 (r" Appendix~\\ref\{sec:moved\} gives the full text\.", ""),
                 (r" \(Part~II, Sec\.~\\ref\{[^}]*\}\)", ""), (r"; Part~II, Sec\.~\\ref\{[^}]*\}\)", ")"),
                 (r" \(Part~II, Table~\\ref\{tab:compare\}, is the full comparison\)", ""),
                 (r" Part~II, Sec\.~\\ref\{sec:verify\} explains each method\.", ""),
                 (r"; Part~II, Sec\.~\\ref\{sec:background-detail\}\)", ")"),
                 (r" Part~II gives the measured table\.", ""),
                 (r"; Part~II explains each method\.", "."),
                 (r" \(Appendix~\\ref\{sec:(?:register|appmethod|releases|status)\}\)", ""),
                 (r"; Appendix~\\ref\{sec:[a-z]+\}", ""),
                 (r" Appendix~\\ref\{sec:releases\} gives the record by release\.", ""),
                 (r"are those of the research log, which lists every hypothesis, including those that are not confirmed\.",
                  "are those of the full version of this paper, which lists every hypothesis, including those that are not confirmed."),
                 (r" The history of each experiment \(what was tried first, what was fixed\) is in Appendix~\\ref\{sec:exprecords\}\.", ""),
                 (r"A failed one is a line in the research log and the Discussion\.",
                  "A failed one is reported in the Discussion."),
                 (r"\(Sec\.~\\ref\{sec:abi\}\)", "")]:
    src, n = re.subn(pat, rep, src)
    print("blind pattern", n, pat[:50])
if "Part~II" in src:
    sys.exit("Part~II left: " + src[src.index("Part~II") - 80:src.index("Part~II") + 60])
sub("the Mimir~III dataset (Fig.~\\ref{fig:yggdrasil}):", "the Mimir~III dataset:")
sub(" (Fig.~\\ref{fig:yggdrasil}, in Part II)", "")
src = re.sub(r"The other tools branch from it and use it as a package:[^\n]*\n", "The other tools of the suite (an experiment runner and GUI, a simulation bridge, a game, a reverse-engineering tool, a placement experiment, the circuit knowledge base and a record-and-replay viewer) branch from it and use it as a package. All the code is public (URL withheld for review).\n", src)
sub(" The companion placement experiment~\\cite{nornsIII}, which uses the attribution, reports that it agreed with VPR's critical-path delay within 0.5\\% in 999 of 999 runs (a different property, not part of this verdict).", "")
sub("After the ``MAIking software'' moment of December 2025, when AI coding agents began", "After the moment of December 2025 when AI coding agents began")
sub("I state the caveats of this MAIking:", "I state the caveats of this build:")
sub("Then came what I call the ``MAIking software'' moment of December 2025: AI coding agents", "Then came the moment of December 2025: AI coding agents")
sub("Gharibian et al.\\ addressed it in placement by recovering", "Earlier work addressed it in placement by recovering")
sub("; our latest paper applies data-mining clustering to such algorithms", "; later work applies data-mining clustering to such algorithms")
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
for old in os.listdir("IMAGE/blind"):
    if old.endswith(".tex") and old[:-4] not in figs:
        os.remove(os.path.join("IMAGE/blind", old))
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
