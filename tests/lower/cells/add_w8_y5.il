attribute \top 1
module \m
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 5 output 3 \y
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  cell $add $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y { $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 }
  end
  connect \y { $n26 $n25 $n24 $n23 $n22 }
end
