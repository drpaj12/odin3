attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y
  wire width 4 \c
  connect \y \b
  connect \c 4'0000
end
