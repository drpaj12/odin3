attribute \top 1
module \t
  wire width 3 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 output 5 \y
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n38
  wire $n39
  wire $n40
  attribute \full_case 1
  cell $eq $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 3'010
    connect \Y $n30
  end
  attribute \full_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 3'011
    connect \Y $n31
  end
  attribute \full_case 1
  cell $eq $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 3'100
    connect \Y $n32
  end
  attribute \full_case 1
  cell $reduce_or $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A { $n32 $n31 $n30 }
    connect \Y $n33
  end
  attribute \full_case 1
  cell $eq $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 3'000
    connect \Y $n38
  end
  attribute \full_case 1
  cell $eq $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 3'001
    connect \Y $n39
  end
  attribute \full_case 1
  cell $reduce_or $c15
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { $n39 $n38 }
    connect \Y $n40
  end
  attribute \full_case 1
  cell $pmux $c16
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A \c
    connect \B { \a \b }
    connect \S { $n40 $n33 }
    connect \Y \y
  end
end
