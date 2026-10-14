attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  connect \y { \a [3] 1'0 \a [1] 1'0 }
end
