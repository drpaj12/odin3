// nest: -sv scoping across -f and -F, -f under -F (I5), -F entries against the nested list.
src/a.v
-f lists/one.f
src/b.v
-F lists/two.f
src/c.v
