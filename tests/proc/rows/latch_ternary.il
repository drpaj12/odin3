attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire width 4 input 4 \d
  wire width 4 output 5 \y
  wire \en
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  cell $mux $c6
    parameter \WIDTH 1
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y \en
  end
  cell $not $c11
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \en
    connect \Y $n27
  end
  cell $reduce_or $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \Y $n28
  end
  cell $and $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n27
    connect \B $n28
    connect \Y $n29
  end
  cell $reduce_or $c14
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n29
    connect \Y $n30
  end
  cell $not $c15
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n30
    connect \Y $n31
  end
  cell $dlatch $c16
    parameter \WIDTH 4
    parameter \EN_POLARITY 1
    connect \EN $n31
    connect \D { $n16 $n15 $n14 $n13 }
    connect \Q \y
  end
  attribute \full_case 1
  cell $mux $c17
    parameter \WIDTH 4
    connect \A \d
    connect \B \d
    connect \S \en
    connect \Y { $n16 $n15 $n14 $n13 }
  end
end
