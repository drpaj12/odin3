# generated: elab-check --write
attribute \top 1
module \n2_ascending
  wire width 4 upto input 1 \a
  wire width 3 upto offset 1 output 2 \y
  wire width 4 upto offset -1 output 3 \z
  connect \y \a [2:0]
  connect \z \a
end
