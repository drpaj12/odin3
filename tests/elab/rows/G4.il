# generated: elab-check --write
attribute \top 1
module \g4_gen_down
  wire width 8 input 1 \a
  wire width 4 output 2 \y
  wire width 8 output 3 \z
  connect \y { \a [0] \a [1] \a [2] \a [3] }
  connect \z \a
end
