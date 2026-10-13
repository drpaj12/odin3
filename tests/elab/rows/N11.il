# generated: elab-check --write
attribute \top 1
module \n11_net_arrays
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y
  wire width 4 \w[0]
  wire width 4 \w[1]
  wire \s[0]
  wire \s[1]
  wire \s[2]
  wire \s[3]
  connect \y \b
  connect \w[0] \a
  connect \w[1] \b
  connect \s[2] \a [0]
end
