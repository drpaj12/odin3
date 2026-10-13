attribute \top 1
module \t
  wire input 1 \en
  wire width 4 input 2 \d
  wire width 8 output 3 \y
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  cell $not $c4
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \d
    connect \Y { $n29 $n28 $n27 $n26 }
  end
  cell $not $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \en
    connect \Y $n36
  end
  cell $reduce_or $c10
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \Y $n37
  end
  cell $and $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n36
    connect \B $n37
    connect \Y $n38
  end
  cell $reduce_or $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n38
    connect \Y $n39
  end
  cell $not $c13
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n39
    connect \Y $n40
  end
  cell $dlatch $c14
    parameter \WIDTH 4
    parameter \EN_POLARITY 1
    connect \EN $n40
    connect \D { $n17 $n16 $n15 $n14 }
    connect \Q \y [3:0]
  end
  attribute \full_case 1
  cell $mux $c15
    parameter \WIDTH 4
    connect \A { $n29 $n28 $n27 $n26 }
    connect \B { $n29 $n28 $n27 $n26 }
    connect \S \en
    connect \Y { $n17 $n16 $n15 $n14 }
  end
  connect \y [7:4] \d
end
