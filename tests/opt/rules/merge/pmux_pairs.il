attribute \top 1
module \t
  wire input 1 \s0
  wire input 2 \s1
  wire width 2 input 3 \a
  wire width 2 input 4 \b
  wire width 2 input 5 \c
  wire width 2 output 6 \y
  wire width 2 output 7 \z
  wire $n22
  wire $n25
  attribute \full_case 1
  attribute \parallel_case 1
  cell $eq $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \B \s1
    connect \Y $n22
  end
  attribute \full_case 1
  attribute \parallel_case 1
  cell $eq $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A 1'1
    connect \B \s0
    connect \Y $n25
  end
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c19
    parameter \WIDTH 2
    parameter \S_WIDTH 2
    connect \A \c
    connect \B { \a \b }
    connect \S { $n25 $n22 }
    connect \Y \y
  end
  connect \z \y
end
