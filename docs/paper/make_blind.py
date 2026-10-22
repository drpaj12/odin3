#!/usr/bin/env python3
"""Make odin3_blind.tex (FCCM double-blind, Part I only) from odin3.tex. Run in docs/paper."""
import os, re, sys
NAME, CODE = "AnonSynth", "anonsynth"
src = open("odin3.tex").read()
keys = ["jamieson2005odin","jamieson2011odin2","verilogTown","verilogtown2017","faruque2025gatebreaker",
        "gharibian2010finding","gharibian2013analyzing","gharibian2014identifying","jamieson2022datamining",
        "sarihi2024seekers"]
amap = {k: "anon%02d" % (i + 1) for i, k in enumerate(keys)}
def sub(old, new, s=None):
    global src
    n = src.count(old)
    if n != 1:
        sys.exit("count %d: %s" % (n, old[:70]))
    src = src.replace(old, new)
def rename(t):
    t = t.replace("Odin~III", NAME).replace("Odin III", NAME)
    return re.sub(r"(?<!IMAGE/)odin3(?!-tikz-styles)", CODE, t)
# Part I only: cut Part II and the second reference list
head, sep, _ = src.partition("\\bibliography{joined}\n")
assert sep
src = head + "\\bibliography{blind,joined}\n\n\\end{document}\n"
src = re.sub(r"\\usepackage\[resetlabels\]\{multibib\}\n\\newcites\{all\}\{[^}]*\}\n", "", src)
src = re.sub(r"% Part I keys[^\n]*\n\\nociteall\{[^}]*\}\n", "", src)
for pat, rep in [(r" \(Part~II, Sec\.~\\ref\{[^}]*\}\)", ""), (r"; Part~II, Sec\.~\\ref\{[^}]*\}\)", ")"),
                 (r" \(Part~II, Table~\\ref\{tab:compare\}, is the full comparison\)", ""),
                 (r"; Part~II, Sec\.~\\ref\{sec:verify\} explains each\.", "."),
                 (r"; Part~II details the system and its use after version~1\.0\.", ".")]:
    src, n = re.subn(pat, rep, src)
    if n == 0:
        sys.exit("no match: " + pat)
if "Part~II" in src:
    sys.exit("Part~II left: " + src[src.index("Part~II") - 80:src.index("Part~II") + 60])
# authors and URLs
sub("""\\author{\\IEEEauthorblockN{Peter Jamieson}
\\IEEEauthorblockA{Electrical and Computer Engineering, Miami University\\\\
\\todo{Oxford, OH, USA}\\\\
\\todo{jamiespa@miamioh.edu}}}""",
"""\\author{\\IEEEauthorblockN{Anonymous Author(s)}
\\IEEEauthorblockA{Affiliation withheld for double-blind review}}""")
sub("""started from nothing. I used Icarus Verilog~\\cite{icarus} as the front end, but Icarus treats Verilog as a simulation language, and building synthesis on a simulator's front end misled me and cost a young engineer a year of work to learn the basics of Verilog synthesis. I wrote Odin~II~\\cite{jamieson2011odin2} in my extra time during my post-doc at Imperial College, while my future wife was out of the country, and there I learned to write a compiler front end properly:""",
"""started from nothing. It used Icarus Verilog~\\cite{icarus} as the front end, but Icarus treats Verilog as a simulation language, and building synthesis on a simulator's front end cost a year of work to learn the basics of Verilog synthesis. Odin~II~\\cite{jamieson2011odin2} followed and wrote a compiler front end properly:""")
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
sub("Gharibian et al.\\ met it in placement", "Earlier work met it in placement")
sub("Midgard~III (verilogTown~\\cite{verilogtown2017})", "Midgard~III (a Verilog teaching game~\\cite{verilogtown2017})")
sub("I wrote Odin in 2005 and Odin~II in 2010.", "Odin was written in 2005 and Odin~II in 2010.")
sub("the original creation of Odin and Odin II.  We verified our original work with humans, which was costly.", "the original creation of Odin and Odin~II, which humans verified at high cost.")
sub("Our latest paper applies", "Later work applies")
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
for bad in ("drpaj12", "Jamieson", "Miami", "Odin~III", "Odin III", "verilogTown"):
    if bad in src:
        sys.exit("leak: " + bad)
hdr = ("% Double-blind 8-page version for FCCM (Part I only), made from odin3.tex by a script: authors,\n"
       "% self-citations (blind.bib), repository URLs and the tool name (placeholder " + NAME + ") withheld;\n"
       "% figures from IMAGE/blind/. On acceptance use odin3.tex.\n")
open("odin3_blind.tex", "w").write(hdr + src)
print("ok", figs)
