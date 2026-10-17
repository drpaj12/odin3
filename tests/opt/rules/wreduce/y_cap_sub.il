attribute \top 1
module \y_cap_sub
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 16 output 3 \y
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  cell $sub $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 9
    connect \A \a
    connect \B \b
    connect \Y { $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 }
  end
  connect \y { $n41 $n41 $n41 $n41 $n41 $n41 $n41 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 }
end
