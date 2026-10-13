attribute \top 1
module \t
  wire width 4 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 input 5 \d
  wire width 4 output 6 \y
  wire $n35
  wire $n40
  attribute \full_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \s [3:1]
    connect \B 3'001
    connect \Y $n35
  end
  attribute \full_case 1
  cell $eq $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s [3:2]
    connect \B 2'01
    connect \Y $n40
  end
  attribute \full_case 1
  cell $pmux $c12
    parameter \WIDTH 4
    parameter \S_WIDTH 3
    connect \A \d
    connect \B { \a \b \c }
    connect \S { \s [3] $n40 $n35 }
    connect \Y \y
  end
end
