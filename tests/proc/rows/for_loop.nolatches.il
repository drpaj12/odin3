attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  wire width 32 signed \i
  connect \y { \a [0] \a [1] \a [2] \a [3] }
end
