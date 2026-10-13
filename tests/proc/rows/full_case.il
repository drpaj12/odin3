attribute \top 1
module \t
  wire width 2 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 output 5 \y
  wire $n30
  wire $n35
  wire $n36
  attribute \full_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'10
    connect \Y $n30
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
    connect \Y $n35
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
    connect \Y $n36
  end
  attribute \full_case 1
  cell $pmux $c13
    parameter \WIDTH 4
    parameter \S_WIDTH 3
    connect \A 4'xxxx
    connect \B { \a \b \c }
    connect \S { $n36 $n35 $n30 }
    connect \Y \y
  end
end
