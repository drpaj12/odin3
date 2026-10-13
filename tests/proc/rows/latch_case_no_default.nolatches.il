attribute \top 1
module \t
  wire width 2 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 output 4 \y
  wire width 4 output 5 \z
  wire $n38
  wire $n43
  wire $n44
  wire $n49
  attribute \full_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n38
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
    connect \Y $n43
  end
  attribute \full_case 1
  cell $pmux $c12
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A 4'xxxx
    connect \B { \a \b }
    connect \S { $n43 $n38 }
    connect \Y \y
  end
  attribute \full_case 1
  cell $eq $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'10
    connect \Y $n44
  end
  attribute \full_case 1
  cell $eq $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n49
  end
  attribute \full_case 1
  cell $pmux $c15
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A 4'xxxx
    connect \B { \b \a }
    connect \S { $n49 $n44 }
    connect \Y \z
  end
end
