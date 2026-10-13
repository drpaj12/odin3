# generated: elab-check --write
attribute \top 1
module \m2_ansi
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 signed \s
  wire width 8 input 5 signed \ws
  wire width 4 output 6 \y
  wire width 4 output 7 \q
  wire width 5 output 8 signed \z
  connect \y \a
  connect \z { \s [3] \s }
end
