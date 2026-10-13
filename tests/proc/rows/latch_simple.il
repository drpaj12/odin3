attribute \top 1
module \t
  wire input 1 \en
  wire width 4 input 2 \d
  wire width 4 output 3 \q
  wire $n10
  wire $n11
  wire $n12
  wire $n13
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  cell $not $c8
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \en
    connect \Y $n24
  end
  cell $reduce_or $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \Y $n25
  end
  cell $and $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n24
    connect \B $n25
    connect \Y $n26
  end
  cell $reduce_or $c11
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n26
    connect \Y $n27
  end
  cell $not $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n27
    connect \Y $n28
  end
  cell $dlatch $c13
    parameter \WIDTH 4
    parameter \EN_POLARITY 1
    connect \EN $n28
    connect \D { $n13 $n12 $n11 $n10 }
    connect \Q \q
  end
  attribute \full_case 1
  cell $mux $c14
    parameter \WIDTH 4
    connect \A \d
    connect \B \d
    connect \S \en
    connect \Y { $n13 $n12 $n11 $n10 }
  end
end
