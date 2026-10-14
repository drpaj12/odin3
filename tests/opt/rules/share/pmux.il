attribute \top 1
module \t
  wire input 1 \s0
  wire input 2 \s1
  wire width 4 input 3 \a
  wire width 4 input 4 \b
  wire width 4 input 5 \c
  wire width 4 input 6 \d
  wire width 4 output 7 \y
  wire $n40
  wire $n45
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  attribute \full_case 1
  attribute \parallel_case 1
  cell $eq $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \B \s1
    connect \Y $n40
  end
  attribute \full_case 1
  attribute \parallel_case 1
  cell $eq $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \B \s0
    connect \Y $n45
  end
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c14
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A \d
    connect \B { $n54 $n53 $n52 $n51 $n54 $n53 $n52 $n51 }
    connect \S { $n45 $n40 }
    connect \Y \y
  end
  cell $pmux $c16
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A 4'xxxx
    connect \B { \b \c }
    connect \S { $n45 $n40 }
    connect \Y { $n50 $n49 $n48 $n47 }
  end
  cell $add $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n50 $n49 $n48 $n47 }
    connect \Y { $n54 $n53 $n52 $n51 }
  end
end
