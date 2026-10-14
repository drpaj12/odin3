attribute \top 1
module \t
  wire width 2 input 1 \s
  wire width 2 input 2 \a
  wire width 2 output 3 \y
  wire $n14
  wire $n17
  attribute \full_case 1
  cell $eq $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'10
    connect \Y $n14
  end
  attribute \full_case 1
  cell $eq $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n17
  end
  connect \y \a
end
