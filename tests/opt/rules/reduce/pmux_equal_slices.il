attribute \top 1
module \t
  wire input 1 \s0
  wire input 2 \s1
  wire input 3 \s2
  wire width 2 input 4 \a
  wire width 2 input 5 \b
  wire width 2 input 6 \c
  wire width 2 output 7 \y
  wire $n17
  wire $n20
  wire $n21
  wire $n22
  attribute \full_case 1
  attribute \parallel_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \B \s2
    connect \Y $n17
  end
  attribute \full_case 1
  attribute \parallel_case 1
  cell $eq $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \B \s1
    connect \Y $n20
  end
  attribute \full_case 1
  attribute \parallel_case 1
  cell $eq $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \B \s0
    connect \Y $n21
  end
  cell $reduce_or $c14
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { $n20 $n17 }
    connect \Y $n22
  end
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c15
    parameter \WIDTH 2
    parameter \S_WIDTH 2
    connect \A \c
    connect \B { \a \b }
    connect \S { $n21 $n22 }
    connect \Y \y
  end
end
