attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  wire width 4 \k
  connect \y { 2'00 \a [3:2] }
  connect \k 4'0100
end
