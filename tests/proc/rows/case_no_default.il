attribute \top 1
module \t
  wire width 2 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 output 5 \y
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n29
  wire $n34
  wire $n35
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  attribute \full_case 1
  cell $eq $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'10
    connect \Y $n29
  end
  attribute \full_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n34
  end
  attribute \full_case 1
  cell $eq $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n35
  end
  cell $not $c14
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n35
    connect \Y $n37
  end
  cell $not $c15
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n34
    connect \Y $n38
  end
  cell $not $c16
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n29
    connect \Y $n39
  end
  cell $reduce_or $c17
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \Y $n40
  end
  cell $and $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n39
    connect \B $n40
    connect \Y $n41
  end
  cell $reduce_or $c19
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n41
    connect \Y $n42
  end
  cell $and $c20
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n38
    connect \B $n42
    connect \Y $n43
  end
  cell $reduce_or $c21
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n43
    connect \Y $n44
  end
  cell $and $c22
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n37
    connect \B $n44
    connect \Y $n45
  end
  cell $reduce_or $c23
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n45
    connect \Y $n46
  end
  cell $not $c24
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n46
    connect \Y $n47
  end
  cell $dlatch $c25
    parameter \WIDTH 4
    parameter \EN_POLARITY 1
    connect \EN $n47
    connect \D { $n22 $n21 $n20 $n19 }
    connect \Q \y
  end
  attribute \full_case 1
  cell $pmux $c26
    parameter \WIDTH 4
    parameter \S_WIDTH 3
    connect \A \a
    connect \B { \a \b \c }
    connect \S { $n35 $n34 $n29 }
    connect \Y { $n22 $n21 $n20 $n19 }
  end
end
