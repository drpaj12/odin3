attribute \top 1
module \t
  wire width 2 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 output 4 \y
  wire width 4 output 5 \z
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n37
  wire $n42
  wire $n43
  wire $n48
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  wire $n65
  attribute \full_case 1
  cell $eq $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n37
  end
  attribute \full_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n42
  end
  attribute \full_case 1
  cell $eq $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'10
    connect \Y $n43
  end
  attribute \full_case 1
  cell $eq $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n48
  end
  cell $not $c16
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n42
    connect \Y $n50
  end
  cell $not $c17
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n37
    connect \Y $n51
  end
  cell $reduce_or $c18
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \Y $n52
  end
  cell $and $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n51
    connect \B $n52
    connect \Y $n53
  end
  cell $reduce_or $c20
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n53
    connect \Y $n54
  end
  cell $and $c21
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n50
    connect \B $n54
    connect \Y $n55
  end
  cell $reduce_or $c22
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n55
    connect \Y $n56
  end
  cell $not $c23
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n56
    connect \Y $n57
  end
  cell $dlatch $c24
    parameter \WIDTH 4
    parameter \EN_POLARITY 1
    connect \EN $n57
    connect \D { $n22 $n21 $n20 $n19 }
    connect \Q \y
  end
  cell $not $c25
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n48
    connect \Y $n58
  end
  cell $not $c26
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n43
    connect \Y $n59
  end
  cell $reduce_or $c27
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \Y $n60
  end
  cell $and $c28
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n59
    connect \B $n60
    connect \Y $n61
  end
  cell $reduce_or $c29
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n61
    connect \Y $n62
  end
  cell $and $c30
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n58
    connect \B $n62
    connect \Y $n63
  end
  cell $reduce_or $c31
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n63
    connect \Y $n64
  end
  cell $not $c32
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n64
    connect \Y $n65
  end
  cell $dlatch $c33
    parameter \WIDTH 4
    parameter \EN_POLARITY 1
    connect \EN $n65
    connect \D { $n26 $n25 $n24 $n23 }
    connect \Q \z
  end
  attribute \full_case 1
  cell $pmux $c34
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A \a
    connect \B { \a \b }
    connect \S { $n42 $n37 }
    connect \Y { $n22 $n21 $n20 $n19 }
  end
  attribute \full_case 1
  cell $pmux $c35
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A \b
    connect \B { \b \a }
    connect \S { $n48 $n43 }
    connect \Y { $n26 $n25 $n24 $n23 }
  end
end
