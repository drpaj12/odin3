attribute \top 1
module \y_cap_add
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 32 output 3 \y
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  cell $add $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 9
    connect \A \a
    connect \B \b
    connect \Y { $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 $n50 }
  end
  connect \y { 23'00000000000000000000000 $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 $n50 }
end
