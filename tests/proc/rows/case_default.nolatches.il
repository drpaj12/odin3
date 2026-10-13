attribute \top 1
module \t
  wire width 2 input 1 \s
  wire width 8 input 2 \a
  wire width 8 input 3 \b
  wire width 8 input 4 \c
  wire width 8 input 5 \d
  wire width 8 output 6 \y
  wire $n61
  wire $n70
  wire $n71
  attribute \full_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'10
    connect \Y $n61
  end
  attribute \full_case 1
  cell $eq $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n70
  end
  attribute \full_case 1
  cell $eq $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n71
  end
  attribute \full_case 1
  cell $pmux $c13
    parameter \WIDTH 8
    parameter \S_WIDTH 3
    connect \A \d
    connect \B { \a \b \c }
    connect \S { $n71 $n70 $n61 }
    connect \Y \y
  end
end
