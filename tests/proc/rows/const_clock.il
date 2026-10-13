attribute \top 1
module \t
  wire width 4 input 1 \d
  wire width 4 output 2 \q
  wire \c
  cell $dff $c5
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK 1'0
    connect \D \d
    connect \Q \q
  end
  connect \c 1'0
end
