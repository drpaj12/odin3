attribute \top 1
module \y_cap_mul
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 16 output 3 \y
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  cell $mul $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y { $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 }
  end
  connect \y { 8'00000000 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 }
end
